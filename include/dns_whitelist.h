#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <WiFi.h>
#include <WiFiUdp.h>

struct DNSFilterConfig {
    bool enabled = false;
    char allowlist[385] = {};
};

static constexpr size_t kDnsBlockedLogCapacity = 20;

struct DNSBlockedRequestLogEntry {
    char domain[128] = {};
    char clientIP[16] = {};
    char qType[12] = {};
    uint32_t blockedAtUptimeSeconds = 0;
};

class DNSWhitelistServer {
  public:
    bool begin(uint16_t port = 53);
    void stop();
    void processNextRequest(const DNSFilterConfig& cfg, bool upstreamReady);
    size_t copyBlockedRequests(DNSBlockedRequestLogEntry* dest, size_t maxEntries, uint32_t& totalBlockedCount);
    void clearBlockedRequests();

  private:
    WiFiUDP udp_;
    bool started_ = false;
    DNSBlockedRequestLogEntry blockedRequests_[kDnsBlockedLogCapacity];
    size_t blockedRequestCount_ = 0;
    size_t blockedRequestHead_ = 0;
    uint32_t totalBlockedCount_ = 0;

    String normalizeDomain(const String& domain) const;
    bool parseDomainName(const uint8_t* query, size_t length, size_t& offset, String& domain) const;
    bool isAllowedDomain(const DNSFilterConfig& cfg, const String& domain) const;
    String formatQueryType(uint16_t qType) const;
    void logBlockedRequest(const String& domain, uint16_t qType);
    void sendErrorResponse(const uint8_t* query, size_t questionEnd, uint16_t requestFlags, uint16_t rcode);
    void sendNoAnswerResponse(const uint8_t* query, size_t questionEnd, uint16_t requestFlags);
    void sendIPv4Answer(const uint8_t* query, size_t questionEnd, uint16_t requestFlags, const IPAddress& ip);
};
