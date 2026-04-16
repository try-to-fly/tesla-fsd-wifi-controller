#include "dns_ip_blocker.h"

#include <Arduino.h>
#include <WiFi.h>
#include <cctype>
#include <cstring>

#include "freertos/FreeRTOS.h"

namespace {

constexpr size_t kBlockedIpCapacity = 48;
constexpr uint32_t kIpEntryTtlSeconds = 3600;
constexpr uint32_t kRuleResolveIntervalSeconds = 15;
constexpr uint32_t kFullRefreshIntervalSeconds = 300;

struct CachedIpEntry {
    uint32_t ipHostOrder = 0;
    char domain[128] = {};
    uint32_t lastSeenAtSeconds = 0;
};

portMUX_TYPE gDnsIpPolicyMux = portMUX_INITIALIZER_UNLOCKED;
CachedIpEntry gBlockedIps[kBlockedIpCapacity];
size_t gBlockedIpCount = 0;
char gBlocklistSnapshot[385] = {};
uint32_t gLastBlockResolveAtSeconds = 0;
uint32_t gLastFullRefreshAtSeconds = 0;
size_t gNextBlockResolveRuleIndex = 0;

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
    String current = rules == nullptr ? "" : String(rules);
    return current.equals(snapshot == nullptr ? "" : String(snapshot));
}

void clearEntries(CachedIpEntry* entries, size_t capacity, size_t& count) {
    for (size_t i = 0; i < capacity; ++i) {
        entries[i] = CachedIpEntry{};
    }
    count = 0;
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

void resolveAllRules(const char* rules) {
    size_t ruleCount = countRules(rules);
    for (size_t i = 0; i < ruleCount; ++i) {
        String rule;
        if (getRuleByIndex(rules, i, rule)) {
            resolveAndRememberDomain(rule);
        }
    }
}

void resetStateLocked(const char* blocklistRules, uint32_t nowSeconds) {
    clearEntries(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount);

    memset(gBlocklistSnapshot, 0, sizeof(gBlocklistSnapshot));
    if (blocklistRules != nullptr) {
        String(blocklistRules).toCharArray(gBlocklistSnapshot, sizeof(gBlocklistSnapshot));
    }

    gLastBlockResolveAtSeconds = 0;
    gLastFullRefreshAtSeconds = nowSeconds;
    gNextBlockResolveRuleIndex = 0;
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
    bool shouldPrimeBlocklist = false;
    bool didReset = false;
    (void)allowlistRules;

    portENTER_CRITICAL(&gDnsIpPolicyMux);
    bool shouldReset =
        !rulesEnabled ||
        !snapshotMatches(blocklistRules, gBlocklistSnapshot);

    if (shouldReset) {
        resetStateLocked(blocklistRules, nowSeconds);
        didReset = true;
    } else {
        pruneEntriesLocked(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount, nowSeconds);
        shouldPrimeBlocklist = blocklistHasRules && gBlockedIpCount == 0;
    }
    portEXIT_CRITICAL(&gDnsIpPolicyMux);

    if (!rulesEnabled || !upstreamReady) return;

    if (didReset || shouldPrimeBlocklist) {
        if (blocklistHasRules) {
            resolveAllRules(blocklistRules);
            gLastBlockResolveAtSeconds = nowSeconds;
        }
        if (didReset) {
            gNextBlockResolveRuleIndex = 0;
        }
        if (didReset || shouldPrimeBlocklist) {
            return;
        }
    }

    if (nowSeconds - gLastFullRefreshAtSeconds >= kFullRefreshIntervalSeconds) {
        portENTER_CRITICAL(&gDnsIpPolicyMux);
        clearEntries(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount);
        gLastFullRefreshAtSeconds = nowSeconds;
        gNextBlockResolveRuleIndex = 0;
        portEXIT_CRITICAL(&gDnsIpPolicyMux);
    }

    if (blocklistHasRules && nowSeconds - gLastBlockResolveAtSeconds >= kRuleResolveIntervalSeconds) {
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

extern "C" int dnsHookIp4CanForward(uint32_t destAddrHostOrder) {
    uint32_t nowSeconds = millis() / 1000;

    portENTER_CRITICAL(&gDnsIpPolicyMux);
    pruneEntriesLocked(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount, nowSeconds);

    if (containsIpLocked(gBlockedIps, gBlockedIpCount, destAddrHostOrder)) {
        portEXIT_CRITICAL(&gDnsIpPolicyMux);
        return 0;
    }

    portEXIT_CRITICAL(&gDnsIpPolicyMux);
    return -1;
}
