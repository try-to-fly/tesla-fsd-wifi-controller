#pragma once
#include <cstdint>
#include <algorithm>
#if defined(ARDUINO)
#include <Arduino.h>
#else
uint32_t millis();
#endif
#include "can_frame_types.h"
#include "drivers/can_driver.h"
#include "can_helpers.h"
#include "debug_log.h"
#include "speed_limit_policy.h"

// ── Runtime-configurable state (shared with web server) ──
struct FSDConfig {
    volatile bool     fsdEnable           = true;
    volatile uint8_t  hwMode              = 1;       // 0=LEGACY, 1=HW3, 2=HW4  (默认 HW3)
    volatile uint8_t  speedProfile        = 1;       // 0-4
    volatile bool     profileModeAuto     = true;    // true=auto from stalk, false=manual
    volatile bool     isaChimeSuppress    = false;
    volatile bool     emergencyDetection  = true;
    volatile bool     chinaMode          = true;   // CN firmware: bypass isFSDSelectedInUI check  (默认开启)
    volatile uint16_t detectedSpeedLimitKph = 0;    // resolved limit from vision/fused/map
    volatile uint8_t  detectedSpeedSource   = 0;    // SpeedLimitSource
    volatile uint8_t  appliedSpeedOffsetKph = 0;    // actual injected offset in km/h
    volatile uint16_t vehicleSpeedCentiKph  = 0;    // current vehicle speed in 0.01 km/h
    volatile uint8_t  vehicleSpeedSource    = 0;    // VehicleSpeedSource
    volatile uint32_t lastVehicleSpeedMillis = 0;

    // Stats
    volatile uint32_t rxCount       = 0;
    volatile uint32_t modifiedCount = 0;
    volatile uint32_t errorCount    = 0;
    volatile uint32_t lastRxMillis  = 0;
    volatile uint32_t lastModifiedMillis = 0;
    volatile bool     canOK         = false;
    volatile bool     fsdTriggered  = false;
    volatile uint32_t uptimeStart   = 0;
};

static FSDConfig cfg;

enum class VehicleSpeedSource : uint8_t {
    None = 0,
    ESP  = 1,
    DI   = 2
};

// ── Filter IDs per HW mode ──
static constexpr uint32_t LEGACY_IDS[] = {69, 1006};
static constexpr uint32_t HW3_IDS[]    = {760, 921, 1016, 1021};
static constexpr uint32_t HW4_IDS[]    = {921, 1016, 1021};
static constexpr uint32_t ESP_VEHICLE_SPEED_ID = 341;
static constexpr uint32_t DI_VEHICLE_SPEED_ID  = 599;
static constexpr uint32_t VEHICLE_SPEED_STALE_MS = 2000;

static uint8_t  hw3RawUserOffsetKph    = 0;
static uint16_t hw3VisionSpeedLimitKph = 0;
static uint16_t hw3FusedSpeedLimitKph  = 0;
static uint16_t hw3MapSpeedLimitKph    = 0;
static uint16_t espVehicleSpeedCentiKph = 0;
static uint32_t espVehicleSpeedMillis = 0;
static uint16_t diVehicleSpeedCentiKph = 0;
static uint32_t diVehicleSpeedMillis = 0;
static constexpr uint32_t HW3_DEBUG_LOG_PERIOD_MS = 1000;
static uint16_t hw3LastLoggedLimitKph = UINT16_MAX;
static uint8_t hw3LastLoggedLimitSource = UINT8_MAX;
static uint32_t hw3LastLimitDebugMillis = 0;
static uint8_t hw3LastLoggedMux0RawOffsetKph = UINT8_MAX;
static bool hw3LastLoggedMux0Triggered = false;
static uint32_t hw3LastMux0DebugMillis = 0;
static uint16_t hw3LastLoggedMux2LimitKph = UINT16_MAX;
static uint8_t hw3LastLoggedMux2Source = UINT8_MAX;
static uint8_t hw3LastLoggedMux2RawOffsetKph = UINT8_MAX;
static uint8_t hw3LastLoggedMux2AppliedOffsetKph = UINT8_MAX;
static uint32_t hw3LastMux2DebugMillis = 0;

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
        case 1: return 4;
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

