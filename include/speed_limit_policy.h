#pragma once

#include <algorithm>
#include <cstdint>

enum class SpeedLimitSource : uint8_t {
    None   = 0,
    Vision = 1,
    Fused  = 2,
    Map    = 3
};

struct SmartSpeedDecision {
    uint8_t percentCap = 0;
    uint8_t hardCapKph = 0;
    uint8_t offsetKph = 0;
};

static constexpr uint8_t HW3_AUTO_OFFSET_FALLBACK_KPH = 10;

inline uint16_t decodeFiveStepSpeedLimitKph(uint8_t raw) {
    if (raw == 0 || raw >= 31) return 0;
    return static_cast<uint16_t>(raw) * 5U;
}

inline uint16_t decodeMapSpeedLimitKph(uint8_t raw) {
    if (raw == 0) return 0;
    return static_cast<uint16_t>(raw) * 5U;
}

inline uint8_t getSmartOffsetPercentCap(uint16_t limitKph) {
    if (limitKph <= 30) return 20;
    if (limitKph <= 40) return 20;
    if (limitKph <= 60) return 18;
    if (limitKph <= 80) return 15;
    if (limitKph < 100) return 12;
    return 10;
}

inline uint8_t getSmartOffsetHardCapKph(uint16_t limitKph) {
    if (limitKph <= 30) return 8;
    if (limitKph <= 40) return 10;
    if (limitKph <= 60) return 12;
    return 15;
}

inline SmartSpeedDecision computeSmartSpeedDecision(uint16_t limitKph) {
    SmartSpeedDecision decision;
    if (limitKph == 0) return decision;

    decision.percentCap = getSmartOffsetPercentCap(limitKph);
    decision.hardCapKph = getSmartOffsetHardCapKph(limitKph);

    uint16_t offset = static_cast<uint16_t>(
        (static_cast<uint32_t>(limitKph) * decision.percentCap + 50U) / 100U
    );
    if (offset == 0) offset = 1;
    decision.offsetKph = static_cast<uint8_t>(std::min<uint16_t>(offset, decision.hardCapKph));
    return decision;
}

inline uint8_t computeSmartOffsetKph(uint16_t limitKph) {
    return computeSmartSpeedDecision(limitKph).offsetKph;
}

// 1021/MUX2 的注入字段在实车上按百分比 raw 解释：raw = percent * 5。
// UI 展示的是 offsetKph（例如 30km/h -> +6），但 CAN 里要写 percentCap（例如 20% -> raw 100）。
inline int encodeOffsetFieldFromPercent(uint8_t offsetPercent) {
    return static_cast<int>(std::min<uint8_t>(offsetPercent, 20U)) * 5;
}
