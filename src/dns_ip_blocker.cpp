#include "dns_ip_blocker.h"

#include <Arduino.h>
#include <WiFi.h>
#include <cctype>
#include <cstring>

#include "freertos/FreeRTOS.h"

namespace {

constexpr size_t kAllowedIpCapacity = 64;
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
CachedIpEntry gAllowedIps[kAllowedIpCapacity];
CachedIpEntry gBlockedIps[kBlockedIpCapacity];
size_t gAllowedIpCount = 0;
size_t gBlockedIpCount = 0;
char gAllowlistSnapshot[385] = {};
char gBlocklistSnapshot[385] = {};
bool gStrictWhitelistActive = false;
uint32_t gLastAllowResolveAtSeconds = 0;
uint32_t gLastBlockResolveAtSeconds = 0;
uint32_t gLastFullRefreshAtSeconds = 0;
size_t gNextAllowResolveRuleIndex = 0;
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

void rememberResolvedIp(const String& domain, uint32_t ipHostOrder, bool allow) {
    uint32_t nowSeconds = millis() / 1000;

    portENTER_CRITICAL(&gDnsIpPolicyMux);
    pruneEntriesLocked(gAllowedIps, kAllowedIpCapacity, gAllowedIpCount, nowSeconds);
    pruneEntriesLocked(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount, nowSeconds);

    if (allow) {
        addOrRefreshEntryLocked(gAllowedIps, kAllowedIpCapacity, gAllowedIpCount, domain, ipHostOrder, nowSeconds);
    } else {
        addOrRefreshEntryLocked(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount, domain, ipHostOrder, nowSeconds);
    }
    portEXIT_CRITICAL(&gDnsIpPolicyMux);
}

void resolveAndRememberDomain(const String& domain, bool allow) {
    if (domain.isEmpty()) return;

    IPAddress resolved;
    if (WiFi.hostByName(domain.c_str(), resolved) == 1) {
        rememberResolvedIp(domain, ipAddressToHostOrder(resolved), allow);
    }
}

void resetStateLocked(const char* allowlistRules, const char* blocklistRules, bool strictWhitelistActive, uint32_t nowSeconds) {
    clearEntries(gAllowedIps, kAllowedIpCapacity, gAllowedIpCount);
    clearEntries(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount);

    memset(gAllowlistSnapshot, 0, sizeof(gAllowlistSnapshot));
    memset(gBlocklistSnapshot, 0, sizeof(gBlocklistSnapshot));
    if (allowlistRules != nullptr) {
        String(allowlistRules).toCharArray(gAllowlistSnapshot, sizeof(gAllowlistSnapshot));
    }
    if (blocklistRules != nullptr) {
        String(blocklistRules).toCharArray(gBlocklistSnapshot, sizeof(gBlocklistSnapshot));
    }

    gStrictWhitelistActive = strictWhitelistActive;
    gLastAllowResolveAtSeconds = 0;
    gLastBlockResolveAtSeconds = 0;
    gLastFullRefreshAtSeconds = nowSeconds;
    gNextAllowResolveRuleIndex = 0;
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
    bool allowlistHasRules = hasRules(allowlistRules);
    bool strictWhitelistActive = rulesEnabled && allowlistHasRules;

    portENTER_CRITICAL(&gDnsIpPolicyMux);
    bool shouldReset =
        !rulesEnabled ||
        !snapshotMatches(allowlistRules, gAllowlistSnapshot) ||
        !snapshotMatches(blocklistRules, gBlocklistSnapshot) ||
        gStrictWhitelistActive != strictWhitelistActive;

    if (shouldReset) {
        resetStateLocked(allowlistRules, blocklistRules, strictWhitelistActive, nowSeconds);
    } else {
        pruneEntriesLocked(gAllowedIps, kAllowedIpCapacity, gAllowedIpCount, nowSeconds);
        pruneEntriesLocked(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount, nowSeconds);
    }
    portEXIT_CRITICAL(&gDnsIpPolicyMux);

    if (!rulesEnabled || !upstreamReady) return;

    if (nowSeconds - gLastFullRefreshAtSeconds >= kFullRefreshIntervalSeconds) {
        portENTER_CRITICAL(&gDnsIpPolicyMux);
        clearEntries(gAllowedIps, kAllowedIpCapacity, gAllowedIpCount);
        clearEntries(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount);
        gLastFullRefreshAtSeconds = nowSeconds;
        gNextAllowResolveRuleIndex = 0;
        gNextBlockResolveRuleIndex = 0;
        portEXIT_CRITICAL(&gDnsIpPolicyMux);
    }

    if (hasRules(blocklistRules) && nowSeconds - gLastBlockResolveAtSeconds >= kRuleResolveIntervalSeconds) {
        size_t ruleCount = countRules(blocklistRules);
        if (ruleCount > 0) {
            String rule;
            if (!getRuleByIndex(blocklistRules, gNextBlockResolveRuleIndex, rule)) {
                gNextBlockResolveRuleIndex = 0;
                getRuleByIndex(blocklistRules, gNextBlockResolveRuleIndex, rule);
            }
            resolveAndRememberDomain(rule, false);
            gLastBlockResolveAtSeconds = nowSeconds;
            gNextBlockResolveRuleIndex = (gNextBlockResolveRuleIndex + 1) % ruleCount;
        }
    }

    if (allowlistHasRules && nowSeconds - gLastAllowResolveAtSeconds >= kRuleResolveIntervalSeconds) {
        size_t ruleCount = countRules(allowlistRules);
        if (ruleCount > 0) {
            String rule;
            if (!getRuleByIndex(allowlistRules, gNextAllowResolveRuleIndex, rule)) {
                gNextAllowResolveRuleIndex = 0;
                getRuleByIndex(allowlistRules, gNextAllowResolveRuleIndex, rule);
            }
            resolveAndRememberDomain(rule, true);
            gLastAllowResolveAtSeconds = nowSeconds;
            gNextAllowResolveRuleIndex = (gNextAllowResolveRuleIndex + 1) % ruleCount;
        }
    }
}

extern "C" void dnsIpBlockerRememberDomain(const char* domain, int upstreamReady) {
    if (!upstreamReady) return;
    String normalized = normalizeDomain(domain);
    if (normalized.isEmpty()) return;
    resolveAndRememberDomain(normalized, false);
}

extern "C" void dnsIpPolicyRememberAllowedIp(const char* domain, uint32_t ipHostOrder) {
    String normalized = normalizeDomain(domain);
    if (normalized.isEmpty() || ipHostOrder == 0) return;
    rememberResolvedIp(normalized, ipHostOrder, true);
}

extern "C" void dnsIpBlockerClear(void) {
    portENTER_CRITICAL(&gDnsIpPolicyMux);
    clearEntries(gAllowedIps, kAllowedIpCapacity, gAllowedIpCount);
    clearEntries(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount);
    portEXIT_CRITICAL(&gDnsIpPolicyMux);
}

extern "C" int dnsHookIp4CanForward(uint32_t destAddrHostOrder) {
    uint32_t nowSeconds = millis() / 1000;

    portENTER_CRITICAL(&gDnsIpPolicyMux);
    pruneEntriesLocked(gAllowedIps, kAllowedIpCapacity, gAllowedIpCount, nowSeconds);
    pruneEntriesLocked(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount, nowSeconds);

    if (containsIpLocked(gBlockedIps, gBlockedIpCount, destAddrHostOrder)) {
        portEXIT_CRITICAL(&gDnsIpPolicyMux);
        return 0;
    }

    if (gStrictWhitelistActive) {
        int result = containsIpLocked(gAllowedIps, gAllowedIpCount, destAddrHostOrder) ? 1 : 0;
        portEXIT_CRITICAL(&gDnsIpPolicyMux);
        return result;
    }

    portEXIT_CRITICAL(&gDnsIpPolicyMux);
    return -1;
}
