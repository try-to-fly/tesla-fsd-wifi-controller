#include "dns_whitelist.h"

#include <cctype>
#include <cstring>

namespace {

constexpr uint16_t kDnsHeaderSize = 12;
constexpr uint16_t kDnsTypeA = 1;
constexpr uint16_t kDnsTypeAAAA = 28;
constexpr uint16_t kDnsClassIN = 1;
constexpr uint32_t kDnsTtlSeconds = 60;
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

bool DNSWhitelistServer::isAllowedDomain(const DNSFilterConfig& cfg, const String& domain) const {
    if (!cfg.enabled) return true;
    if (domain.isEmpty()) return false;

    const char* cursor = cfg.allowlist;
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

String DNSWhitelistServer::formatQueryType(uint16_t qType) const {
    switch (qType) {
        case kDnsTypeA:    return "A";
        case kDnsTypeAAAA: return "AAAA";
        default:           return String("TYPE") + qType;
    }
}

void DNSWhitelistServer::logBlockedRequest(const String& domain, uint16_t qType) {
    DNSBlockedRequestLogEntry entry = {};
    String normalizedDomain = normalizeDomain(domain);
    String typeText = formatQueryType(qType);
    String clientIP = udp_.remoteIP().toString();

    normalizedDomain.toCharArray(entry.domain, sizeof(entry.domain));
    clientIP.toCharArray(entry.clientIP, sizeof(entry.clientIP));
    typeText.toCharArray(entry.qType, sizeof(entry.qType));
    entry.blockedAtUptimeSeconds = millis() / 1000;

    portENTER_CRITICAL(&gDnsBlockedLogMux);
    blockedRequests_[blockedRequestHead_] = entry;
    blockedRequestHead_ = (blockedRequestHead_ + 1) % kDnsBlockedLogCapacity;
    if (blockedRequestCount_ < kDnsBlockedLogCapacity) {
        ++blockedRequestCount_;
    }
    ++totalBlockedCount_;
    portEXIT_CRITICAL(&gDnsBlockedLogMux);
}

size_t DNSWhitelistServer::copyBlockedRequests(DNSBlockedRequestLogEntry* dest, size_t maxEntries, uint32_t& totalBlockedCount) {
    totalBlockedCount = 0;
    if (dest == nullptr || maxEntries == 0) return 0;

    portENTER_CRITICAL(&gDnsBlockedLogMux);
    totalBlockedCount = totalBlockedCount_;
    size_t count = blockedRequestCount_ < maxEntries ? blockedRequestCount_ : maxEntries;

    for (size_t i = 0; i < count; ++i) {
        size_t index = (blockedRequestHead_ + kDnsBlockedLogCapacity - 1 - i) % kDnsBlockedLogCapacity;
        dest[i] = blockedRequests_[index];
    }
    portEXIT_CRITICAL(&gDnsBlockedLogMux);

    return count;
}

void DNSWhitelistServer::clearBlockedRequests() {
    portENTER_CRITICAL(&gDnsBlockedLogMux);
    for (size_t i = 0; i < kDnsBlockedLogCapacity; ++i) {
        blockedRequests_[i] = DNSBlockedRequestLogEntry{};
    }
    blockedRequestCount_ = 0;
    blockedRequestHead_ = 0;
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
        logBlockedRequest(domain, qType);
        sendErrorResponse(query, questionEnd, requestFlags, 5);
        return;
    }

    if (!upstreamReady) {
        sendErrorResponse(query, questionEnd, requestFlags, 2);
        return;
    }

    if (qType == kDnsTypeAAAA) {
        sendNoAnswerResponse(query, questionEnd, requestFlags);
        return;
    }

    if (qType != kDnsTypeA) {
        sendErrorResponse(query, questionEnd, requestFlags, 4);
        return;
    }

    IPAddress resolved;
    if (WiFi.hostByName(domain.c_str(), resolved) == 1) {
        sendIPv4Answer(query, questionEnd, requestFlags, resolved);
    } else {
        sendErrorResponse(query, questionEnd, requestFlags, 3);
    }
}