inline uint8_t decodeRawUserOffsetKph(const CanFrame& frame) {
    int raw = static_cast<int>((frame.data[3] >> 1) & 0x3F) - 30;
    return static_cast<uint8_t>(std::max(raw, 0));
}

inline void clearVehicleSpeedTelemetry() {
    cfg.vehicleSpeedCentiKph = 0;
    cfg.vehicleSpeedSource = static_cast<uint8_t>(VehicleSpeedSource::None);
    cfg.lastVehicleSpeedMillis = 0;
}

inline void publishVehicleSpeedTelemetry(
    uint16_t centiKph,
    VehicleSpeedSource source,
    uint32_t capturedAtMillis
) {
    cfg.vehicleSpeedCentiKph = centiKph;
    cfg.vehicleSpeedSource = static_cast<uint8_t>(source);
    cfg.lastVehicleSpeedMillis = capturedAtMillis;
}

inline void refreshVehicleSpeedTelemetry(uint32_t now) {
    bool espFresh = espVehicleSpeedMillis != 0 && (now - espVehicleSpeedMillis) <= VEHICLE_SPEED_STALE_MS;
    bool diFresh = diVehicleSpeedMillis != 0 && (now - diVehicleSpeedMillis) <= VEHICLE_SPEED_STALE_MS;

    if (espFresh) {
        publishVehicleSpeedTelemetry(espVehicleSpeedCentiKph, VehicleSpeedSource::ESP, espVehicleSpeedMillis);
        return;
    }
    if (diFresh) {
        publishVehicleSpeedTelemetry(diVehicleSpeedCentiKph, VehicleSpeedSource::DI, diVehicleSpeedMillis);
        return;
    }

    clearVehicleSpeedTelemetry();
}

inline void recordESPVehicleSpeed(const CanFrame& frame) {
    uint32_t now = millis();
    if (!decodeESPVehicleSpeedValid(frame)) {
        espVehicleSpeedCentiKph = 0;
        espVehicleSpeedMillis = 0;
        refreshVehicleSpeedTelemetry(now);
        return;
    }

    espVehicleSpeedCentiKph = decodeESPVehicleSpeedCentiKph(frame);
    espVehicleSpeedMillis = now;
    refreshVehicleSpeedTelemetry(now);
}

inline void recordDIVehicleSpeed(const CanFrame& frame) {
    uint32_t now = millis();
    diVehicleSpeedCentiKph = decodeDIVehicleSpeedCentiKph(frame);
    diVehicleSpeedMillis = now;
    refreshVehicleSpeedTelemetry(now);
}

inline void updateVehicleSpeedTelemetry(const CanFrame& frame) {
    if (frame.id == ESP_VEHICLE_SPEED_ID) {
        recordESPVehicleSpeed(frame);
    } else if (frame.id == DI_VEHICLE_SPEED_ID) {
        recordDIVehicleSpeed(frame);
    }
}

inline void clearHW3SpeedLimitTelemetry() {
    cfg.detectedSpeedLimitKph = 0;
    cfg.detectedSpeedSource = static_cast<uint8_t>(SpeedLimitSource::None);
    cfg.appliedSpeedOffsetKph = 0;
}

inline void resetHW3DebugState() {
    hw3LastLoggedLimitKph = UINT16_MAX;
    hw3LastLoggedLimitSource = UINT8_MAX;
    hw3LastLimitDebugMillis = 0;
    hw3LastLoggedMux0RawOffsetKph = UINT8_MAX;
    hw3LastLoggedMux0Triggered = false;
    hw3LastMux0DebugMillis = 0;
    hw3LastLoggedMux2LimitKph = UINT16_MAX;
    hw3LastLoggedMux2Source = UINT8_MAX;
    hw3LastLoggedMux2RawOffsetKph = UINT8_MAX;
    hw3LastLoggedMux2AppliedOffsetKph = UINT8_MAX;
    hw3LastMux2DebugMillis = 0;
}

