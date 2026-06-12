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
    uint16_t targetKph = 0;
    uint8_t offsetKph = 0;
    uint8_t offsetRaw = 0;
};

static constexpr uint8_t HW3_AUTO_OFFSET_FALLBACK_KPH = 10;
static constexpr uint8_t HW3_HIGH_SPEED_OFFSET_PERCENT = 10;

inline uint16_t decodeFiveStepSpeedLimitKph(uint8_t raw) {
    if (raw == 0 || raw >= 31) return 0;
    return static_cast<uint16_t>(raw) * 5U;
}

inline uint16_t decodeMapSpeedLimitKph(uint8_t raw) {
    if (raw == 0) return 0;
    return static_cast<uint16_t>(raw) * 5U;
}

inline uint16_t computePercentOffsetKph(uint16_t limitKph, uint8_t percent) {
    return static_cast<uint16_t>((static_cast<uint32_t>(limitKph) * percent + 50U) / 100U);
}

inline uint16_t computeSmartTargetSpeedKph(uint16_t limitKph) {
    if (limitKph == 0) return 0;
    if (limitKph < 30) return 30;
    if (limitKph == 30) return 45;
    if (limitKph == 40) return 55;
    if (limitKph == 50) return 65;
    if (limitKph == 60) return 72;
    return static_cast<uint16_t>(
        limitKph + computePercentOffsetKph(limitKph, HW3_HIGH_SPEED_OFFSET_PERCENT)
    );
}

// 1021/MUX2 的注入字段在实车上按百分比 raw 解释：raw = offset% * 5。
// 低速策略直接从“目标车速 - 识别限速”换算 raw，可保留 0.2% 精度，避免 40->55 这类目标被整数百分比粗化。
inline int encodeOffsetFieldFromTargetKph(uint16_t limitKph, uint16_t targetKph) {
    if (limitKph == 0 || targetKph <= limitKph) return 0;
    uint32_t offsetKph = static_cast<uint32_t>(targetKph - limitKph);
    uint32_t raw = (offsetKph * 500U + limitKph / 2U) / limitKph;
    return static_cast<int>(std::min<uint32_t>(raw, 255U));
}

inline int encodeOffsetFieldFromPercent(uint16_t offsetPercent) {
    return static_cast<int>(std::min<uint32_t>(static_cast<uint32_t>(offsetPercent) * 5U, 255U));
}

inline SmartSpeedDecision computeSmartSpeedDecision(uint16_t limitKph) {
    SmartSpeedDecision decision;
    if (limitKph == 0) return decision;

    decision.targetKph = computeSmartTargetSpeedKph(limitKph);
    uint16_t offset = decision.targetKph > limitKph
        ? static_cast<uint16_t>(decision.targetKph - limitKph)
        : 0;
    decision.offsetKph = static_cast<uint8_t>(std::min<uint16_t>(offset, 255U));
    decision.offsetRaw = static_cast<uint8_t>(
        limitKph > 60
            ? encodeOffsetFieldFromPercent(HW3_HIGH_SPEED_OFFSET_PERCENT)
            : encodeOffsetFieldFromTargetKph(limitKph, decision.targetKph)
    );
    return decision;
}

inline uint8_t computeSmartOffsetKph(uint16_t limitKph) {
    return computeSmartSpeedDecision(limitKph).offsetKph;
}
