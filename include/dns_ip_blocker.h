#pragma once

#include <stdint.h>

#include "lwip/pbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

void dnsIpPolicyService(const char* allowlistRules, const char* blocklistRules, int rulesEnabled, int upstreamReady);
void dnsIpBlockerRememberAllowedIp(const char* domain, uint32_t ipHostOrder);
void dnsIpBlockerRememberBlockedIp(const char* domain, uint32_t ipHostOrder);
void dnsIpBlockerRememberDomain(const char* domain, int upstreamReady);
void dnsIpBlockerClear(void);
void dnsIpPolicyGetStats(uint32_t* allowCount, uint32_t* blockCount, int* strictAllow, int* enabled);
int dnsHookIp4CanForward(uint32_t destAddrHostOrder);

#ifdef __cplusplus
}
#endif
