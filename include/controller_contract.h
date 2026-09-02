#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace controller_contract {

enum class StatusAudience : uint8_t {
    Web,
    BLE,
};

enum class ConfigValidationError : uint8_t {
    None,
    InvalidHardwareMode,
    InvalidSpeedProfile,
    InvalidAPSSID,
    InvalidAPPassword,
    DNSAllowlistTooLong,
    DNSBlocklistTooLong,
};

struct ConfigValidationInput {
    bool hasHardwareMode = false;
    uint8_t hardwareMode = 0;
    bool hasSpeedProfile = false;
    uint8_t speedProfile = 0;
    bool touchesAP = false;
    size_t apSSIDLength = 0;
    size_t apPasswordLength = 0;
    bool hasDNSAllowlist = false;
    size_t dnsAllowlistLength = 0;
    size_t dnsAllowlistCapacity = 0;
    bool hasDNSBlocklist = false;
    size_t dnsBlocklistLength = 0;
    size_t dnsBlocklistCapacity = 0;
};

inline ConfigValidationError validateConfig(const ConfigValidationInput& input) {
    if (input.hasHardwareMode && input.hardwareMode > 2) {
        return ConfigValidationError::InvalidHardwareMode;
    }
    if (input.hasSpeedProfile && input.speedProfile > 4) {
        return ConfigValidationError::InvalidSpeedProfile;
    }
    if (input.touchesAP && (input.apSSIDLength == 0 || input.apSSIDLength > 32)) {
        return ConfigValidationError::InvalidAPSSID;
    }
    if (input.touchesAP && (input.apPasswordLength < 8 || input.apPasswordLength > 63)) {
        return ConfigValidationError::InvalidAPPassword;
    }
    if (input.hasDNSAllowlist && input.dnsAllowlistLength >= input.dnsAllowlistCapacity) {
        return ConfigValidationError::DNSAllowlistTooLong;
    }
    if (input.hasDNSBlocklist && input.dnsBlocklistLength >= input.dnsBlocklistCapacity) {
        return ConfigValidationError::DNSBlocklistTooLong;
    }
    return ConfigValidationError::None;
}

inline bool includesAPPassword(StatusAudience audience) {
    return audience == StatusAudience::Web;
}

inline bool isSupportedBLEOperation(const char* operation) {
    static const char* const operations[] = {
        "status.get",
        "config.set",
        "upstream.scan",
        "upstream.save",
        "upstream.delete",
        "dns.blocked.clear",
        "debug.read",
        "debug.clear",
    };
    if (operation == nullptr) return false;
    for (const char* supported : operations) {
        if (std::strcmp(operation, supported) == 0) return true;
    }
    return false;
}

}  // namespace controller_contract
