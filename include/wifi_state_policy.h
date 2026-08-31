#pragma once

#include <stdint.h>

enum class APStopAction : uint8_t {
    IgnoreExpectedStop = 0,
    RequestRecovery,
};

enum class UpstreamScanAction : uint8_t {
    Wait = 0,
    ConsumeResults,
    UseSavedFallback,
};

inline bool wifiDeadlineReached(uint32_t now, uint32_t deadline) {
    if (deadline == 0) return true;
    return static_cast<int32_t>(now - deadline) >= 0;
}

inline APStopAction decideAPStopAction(
    bool applyInProgress,
    uint32_t now,
    uint32_t settleUntilMillis
) {
    if (applyInProgress || !wifiDeadlineReached(now, settleUntilMillis)) {
        return APStopAction::IgnoreExpectedStop;
    }
    return APStopAction::RequestRecovery;
}

inline UpstreamScanAction decideUpstreamScanAction(
    bool completionEventReceived,
    bool completionSucceeded,
    uint32_t now,
    uint32_t scanStartedMillis,
    uint32_t timeoutMillis
) {
    if (completionEventReceived) {
        return completionSucceeded
            ? UpstreamScanAction::ConsumeResults
            : UpstreamScanAction::UseSavedFallback;
    }
    if (now - scanStartedMillis >= timeoutMillis) {
        return UpstreamScanAction::UseSavedFallback;
    }
    return UpstreamScanAction::Wait;
}

inline int chooseUpstreamNetworkIndex(int visibleSavedIndex, int fallbackIndex) {
    return visibleSavedIndex >= 0 ? visibleSavedIndex : fallbackIndex;
}
