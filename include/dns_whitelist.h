#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <WiFi.h>
#include <WiFiUdp.h>

struct DNSFilterConfig {
    bool enabled = false;
    char allowlist[1024] = {};
    char blocklist[1024] = {};
};

static constexpr size_t kDnsBlockedDomainCapacity = 20;

struct DNSBlockedDomainStatEntry {
    char domain[128] = {};
    uint32_t count = 0;
    uint32_t lastBlockedAtUptimeSeconds = 0;
};

class DNSWhitelistServer {
  public:
    bool begin(uint16_t port = 53);
    void stop();
    void processNextRequest(const DNSFilterConfig& cfg, bool upstreamReady);
    size_t copyBlockedDomains(DNSBlockedDomainStatEntry* dest, size_t maxEntries, uint32_t& totalBlockedCount);
    void clearBlockedRequests();

  private:
    WiFiUDP udp_;
    WiFiUDP upstreamUdp_;
    bool started_ = false;
    DNSBlockedDomainStatEntry blockedDomains_[kDnsBlockedDomainCapacity];
    size_t blockedDomainCount_ = 0;
    uint32_t totalBlockedCount_ = 0;

    String normalizeDomain(const String& domain) const;
    bool parseDomainName(const uint8_t* query, size_t length, size_t& offset, String& domain) const;
    bool skipDomainName(const uint8_t* packet, size_t length, size_t& offset) const;
    bool hasDomainRules(const char* rules) const;
    bool listContainsDomain(const char* rules, const String& domain) const;
    bool isAllowedDomain(const DNSFilterConfig& cfg, const String& domain) const;
    void logBlockedRequest(const String& domain);
    bool forwardUpstreamQuery(const uint8_t* query, size_t queryLength, uint8_t* response, size_t responseCapacity, size_t& responseLength);
    void cacheAllowedAddressesFromResponse(const String& domain, const uint8_t* response, size_t responseLength);
    void sendBlockedResponse(const uint8_t* query, size_t questionEnd, uint16_t requestFlags, uint16_t qType);
    void sendErrorResponse(const uint8_t* query, size_t questionEnd, uint16_t requestFlags, uint16_t rcode);
    void sendNoAnswerResponse(const uint8_t* query, size_t questionEnd, uint16_t requestFlags);
    void sendIPv4Answer(const uint8_t* query, size_t questionEnd, uint16_t requestFlags, const IPAddress& ip);
};
