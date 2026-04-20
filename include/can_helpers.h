#pragma once
#include "can_frame_types.h"

/*
 * Verified against upstream:
 * https://gitlab.com/Tesla-OPEN-CAN-MOD/tesla-open-can-mod
 */

inline uint8_t readMuxID(const CanFrame& frame) {
    return frame.data[0] & 0x07;
}

inline bool isFSDSelectedInUI(const CanFrame& frame) {
    return (frame.data[4] >> 6) & 0x01;
}

inline void setSpeedProfileV12V13(CanFrame& frame, int profile) {
    frame.data[6] &= ~0x06;
    frame.data[6] |= (profile << 1);
}

inline void setBit(CanFrame& frame, int bit, bool value) {
    int byteIndex = bit / 8;
    int bitIndex = bit % 8;
    uint8_t mask = static_cast<uint8_t>(1U << bitIndex);
    if (value) {
        frame.data[byteIndex] |= mask;
    } else {
        frame.data[byteIndex] &= static_cast<uint8_t>(~mask);
    }
}

inline uint32_t readUnsignedLittleEndian(const CanFrame& frame, uint8_t startBit, uint8_t bitLength) {
    uint32_t value = 0;
    for (uint8_t i = 0; i < bitLength; ++i) {
        uint8_t absoluteBit = static_cast<uint8_t>(startBit + i);
        uint8_t byteIndex = absoluteBit / 8U;
        uint8_t bitIndex = absoluteBit % 8U;
        if ((frame.data[byteIndex] >> bitIndex) & 0x01U) {
            value |= (1UL << i);
        }
    }
    return value;
}

inline bool decodeESPVehicleSpeedValid(const CanFrame& frame) {
    return readUnsignedLittleEndian(frame, 40, 1) == 1U;
}

inline uint16_t decodeESPVehicleSpeedCentiKph(const CanFrame& frame) {
    uint16_t raw = static_cast<uint16_t>(readUnsignedLittleEndian(frame, 42, 10));
    return static_cast<uint16_t>(raw * 50U);
}

inline uint16_t decodeDIVehicleSpeedCentiKph(const CanFrame& frame) {
    uint16_t raw = static_cast<uint16_t>(readUnsignedLittleEndian(frame, 12, 12));
    int32_t centiKph = static_cast<int32_t>(raw) * 8 - 4000;
    if (centiKph < 0) return 0;
    return static_cast<uint16_t>(centiKph);
}
