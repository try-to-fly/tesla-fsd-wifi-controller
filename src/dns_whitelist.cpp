#include "dns_whitelist.h"
#include "dns_ip_blocker.h"

#include <cctype>
#include <cstring>

namespace {

constexpr uint16_t kDnsHeaderSize = 12;
constexpr uint16_t kDnsTypeA = 1;
constexpr uint16_t kDnsTypeAAAA = 28;
constexpr uint16_t kDnsClassIN = 1;
constexpr uint32_t kDnsTtlSeconds = 60;
constexpr uint32_t kDnsForwardTimeoutMs = 1200;
portMUX_TYPE gDnsBlockedLogMux = portMUX_INITIALIZER_UNLOCKED;

uint16_t readU16(const uint8_t* ptr) {
    return static_cast<uint16_t>((ptr[0] << 8) | ptr[1]);
}

void writeU16(uint8_t* ptr, uint16_t value) {
    ptr[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
    ptr[1] = static_cast<uint8_t>(value & 0xFF);
}

void writeU32(uint8_t* ptr, uint32_t value) {
    ptr[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    ptr[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    ptr[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    ptr[3] = static_cast<uint8_t>(value & 0xFF);
}

bool domainMatchesRule(const String& domain, const String& rule) {
    if (rule.isEmpty()) return false;
    if (domain == rule) return true;
    if (domain.length() <= rule.length()) return false;

    size_t offset = domain.length() - rule.length();
    return domain.endsWith(rule) && domain[offset - 1] == '.';
}

}  // namespace

bool DNSWhitelistServer::begin(uint16_t port) {
    stop();
    started_ = udp_.begin(port) == 1;
    return started_;
}

void DNSWhitelistServer::stop() {
    udp_.stop();
    started_ = false;
}

String DNSWhitelistServer::normalizeDomain(const String& domain) const {
    String normalized = domain;
    normalized.trim();
    normalized.toLowerCase();
    while (normalized.endsWith(".")) {
        normalized.remove(normalized.length() - 1);
    }
    return normalized;
}

bool DNSWhitelistServer::parseDomainName(const uint8_t* query, size_t length, size_t& offset, String& domain) const {
    domain = "";
    bool first = true;

    while (offset < length) {
        uint8_t labelLength = query[offset++];
        if (labelLength == 0) return true;
        if ((labelLength & 0xC0) != 0 || offset + labelLength > length) return false;

        if (!first) domain += '.';
        for (uint8_t i = 0; i < labelLength; ++i) {
            domain += static_cast<char>(query[offset++]);
        }
        first = false;
    }

    return false;
}

bool DNSWhitelistServer::skipDomainName(const uint8_t* packet, size_t length, size_t& offset) const {
    while (offset < length) {
        uint8_t labelLength = packet[offset++];
        if (labelLength == 0) return true;
        if ((labelLength & 0xC0) == 0xC0) {
            return offset < length;
        }
        if ((labelLength & 0xC0) != 0 || offset + labelLength > length) return false;
        offset += labelLength;
    }
    return false;
}

bool DNSWhitelistServer::hasDomainRules(const char* rules) const {
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

bool DNSWhitelistServer::listContainsDomain(const char* rules, const String& domain) const {
    if (rules == nullptr || domain.isEmpty()) return false;

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

        String rule(start, cursor - start);
        rule = normalizeDomain(rule);
        if (domainMatchesRule(domain, rule)) return true;
    }

    return false;
}

bool DNSWhitelistServer::isAllowedDomain(const DNSFilterConfig& cfg, const String& domain) const {
    if (!cfg.enabled) return true;
    if (domain.isEmpty()) return false;

    if (listContainsDomain(cfg.blocklist, domain)) return false;
    if (!hasDomainRules(cfg.allowlist)) return true;

    return listContainsDomain(cfg.allowlist, domain);
}

void DNSWhitelistServer::logBlockedRequest(const String& domain) {
    String normalizedDomain = normalizeDomain(domain);
    if (normalizedDomain.isEmpty()) return;

    uint32_t blockedAtUptimeSeconds = millis() / 1000;

    portENTER_CRITICAL(&gDnsBlockedLogMux);
    bool found = false;
    for (size_t i = 0; i < blockedDomainCount_; ++i) {
        if (normalizedDomain.equals(blockedDomains_[i].domain)) {
            ++blockedDomains_[i].count;
            blockedDomains_[i].lastBlockedAtUptimeSeconds = blockedAtUptimeSeconds;
            found = true;
            break;
        }
    }

    if (!found && blockedDomainCount_ < kDnsBlockedDomainCapacity) {
        DNSBlockedDomainStatEntry entry = {};
        normalizedDomain.toCharArray(entry.domain, sizeof(entry.domain));
        entry.count = 1;
        entry.lastBlockedAtUptimeSeconds = blockedAtUptimeSeconds;
        blockedDomains_[blockedDomainCount_++] = entry;
    }
    ++totalBlockedCount_;
    portEXIT_CRITICAL(&gDnsBlockedLogMux);
}

bool DNSWhitelistServer::forwardUpstreamQuery(const uint8_t* query, size_t queryLength, uint8_t* response, size_t responseCapacity, size_t& responseLength) {
    responseLength = 0;
    if (query == nullptr || response == nullptr || queryLength == 0 || responseCapacity == 0) return false;

    IPAddress dnsServerIP = WiFi.dnsIP(0);
    if (dnsServerIP == IPAddress(0, 0, 0, 0)) return false;

    upstreamUdp_.stop();
    if (upstreamUdp_.begin(0) != 1) return false;

    bool success = false;
    do {
        if (!upstreamUdp_.beginPacket(dnsServerIP, 53)) break;
        if (upstreamUdp_.write(query, queryLength) != queryLength) break;
        if (!upstreamUdp_.endPacket()) break;

        uint32_t start = millis();
        while (millis() - start < kDnsForwardTimeoutMs) {
            int packetSize = upstreamUdp_.parsePacket();
            if (packetSize <= 0) {
                delay(5);
                continue;
            }
            if (packetSize > static_cast<int>(responseCapacity)) break;
            int read = upstreamUdp_.read(response, packetSize);
            if (read > 0) {
                responseLength = static_cast<size_t>(read);
                success = true;
            }
            break;
        }
    } while (false);

    upstreamUdp_.stop();
    return success;
}

void DNSWhitelistServer::cacheAllowedAddressesFromResponse(const String& domain, const uint8_t* response, size_t responseLength) {
    if (response == nullptr || responseLength < kDnsHeaderSize) return;

    uint16_t qdCount = readU16(response + 4);
    uint16_t anCount = readU16(response + 6);
    size_t offset = kDnsHeaderSize;

    for (uint16_t i = 0; i < qdCount; ++i) {
        if (!skipDomainName(response, responseLength, offset) || offset + 4 > responseLength) return;
        offset += 4;
    }

    for (uint16_t i = 0; i < anCount; ++i) {
        if (!skipDomainName(response, responseLength, offset) || offset + 10 > responseLength) return;

        uint16_t rrType = readU16(response + offset);
        uint16_t rrClass = readU16(response + offset + 2);
        uint16_t rdLength = readU16(response + offset + 8);
        offset += 10;

        if (offset + rdLength > responseLength) return;

        if (rrType == kDnsTypeA && rrClass == kDnsClassIN && rdLength == 4) {
            (void)domain;
        }
        offset += rdLength;
    }
}

size_t DNSWhitelistServer::copyBlockedDomains(DNSBlockedDomainStatEntry* dest, size_t maxEntries, uint32_t& totalBlockedCount) {
    totalBlockedCount = 0;
    if (dest == nullptr || maxEntries == 0) return 0;

    portENTER_CRITICAL(&gDnsBlockedLogMux);
    totalBlockedCount = totalBlockedCount_;
    size_t count = blockedDomainCount_ < maxEntries ? blockedDomainCount_ : maxEntries;

    for (size_t i = 0; i < count; ++i) {
        dest[i] = blockedDomains_[i];
    }
    portEXIT_CRITICAL(&gDnsBlockedLogMux);

    for (size_t i = 0; i < count; ++i) {
        size_t bestIndex = i;
        for (size_t j = i + 1; j < count; ++j) {
            if (dest[j].count < dest[bestIndex].count ||
                (dest[j].count == dest[bestIndex].count && strcmp(dest[j].domain, dest[bestIndex].domain) < 0)) {
                bestIndex = j;
            }
        }
        if (bestIndex != i) {
            DNSBlockedDomainStatEntry tmp = dest[i];
            dest[i] = dest[bestIndex];
            dest[bestIndex] = tmp;
        }
    }

    return count;
}

void DNSWhitelistServer::clearBlockedRequests() {
    portENTER_CRITICAL(&gDnsBlockedLogMux);
    for (size_t i = 0; i < kDnsBlockedDomainCapacity; ++i) {
        blockedDomains_[i] = DNSBlockedDomainStatEntry{};
    }
    blockedDomainCount_ = 0;
    totalBlockedCount_ = 0;
    portEXIT_CRITICAL(&gDnsBlockedLogMux);
}

void DNSWhitelistServer::sendErrorResponse(const uint8_t* query, size_t questionEnd, uint16_t requestFlags, uint16_t rcode) {
    if (!started_) return;

    uint8_t response[512] = {};
    if (questionEnd > sizeof(response)) return;

    memcpy(response, query, questionEnd);
    uint16_t responseFlags = static_cast<uint16_t>(0x8000 | 0x0080 | (requestFlags & 0x0100) | (rcode & 0x000F));
    writeU16(response + 2, responseFlags);
    writeU16(response + 4, questionEnd > kDnsHeaderSize ? 1 : 0);
    writeU16(response + 6, 0);
    writeU16(response + 8, 0);
    writeU16(response + 10, 0);

    udp_.beginPacket(udp_.remoteIP(), udp_.remotePort());
    udp_.write(response, questionEnd);
    udp_.endPacket();
}

void DNSWhitelistServer::sendNoAnswerResponse(const uint8_t* query, size_t questionEnd, uint16_t requestFlags) {
    sendErrorResponse(query, questionEnd, requestFlags, 0);
}

void DNSWhitelistServer::sendBlockedResponse(const uint8_t* query, size_t questionEnd, uint16_t requestFlags, uint16_t qType) {
    if (qType == kDnsTypeA) {
        sendIPv4Answer(query, questionEnd, requestFlags, IPAddress(0, 0, 0, 0));
        return;
    }

    // Return a successful empty answer instead of REFUSED to reduce client fallback
    // to alternate DNS paths when a domain is intentionally blocked.
    sendNoAnswerResponse(query, questionEnd, requestFlags);
}

void DNSWhitelistServer::sendIPv4Answer(const uint8_t* query, size_t questionEnd, uint16_t requestFlags, const IPAddress& ip) {
    if (!started_) return;

    uint8_t response[512] = {};
    const size_t answerSize = 16;
    const size_t responseSize = questionEnd + answerSize;
    if (responseSize > sizeof(response)) return;

    memcpy(response, query, questionEnd);
    uint16_t responseFlags = static_cast<uint16_t>(0x8000 | 0x0080 | (requestFlags & 0x0100));
    writeU16(response + 2, responseFlags);
    writeU16(response + 4, 1);
    writeU16(response + 6, 1);
    writeU16(response + 8, 0);
    writeU16(response + 10, 0);

    size_t offset = questionEnd;
    response[offset++] = 0xC0;
    response[offset++] = 0x0C;
    writeU16(response + offset, kDnsTypeA);
    offset += 2;
    writeU16(response + offset, kDnsClassIN);
    offset += 2;
    writeU32(response + offset, kDnsTtlSeconds);
    offset += 4;
    writeU16(response + offset, 4);
    offset += 2;
    response[offset++] = ip[0];
    response[offset++] = ip[1];
    response[offset++] = ip[2];
    response[offset++] = ip[3];

    udp_.beginPacket(udp_.remoteIP(), udp_.remotePort());
    udp_.write(response, responseSize);
    udp_.endPacket();
}

void DNSWhitelistServer::processNextRequest(const DNSFilterConfig& cfg, bool upstreamReady) {
    if (!started_) return;

    int packetSize = udp_.parsePacket();
    if (packetSize <= 0 || packetSize > 512) return;

    uint8_t query[512] = {};
    int read = udp_.read(query, packetSize);
    if (read < static_cast<int>(kDnsHeaderSize)) return;

    uint16_t requestFlags = readU16(query + 2);
    uint16_t qdCount = readU16(query + 4);
    uint16_t qr = requestFlags & 0x8000;
    uint16_t opcode = (requestFlags >> 11) & 0x0F;

    if (qr != 0 || opcode != 0 || qdCount != 1) {
        sendErrorResponse(query, kDnsHeaderSize, requestFlags, 4);
        return;
    }

    size_t offset = kDnsHeaderSize;
    String domain;
    if (!parseDomainName(query, read, offset, domain) || offset + 4 > static_cast<size_t>(read)) {
        sendErrorResponse(query, kDnsHeaderSize, requestFlags, 1);
        return;
    }

    const size_t questionEnd = offset + 4;
    domain = normalizeDomain(domain);

    uint16_t qType = readU16(query + offset);
    uint16_t qClass = readU16(query + offset + 2);

    if (qClass != kDnsClassIN) {
        sendErrorResponse(query, questionEnd, requestFlags, 5);
        return;
    }

    if (!isAllowedDomain(cfg, domain)) {
        logBlockedRequest(domain);
        dnsIpBlockerRememberDomain(domain.c_str(), upstreamReady ? 1 : 0);
        sendBlockedResponse(query, questionEnd, requestFlags, qType);
        return;
    }

    if (!upstreamReady) {
        sendErrorResponse(query, questionEnd, requestFlags, 2);
        return;
    }

    uint8_t upstreamResponse[512] = {};
    size_t upstreamResponseLength = 0;
    if (forwardUpstreamQuery(query, read, upstreamResponse, sizeof(upstreamResponse), upstreamResponseLength)) {
        cacheAllowedAddressesFromResponse(domain, upstreamResponse, upstreamResponseLength);
        udp_.beginPacket(udp_.remoteIP(), udp_.remotePort());
        udp_.write(upstreamResponse, upstreamResponseLength);
        udp_.endPacket();
        return;
    }

    if (qType != kDnsTypeA) {
        sendNoAnswerResponse(query, questionEnd, requestFlags);
        return;
    }

    IPAddress resolved;
    if (WiFi.hostByName(domain.c_str(), resolved) == 1) {
        sendIPv4Answer(query, questionEnd, requestFlags, resolved);
    } else {
        sendErrorResponse(query, questionEnd, requestFlags, 3);
    }
}
