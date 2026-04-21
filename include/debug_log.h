#pragma once

#include <cstdint>

#if defined(ARDUINO)
void debugLogEvent(const char* tag, const char* message);
void debugLogHW3Limit(
    uint16_t fusedKph,
    uint16_t visionKph,
    uint16_t mapKph,
    uint16_t detectedKph,
    uint8_t detectedSource
);
void debugLogHW3Mux0(
    bool fsdTriggered,
    bool fsdEnable,
    uint8_t rawUserOffsetKph,
    uint8_t d3,
    uint8_t d4
);
void debugLogHW3Mux2(
    uint16_t detectedSpeedLimitKph,
    uint8_t detectedSource,
    uint8_t rawUserOffsetKph,
    uint8_t appliedOffsetKph,
    int speedOffset,
    uint8_t d0,
    uint8_t d1,
    bool sendOk
);
#else
inline void debugLogEvent(const char*, const char*) {}
inline void debugLogHW3Limit(uint16_t, uint16_t, uint16_t, uint16_t, uint8_t) {}
inline void debugLogHW3Mux0(bool, bool, uint8_t, uint8_t, uint8_t) {}
inline void debugLogHW3Mux2(uint16_t, uint8_t, uint8_t, uint8_t, int, uint8_t, uint8_t, bool) {}
#endif