inline void resetHW3SpeedLimitState() {
    hw3RawUserOffsetKph = 0;
    hw3VisionSpeedLimitKph = 0;
    hw3FusedSpeedLimitKph = 0;
    hw3MapSpeedLimitKph = 0;
    clearHW3SpeedLimitTelemetry();
    resetHW3DebugState();
}

inline void resetVehicleSpeedState() {
    espVehicleSpeedCentiKph = 0;
    espVehicleSpeedMillis = 0;
    diVehicleSpeedCentiKph = 0;
    diVehicleSpeedMillis = 0;
    clearVehicleSpeedTelemetry();
}

inline void updateHW3DetectedSpeedLimit() {
    if (hw3FusedSpeedLimitKph > 0) {
        cfg.detectedSpeedLimitKph = hw3FusedSpeedLimitKph;
        cfg.detectedSpeedSource = static_cast<uint8_t>(SpeedLimitSource::Fused);
    } else if (hw3VisionSpeedLimitKph > 0) {
        cfg.detectedSpeedLimitKph = hw3VisionSpeedLimitKph;
        cfg.detectedSpeedSource = static_cast<uint8_t>(SpeedLimitSource::Vision);
    } else if (hw3MapSpeedLimitKph > 0) {
        cfg.detectedSpeedLimitKph = hw3MapSpeedLimitKph;
        cfg.detectedSpeedSource = static_cast<uint8_t>(SpeedLimitSource::Map);
    } else {
        cfg.detectedSpeedLimitKph = 0;
        cfg.detectedSpeedSource = static_cast<uint8_t>(SpeedLimitSource::None);
    }

    uint32_t now = millis();
    bool changed = cfg.detectedSpeedLimitKph != hw3LastLoggedLimitKph
        || cfg.detectedSpeedSource != hw3LastLoggedLimitSource;
    bool due = hw3LastLimitDebugMillis == 0 || (now - hw3LastLimitDebugMillis) >= HW3_DEBUG_LOG_PERIOD_MS;
    if (!changed && !due) return;

    hw3LastLoggedLimitKph = cfg.detectedSpeedLimitKph;
    hw3LastLoggedLimitSource = cfg.detectedSpeedSource;
    hw3LastLimitDebugMillis = now;
    debugLogHW3Limit(
        hw3FusedSpeedLimitKph,
        hw3VisionSpeedLimitKph,
        hw3MapSpeedLimitKph,
        cfg.detectedSpeedLimitKph,
        cfg.detectedSpeedSource
    );
}

inline void recordRxActivity() {
    cfg.rxCount++;
    cfg.lastRxMillis = millis();
}

inline void recordSendResult(bool sent) {
    if (sent) {
        cfg.modifiedCount++;
        cfg.lastModifiedMillis = millis();
    } else {
        cfg.errorCount++;
    }
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
            recordSendResult(driver.send(frame));
        }
        if (index == 1) {
            setBit(frame, 19, false);
            recordSendResult(driver.send(frame));
        }
    }
}

