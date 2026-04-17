#pragma once
#include <cstdint>
#include <algorithm>
#include "can_frame_types.h"
#include "drivers/can_driver.h"
#include "can_helpers.h"

// ── Runtime-configurable state (shared with web server) ──
struct FSDConfig {
    volatile bool     fsdEnable           = true;
    volatile uint8_t  hwMode              = 2;       // 0=LEGACY, 1=HW3, 2=HW4
    volatile uint8_t  speedProfile        = 1;       // 0-4
    volatile bool     profileModeAuto     = true;    // true=auto from stalk, false=manual
    volatile bool     speedOffsetEnable   = false;   // HW3 only, inject configured offset
    volatile uint8_t  speedOffsetPercent  = 0;       // 0-50 (% over limit)
    volatile bool     isaChimeSuppress    = false;
    volatile bool     emergencyDetection  = true;
    volatile bool     chinaMode          = false;  // CN firmware: bypass isFSDSelectedInUI check

    // Stats
    volatile uint32_t rxCount       = 0;
    volatile uint32_t modifiedCount = 0;
    volatile uint32_t errorCount    = 0;
    volatile bool     canOK         = false;
    volatile bool     fsdTriggered  = false;
    volatile uint32_t uptimeStart   = 0;
};

static FSDConfig cfg;

// ── Filter IDs per HW mode ──
static constexpr uint32_t LEGACY_IDS[] = {69, 1006};
static constexpr uint32_t HW3_IDS[]    = {1016, 1021};
static constexpr uint32_t HW4_IDS[]    = {921, 1016, 1021};

inline const uint32_t* getFilterIds() {
    switch (cfg.hwMode) {
        case 0: return LEGACY_IDS;
        case 1: return HW3_IDS;
        default: return HW4_IDS;
    }
}
inline uint8_t getFilterIdCount() {
    switch (cfg.hwMode) {
        case 0: return 2;
        case 1: return 2;
        default: return 3;
    }
}

inline bool isFilteredId(uint32_t id) {
    auto* ids = getFilterIds();
    auto  cnt = getFilterIdCount();
    for (uint8_t i = 0; i < cnt; i++) {
        if (ids[i] == id) return true;
    }
    return false;
}

// ── Handler: Legacy ──
static void handleLegacy(CanFrame& frame, CanDriver& driver) {
    if (frame.id == 69 && cfg.profileModeAuto) {
        uint8_t pos = frame.data[1] >> 5;
        if      (pos <= 1) cfg.speedProfile = 2;
        else if (pos == 2) cfg.speedProfile = 1;
        else               cfg.speedProfile = 0;
        return;
    }
    if (frame.id == 1006) {
        auto index = readMuxID(frame);
        if (index == 0) cfg.fsdTriggered = cfg.chinaMode || isFSDSelectedInUI(frame);
        if (index == 0 && cfg.fsdTriggered && cfg.fsdEnable) {
            setBit(frame, 46, true);
            setSpeedProfileV12V13(frame, cfg.speedProfile);
            if (driver.send(frame)) cfg.modifiedCount++;
            else cfg.errorCount++;
        }
        if (index == 1) {
            setBit(frame, 19, false);
            if (driver.send(frame)) cfg.modifiedCount++;
            else cfg.errorCount++;
        }
    }
}

// ── Handler: HW3 ──
static void handleHW3(CanFrame& frame, CanDriver& driver) {
    if (frame.id == 1016 && cfg.profileModeAuto) {
        uint8_t fd = (frame.data[5] & 0b11100000) >> 5;
        switch (fd) {
            case 1: cfg.speedProfile = 2; break;
            case 2: cfg.speedProfile = 1; break;
            case 3: cfg.speedProfile = 0; break;
        }
        return;
    }
    if (frame.id == 1021) {
        auto index = readMuxID(frame);
        if (index == 0) cfg.fsdTriggered = cfg.chinaMode || isFSDSelectedInUI(frame);
        if (index == 0 && cfg.fsdTriggered && cfg.fsdEnable) {
            setBit(frame, 46, true);
            setSpeedProfileV12V13(frame, cfg.speedProfile);
            if (driver.send(frame)) cfg.modifiedCount++;
            else cfg.errorCount++;
        }
        if (index == 1) {
            setBit(frame, 19, false);
            if (driver.send(frame)) cfg.modifiedCount++;
            else cfg.errorCount++;
        }
        if (index == 2 && cfg.fsdTriggered && cfg.fsdEnable) {
            int speedOffset = cfg.speedOffsetEnable
                ? static_cast<int>(cfg.speedOffsetPercent) * 4
                : std::max(std::min(((int)((frame.data[3] >> 1) & 0x3F) - 30) * 5, 100), 0);
            frame.data[0] &= ~(0b11000000);
            frame.data[1] &= ~(0b00111111);
            frame.data[0] |= (speedOffset & 0x03) << 6;
            frame.data[1] |= (speedOffset >> 2);
            if (driver.send(frame)) cfg.modifiedCount++;
            else cfg.errorCount++;
        }
    }
}

// ── Handler: HW4 ──
static void handleHW4(CanFrame& frame, CanDriver& driver) {
    if (cfg.isaChimeSuppress && frame.id == 921) {
        frame.data[1] |= 0x20;
        uint8_t sum = 0;
        for (int i = 0; i < 7; i++) sum += frame.data[i];
        sum += (921 & 0xFF) + (921 >> 8);
        frame.data[7] = sum & 0xFF;
        if (driver.send(frame)) cfg.modifiedCount++;
        else cfg.errorCount++;
        return;
    }
    if (frame.id == 1016 && cfg.profileModeAuto) {
        auto fd = (frame.data[5] & 0b11100000) >> 5;
        switch (fd) {
            case 1: cfg.speedProfile = 3; break;
            case 2: cfg.speedProfile = 2; break;
            case 3: cfg.speedProfile = 1; break;
            case 4: cfg.speedProfile = 0; break;
            case 5: cfg.speedProfile = 4; break;
        }
    }
    if (frame.id == 1021) {
        auto index = readMuxID(frame);
        if (index == 0) cfg.fsdTriggered = cfg.chinaMode || isFSDSelectedInUI(frame);
        if (index == 0 && cfg.fsdTriggered && cfg.fsdEnable) {
            setBit(frame, 46, true);
            setBit(frame, 60, true);
            if (cfg.emergencyDetection) setBit(frame, 59, true);
            if (driver.send(frame)) cfg.modifiedCount++;
            else cfg.errorCount++;
        }
        if (index == 1) {
            setBit(frame, 19, false);
            setBit(frame, 47, true);
            if (driver.send(frame)) cfg.modifiedCount++;
            else cfg.errorCount++;
        }
        if (index == 2) {
            frame.data[7] &= ~(0x07 << 4);
            frame.data[7] |= (cfg.speedProfile & 0x07) << 4;
            if (driver.send(frame)) cfg.modifiedCount++;
            else cfg.errorCount++;
        }
    }
}

// ── Unified dispatch ──
static void handleMessage(CanFrame& frame, CanDriver& driver) {
    cfg.rxCount++;
    if (!isFilteredId(frame.id)) return;
    switch (cfg.hwMode) {
        case 0: handleLegacy(frame, driver); break;
        case 1: handleHW3(frame, driver); break;
        default: handleHW4(frame, driver); break;
    }
}
