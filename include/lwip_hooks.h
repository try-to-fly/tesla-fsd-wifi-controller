#pragma once

#include "dns_ip_blocker.h"

#define LWIP_HOOK_IP4_CANFORWARD(p, dest) dnsHookIp4CanForward(dest)
