#pragma once

#include <stdint.h>

#include "lwip/pbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

void dnsIpPolicyService(const char* allowlistRules, const char* blocklistRules, int rulesEnabled, int upstreamReady);
void dnsIpBlockerRememberDomain(const char* domain, int upstreamReady);
void dnsIpBlockerClear(void);
int dnsHookIp4CanForward(uint32_t destAddrHostOrder);

#ifdef __cplusplus
}
#endif
