#include "dns_ip_blocker.h"

#include <Arduino.h>
#include <WiFi.h>
#include <cctype>
#include <cstring>

#include "freertos/FreeRTOS.h"

namespace {

constexpr size_t kBlockedIpCapacity = 48;
constexpr size_t kRuleSnapshotBytes = 1024;
constexpr uint32_t kIpEntryTtlSeconds = 3600;
constexpr uint32_t kRuleResolveIntervalSeconds = 15;

struct CachedIpEntry {
    uint32_t ipHostOrder = 0;
    char domain[128] = {};
    uint32_t lastSeenAtSeconds = 0;
};

portMUX_TYPE gDnsIpPolicyMux = portMUX_INITIALIZER_UNLOCKED;
CachedIpEntry gBlockedIps[kBlockedIpCapacity];
size_t gBlockedIpCount = 0;
char gBlocklistSnapshot[kRuleSnapshotBytes] = {};
uint32_t gLastBlockResolveAtSeconds = 0;
size_t gNextBlockResolveRuleIndex = 0;
int gPolicyEnabled = 0;
int gStrictAllow = 0;

String normalizeDomain(const char* domain) {
    String normalized = domain == nullptr ? "" : String(domain);
    normalized.trim();
    normalized.toLowerCase();
    while (normalized.endsWith(".")) {
        normalized.remove(normalized.length() - 1);
    }
    return normalized;
}

bool hasRules(const char* rules) {
    if (rules == nullptr) return false;

    const char* cursor = rules;
    while (*cursor) {
        while (*cursor && (isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) {
            ++cursor;
        }
        if (!*cursor) break;
        return true;
    }
    return false;
}

bool snapshotMatches(const char* rules, const char* snapshot) {
    const char* current = rules == nullptr ? "" : rules;
    const char* expected = snapshot == nullptr ? "" : snapshot;
    return strcmp(current, expected) == 0;
}

void clearEntries(CachedIpEntry* entries, size_t capacity, size_t& count) {
    for (size_t i = 0; i < capacity; ++i) {
        entries[i] = CachedIpEntry{};
    }
    count = 0;
}

void copySnapshot(char* dest, size_t destSize, const char* rules) {
    memset(dest, 0, destSize);
    if (rules == nullptr || destSize == 0) return;
    strncpy(dest, rules, destSize - 1);
}

void pruneEntriesLocked(CachedIpEntry* entries, size_t capacity, size_t& count, uint32_t nowSeconds) {
    size_t writeIndex = 0;
    for (size_t i = 0; i < count; ++i) {
        if (nowSeconds - entries[i].lastSeenAtSeconds <= kIpEntryTtlSeconds) {
            if (writeIndex != i) {
                entries[writeIndex] = entries[i];
            }
            ++writeIndex;
        }
    }

    for (size_t i = writeIndex; i < capacity; ++i) {
        entries[i] = CachedIpEntry{};
    }
    count = writeIndex;
}

void addOrRefreshEntryLocked(CachedIpEntry* entries, size_t capacity, size_t& count,
                             const String& domain, uint32_t ipHostOrder, uint32_t nowSeconds) {
    if (ipHostOrder == 0) return;

    for (size_t i = 0; i < count; ++i) {
        if (entries[i].ipHostOrder == ipHostOrder) {
            entries[i].lastSeenAtSeconds = nowSeconds;
            if (domain.length() > 0) {
                domain.toCharArray(entries[i].domain, sizeof(entries[i].domain));
            }
            return;
        }
    }

    CachedIpEntry entry = {};
    entry.ipHostOrder = ipHostOrder;
    entry.lastSeenAtSeconds = nowSeconds;
    domain.toCharArray(entry.domain, sizeof(entry.domain));

    if (count < capacity) {
        entries[count++] = entry;
        return;
    }

    size_t oldestIndex = 0;
    for (size_t i = 1; i < count; ++i) {
        if (entries[i].lastSeenAtSeconds < entries[oldestIndex].lastSeenAtSeconds) {
            oldestIndex = i;
        }
    }
    entries[oldestIndex] = entry;
}

bool getRuleByIndex(const char* rules, size_t targetIndex, String& outRule) {
    outRule = "";
    if (rules == nullptr) return false;

    size_t currentIndex = 0;
    const char* cursor = rules;
    while (*cursor) {
        while (*cursor && (isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) {
            ++cursor;
        }
        if (!*cursor) break;

        const char* start = cursor;
        while (*cursor && !(isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) {
            ++cursor;
        }

        if (currentIndex == targetIndex) {
            String rule(start, cursor - start);
            outRule = normalizeDomain(rule.c_str());
            return !outRule.isEmpty();
        }
        ++currentIndex;
    }

    return false;
}

size_t countRules(const char* rules) {
    size_t count = 0;
    String rule;
    while (getRuleByIndex(rules, count, rule)) {
        ++count;
    }
    return count;
}

uint32_t ipAddressToHostOrder(const IPAddress& ip) {
    return
        (static_cast<uint32_t>(ip[0]) << 24) |
        (static_cast<uint32_t>(ip[1]) << 16) |
        (static_cast<uint32_t>(ip[2]) << 8) |
        static_cast<uint32_t>(ip[3]);
}

void rememberResolvedIp(const String& domain, uint32_t ipHostOrder) {
    uint32_t nowSeconds = millis() / 1000;

    portENTER_CRITICAL(&gDnsIpPolicyMux);
    pruneEntriesLocked(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount, nowSeconds);
    addOrRefreshEntryLocked(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount, domain, ipHostOrder, nowSeconds);
    portEXIT_CRITICAL(&gDnsIpPolicyMux);
}

void resolveAndRememberDomain(const String& domain) {
    if (domain.isEmpty()) return;

    IPAddress resolved;
    if (WiFi.hostByName(domain.c_str(), resolved) == 1) {
        rememberResolvedIp(domain, ipAddressToHostOrder(resolved));
    }
}

bool containsIpLocked(const CachedIpEntry* entries, size_t count, uint32_t ipHostOrder) {
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].ipHostOrder == ipHostOrder) return true;
    }
    return false;
}

}  // namespace

