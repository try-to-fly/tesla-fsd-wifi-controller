#pragma once

#include <WiFi.h>
#include <WiFiUdp.h>

struct DNSFilterConfig {
    bool enabled = false;
    char allowlist[385] = {};
};

class DNSWhitelistServer {
  public:
    bool begin(uint16_t port = 53);
    void stop();
    void processNextRequest(const DNSFilterConfig& cfg, bool upstreamReady);

  private:
    WiFiUDP udp_;
    bool started_ = false;

    String normalizeDomain(const String& domain) const;
    bool parseDomainName(const uint8_t* query, size_t length, size_t& offset, String& domain) const;
    bool isAllowedDomain(const DNSFilterConfig& cfg, const String& domain) const;
    void sendErrorResponse(const uint8_t* query, size_t questionEnd, uint16_t requestFlags, uint16_t rcode);
    void sendNoAnswerResponse(const uint8_t* query, size_t questionEnd, uint16_t requestFlags);
    void sendIPv4Answer(const uint8_t* query, size_t questionEnd, uint16_t requestFlags, const IPAddress& ip);
};