// ── Handler: HW3 ──
static void handleHW3(CanFrame& frame, CanDriver& driver) {
    if (frame.id == 760) {
        hw3MapSpeedLimitKph = decodeMapSpeedLimitKph(frame.data[6] & 0x1F);
        updateHW3DetectedSpeedLimit();
        return;
    }
    if (frame.id == 921) {
        hw3FusedSpeedLimitKph = decodeFiveStepSpeedLimitKph(frame.data[1] & 0x1F);
        hw3VisionSpeedLimitKph = decodeFiveStepSpeedLimitKph(frame.data[2] & 0x1F);
        updateHW3DetectedSpeedLimit();
        return;
    }
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
        if (index == 0) {
            cfg.fsdTriggered = cfg.chinaMode || isFSDSelectedInUI(frame);
            hw3RawUserOffsetKph = decodeRawUserOffsetKph(frame);
            if (!cfg.fsdTriggered || !cfg.fsdEnable) cfg.appliedSpeedOffsetKph = 0;
            uint32_t now = millis();
            bool mux0Changed = hw3RawUserOffsetKph != hw3LastLoggedMux0RawOffsetKph
                || cfg.fsdTriggered != hw3LastLoggedMux0Triggered;
            bool mux0Due = hw3LastMux0DebugMillis == 0 || (now - hw3LastMux0DebugMillis) >= HW3_DEBUG_LOG_PERIOD_MS;
            if (mux0Changed || mux0Due) {
                hw3LastLoggedMux0RawOffsetKph = hw3RawUserOffsetKph;
                hw3LastLoggedMux0Triggered = cfg.fsdTriggered;
                hw3LastMux0DebugMillis = now;
                debugLogHW3Mux0(
                    cfg.fsdTriggered,
                    cfg.fsdEnable,
                    hw3RawUserOffsetKph,
                    frame.data[3],
                    frame.data[4]
                );
            }
        }
        if (index == 0 && cfg.fsdTriggered && cfg.fsdEnable) {
            setBit(frame, 46, true);
            setSpeedProfileV12V13(frame, cfg.speedProfile);
            recordSendResult(driver.send(frame));
        }
        if (index == 1) {
            setBit(frame, 19, false);
            recordSendResult(driver.send(frame));
        }
        if (index == 2 && cfg.fsdTriggered && cfg.fsdEnable) {
            uint8_t originalOffsetKph = static_cast<uint8_t>(std::min<uint16_t>(hw3RawUserOffsetKph, 30));
            uint8_t appliedOffsetKph = cfg.detectedSpeedLimitKph > 0
                ? computeSmartOffsetKph(cfg.detectedSpeedLimitKph)
                : std::min<uint8_t>(originalOffsetKph, HW3_AUTO_OFFSET_FALLBACK_KPH);
            int speedOffset = encodeOffsetFieldFromKph(appliedOffsetKph);
            cfg.appliedSpeedOffsetKph = appliedOffsetKph;
            frame.data[0] &= ~(0b11000000);
            frame.data[1] &= ~(0b00111111);
            frame.data[0] |= (speedOffset & 0x03) << 6;
            frame.data[1] |= (speedOffset >> 2);
            bool sent = driver.send(frame);
            recordSendResult(sent);
            uint32_t now = millis();
            bool mux2Changed = cfg.detectedSpeedLimitKph != hw3LastLoggedMux2LimitKph
                || cfg.detectedSpeedSource != hw3LastLoggedMux2Source
                || hw3RawUserOffsetKph != hw3LastLoggedMux2RawOffsetKph
                || appliedOffsetKph != hw3LastLoggedMux2AppliedOffsetKph;
            bool mux2Due = hw3LastMux2DebugMillis == 0 || (now - hw3LastMux2DebugMillis) >= HW3_DEBUG_LOG_PERIOD_MS;
            if (mux2Changed || mux2Due || !sent) {
                hw3LastLoggedMux2LimitKph = cfg.detectedSpeedLimitKph;
                hw3LastLoggedMux2Source = cfg.detectedSpeedSource;
                hw3LastLoggedMux2RawOffsetKph = hw3RawUserOffsetKph;
                hw3LastLoggedMux2AppliedOffsetKph = appliedOffsetKph;
                hw3LastMux2DebugMillis = now;
                debugLogHW3Mux2(
                    cfg.detectedSpeedLimitKph,
                    cfg.detectedSpeedSource,
                    hw3RawUserOffsetKph,
                    appliedOffsetKph,
                    speedOffset,
                    frame.data[0],
                    frame.data[1],
                    sent
                );
            }
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
        recordSendResult(driver.send(frame));
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
            recordSendResult(driver.send(frame));
        }
        if (index == 1) {
            setBit(frame, 19, false);
            setBit(frame, 47, true);
            recordSendResult(driver.send(frame));
        }
        if (index == 2) {
            frame.data[7] &= ~(0x07 << 4);
            frame.data[7] |= (cfg.speedProfile & 0x07) << 4;
            recordSendResult(driver.send(frame));
        }
    }
}

// ── Unified dispatch ──
static void handleMessage(CanFrame& frame, CanDriver& driver) {
    recordRxActivity();
    updateVehicleSpeedTelemetry(frame);
    if (!isFilteredId(frame.id)) return;
    switch (cfg.hwMode) {
        case 0: handleLegacy(frame, driver); break;
        case 1: handleHW3(frame, driver); break;
        default: handleHW4(frame, driver); break;
    }
}