extern "C" void dnsIpPolicyService(const char* allowlistRules, const char* blocklistRules, int rulesEnabled, int upstreamReady) {
    uint32_t nowSeconds = millis() / 1000;
    bool blocklistHasRules = hasRules(blocklistRules);
    bool allowlistHasRules = hasRules(allowlistRules);

    portENTER_CRITICAL(&gDnsIpPolicyMux);
    gPolicyEnabled = rulesEnabled ? 1 : 0;
    gStrictAllow = (rulesEnabled && allowlistHasRules) ? 1 : 0;

    bool shouldReset = !rulesEnabled || !snapshotMatches(blocklistRules, gBlocklistSnapshot);
    bool shouldPrimeBlocklist = false;

    if (shouldReset) {
        clearEntries(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount);
        copySnapshot(gBlocklistSnapshot, sizeof(gBlocklistSnapshot), blocklistRules);
        gLastBlockResolveAtSeconds = 0;
        gNextBlockResolveRuleIndex = 0;
    } else {
        pruneEntriesLocked(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount, nowSeconds);
        shouldPrimeBlocklist = blocklistHasRules && gBlockedIpCount == 0;
    }
    portEXIT_CRITICAL(&gDnsIpPolicyMux);

    if (!rulesEnabled || !upstreamReady) return;

    if (shouldReset || shouldPrimeBlocklist) {
        gNextBlockResolveRuleIndex = 0;
    }

    if (blocklistHasRules && (shouldPrimeBlocklist || nowSeconds - gLastBlockResolveAtSeconds >= kRuleResolveIntervalSeconds)) {
        size_t ruleCount = countRules(blocklistRules);
        if (ruleCount > 0) {
            String rule;
            if (!getRuleByIndex(blocklistRules, gNextBlockResolveRuleIndex, rule)) {
                gNextBlockResolveRuleIndex = 0;
                getRuleByIndex(blocklistRules, gNextBlockResolveRuleIndex, rule);
            }
            resolveAndRememberDomain(rule);
            gLastBlockResolveAtSeconds = nowSeconds;
            gNextBlockResolveRuleIndex = (gNextBlockResolveRuleIndex + 1) % ruleCount;
        }
    }
}

extern "C" void dnsIpBlockerRememberDomain(const char* domain, int upstreamReady) {
    if (!upstreamReady) return;
    String normalized = normalizeDomain(domain);
    if (normalized.isEmpty()) return;
    resolveAndRememberDomain(normalized);
}

extern "C" void dnsIpBlockerClear(void) {
    portENTER_CRITICAL(&gDnsIpPolicyMux);
    clearEntries(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount);
    portEXIT_CRITICAL(&gDnsIpPolicyMux);
}

extern "C" void dnsIpPolicyGetStats(uint32_t* allowCount, uint32_t* blockCount, int* strictAllow, int* enabled) {
    portENTER_CRITICAL(&gDnsIpPolicyMux);
    if (allowCount != nullptr) *allowCount = 0;
    if (blockCount != nullptr) *blockCount = static_cast<uint32_t>(gBlockedIpCount);
    if (strictAllow != nullptr) *strictAllow = gStrictAllow;
    if (enabled != nullptr) *enabled = gPolicyEnabled;
    portEXIT_CRITICAL(&gDnsIpPolicyMux);
}

extern "C" int dnsHookIp4CanForward(uint32_t destAddrHostOrder) {
    uint32_t nowSeconds = millis() / 1000;

    portENTER_CRITICAL(&gDnsIpPolicyMux);
    if (!gPolicyEnabled) {
        portEXIT_CRITICAL(&gDnsIpPolicyMux);
        return -1;
    }

    pruneEntriesLocked(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount, nowSeconds);

    if (containsIpLocked(gBlockedIps, gBlockedIpCount, destAddrHostOrder)) {
        portEXIT_CRITICAL(&gDnsIpPolicyMux);
        return 0;
    }

    portEXIT_CRITICAL(&gDnsIpPolicyMux);
    return -1;
}
