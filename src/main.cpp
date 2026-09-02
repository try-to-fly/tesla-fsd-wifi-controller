/*
 * Tesla Open CAN Mod — ESP32 Web Edition
 * Core 0: WiFi AP+STA + AsyncWebServer + OTA
 * Core 1: CAN bus read/modify/write (TWAI)
 *
 * GPLv3 — Based on tesla-open-can-mod
 */

#include <Arduino.h>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <Update.h>
#include <Preferences.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/semphr.h>
#include <mbedtls/sha256.h>

#include "lwip/lwip_napt.h"

#include "can_frame_types.h"
#include "ble_protocol.h"
#include "controller_contract.h"
#include "debug_log.h"
#include "dns_whitelist.h"
#include "dns_ip_blocker.h"
#include "drivers/twai_driver.h"
#include "handlers.h"
#include "web_ui.h"
#include "wifi_state_policy.h"

// ── WiFi AP config ──
static const char* DEFAULT_AP_SSID = "FSD-Controller";
static const char* DEFAULT_AP_PASS = "12345678";   // min 8 chars
static const IPAddress AP_IP(9, 9, 9, 9);
static const IPAddress AP_GATEWAY(9, 9, 9, 9);
static const IPAddress AP_SUBNET(255, 255, 255, 0);
static constexpr uint32_t AP_CONFIG_APPLY_DELAY_MS = 800;
static constexpr uint32_t AP_RESTART_MIN_INTERVAL_MS = 8000;
static constexpr uint32_t AP_HEALTH_CHECK_MS = 10000;
static constexpr uint32_t AP_EVENT_SETTLE_MS = 1500;
static constexpr uint32_t UPSTREAM_CONNECT_TIMEOUT_MS = 15000;
static constexpr uint32_t UPSTREAM_SCAN_TIMEOUT_MS = 15000;
static constexpr uint32_t UPSTREAM_RETRY_THROTTLED_MS = 60000;
static constexpr uint8_t UPSTREAM_BACKOFF_STEPS = 4;
static constexpr uint32_t UPSTREAM_BACKOFF_MS[UPSTREAM_BACKOFF_STEPS] = {3000, 5000, 15000, 60000};
static constexpr uint8_t MAX_UPSTREAM_NETWORKS = 10;
static constexpr uint8_t MAX_SCAN_RESULTS = 12;
static constexpr uint32_t THERMAL_SAMPLE_MS = 5000;
static constexpr uint32_t CAN_ACTIVITY_WINDOW_MS = 2000;
static constexpr float CHIP_TEMP_WARN_C = 65.0f;
static constexpr float CHIP_TEMP_THROTTLE_C = 75.0f;
static constexpr float CHIP_TEMP_PROTECT_C = 80.0f;
static constexpr float CHIP_TEMP_WARN_CLEAR_C = 62.0f;
static constexpr float CHIP_TEMP_THROTTLE_CLEAR_C = 72.0f;
static constexpr float CHIP_TEMP_PROTECT_CLEAR_C = 72.0f;
static constexpr float CHIP_TEMP_EMA_ALPHA = 0.25f;
static constexpr uint32_t DEBUG_HEARTBEAT_MS = 3000;
static constexpr size_t DEBUG_LOG_CAPACITY = 16 * 1024;
static constexpr uint32_t OTA_STATUS_STALE_MS = 8000;
static constexpr uint32_t OTA_RESTART_DELAY_MS = 1500;
static constexpr const char* BLE_SERVICE_UUID = "F5D00001-8B1A-4E7A-9C2D-7A0D5E1C0001";
static constexpr const char* BLE_COMMAND_UUID = "F5D00002-8B1A-4E7A-9C2D-7A0D5E1C0001";
static constexpr const char* BLE_RESPONSE_UUID = "F5D00003-8B1A-4E7A-9C2D-7A0D5E1C0001";
static constexpr const char* BLE_TELEMETRY_UUID = "F5D00004-8B1A-4E7A-9C2D-7A0D5E1C0001";
static constexpr uint32_t BLE_TELEMETRY_INTERVAL_MS = 1000;

// ── Globals ──
static TWAIDriver     canDriver;
static AsyncWebServer server(80);
static Preferences    prefs;
static DNSWhitelistServer dnsServer;
static volatile bool  otaPendingRestart = false;
static bool           natEnabled = false;
static volatile size_t debugLogCachedBytes = 0;
static char           debugLogBuf[DEBUG_LOG_CAPACITY];
static size_t         debugLogHead = 0;
static size_t         debugLogUsed = 0;
static BLEServer*     bleServer = nullptr;
static BLECharacteristic* bleResponseCharacteristic = nullptr;
static BLECharacteristic* bleTelemetryCharacteristic = nullptr;
static BLESecurity*   bleSecurity = nullptr;
static QueueHandle_t  bleCommandQueue = nullptr;
static SemaphoreHandle_t configMutex = nullptr;
static volatile bool  bleClientConnected = false;
static volatile bool  bleRequestBusy = false;
static volatile uint32_t bleConnectionGeneration = 0;
static bool           bleControlStarted = false;
static ble_protocol::Assembler bleRequestAssembler;

void updateBLEPasskey();

struct LocalAPConfig {
    char ssid[33] = "FSD-Controller";
    char pass[65] = "12345678";
    volatile bool applyRequested = false;
    uint32_t applyAtMillis = 0;
};

struct SavedUpstreamNetwork {
    char ssid[33] = {};
    char pass[65] = {};
};

struct UpstreamScanResult {
    char ssid[33]   = {};
    int32_t rssi    = -127;
    bool secure     = false;
    bool saved      = false;
};

struct UpstreamWiFiConfig {
    bool                 enabled             = false;
    SavedUpstreamNetwork networks[MAX_UPSTREAM_NETWORKS];
    uint8_t              networkCount        = 0;
    int8_t               activeIndex         = -1;
    uint8_t              nextTryIndex        = 0;
    volatile bool        applyRequested      = false;
    uint32_t             lastAttemptMillis   = 0;
};

struct ConfigPatch {
    bool hasFsdEnable = false;
    bool fsdEnable = false;
    bool hasHwMode = false;
    uint8_t hwMode = 0;
    bool hasSpeedProfile = false;
    uint8_t speedProfile = 0;
    bool hasProfileMode = false;
    bool profileModeAuto = false;
    bool hasIsaChime = false;
    bool isaChime = false;
    bool hasEmergencyDetection = false;
    bool emergencyDetection = false;
    bool hasChinaMode = false;
    bool chinaMode = false;
    bool hasAPSSID = false;
    String apSSID;
    bool hasAPPass = false;
    String apPass;
    bool hasUpstreamEnable = false;
    bool upstreamEnable = false;
    bool hasDNSWhitelistEnable = false;
    bool dnsWhitelistEnable = false;
    bool hasDNSAllowlist = false;
    String dnsAllowlist;
    bool hasDNSBlocklist = false;
    String dnsBlocklist;
};

struct BLECommand {
    uint16_t messageId = 0;
    uint32_t connectionGeneration = 0;
    std::string payload;
    std::string errorCode;
    std::string framingError;
};

enum class UpstreamConnectPhase : uint8_t {
    Idle = 0,
    Scanning,
    Connecting,
    Connected,
    Backoff
};

struct UpstreamRuntime {
    UpstreamConnectPhase phase = UpstreamConnectPhase::Idle;
    uint32_t lastAttemptMillis = 0;
    uint32_t connectStartedMillis = 0;
    uint8_t backoffIndex = 0;
    uint32_t retryCount = 0;
    int lastDisconnectReason = 0;
};

static UpstreamWiFiConfig wifiCfg;
static UpstreamRuntime upstreamRt;
static volatile bool upstreamScanInProgress = false;
static volatile bool upstreamAutomaticScanInProgress = false;
static volatile bool upstreamScanCompletionPending = false;
static volatile bool upstreamScanCompletionSucceeded = false;
static uint32_t upstreamScanStartedMillis = 0;
static volatile bool apStarted = false;
static volatile bool apRestartRequested = false;
static volatile bool apApplyInProgress = false;
static volatile uint32_t apEventSettleUntilMillis = 0;
static uint32_t lastApRestartMillis = 0;
static volatile uint32_t lastApHealthCheckMillis = 0;
static esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;
static LocalAPConfig apCfg;
static SemaphoreHandle_t debugLogMutex = nullptr;
static bool debugLogReady = false;

static DNSFilterConfig dnsCfg;
static const char* TAG = "FSD";

enum class OTAStatusPhase : uint8_t {
    Idle = 0,
    Uploading,
    Finishing,
    SuccessRebooting,
    FailedBegin,
    FailedWrite,
    FailedEnd,
    Aborted
};

struct OTAStatus {
    OTAStatusPhase phase = OTAStatusPhase::Idle;
    String filename;
    size_t bytesReceived = 0;
    size_t totalBytes = 0;
    uint8_t errorCode = 0;
    String errorMessage;
    String hintMessage;
    bool shouldReboot = false;
    uint32_t updatedAtMillis = 0;
    uint32_t startedAtMillis = 0;
};

static OTAStatus otaStatus;

enum class ThermalLevel : uint8_t {
    Normal = 0,
    Warning,
    Throttled,
    Protect
};

struct ThermalStatus {
    float currentC = NAN;
    float averageC = NAN;
    uint32_t lastSampleMillis = 0;
    ThermalLevel level = ThermalLevel::Normal;
};

static ThermalStatus thermalStatus;

#ifndef PIN_LED
#define PIN_LED 2   // ESP32 DevKit onboard LED
#endif

#ifndef WIFI_SCAN_RUNNING
#define WIFI_SCAN_RUNNING (-1)
#endif
#ifndef WIFI_SCAN_FAILED
#define WIFI_SCAN_FAILED (-2)
#endif

const char* getTwaiStateName(twai_state_t state) {
    switch (state) {
        case TWAI_STATE_STOPPED:    return "STOPPED";
        case TWAI_STATE_RUNNING:    return "RUNNING";
        case TWAI_STATE_BUS_OFF:    return "BUS_OFF";
        case TWAI_STATE_RECOVERING: return "RECOVERING";
        default:                    return "UNKNOWN";
    }
}

const char* getResetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "ext";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int-wdt";
        case ESP_RST_TASK_WDT:  return "task-wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

const char* getUpstreamPhaseName(UpstreamConnectPhase phase) {
    switch (phase) {
        case UpstreamConnectPhase::Scanning:   return "scanning";
        case UpstreamConnectPhase::Connecting: return "connecting";
        case UpstreamConnectPhase::Connected:  return "connected";
        case UpstreamConnectPhase::Backoff:    return "backoff";
        case UpstreamConnectPhase::Idle:
        default:
            return "idle";
    }
}

void copyStringToBuffer(char* dest, size_t size, const String& value) {
    memset(dest, 0, size);
    value.toCharArray(dest, size);
}

String jsonEscape(const String& value) {
    String escaped;
    escaped.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"':  escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': break;
            case '\t': escaped += "\\t"; break;
            default:
                escaped += (static_cast<unsigned char>(c) < 0x20) ? ' ' : c;
                break;
        }
    }
    return escaped;
}

const char* getOTAStatusPhaseName(OTAStatusPhase phase) {
    switch (phase) {
        case OTAStatusPhase::Idle:             return "idle";
        case OTAStatusPhase::Uploading:        return "uploading";
        case OTAStatusPhase::Finishing:        return "finishing";
        case OTAStatusPhase::SuccessRebooting: return "success-rebooting";
        case OTAStatusPhase::FailedBegin:      return "failed-begin";
        case OTAStatusPhase::FailedWrite:      return "failed-write";
        case OTAStatusPhase::FailedEnd:        return "failed-end";
        case OTAStatusPhase::Aborted:          return "aborted";
        default:                               return "idle";
    }
}

String normalizeBinFileName(const String& filename) {
    String normalized = filename;
    normalized.trim();
    normalized.toLowerCase();
    return normalized;
}

bool isWrongOTABinaryName(const String& filename) {
    String normalized = normalizeBinFileName(filename);
    return normalized.endsWith("full.bin")
        || normalized.endsWith("bootloader.bin")
        || normalized.endsWith("partitions.bin");
}

String buildOTAHintMessage(OTAStatusPhase phase, const String& filename, uint8_t errorCode, const String& errorMessage) {
    if (isWrongOTABinaryName(filename)) {
        return "WebUI OTA 只能上传应用固件 firmware.bin，full.bin / bootloader.bin / partitions.bin 需要走整片刷写。";
    }

    switch (phase) {
        case OTAStatusPhase::SuccessRebooting:
            return "固件写入完成，设备即将重启。等待 2-5 秒后重新连接页面。";
        case OTAStatusPhase::FailedBegin:
            if (errorCode == UPDATE_ERROR_SPACE || errorCode == UPDATE_ERROR_SIZE) {
                return "设备没有足够 OTA 空间，或上传的 bin 超过应用分区大小。请重新构建 firmware.bin 后再试。";
            }
            if (errorCode == UPDATE_ERROR_NO_PARTITION) {
                return "设备未找到可用 OTA 分区，请确认当前固件和分区表支持 OTA。";
            }
            return "设备拒绝开始写入固件。请确认上传的是当前版本构建出的 firmware.bin，并查看串口日志。";
        case OTAStatusPhase::FailedWrite:
            if (errorCode == UPDATE_ERROR_WRITE || errorCode == UPDATE_ERROR_ERASE || errorCode == UPDATE_ERROR_READ) {
                return "固件已经传到设备，但写入 Flash 失败。请检查供电、Flash 健康状况，并查看串口日志。";
            }
            if (errorCode == UPDATE_ERROR_STREAM) {
                return "上传流在设备侧被打断。通过设备热点 OTA 时更容易出现，建议靠近设备或改用局域网 IP。";
            }
            return "设备在写入固件时出错。请重试一次，并留意串口里的 Update 报错。";
        case OTAStatusPhase::FailedEnd:
            if (errorCode == UPDATE_ERROR_MAGIC_BYTE) {
                return "这个 bin 不是可启动的应用镜像。请确认选择的是 firmware.bin，而不是 full.bin 或其他分包。";
            }
            if (errorCode == UPDATE_ERROR_MD5 || errorCode == UPDATE_ERROR_ACTIVATE) {
                return "固件已传完，但校验或激活失败。建议重新导出固件并再次上传。";
            }
            return "固件上传完成，但收尾校验失败。请查看串口日志确认具体原因。";
        case OTAStatusPhase::Aborted:
            if (errorMessage.indexOf("重启") >= 0) {
                return "设备在上传过程中离线，可能已经重启。请重新连接页面并确认当前固件版本。";
            }
            return "上传链路在完成前中断。通过设备热点 OTA 时，如果热点切信道、信号抖动或浏览器切后台，都可能导致中断。";
        default:
            return "";
    }
}

String buildOTAErrorMessage(const String& prefix, uint8_t errorCode) {
    String message = prefix;
    if (errorCode == UPDATE_ERROR_OK) return message;
    message += ": ";
    message += Update.errorString();
    message += " (code ";
    message += String(static_cast<unsigned>(errorCode));
    message += ")";
    return message;
}

void setOTAStatus(
    OTAStatusPhase phase,
    const String& filename,
    size_t bytesReceived,
    size_t totalBytes,
    uint8_t errorCode,
    const String& errorMessage,
    bool shouldReboot
) {
    otaStatus.phase = phase;
    otaStatus.filename = filename;
    otaStatus.bytesReceived = bytesReceived;
    otaStatus.totalBytes = totalBytes;
    otaStatus.errorCode = errorCode;
    otaStatus.errorMessage = errorMessage;
    otaStatus.hintMessage = buildOTAHintMessage(phase, filename, errorCode, errorMessage);
    otaStatus.shouldReboot = shouldReboot;
    otaStatus.updatedAtMillis = millis();
    if (phase == OTAStatusPhase::Uploading && bytesReceived == 0) {
        otaStatus.startedAtMillis = otaStatus.updatedAtMillis;
    } else if (otaStatus.startedAtMillis == 0) {
        otaStatus.startedAtMillis = otaStatus.updatedAtMillis;
    }
}

void markOTAUploadAbortedIfStale() {
    if (otaStatus.phase != OTAStatusPhase::Uploading && otaStatus.phase != OTAStatusPhase::Finishing) return;

    uint32_t now = millis();
    if (otaStatus.updatedAtMillis == 0 || now - otaStatus.updatedAtMillis < OTA_STATUS_STALE_MS) return;

    if (Update.isRunning()) Update.abort();
    setOTAStatus(
        OTAStatusPhase::Aborted,
        otaStatus.filename,
        otaStatus.bytesReceived,
        otaStatus.totalBytes,
        UPDATE_ERROR_ABORT,
        "上传连接已中断，设备没有收到完整固件。",
        false
    );
}

String buildOTAStatusJson() {
    markOTAUploadAbortedIfStale();

    String filename = jsonEscape(otaStatus.filename);
    String errorMessage = jsonEscape(otaStatus.errorMessage);
    String hintMessage = jsonEscape(otaStatus.hintMessage);
    String json;

    json.reserve(320);
    json += "{";
    json += "\"phase\":\"";
    json += getOTAStatusPhaseName(otaStatus.phase);
    json += "\",\"filename\":\"";
    json += filename;
    json += "\",\"bytesReceived\":";
    json += String(static_cast<unsigned>(otaStatus.bytesReceived));
    json += ",\"totalBytes\":";
    json += otaStatus.totalBytes > 0 ? String(static_cast<unsigned>(otaStatus.totalBytes)) : "null";
    json += ",\"errorCode\":";
    json += String(static_cast<unsigned>(otaStatus.errorCode));
    json += ",\"errorMessage\":\"";
    json += errorMessage;
    json += "\",\"hint\":\"";
    json += hintMessage;
    json += "\",\"shouldReboot\":";
    json += (otaStatus.shouldReboot ? "true" : "false");
    json += ",\"updatedAtMs\":";
    json += String(static_cast<unsigned>(otaStatus.updatedAtMillis));
    json += ",\"startedAtMs\":";
    json += String(static_cast<unsigned>(otaStatus.startedAtMillis));
    json += "}";
    return json;
}

String buildOTAResponseJson(bool ok) {
    String statusJson = buildOTAStatusJson();
    if (statusJson.endsWith("}")) statusJson.remove(statusJson.length() - 1);

    String json;
    json.reserve(statusJson.length() + 24);
    json = statusJson;
    json += ",\"ok\":";
    json += (ok ? "true" : "false");
    json += "}";
    return json;
}

String formatHexByte(uint8_t value) {
    char buffer[3];
    snprintf(buffer, sizeof(buffer), "%02X", value);
    return String(buffer);
}

bool lockDebugLog() {
    return debugLogMutex != nullptr && xSemaphoreTake(debugLogMutex, pdMS_TO_TICKS(100)) == pdTRUE;
}

void unlockDebugLog() {
    if (debugLogMutex != nullptr) xSemaphoreGive(debugLogMutex);
}

void appendDebugLogBytesLocked(const char* data, size_t length) {
    if (data == nullptr || length == 0) return;
    if (length > DEBUG_LOG_CAPACITY) {
        data += length - DEBUG_LOG_CAPACITY;
        length = DEBUG_LOG_CAPACITY;
    }

    for (size_t i = 0; i < length; ++i) {
        debugLogBuf[debugLogHead] = data[i];
        debugLogHead = (debugLogHead + 1) % DEBUG_LOG_CAPACITY;
        if (debugLogUsed < DEBUG_LOG_CAPACITY) {
            ++debugLogUsed;
        }
    }

    debugLogCachedBytes = debugLogUsed;
}

void appendDebugLogRecord(const char* tag, const String& body) {
    if (!debugLogReady || !lockDebugLog()) return;

    String line;
    line.reserve(body.length() + 32);
    line += "[";
    line += String(static_cast<unsigned long>(millis()));
    line += "][";
    line += tag;
    line += "] ";
    line += body;
    line += "\n";

    appendDebugLogBytesLocked(line.c_str(), line.length());
    unlockDebugLog();
}

size_t getDebugLogSizeBytes() {
    return debugLogReady ? static_cast<size_t>(debugLogCachedBytes) : 0;
}

String readDebugLogText() {
    if (!debugLogReady || !lockDebugLog()) return "";

    String content;
    if (debugLogUsed == 0) {
        unlockDebugLog();
        return content;
    }

    content.reserve(debugLogUsed + 1);
    if (debugLogUsed == DEBUG_LOG_CAPACITY) {
        content += String(debugLogBuf + debugLogHead, DEBUG_LOG_CAPACITY - debugLogHead);
        if (debugLogHead > 0) content += String(debugLogBuf, debugLogHead);
    } else {
        content += String(debugLogBuf, debugLogUsed);
    }

    unlockDebugLog();
    return content;
}

void clearDebugLogStorage() {
    if (!debugLogReady || !lockDebugLog()) return;
    debugLogHead = 0;
    debugLogUsed = 0;
    debugLogCachedBytes = 0;
    unlockDebugLog();
}

void debugLogEvent(const char* tag, const char* message) {
    appendDebugLogRecord(tag, String(message));
}

void debugLogHW3Limit(
    uint16_t fusedKph,
    uint16_t visionKph,
    uint16_t mapKph,
    uint16_t detectedKph,
    uint8_t detectedSource
) {
    String body;
    body.reserve(96);
    body += "selected=";
    body += String(detectedKph);
    body += " src=";
    body += String(detectedSource);
    body += " fused=";
    body += String(fusedKph);
    body += " vision=";
    body += String(visionKph);
    body += " map=";
    body += String(mapKph);
    appendDebugLogRecord("HW3-LIMIT", body);
}

void debugLogHW3Mux0(
    bool fsdTriggered,
    bool fsdEnable,
    uint8_t rawUserOffsetKph,
    uint8_t d3,
    uint8_t d4
) {
    String body;
    body.reserve(96);
    body += "trig=";
    body += String(static_cast<int>(fsdTriggered));
    body += " fsdEn=";
    body += String(static_cast<int>(fsdEnable));
    body += " rawKph=";
    body += String(rawUserOffsetKph);
    body += " d3=0x";
    body += formatHexByte(d3);
    body += " d4=0x";
    body += formatHexByte(d4);
    appendDebugLogRecord("HW3-MUX0", body);
}

void debugLogHW3Mux2(
    uint16_t detectedSpeedLimitKph,
    uint8_t detectedSource,
    uint8_t rawUserOffsetKph,
    uint8_t appliedOffsetKph,
    int speedOffset,
    uint8_t d0,
    uint8_t d1,
    bool sendOk
) {
    String body;
    body.reserve(128);
    body += "limit=";
    body += String(detectedSpeedLimitKph);
    body += " src=";
    body += String(detectedSource);
    body += " rawKph=";
    body += String(rawUserOffsetKph);
    body += " appliedKph=";
    body += String(appliedOffsetKph);
    body += " encoded=";
    body += String(speedOffset);
    body += " d0=0x";
    body += formatHexByte(d0);
    body += " d1=0x";
    body += formatHexByte(d1);
    body += " send=";
    body += String(static_cast<int>(sendOk));
    appendDebugLogRecord("HW3-MUX2", body);
}

bool isReservedUpstreamSSID(const String& ssid) {
    return ssid.equals(apCfg.ssid);
}

void clearSavedUpstreamNetwork(SavedUpstreamNetwork& network) {
    network = SavedUpstreamNetwork{};
}

void clearSavedUpstreamNetworks() {
    for (uint8_t i = 0; i < MAX_UPSTREAM_NETWORKS; ++i) {
        clearSavedUpstreamNetwork(wifiCfg.networks[i]);
    }
    wifiCfg.networkCount = 0;
    wifiCfg.activeIndex = -1;
    wifiCfg.nextTryIndex = 0;
}

int findSavedUpstreamNetwork(const String& ssid) {
    for (uint8_t i = 0; i < wifiCfg.networkCount; ++i) {
        if (ssid.equals(wifiCfg.networks[i].ssid)) return i;
    }
    return -1;
}

bool addOrUpdateSavedUpstreamNetwork(const String& ssidInput, const String& passInput, bool overwritePass) {
    String ssid = ssidInput;
    ssid.trim();

    if (ssid.isEmpty() || ssid.length() > 32 || passInput.length() > 63 || isReservedUpstreamSSID(ssid)) {
        return false;
    }

    int index = findSavedUpstreamNetwork(ssid);
    bool isNew = index < 0;

    if (isNew) {
        if (wifiCfg.networkCount >= MAX_UPSTREAM_NETWORKS) return false;
        index = wifiCfg.networkCount++;
        clearSavedUpstreamNetwork(wifiCfg.networks[index]);
        copyStringToBuffer(wifiCfg.networks[index].ssid, sizeof(wifiCfg.networks[index].ssid), ssid);
    }

    if (isNew || overwritePass) {
        copyStringToBuffer(wifiCfg.networks[index].pass, sizeof(wifiCfg.networks[index].pass), passInput);
    }

    return true;
}

bool removeSavedUpstreamNetwork(const String& ssidInput) {
    String ssid = ssidInput;
    ssid.trim();

    int index = findSavedUpstreamNetwork(ssid);
    if (index < 0) return false;

    for (uint8_t i = index; i + 1 < wifiCfg.networkCount; ++i) {
        wifiCfg.networks[i] = wifiCfg.networks[i + 1];
    }

    if (wifiCfg.networkCount > 0) {
        --wifiCfg.networkCount;
        clearSavedUpstreamNetwork(wifiCfg.networks[wifiCfg.networkCount]);
    }

    if (wifiCfg.networkCount == 0) {
        wifiCfg.activeIndex = -1;
        wifiCfg.nextTryIndex = 0;
    } else {
        if (wifiCfg.activeIndex == index) {
            wifiCfg.activeIndex = -1;
        } else if (wifiCfg.activeIndex > index) {
            --wifiCfg.activeIndex;
        }
        if (wifiCfg.nextTryIndex >= wifiCfg.networkCount) {
            wifiCfg.nextTryIndex = 0;
        }
    }

    return true;
}

bool hasUpstreamCredentials() {
    return wifiCfg.networkCount > 0;
}

String getConnectedUpstreamSSID() {
    return WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String();
}

String getActiveUpstreamSSID() {
    if (WiFi.status() == WL_CONNECTED) return WiFi.SSID();
    if (wifiCfg.activeIndex >= 0 && wifiCfg.activeIndex < wifiCfg.networkCount) {
        return String(wifiCfg.networks[wifiCfg.activeIndex].ssid);
    }
    return String();
}

String buildSavedUpstreamNetworksJson() {
    String json = "[";
    json.reserve(wifiCfg.networkCount * 96 + 2);

    String connectedSSID = getConnectedUpstreamSSID();
    String activeSSID = getActiveUpstreamSSID();

    for (uint8_t i = 0; i < wifiCfg.networkCount; ++i) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"";
        json += jsonEscape(String(wifiCfg.networks[i].ssid));
        json += "\",\"hasPass\":";
        json += (wifiCfg.networks[i].pass[0] != '\0' ? "true" : "false");
        json += ",\"connected\":";
        json += (connectedSSID.equals(wifiCfg.networks[i].ssid) ? "true" : "false");
        json += ",\"active\":";
        json += (activeSSID.equals(wifiCfg.networks[i].ssid) ? "true" : "false");
        json += "}";
    }

    json += "]";
    return json;
}

String buildBlockedDnsRequestsJson(uint32_t& totalBlockedCount, size_t& recentBlockedCount) {
    DNSBlockedDomainStatEntry entries[kDnsBlockedDomainCapacity];
    totalBlockedCount = 0;
    recentBlockedCount = dnsServer.copyBlockedDomains(entries, kDnsBlockedDomainCapacity, totalBlockedCount);

    String json = "[";
    json.reserve(recentBlockedCount * 96 + 2);

    for (size_t i = 0; i < recentBlockedCount; ++i) {
        if (i > 0) json += ",";
        json += "{\"domain\":\"";
        json += jsonEscape(String(entries[i].domain));
        json += "\",\"count\":";
        json += String(entries[i].count);
        json += ",\"lastBlockedAt\":";
        json += String(entries[i].lastBlockedAtUptimeSeconds);
        json += "}";
    }

    json += "]";
    return json;
}

bool requestLocalAPRestart(const char* reason, bool force = false) {
    APStopAction action = decideAPStopAction(
        apApplyInProgress,
        millis(),
        apEventSettleUntilMillis
    );
    if (!force && action == APStopAction::IgnoreExpectedStop) {
        ESP_LOGI(TAG, "AP restart ignored during settle (%s)", reason ? reason : "unknown");
        return false;
    }
    apRestartRequested = true;
    ESP_LOGI(TAG, "AP restart requested (%s)", reason ? reason : "unknown");
    return true;
}

void logWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_AP_START:
            apStarted = true;
            apRestartRequested = false;
            ESP_LOGI(
                TAG,
                "wifi AP started ssid=%s ip=%s channel=%d",
                apCfg.ssid,
                WiFi.softAPIP().toString().c_str(),
                WiFi.channel()
            );
            break;
        case ARDUINO_EVENT_WIFI_AP_STOP:
            ESP_LOGW(TAG, "wifi AP stopped");
            if (requestLocalAPRestart("ap-stop")) {
                apStarted = false;
            }
            break;
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            ESP_LOGI(
                TAG,
                "wifi AP client event=%s clients=%u",
                WiFi.eventName(event),
                static_cast<unsigned>(WiFi.softAPgetStationNum())
            );
            break;
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            ESP_LOGI(
                TAG,
                "wifi STA connected ssid=%s channel=%d",
                WiFi.SSID().c_str(),
                WiFi.channel()
            );
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            ESP_LOGI(
                TAG,
                "wifi STA got ip=%s",
                IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str()
            );
            lastApHealthCheckMillis = millis();
            ESP_LOGI(TAG, "AP health check deferred after STA got IP");
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            upstreamRt.lastDisconnectReason = static_cast<int>(info.wifi_sta_disconnected.reason);
            ESP_LOGW(
                TAG,
                "wifi STA disconnected reason=%d",
                upstreamRt.lastDisconnectReason
            );
            break;
        case ARDUINO_EVENT_WIFI_SCAN_DONE:
            if (upstreamAutomaticScanInProgress) {
                upstreamScanCompletionSucceeded = info.wifi_scan_done.status == 0;
                upstreamScanCompletionPending = true;
                ESP_LOGI(
                    TAG,
                    "upstream scan event status=%lu results=%u",
                    static_cast<unsigned long>(info.wifi_scan_done.status),
                    static_cast<unsigned>(info.wifi_scan_done.number)
                );
            } else {
                ESP_LOGI(TAG, "wifi event=SCAN_DONE");
            }
            break;
        default:
            ESP_LOGI(TAG, "wifi event=%s", WiFi.eventName(event));
            break;
    }
}

void logRuntimeHeartbeat() {
    static uint32_t lastBeat = 0;
    uint32_t now = millis();
    if (lastBeat != 0 && now - lastBeat < DEBUG_HEARTBEAT_MS) return;
    lastBeat = now;

    twai_status_info_t twaiStatus = {};
    esp_err_t twaiErr = twai_get_status_info(&twaiStatus);
    wl_status_t staStatus = WiFi.status();
    String staSSID = getConnectedUpstreamSSID();
    String staIP = (staStatus == WL_CONNECTED) ? WiFi.localIP().toString() : String("-");

    if (twaiErr != ESP_OK) {
        ESP_LOGW(TAG, "twai status unavailable err=%s", esp_err_to_name(twaiErr));
        return;
    }

    ESP_LOGI(
        TAG,
        "beat rx=%lu mod=%lu err=%lu fsdTrig=%d fsdEn=%d hw=%u china=%d apClients=%u apIP=%s apRun=%d sta=%d staSSID=%s staIP=%s phase=%s retry=%lu disc=%d heap=%u minHeap=%u rst=%s twai=%s rxErr=%lu txErr=%lu busErr=%lu rxMiss=%lu txFail=%lu",
        static_cast<unsigned long>(cfg.rxCount),
        static_cast<unsigned long>(cfg.modifiedCount),
        static_cast<unsigned long>(cfg.errorCount),
        static_cast<int>(cfg.fsdTriggered),
        static_cast<int>(cfg.fsdEnable),
        static_cast<unsigned>(cfg.hwMode),
        static_cast<int>(cfg.chinaMode),
        static_cast<unsigned>(WiFi.softAPgetStationNum()),
        WiFi.softAPIP().toString().c_str(),
        apStarted ? 1 : 0,
        static_cast<int>(staStatus),
        staSSID.isEmpty() ? "-" : staSSID.c_str(),
        staIP.c_str(),
        getUpstreamPhaseName(upstreamRt.phase),
        static_cast<unsigned long>(upstreamRt.retryCount),
        upstreamRt.lastDisconnectReason,
        static_cast<unsigned>(ESP.getFreeHeap()),
        static_cast<unsigned>(ESP.getMinFreeHeap()),
        getResetReasonName(bootResetReason),
        getTwaiStateName(twaiStatus.state),
        static_cast<unsigned long>(twaiStatus.rx_error_counter),
        static_cast<unsigned long>(twaiStatus.tx_error_counter),
        static_cast<unsigned long>(twaiStatus.bus_error_count),
        static_cast<unsigned long>(twaiStatus.rx_missed_count),
        static_cast<unsigned long>(twaiStatus.tx_failed_count)
    );
}

int fillUniqueScanResults(int foundCount, UpstreamScanResult* results, size_t maxResults) {
    int uniqueCount = 0;
    if (foundCount <= 0 || results == nullptr || maxResults == 0) return 0;

    for (int i = 0; i < foundCount; ++i) {
        String ssid = WiFi.SSID(i);
        ssid.trim();

        if (ssid.isEmpty() || isReservedUpstreamSSID(ssid)) continue;

        int existing = -1;
        for (int j = 0; j < uniqueCount; ++j) {
            if (ssid.equals(results[j].ssid)) {
                existing = j;
                break;
            }
        }

        int32_t rssi = WiFi.RSSI(i);
        bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        bool saved = findSavedUpstreamNetwork(ssid) >= 0;

        if (existing >= 0) {
            if (rssi > results[existing].rssi) {
                copyStringToBuffer(results[existing].ssid, sizeof(results[existing].ssid), ssid);
                results[existing].rssi = rssi;
                results[existing].secure = secure;
                results[existing].saved = saved;
            }
            continue;
        }

        if (uniqueCount >= static_cast<int>(maxResults)) continue;

        copyStringToBuffer(results[uniqueCount].ssid, sizeof(results[uniqueCount].ssid), ssid);
        results[uniqueCount].rssi = rssi;
        results[uniqueCount].secure = secure;
        results[uniqueCount].saved = saved;
        ++uniqueCount;
    }

    for (int i = 0; i < uniqueCount - 1; ++i) {
        for (int j = i + 1; j < uniqueCount; ++j) {
            if (results[j].rssi > results[i].rssi) {
                UpstreamScanResult tmp = results[i];
                results[i] = results[j];
                results[j] = tmp;
            }
        }
    }

    return uniqueCount;
}

bool isUpstreamRadioBusy() {
    return upstreamRt.phase == UpstreamConnectPhase::Scanning
        || upstreamRt.phase == UpstreamConnectPhase::Connecting
        || upstreamScanInProgress;
}

int performUpstreamScan(UpstreamScanResult* results, size_t maxResults) {
    if (isUpstreamRadioBusy()) return -1;
    upstreamScanInProgress = true;

    WiFi.scanDelete();
    int foundCount = WiFi.scanNetworks(false, true);
    int uniqueCount = fillUniqueScanResults(foundCount, results, maxResults);

    WiFi.scanDelete();
    upstreamScanInProgress = false;
    return uniqueCount;
}

int pickSavedNetworkFromScan(const UpstreamScanResult* results, int scanCount) {
    if (results == nullptr || scanCount <= 0) return -1;
    for (int i = 0; i < scanCount; ++i) {
        int index = findSavedUpstreamNetwork(String(results[i].ssid));
        if (index >= 0) return index;
    }
    return -1;
}

int pickRoundRobinUpstreamNetworkIndex() {
    if (!hasUpstreamCredentials()) return -1;
    if (wifiCfg.nextTryIndex >= wifiCfg.networkCount) {
        wifiCfg.nextTryIndex = 0;
    }
    int selectedIndex = wifiCfg.nextTryIndex;
    wifiCfg.nextTryIndex = (wifiCfg.nextTryIndex + 1) % wifiCfg.networkCount;
    return selectedIndex;
}

uint8_t getDNSRuleCount(const char* rules) {
    uint8_t count = 0;
    if (rules == nullptr) return 0;

    const char* cursor = rules;

    while (*cursor) {
        while (*cursor && (isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) {
            ++cursor;
        }
        if (!*cursor) break;
        ++count;
        while (*cursor && !(isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) {
            ++cursor;
        }
    }

    return count;
}

const char* getThermalStatusText() {
    switch (thermalStatus.level) {
        case ThermalLevel::Warning:   return "温度偏高";
        case ThermalLevel::Throttled: return "高温降频";
        case ThermalLevel::Protect:   return "过热保护中";
        case ThermalLevel::Normal:
        default:
            return "正常";
    }
}

bool isThermalProtectionActive() {
    return thermalStatus.level == ThermalLevel::Protect;
}

bool isThermalThrottleActive() {
    return thermalStatus.level == ThermalLevel::Throttled || thermalStatus.level == ThermalLevel::Protect;
}

void updateThermalLevel() {
    if (!std::isfinite(thermalStatus.averageC)) {
        thermalStatus.level = ThermalLevel::Normal;
        return;
    }

    ThermalLevel previousLevel = thermalStatus.level;
    ThermalLevel nextLevel = previousLevel;
    float avgC = thermalStatus.averageC;

    switch (previousLevel) {
        case ThermalLevel::Protect:
            if (avgC <= CHIP_TEMP_PROTECT_CLEAR_C) {
                if (avgC >= CHIP_TEMP_THROTTLE_C) nextLevel = ThermalLevel::Throttled;
                else if (avgC >= CHIP_TEMP_WARN_C) nextLevel = ThermalLevel::Warning;
                else nextLevel = ThermalLevel::Normal;
            }
            break;
        case ThermalLevel::Throttled:
            if (avgC >= CHIP_TEMP_PROTECT_C) nextLevel = ThermalLevel::Protect;
            else if (avgC <= CHIP_TEMP_THROTTLE_CLEAR_C) {
                nextLevel = avgC >= CHIP_TEMP_WARN_C ? ThermalLevel::Warning : ThermalLevel::Normal;
            }
            break;
        case ThermalLevel::Warning:
            if (avgC >= CHIP_TEMP_PROTECT_C) nextLevel = ThermalLevel::Protect;
            else if (avgC >= CHIP_TEMP_THROTTLE_C) nextLevel = ThermalLevel::Throttled;
            else if (avgC <= CHIP_TEMP_WARN_CLEAR_C) nextLevel = ThermalLevel::Normal;
            break;
        case ThermalLevel::Normal:
        default:
            if (avgC >= CHIP_TEMP_PROTECT_C) nextLevel = ThermalLevel::Protect;
            else if (avgC >= CHIP_TEMP_THROTTLE_C) nextLevel = ThermalLevel::Throttled;
            else if (avgC >= CHIP_TEMP_WARN_C) nextLevel = ThermalLevel::Warning;
            break;
    }

    if (nextLevel != previousLevel) {
        thermalStatus.level = nextLevel;
        Serial.printf(
            "Thermal state changed: %s (current=%.1fC avg=%.1fC)\n",
            getThermalStatusText(),
            thermalStatus.currentC,
            thermalStatus.averageC
        );
    }
}

void serviceThermalStatus() {
    uint32_t now = millis();
    if (thermalStatus.lastSampleMillis != 0 && now - thermalStatus.lastSampleMillis < THERMAL_SAMPLE_MS) {
        return;
    }

    thermalStatus.lastSampleMillis = now;
    float currentC = temperatureRead();
    if (!std::isfinite(currentC) || currentC < -40.0f || currentC > 125.0f) {
        return;
    }

    thermalStatus.currentC = currentC;
    if (std::isfinite(thermalStatus.averageC)) {
        thermalStatus.averageC =
            (thermalStatus.averageC * (1.0f - CHIP_TEMP_EMA_ALPHA)) +
            (currentC * CHIP_TEMP_EMA_ALPHA);
    } else {
        thermalStatus.averageC = currentC;
    }

    updateThermalLevel();
}

const char* getUpstreamStatusText() {
    if (!wifiCfg.enabled)          return "未启用";
    if (!hasUpstreamCredentials()) return "未配置热点";
    if (isThermalProtectionActive()) return "过热保护中";
    if (WiFi.status() == WL_CONNECTED) return "已连接";

    switch (upstreamRt.phase) {
        case UpstreamConnectPhase::Scanning:   return "正在扫描";
        case UpstreamConnectPhase::Connecting: return "连接中";
        case UpstreamConnectPhase::Backoff:    return "等待重试";
        default:
            break;
    }

    switch (WiFi.status()) {
        case WL_NO_SSID_AVAIL:   return "找不到热点";
        case WL_CONNECT_FAILED:  return "连接失败";
        case WL_CONNECTION_LOST: return "连接丢失";
        case WL_DISCONNECTED:    return "未连接";
        case WL_IDLE_STATUS:
        default:
            return "连接中";
    }
}

const char* getNATStatusText() {
    if (natEnabled) return "已启用";
    if (WiFi.status() == WL_CONNECTED) return "待启用";
    return "未连接上游";
}

const char* getUpstreamSignalText(int32_t rssi) {
    if (rssi >= -55) return "优秀";
    if (rssi >= -67) return "良好";
    if (rssi >= -75) return "一般";
    return "较弱";
}

void requestUpstreamApply() {
    wifiCfg.applyRequested = true;
}

void requestLocalAPApply() {
    apCfg.applyRequested = true;
    apCfg.applyAtMillis = millis() + AP_CONFIG_APPLY_DELAY_MS;
}

void clearAutomaticUpstreamScanState(bool stopActiveScan) {
    bool wasScanning = upstreamAutomaticScanInProgress;
    upstreamAutomaticScanInProgress = false;
    upstreamRt.phase = UpstreamConnectPhase::Idle;

    if (stopActiveScan && wasScanning) {
        esp_err_t stopErr = esp_wifi_scan_stop();
        if (stopErr != ESP_OK && stopErr != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "upstream scan stop err=%s", esp_err_to_name(stopErr));
        }
    }

    WiFi.scanDelete();
    upstreamScanInProgress = false;
    upstreamScanCompletionPending = false;
    upstreamScanCompletionSucceeded = false;
    upstreamScanStartedMillis = 0;
}

void resetUpstreamRuntime(bool clearBackoff) {
    clearAutomaticUpstreamScanState(true);
    upstreamRt.phase = UpstreamConnectPhase::Idle;
    wifiCfg.activeIndex = -1;
    if (clearBackoff) {
        upstreamRt.backoffIndex = 0;
        upstreamRt.retryCount = 0;
        wifiCfg.nextTryIndex = 0;
    }
    upstreamRt.connectStartedMillis = 0;
}

void markUpstreamConnected() {
    int connectedIndex = findSavedUpstreamNetwork(WiFi.SSID());
    if (connectedIndex >= 0) wifiCfg.activeIndex = connectedIndex;
    upstreamRt.phase = UpstreamConnectPhase::Connected;
    upstreamRt.backoffIndex = 0;
    wifiCfg.lastAttemptMillis = millis();
    upstreamRt.lastAttemptMillis = wifiCfg.lastAttemptMillis;
}

uint32_t currentUpstreamBackoffMs() {
    uint8_t index = upstreamRt.backoffIndex;
    if (index >= UPSTREAM_BACKOFF_STEPS) index = UPSTREAM_BACKOFF_STEPS - 1;
    uint32_t backoffMs = UPSTREAM_BACKOFF_MS[index];
    if (isThermalThrottleActive() && backoffMs < UPSTREAM_RETRY_THROTTLED_MS) {
        backoffMs = UPSTREAM_RETRY_THROTTLED_MS;
    }
    return backoffMs;
}

void enterUpstreamBackoff(const char* reason) {
    clearAutomaticUpstreamScanState(true);
    uint32_t delayMs = currentUpstreamBackoffMs();
    if (upstreamRt.backoffIndex + 1 < UPSTREAM_BACKOFF_STEPS) {
        ++upstreamRt.backoffIndex;
    }
    upstreamRt.phase = UpstreamConnectPhase::Backoff;
    uint32_t now = millis();
    wifiCfg.lastAttemptMillis = now;
    upstreamRt.lastAttemptMillis = now;
    ESP_LOGW(
        TAG,
        "upstream backoff reason=%s delay=%ums retry=%lu disc=%d",
        reason ? reason : "unknown",
        static_cast<unsigned>(delayMs),
        static_cast<unsigned long>(upstreamRt.retryCount),
        upstreamRt.lastDisconnectReason
    );
    if (debugLogReady) {
        String line;
        line.reserve(80);
        line += reason ? reason : "unknown";
        line += " delay=";
        line += String(static_cast<unsigned>(delayMs));
        line += " retry=";
        line += String(static_cast<unsigned>(upstreamRt.retryCount));
        line += " disc=";
        line += String(upstreamRt.lastDisconnectReason);
        appendDebugLogRecord("WIFI", line);
    }
}

bool localAPIsHealthy() {
    return apStarted
        && WiFi.softAPIP() == AP_IP
        && WiFi.softAPSSID().equals(apCfg.ssid);
}

bool applyLocalAPConfig(bool forceConfigApply) {
    if (!forceConfigApply && localAPIsHealthy()) {
        apRestartRequested = false;
        return true;
    }

    apApplyInProgress = true;
    apEventSettleUntilMillis = millis() + AP_EVENT_SETTLE_MS;
    bool ipConfigOk = WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    bool ok = ipConfigOk && WiFi.softAP(apCfg.ssid, apCfg.pass);
    apApplyInProgress = false;
    lastApRestartMillis = millis();
    apRestartRequested = false;
    apStarted = ok;
    if (ok) {
        ESP_LOGI(TAG, "local AP ready ssid=%s ip=%s channel=%d", apCfg.ssid, WiFi.softAPIP().toString().c_str(), WiFi.channel());
        Serial.printf("WiFi AP: %s  IP: %s\n", apCfg.ssid, WiFi.softAPIP().toString().c_str());
        if (WiFi.status() == WL_CONNECTED) {
            ip_napt_enable(static_cast<u32_t>(AP_IP), 1);
            natEnabled = true;
        }
    } else {
        ESP_LOGE(TAG, "local AP start failed ssid=%s", apCfg.ssid);
        Serial.printf("WiFi AP restart failed for SSID: %s\n", apCfg.ssid);
        requestLocalAPRestart("ap-start-failed", true);
    }
    return ok;
}

void beginUpstreamConnect(int networkIndex) {
    if (networkIndex < 0 || networkIndex >= wifiCfg.networkCount) {
        wifiCfg.activeIndex = -1;
        enterUpstreamBackoff("no-network");
        return;
    }

    wifiCfg.activeIndex = networkIndex;
    wifiCfg.nextTryIndex = (networkIndex + 1) % wifiCfg.networkCount;

    const SavedUpstreamNetwork& network = wifiCfg.networks[networkIndex];
    ++upstreamRt.retryCount;
    ESP_LOGI(
        TAG,
        "connecting upstream WiFi ssid=%s retry=%lu",
        network.ssid,
        static_cast<unsigned long>(upstreamRt.retryCount)
    );
    Serial.printf("Connecting upstream WiFi: %s\n", network.ssid);
    WiFi.disconnect(false, false);
    if (network.pass[0] != '\0') {
        WiFi.begin(network.ssid, network.pass);
    } else {
        WiFi.begin(network.ssid);
    }
    uint32_t now = millis();
    wifiCfg.lastAttemptMillis = now;
    upstreamRt.lastAttemptMillis = now;
    upstreamRt.connectStartedMillis = now;
    upstreamRt.phase = UpstreamConnectPhase::Connecting;
}

void finishUpstreamScanAndConnect(
    UpstreamScanAction action,
    bool stopActiveScan,
    const char* fallbackReason
) {
    UpstreamScanResult results[MAX_SCAN_RESULTS];
    int uniqueCount = 0;
    int visibleSavedIndex = -1;

    if (action == UpstreamScanAction::ConsumeResults) {
        int foundCount = WiFi.scanComplete();
        if (foundCount > 0) {
            uniqueCount = fillUniqueScanResults(foundCount, results, MAX_SCAN_RESULTS);
            visibleSavedIndex = pickSavedNetworkFromScan(results, uniqueCount);
        } else if (foundCount == WIFI_SCAN_FAILED) {
            fallbackReason = "scan-results-unavailable";
        }
    }

    clearAutomaticUpstreamScanState(stopActiveScan);

    int fallbackIndex = visibleSavedIndex >= 0
        ? -1
        : pickRoundRobinUpstreamNetworkIndex();
    int networkIndex = chooseUpstreamNetworkIndex(visibleSavedIndex, fallbackIndex);

    if (visibleSavedIndex < 0 && networkIndex >= 0) {
        ESP_LOGW(
            TAG,
            "upstream scan fallback reason=%s savedIndex=%d",
            fallbackReason ? fallbackReason : "no-visible-saved-hotspot",
            networkIndex
        );
    }

    if (networkIndex < 0) {
        enterUpstreamBackoff("no-saved-hotspot");
        return;
    }

    beginUpstreamConnect(networkIndex);
}

void tryStartUpstreamAttempt() {
    if (!wifiCfg.enabled || !hasUpstreamCredentials()) return;
    if (isThermalProtectionActive()) return;
    if (WiFi.status() == WL_CONNECTED) {
        markUpstreamConnected();
        return;
    }
    if (isUpstreamRadioBusy()) return;

    clearAutomaticUpstreamScanState(false);
    upstreamRt.phase = UpstreamConnectPhase::Scanning;
    upstreamScanInProgress = true;
    upstreamAutomaticScanInProgress = true;
    upstreamScanStartedMillis = millis();
    wifiCfg.lastAttemptMillis = upstreamScanStartedMillis;
    upstreamRt.lastAttemptMillis = upstreamScanStartedMillis;

    int scanRc = WiFi.scanNetworks(true, true);
    if (scanRc == WIFI_SCAN_RUNNING || scanRc >= 0) {
        ESP_LOGI(TAG, "upstream async scan started rc=%d", scanRc);
        if (scanRc >= 0) {
            upstreamScanCompletionSucceeded = true;
            upstreamScanCompletionPending = true;
        }
        return;
    }

    ESP_LOGW(TAG, "upstream scan start failed rc=%d, using saved hotspot", scanRc);
    finishUpstreamScanAndConnect(
        UpstreamScanAction::UseSavedFallback,
        false,
        "scan-start-failed"
    );
}

void applyUpstreamWiFiConfig() {
    if (isThermalProtectionActive()) {
        WiFi.disconnect(false, true);
        resetUpstreamRuntime(true);
        wifiCfg.lastAttemptMillis = 0;
        upstreamRt.lastAttemptMillis = 0;
        ESP_LOGW(TAG, "upstream WiFi paused by thermal protection");
        Serial.println("Upstream WiFi paused by thermal protection");
        return;
    }

    if (!wifiCfg.enabled || !hasUpstreamCredentials()) {
        WiFi.disconnect(false, true);
        resetUpstreamRuntime(true);
        wifiCfg.lastAttemptMillis = 0;
        upstreamRt.lastAttemptMillis = 0;
        ESP_LOGI(TAG, "upstream WiFi disabled enabled=%d saved=%u", static_cast<int>(wifiCfg.enabled), static_cast<unsigned>(wifiCfg.networkCount));
        Serial.println("Upstream WiFi disabled");
        return;
    }

    WiFi.disconnect(false, true);
    resetUpstreamRuntime(true);
    wifiCfg.lastAttemptMillis = 0;
    upstreamRt.lastAttemptMillis = 0;
    tryStartUpstreamAttempt();
}

void serviceLocalAP() {
    uint32_t now = millis();

    if (apCfg.applyRequested && static_cast<int32_t>(now - apCfg.applyAtMillis) >= 0) {
        apCfg.applyRequested = false;
        applyLocalAPConfig(true);
        return;
    }

    if (lastApHealthCheckMillis == 0 || now - lastApHealthCheckMillis >= AP_HEALTH_CHECK_MS) {
        lastApHealthCheckMillis = now;
        if (!localAPIsHealthy()) {
            requestLocalAPRestart("health-check");
        }
    }

    if (!apRestartRequested) return;
    if (isUpstreamRadioBusy()) return;
    if (lastApRestartMillis != 0 && now - lastApRestartMillis < AP_RESTART_MIN_INTERVAL_MS) return;

    applyLocalAPConfig(false);
}

void serviceUpstreamWiFi() {
    if (wifiCfg.applyRequested) {
        wifiCfg.applyRequested = false;
        applyUpstreamWiFiConfig();
        return;
    }

    if (!wifiCfg.enabled || !hasUpstreamCredentials()) {
        if (upstreamRt.phase != UpstreamConnectPhase::Idle) {
            WiFi.disconnect(false, true);
            resetUpstreamRuntime(true);
        }
        return;
    }

    if (isThermalProtectionActive()) {
        if (WiFi.status() == WL_CONNECTED || isUpstreamRadioBusy()) {
            WiFi.disconnect(false, true);
            resetUpstreamRuntime(true);
            ESP_LOGW(TAG, "upstream WiFi disconnected due to thermal protection");
            Serial.println("Upstream WiFi disconnected due to thermal protection");
        }
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (upstreamRt.phase == UpstreamConnectPhase::Scanning) {
            clearAutomaticUpstreamScanState(true);
        }
        markUpstreamConnected();
        return;
    }

    if (upstreamRt.phase == UpstreamConnectPhase::Connected) {
        enterUpstreamBackoff("sta-lost");
        return;
    }

    if (upstreamRt.phase == UpstreamConnectPhase::Scanning) {
        bool completionPending = upstreamScanCompletionPending;
        bool completionSucceeded = upstreamScanCompletionSucceeded;
        uint32_t now = millis();
        UpstreamScanAction action = decideUpstreamScanAction(
            completionPending,
            completionSucceeded,
            now,
            upstreamScanStartedMillis,
            UPSTREAM_SCAN_TIMEOUT_MS
        );

        if (action == UpstreamScanAction::Wait) return;

        bool timedOut = !completionPending;
        finishUpstreamScanAndConnect(
            action,
            timedOut,
            timedOut ? "scan-timeout" : (completionSucceeded ? nullptr : "scan-failed")
        );
        return;
    }

    if (upstreamRt.phase == UpstreamConnectPhase::Connecting) {
        uint32_t now = millis();
        if (now - upstreamRt.connectStartedMillis < UPSTREAM_CONNECT_TIMEOUT_MS) return;
        ESP_LOGW(TAG, "upstream connect timeout");
        WiFi.disconnect(false, false);
        enterUpstreamBackoff("connect-timeout");
        return;
    }

    uint32_t now = millis();
    uint32_t waitMs = (upstreamRt.lastAttemptMillis == 0) ? 0 : currentUpstreamBackoffMs();
    if (waitMs > 0 && now - upstreamRt.lastAttemptMillis < waitMs) return;

    tryStartUpstreamAttempt();
}

void syncNATState() {
    bool shouldEnable = WiFi.status() == WL_CONNECTED;
    if (natEnabled == shouldEnable) return;

    ip_napt_enable(static_cast<u32_t>(AP_IP), shouldEnable ? 1 : 0);
    natEnabled = shouldEnable;
    ESP_LOGI(TAG, "NAPT %s on %s", natEnabled ? "enabled" : "disabled", AP_IP.toString().c_str());
    Serial.printf("NAPT %s on %s\n", natEnabled ? "enabled" : "disabled", AP_IP.toString().c_str());
}

// ═══════════════════════════════════════════
//  Config persistence (NVS)
// ═══════════════════════════════════════════

void loadConfig() {
    prefs.begin("fsd", true);  // read-only
    cfg.fsdEnable          = prefs.getBool("fsdEn", true);
    cfg.hwMode             = prefs.getUChar("hwMode", 1);   // 默认 HW3
    cfg.speedProfile       = prefs.getUChar("spPro", 1);
    cfg.profileModeAuto    = prefs.getBool("proAuto", true);
    cfg.isaChimeSuppress   = prefs.getBool("isaChm", false);
    cfg.emergencyDetection = prefs.getBool("emDet", true);
    cfg.chinaMode          = prefs.getBool("cnMode", true);  // 默认开启 chinaMode
    copyStringToBuffer(apCfg.ssid, sizeof(apCfg.ssid), prefs.getString("apSsid", DEFAULT_AP_SSID));
    copyStringToBuffer(apCfg.pass, sizeof(apCfg.pass), prefs.getString("apPass", DEFAULT_AP_PASS));
    clearSavedUpstreamNetworks();
    wifiCfg.enabled        = prefs.getBool("upEn", false);
    uint8_t storedNetworkCount = prefs.getUChar("upCnt", 0);
    for (uint8_t i = 0; i < storedNetworkCount && i < MAX_UPSTREAM_NETWORKS; ++i) {
        char ssidKey[10];
        char passKey[10];
        snprintf(ssidKey, sizeof(ssidKey), "upSsid%u", i);
        snprintf(passKey, sizeof(passKey), "upPass%u", i);

        String ssid = prefs.getString(ssidKey, "");
        String pass = prefs.getString(passKey, "");
        if (!ssid.isEmpty() && pass.length() <= 63) {
            addOrUpdateSavedUpstreamNetwork(ssid, pass, true);
        }
    }
    dnsCfg.enabled         = prefs.getBool("dnsEn", false);
    copyStringToBuffer(dnsCfg.allowlist, sizeof(dnsCfg.allowlist), prefs.getString("dnsList", ""));
    copyStringToBuffer(dnsCfg.blocklist, sizeof(dnsCfg.blocklist), prefs.getString("dnsBlk", ""));
    prefs.end();

    // Clamp values
    if (cfg.hwMode > 2)       cfg.hwMode = 2;
    if (cfg.speedProfile > 4) cfg.speedProfile = 1;
    if (apCfg.ssid[0] == '\0') {
        copyStringToBuffer(apCfg.ssid, sizeof(apCfg.ssid), String(DEFAULT_AP_SSID));
    }
    size_t apPassLen = strlen(apCfg.pass);
    if (apPassLen < 8 || apPassLen > 63) {
        copyStringToBuffer(apCfg.pass, sizeof(apCfg.pass), String(DEFAULT_AP_PASS));
    }
}

void saveConfig() {
    prefs.begin("fsd", false);  // read-write
    prefs.putBool("fsdEn",   cfg.fsdEnable);
    prefs.putUChar("hwMode", cfg.hwMode);
    prefs.putUChar("spPro",  cfg.speedProfile);
    prefs.putBool("proAuto", cfg.profileModeAuto);
    prefs.remove("spOffEn");
    prefs.remove("spOffCap");
    prefs.remove("spOffPct");
    prefs.putBool("isaChm",  cfg.isaChimeSuppress);
    prefs.putBool("emDet",   cfg.emergencyDetection);
    prefs.putBool("cnMode",  cfg.chinaMode);
    prefs.putString("apSsid", apCfg.ssid);
    prefs.putString("apPass", apCfg.pass);
    prefs.putBool("upEn",    wifiCfg.enabled);
    prefs.putUChar("upCnt",  wifiCfg.networkCount);
    for (uint8_t i = 0; i < MAX_UPSTREAM_NETWORKS; ++i) {
        char ssidKey[10];
        char passKey[10];
        snprintf(ssidKey, sizeof(ssidKey), "upSsid%u", i);
        snprintf(passKey, sizeof(passKey), "upPass%u", i);
        if (i < wifiCfg.networkCount) {
            prefs.putString(ssidKey, wifiCfg.networks[i].ssid);
            prefs.putString(passKey, wifiCfg.networks[i].pass);
        } else {
            prefs.remove(ssidKey);
            prefs.remove(passKey);
        }
    }
    prefs.putBool("dnsEn",   dnsCfg.enabled);
    prefs.putString("dnsList", dnsCfg.allowlist);
    prefs.putString("dnsBlk", dnsCfg.blocklist);
    prefs.end();
}

bool applyConfigPatch(const ConfigPatch& patch, String& error) {
    if (configMutex == nullptr || xSemaphoreTake(configMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        error = "配置正忙，请稍后重试";
        return false;
    }

    String nextAPSSID = patch.hasAPSSID ? patch.apSSID : String(apCfg.ssid);
    String nextAPPass = patch.hasAPPass ? patch.apPass : String(apCfg.pass);
    String nextAllowlist = patch.dnsAllowlist;
    String nextBlocklist = patch.dnsBlocklist;
    nextAPSSID.trim();
    nextAllowlist.trim();
    nextBlocklist.trim();

    controller_contract::ConfigValidationInput validation;
    validation.hasHardwareMode = patch.hasHwMode;
    validation.hardwareMode = patch.hwMode;
    validation.hasSpeedProfile = patch.hasSpeedProfile;
    validation.speedProfile = patch.speedProfile;
    validation.touchesAP = patch.hasAPSSID || patch.hasAPPass;
    validation.apSSIDLength = nextAPSSID.length();
    validation.apPasswordLength = nextAPPass.length();
    validation.hasDNSAllowlist = patch.hasDNSAllowlist;
    validation.dnsAllowlistLength = nextAllowlist.length();
    validation.dnsAllowlistCapacity = sizeof(dnsCfg.allowlist);
    validation.hasDNSBlocklist = patch.hasDNSBlocklist;
    validation.dnsBlocklistLength = nextBlocklist.length();
    validation.dnsBlocklistCapacity = sizeof(dnsCfg.blocklist);
    switch (controller_contract::validateConfig(validation)) {
        case controller_contract::ConfigValidationError::InvalidHardwareMode:
            error = "硬件版本无效";
            break;
        case controller_contract::ConfigValidationError::InvalidSpeedProfile:
            error = "速度模式无效";
            break;
        case controller_contract::ConfigValidationError::InvalidAPSSID:
            error = "热点名称长度必须为 1-32 个字符";
            break;
        case controller_contract::ConfigValidationError::InvalidAPPassword:
            error = "热点密码长度必须为 8-63 个字符";
            break;
        case controller_contract::ConfigValidationError::DNSAllowlistTooLong:
            error = "DNS 白名单内容过长";
            break;
        case controller_contract::ConfigValidationError::DNSBlocklistTooLong:
            error = "DNS 黑名单内容过长";
            break;
        case controller_contract::ConfigValidationError::None:
            break;
    }

    if (!error.isEmpty()) {
        xSemaphoreGive(configMutex);
        return false;
    }

    bool changed = false;
    bool wifiChanged = false;
    bool apChanged = false;
    bool apPasswordChanged = false;

    if (patch.hasFsdEnable && cfg.fsdEnable != patch.fsdEnable) {
        cfg.fsdEnable = patch.fsdEnable;
        if (!cfg.fsdEnable) cfg.appliedSpeedOffsetKph = 0;
        changed = true;
    }
    if (patch.hasHwMode && cfg.hwMode != patch.hwMode) {
        if (patch.hwMode == 1 || cfg.hwMode == 1) resetHW3SpeedLimitState();
        cfg.hwMode = patch.hwMode;
        changed = true;
    }
    if (patch.hasSpeedProfile && cfg.speedProfile != patch.speedProfile) {
        cfg.speedProfile = patch.speedProfile;
        changed = true;
    }
    if (patch.hasProfileMode && cfg.profileModeAuto != patch.profileModeAuto) {
        cfg.profileModeAuto = patch.profileModeAuto;
        changed = true;
    }
    if (patch.hasIsaChime && cfg.isaChimeSuppress != patch.isaChime) {
        cfg.isaChimeSuppress = patch.isaChime;
        changed = true;
    }
    if (patch.hasEmergencyDetection && cfg.emergencyDetection != patch.emergencyDetection) {
        cfg.emergencyDetection = patch.emergencyDetection;
        changed = true;
    }
    if (patch.hasChinaMode && cfg.chinaMode != patch.chinaMode) {
        cfg.chinaMode = patch.chinaMode;
        changed = true;
    }
    if ((patch.hasAPSSID || patch.hasAPPass)
        && (nextAPSSID != String(apCfg.ssid) || nextAPPass != String(apCfg.pass))) {
        apPasswordChanged = nextAPPass != String(apCfg.pass);
        copyStringToBuffer(apCfg.ssid, sizeof(apCfg.ssid), nextAPSSID);
        copyStringToBuffer(apCfg.pass, sizeof(apCfg.pass), nextAPPass);
        changed = true;
        apChanged = true;
    }
    if (patch.hasUpstreamEnable && wifiCfg.enabled != patch.upstreamEnable) {
        wifiCfg.enabled = patch.upstreamEnable;
        changed = true;
        wifiChanged = true;
    }
    if (patch.hasDNSWhitelistEnable && dnsCfg.enabled != patch.dnsWhitelistEnable) {
        dnsCfg.enabled = patch.dnsWhitelistEnable;
        changed = true;
    }
    if (patch.hasDNSAllowlist && nextAllowlist != String(dnsCfg.allowlist)) {
        copyStringToBuffer(dnsCfg.allowlist, sizeof(dnsCfg.allowlist), nextAllowlist);
        changed = true;
    }
    if (patch.hasDNSBlocklist && nextBlocklist != String(dnsCfg.blocklist)) {
        copyStringToBuffer(dnsCfg.blocklist, sizeof(dnsCfg.blocklist), nextBlocklist);
        changed = true;
    }

    if (changed) saveConfig();
    if (apChanged) requestLocalAPApply();
    if (wifiChanged) requestUpstreamApply();
    if (apPasswordChanged) updateBLEPasskey();
    xSemaphoreGive(configMutex);
    return true;
}

bool saveUpstreamNetwork(const String& ssidInput, const String& pass, bool overwritePass, String& error) {
    String ssid = ssidInput;
    ssid.trim();
    if (ssid.isEmpty()) error = "热点名称不能为空";
    else if (ssid.length() > 32 || pass.length() > 63) error = "热点名称或密码长度不合法";
    else if (isReservedUpstreamSSID(ssid)) error = "不能保存本机发射的热点";
    if (!error.isEmpty()) return false;

    if (configMutex == nullptr || xSemaphoreTake(configMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        error = "配置正忙，请稍后重试";
        return false;
    }
    if (findSavedUpstreamNetwork(ssid) < 0 && wifiCfg.networkCount >= MAX_UPSTREAM_NETWORKS) {
        error = "已达到可保存热点上限";
    } else if (!addOrUpdateSavedUpstreamNetwork(ssid, pass, overwritePass)) {
        error = "保存热点失败";
    } else {
        saveConfig();
        requestUpstreamApply();
    }
    xSemaphoreGive(configMutex);
    return error.isEmpty();
}

bool deleteUpstreamNetwork(const String& ssidInput, String& error) {
    String ssid = ssidInput;
    ssid.trim();
    if (ssid.isEmpty()) {
        error = "热点名称不能为空";
        return false;
    }
    if (configMutex == nullptr || xSemaphoreTake(configMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        error = "配置正忙，请稍后重试";
        return false;
    }
    if (!removeSavedUpstreamNetwork(ssid)) {
        error = "热点不存在";
    } else {
        saveConfig();
        requestUpstreamApply();
    }
    xSemaphoreGive(configMutex);
    return error.isEmpty();
}

String buildUpstreamScanJson(int& statusCode, String& error) {
    UpstreamScanResult results[MAX_SCAN_RESULTS];
    int resultCount = performUpstreamScan(results, MAX_SCAN_RESULTS);
    if (resultCount < 0) {
        statusCode = 409;
        error = "热点搜索忙，请稍后再试";
        return String();
    }

    String json = "{\"results\":[";
    json.reserve(1200);
    for (int i = 0; i < resultCount; ++i) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"";
        json += jsonEscape(String(results[i].ssid));
        json += "\",\"rssi\":";
        json += String(results[i].rssi);
        json += ",\"secure\":";
        json += (results[i].secure ? "true" : "false");
        json += ",\"saved\":";
        json += (results[i].saved ? "true" : "false");
        json += "}";
    }
    json += "]}";
    statusCode = 200;
    return json;
}

String buildStatusJson(
    controller_contract::StatusAudience audience = controller_contract::StatusAudience::Web
) {
    uint32_t now = millis();
    uint32_t uptime = (now - cfg.uptimeStart) / 1000;
    bool upstreamConnected = WiFi.status() == WL_CONNECTED;
    int32_t upstreamRSSI = upstreamConnected ? WiFi.RSSI() : 0;
    int32_t wifiChannel = WiFi.channel();
    int32_t apClientCount = WiFi.softAPgetStationNum();
    String activeSSID = jsonEscape(getActiveUpstreamSSID());
    String connectedSSID = jsonEscape(getConnectedUpstreamSSID());
    String apSSID = jsonEscape(String(apCfg.ssid));
    String apPass = jsonEscape(String(apCfg.pass));
    String upstreamIP = upstreamConnected ? WiFi.localIP().toString() : "";
    String apIP = WiFi.softAPIP().toString();
    String upstreamStatus = jsonEscape(String(getUpstreamStatusText()));
    String upstreamSignal = upstreamConnected ? jsonEscape(String(getUpstreamSignalText(upstreamRSSI))) : "";
    String dnsAllowlist = jsonEscape(String(dnsCfg.allowlist));
    String dnsBlocklist = jsonEscape(String(dnsCfg.blocklist));
    String natStatus = jsonEscape(String(getNATStatusText()));
    String thermalStatusText = jsonEscape(String(getThermalStatusText()));
    String savedNetworks = buildSavedUpstreamNetworksJson();
    bool rxSeen = cfg.lastRxMillis != 0;
    bool modifiedSeen = cfg.lastModifiedMillis != 0;
    uint32_t rxAgeMs = rxSeen ? now - cfg.lastRxMillis : 0;
    uint32_t modifiedAgeMs = modifiedSeen ? now - cfg.lastModifiedMillis : 0;
    bool rxActive = rxSeen && rxAgeMs <= CAN_ACTIVITY_WINDOW_MS;
    bool modifiedActive = modifiedSeen && modifiedAgeMs <= CAN_ACTIVITY_WINDOW_MS;
    bool canReady = cfg.canOK;
    bool canHealthy = canReady && rxActive;
    bool vehicleSpeedSeen = cfg.lastVehicleSpeedMillis != 0;
    uint32_t vehicleSpeedAgeMs = vehicleSpeedSeen ? now - cfg.lastVehicleSpeedMillis : 0;
    bool vehicleSpeedValid = vehicleSpeedSeen && vehicleSpeedAgeMs <= VEHICLE_SPEED_STALE_MS;
    uint32_t dnsBlockedCount = 0;
    size_t dnsBlockedRecentCount = 0;
    String dnsBlockedRequests = buildBlockedDnsRequestsJson(dnsBlockedCount, dnsBlockedRecentCount);
    size_t debugLogBytes = debugLogReady ? static_cast<size_t>(debugLogCachedBytes) : 0;
    String json;

    json.reserve(7800);
    json += "{";
    json += "\"rx\":";
    json += String((unsigned)cfg.rxCount);
    json += ",\"modified\":";
    json += String((unsigned)cfg.modifiedCount);
    json += ",\"errors\":";
    json += String((unsigned)cfg.errorCount);
    json += ",\"uptime\":";
    json += String((unsigned)uptime);
    json += ",\"rxActive\":";
    json += (rxActive ? "true" : "false");
    json += ",\"modifiedActive\":";
    json += (modifiedActive ? "true" : "false");
    json += ",\"rxAgeMs\":";
    json += rxSeen ? String((unsigned)rxAgeMs) : "null";
    json += ",\"modifiedAgeMs\":";
    json += modifiedSeen ? String((unsigned)modifiedAgeMs) : "null";
    json += ",\"canReady\":";
    json += (canReady ? "true" : "false");
    json += ",\"chipTempC\":";
    json += std::isfinite(thermalStatus.currentC) ? String(thermalStatus.currentC, 1) : "null";
    json += ",\"chipTempAvgC\":";
    json += std::isfinite(thermalStatus.averageC) ? String(thermalStatus.averageC, 1) : "null";
    json += ",\"thermalStatus\":\"";
    json += thermalStatusText;
    json += "\",\"thermalProtect\":";
    json += (isThermalProtectionActive() ? "true" : "false");
    json += ",\"canOK\":";
    json += (canHealthy ? "true" : "false");
    json += ",\"fsdTriggered\":";
    json += (cfg.fsdTriggered ? "true" : "false");
    json += ",\"fsdEnable\":";
    json += String((int)cfg.fsdEnable);
    json += ",\"hwMode\":";
    json += String((int)cfg.hwMode);
    json += ",\"speedProfile\":";
    json += String((int)cfg.speedProfile);
    json += ",\"profileMode\":";
    json += String((int)cfg.profileModeAuto);
    json += ",\"detectedSpeedLimitKph\":";
    json += String((unsigned)cfg.detectedSpeedLimitKph);
    json += ",\"detectedSpeedSource\":";
    json += String((int)cfg.detectedSpeedSource);
    json += ",\"appliedSpeedOffsetKph\":";
    json += String((int)cfg.appliedSpeedOffsetKph);
    json += ",\"vehicleSpeedKph\":";
    json += String(static_cast<float>(cfg.vehicleSpeedCentiKph) / 100.0f, 1);
    json += ",\"vehicleSpeedSource\":";
    json += String((int)cfg.vehicleSpeedSource);
    json += ",\"vehicleSpeedAgeMs\":";
    json += vehicleSpeedSeen ? String((unsigned)vehicleSpeedAgeMs) : "null";
    json += ",\"vehicleSpeedValid\":";
    json += (vehicleSpeedValid ? "true" : "false");
    json += ",\"debugLogReady\":";
    json += (debugLogReady ? "true" : "false");
    json += ",\"debugLogBytes\":";
    json += String(static_cast<unsigned>(debugLogBytes));
    json += ",\"resetReason\":\"";
    json += getResetReasonName(bootResetReason);
    json += "\",\"freeHeap\":";
    json += String(static_cast<unsigned>(ESP.getFreeHeap()));
    json += ",\"minFreeHeap\":";
    json += String(static_cast<unsigned>(ESP.getMinFreeHeap()));
    json += ",\"apRunning\":";
    json += (apStarted ? "true" : "false");
    json += ",\"upstreamPhase\":\"";
    json += getUpstreamPhaseName(upstreamRt.phase);
    json += "\",\"upstreamRetryCount\":";
    json += String(static_cast<unsigned>(upstreamRt.retryCount));
    json += ",\"lastStaDisconnectReason\":";
    json += String(upstreamRt.lastDisconnectReason);
    json += ",\"isaChime\":";
    json += String((int)cfg.isaChimeSuppress);
    json += ",\"emergencyDet\":";
    json += String((int)cfg.emergencyDetection);
    json += ",\"chinaMode\":";
    json += String((int)cfg.chinaMode);
    json += ",\"upstreamEnable\":";
    json += String((int)wifiCfg.enabled);
    json += ",\"upstreamConfigured\":";
    json += String(hasUpstreamCredentials() ? 1 : 0);
    json += ",\"upstreamConnected\":";
    json += (upstreamConnected ? "true" : "false");
    json += ",\"upstreamRSSI\":";
    json += upstreamConnected ? String(upstreamRSSI) : "null";
    json += ",\"wifiChannel\":";
    json += String(wifiChannel);
    json += ",\"apClients\":";
    json += String(apClientCount);
    json += ",\"apPasswordConfigured\":";
    json += (strlen(apCfg.pass) >= 8 ? "true" : "false");
    json += ",\"apPasswordIsDefault\":";
    json += (String(apCfg.pass) == String(DEFAULT_AP_PASS) ? "true" : "false");
    if (controller_contract::includesAPPassword(audience)) {
        json += ",\"apPassword\":\"";
        json += apPass;
        json += "\"";
    }
    json += ",\"upstreamSSID\":\"";
    json += activeSSID;
    json += "\",\"connectedUpstreamSSID\":\"";
    json += connectedSSID;
    json += "\",\"upstreamSavedCount\":";
    json += String((unsigned)wifiCfg.networkCount);
    json += ",\"upstreamNetworks\":";
    json += savedNetworks;
    json += ",\"upstreamStatus\":\"";
    json += upstreamStatus;
    json += "\",\"upstreamSignal\":\"";
    json += upstreamSignal;
    json += "\",\"upstreamIP\":\"";
    json += jsonEscape(upstreamIP);
    json += "\",\"apSSID\":\"";
    json += apSSID;
    json += "\",\"apIP\":\"";
    json += jsonEscape(apIP);
    json += "\",\"dnsWhitelistEnable\":";
    json += String((int)dnsCfg.enabled);
    json += ",\"dnsWhitelistCount\":";
    json += String((unsigned)getDNSRuleCount(dnsCfg.allowlist));
    json += ",\"dnsBlacklistCount\":";
    json += String((unsigned)getDNSRuleCount(dnsCfg.blocklist));
    json += ",\"dnsAllowlist\":\"";
    json += dnsAllowlist;
    json += "\",\"dnsBlocklist\":\"";
    json += dnsBlocklist;
    json += "\",\"dnsBlockedCount\":";
    json += String((unsigned)dnsBlockedCount);
    json += ",\"dnsBlockedRecentCount\":";
    json += String((unsigned)dnsBlockedRecentCount);
    json += ",\"dnsBlockedRequests\":";
    json += dnsBlockedRequests;
    {
        uint32_t dnsAllowIpCount = 0;
        uint32_t dnsBlockIpCount = 0;
        int dnsStrictAllow = 0;
        int dnsPolicyEnabled = 0;
        dnsIpPolicyGetStats(&dnsAllowIpCount, &dnsBlockIpCount, &dnsStrictAllow, &dnsPolicyEnabled);
        json += ",\"dnsPolicyEnabled\":";
        json += String(dnsPolicyEnabled);
        json += ",\"dnsStrictAllow\":";
        json += String(dnsStrictAllow);
        json += ",\"dnsAllowIpCount\":";
        json += String(static_cast<unsigned>(dnsAllowIpCount));
        json += ",\"dnsBlockIpCount\":";
        json += String(static_cast<unsigned>(dnsBlockIpCount));
        json += ",\"dnsForwardPolicy\":\"";
        if (!dnsPolicyEnabled) json += "off";
        else if (dnsStrictAllow) json += "strict-allow";
        else json += "blocklist-only";
        json += "\"";
    }
    json += ",\"natEnabled\":";
    json += String(natEnabled ? 1 : 0);
    json += ",\"natStatus\":\"";
    json += natStatus;
    json += "\"";
    json += "}";
    return json;
}

uint32_t deriveBLEPasskey() {
    uint8_t digest[32] = {};
    const auto* password = reinterpret_cast<const uint8_t*>(apCfg.pass);
    mbedtls_sha256_ret(password, strlen(apCfg.pass), digest, 0);
    uint32_t value = (static_cast<uint32_t>(digest[0]) << 24)
        | (static_cast<uint32_t>(digest[1]) << 16)
        | (static_cast<uint32_t>(digest[2]) << 8)
        | static_cast<uint32_t>(digest[3]);
    return value % 1000000U;
}

void updateBLEPasskey() {
    if (bleSecurity == nullptr) return;
    bleSecurity->setStaticPIN(deriveBLEPasskey());
    bleSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
}

String buildBLESuccess(uint16_t messageId, const String& data = "null") {
    String json;
    json.reserve(data.length() + 48);
    json += "{\"id\":";
    json += String(messageId);
    json += ",\"ok\":true,\"data\":";
    json += data;
    json += "}";
    return json;
}

String buildBLEError(uint16_t messageId, const String& code, const String& message) {
    String json;
    json.reserve(code.length() + message.length() + 80);
    json += "{\"id\":";
    json += String(messageId);
    json += ",\"ok\":false,\"error\":{\"code\":\"";
    json += jsonEscape(code);
    json += "\",\"message\":\"";
    json += jsonEscape(message);
    json += "\"}}";
    return json;
}

bool parseJSONBool(cJSON* object, const char* key, bool& present, bool& value, String& error) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (item == nullptr) return true;
    present = true;
    if (cJSON_IsBool(item)) value = cJSON_IsTrue(item);
    else if (cJSON_IsNumber(item)) value = item->valueint != 0;
    else {
        error = String(key) + " 必须是布尔值";
        return false;
    }
    return true;
}

bool parseJSONUInt8(cJSON* object, const char* key, bool& present, uint8_t& value, String& error) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (item == nullptr) return true;
    present = true;
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > 255) {
        error = String(key) + " 必须是有效整数";
        return false;
    }
    value = static_cast<uint8_t>(item->valueint);
    return true;
}

bool parseJSONString(cJSON* object, const char* key, bool& present, String& value, String& error) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (item == nullptr) return true;
    present = true;
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        error = String(key) + " 必须是字符串";
        return false;
    }
    value = item->valuestring;
    return true;
}

bool parseBLEConfigPatch(cJSON* args, ConfigPatch& patch, String& error) {
    if (!cJSON_IsObject(args)) {
        error = "config.set 缺少 args 对象";
        return false;
    }
    return parseJSONBool(args, "fsdEnable", patch.hasFsdEnable, patch.fsdEnable, error)
        && parseJSONUInt8(args, "hwMode", patch.hasHwMode, patch.hwMode, error)
        && parseJSONUInt8(args, "speedProfile", patch.hasSpeedProfile, patch.speedProfile, error)
        && parseJSONBool(args, "profileMode", patch.hasProfileMode, patch.profileModeAuto, error)
        && parseJSONBool(args, "isaChime", patch.hasIsaChime, patch.isaChime, error)
        && parseJSONBool(args, "emergencyDet", patch.hasEmergencyDetection, patch.emergencyDetection, error)
        && parseJSONBool(args, "chinaMode", patch.hasChinaMode, patch.chinaMode, error)
        && parseJSONString(args, "apSSID", patch.hasAPSSID, patch.apSSID, error)
        && parseJSONString(args, "apPass", patch.hasAPPass, patch.apPass, error)
        && parseJSONBool(args, "upstreamEnable", patch.hasUpstreamEnable, patch.upstreamEnable, error)
        && parseJSONBool(args, "dnsWhitelistEnable", patch.hasDNSWhitelistEnable, patch.dnsWhitelistEnable, error)
        && parseJSONString(args, "dnsAllowlist", patch.hasDNSAllowlist, patch.dnsAllowlist, error)
        && parseJSONString(args, "dnsBlocklist", patch.hasDNSBlocklist, patch.dnsBlocklist, error);
}

String dispatchBLECommand(uint16_t frameMessageId, const std::string& payload) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        cJSON_ParseWithLength(payload.c_str(), payload.size()),
        cJSON_Delete
    );
    if (!root || !cJSON_IsObject(root.get())) {
        return buildBLEError(frameMessageId, "invalid-json", "请求不是有效 JSON 对象");
    }

    cJSON* idItem = cJSON_GetObjectItemCaseSensitive(root.get(), "id");
    cJSON* opItem = cJSON_GetObjectItemCaseSensitive(root.get(), "op");
    cJSON* args = cJSON_GetObjectItemCaseSensitive(root.get(), "args");
    if (!cJSON_IsNumber(idItem) || idItem->valueint < 0 || idItem->valueint > 65535) {
        return buildBLEError(frameMessageId, "invalid-id", "请求 id 无效");
    }
    if (static_cast<uint16_t>(idItem->valueint) != frameMessageId) {
        return buildBLEError(frameMessageId, "id-mismatch", "帧消息 ID 与 JSON id 不一致");
    }
    if (!cJSON_IsString(opItem) || opItem->valuestring == nullptr) {
        return buildBLEError(frameMessageId, "invalid-op", "请求缺少 op");
    }

    String op = opItem->valuestring;
    if (!controller_contract::isSupportedBLEOperation(op.c_str())) {
        return buildBLEError(frameMessageId, "unknown-op", "不支持的操作");
    }
    if (op == "status.get") {
        return buildBLESuccess(
            frameMessageId,
            buildStatusJson(controller_contract::StatusAudience::BLE)
        );
    }

    if (op == "config.set") {
        ConfigPatch patch;
        String error;
        if (!parseBLEConfigPatch(args, patch, error) || !applyConfigPatch(patch, error)) {
            return buildBLEError(frameMessageId, "invalid-config", error);
        }
        return buildBLESuccess(
            frameMessageId,
            buildStatusJson(controller_contract::StatusAudience::BLE)
        );
    }

    if (op == "upstream.scan") {
        int statusCode = 200;
        String error;
        String data = buildUpstreamScanJson(statusCode, error);
        if (statusCode != 200) return buildBLEError(frameMessageId, "scan-busy", error);
        return buildBLESuccess(frameMessageId, data);
    }

    if (op == "upstream.save") {
        if (!cJSON_IsObject(args)) return buildBLEError(frameMessageId, "invalid-args", "缺少热点参数");
        cJSON* ssidItem = cJSON_GetObjectItemCaseSensitive(args, "ssid");
        cJSON* passItem = cJSON_GetObjectItemCaseSensitive(args, "pass");
        if (!cJSON_IsString(ssidItem) || ssidItem->valuestring == nullptr) {
            return buildBLEError(frameMessageId, "invalid-ssid", "缺少热点名称");
        }
        if (passItem != nullptr && (!cJSON_IsString(passItem) || passItem->valuestring == nullptr)) {
            return buildBLEError(frameMessageId, "invalid-pass", "热点密码必须是字符串");
        }
        String error;
        String pass = passItem == nullptr ? String() : String(passItem->valuestring);
        if (!saveUpstreamNetwork(ssidItem->valuestring, pass, passItem != nullptr, error)) {
            return buildBLEError(frameMessageId, "save-failed", error);
        }
        return buildBLESuccess(
            frameMessageId,
            buildStatusJson(controller_contract::StatusAudience::BLE)
        );
    }

    if (op == "upstream.delete") {
        cJSON* ssidItem = cJSON_IsObject(args) ? cJSON_GetObjectItemCaseSensitive(args, "ssid") : nullptr;
        if (!cJSON_IsString(ssidItem) || ssidItem->valuestring == nullptr) {
            return buildBLEError(frameMessageId, "invalid-ssid", "缺少热点名称");
        }
        String error;
        if (!deleteUpstreamNetwork(ssidItem->valuestring, error)) {
            return buildBLEError(frameMessageId, "delete-failed", error);
        }
        return buildBLESuccess(
            frameMessageId,
            buildStatusJson(controller_contract::StatusAudience::BLE)
        );
    }

    if (op == "dns.blocked.clear") {
        dnsServer.clearBlockedRequests();
        return buildBLESuccess(
            frameMessageId,
            buildStatusJson(controller_contract::StatusAudience::BLE)
        );
    }

    if (op == "debug.read") {
        if (!debugLogReady) return buildBLEError(frameMessageId, "log-unavailable", "诊断日志不可用");
        String data = "{\"text\":\"";
        data += jsonEscape(readDebugLogText());
        data += "\"}";
        return buildBLESuccess(frameMessageId, data);
    }

    if (op == "debug.clear") {
        clearDebugLogStorage();
        return buildBLESuccess(frameMessageId);
    }

    return buildBLEError(frameMessageId, "unknown-op", "不支持的操作");
}

void sendBLEResponse(uint16_t messageId, String response, bool errorResponse = false) {
    if (response.length() > ble_protocol::kMaxResponseBytes) {
        response = buildBLEError(messageId, "response-too-large", "响应超过 BLE 大小限制");
        errorResponse = true;
    }
    if (!bleClientConnected || bleResponseCharacteristic == nullptr || bleServer == nullptr) return;

    uint16_t mtu = bleServer->getPeerMTU(bleServer->getConnId());
    if (mtu < 23) mtu = 23;
    size_t payloadSize = std::min<size_t>(180, mtu - 3 - ble_protocol::kHeaderSize);
    if (payloadSize == 0) payloadSize = 1;
    size_t offset = 0;
    uint16_t chunkIndex = 0;

    do {
        size_t chunkLength = std::min(payloadSize, response.length() - offset);
        std::vector<uint8_t> frame(ble_protocol::kHeaderSize + chunkLength);
        uint8_t flags = 0;
        if (offset == 0) flags |= ble_protocol::kFlagStart;
        if (offset + chunkLength >= response.length()) flags |= ble_protocol::kFlagEnd;
        if (errorResponse) flags |= ble_protocol::kFlagError;
        ble_protocol::writeHeader(
            frame.data(),
            ble_protocol::FrameHeader{ble_protocol::kVersion, flags, messageId, chunkIndex}
        );
        memcpy(frame.data() + ble_protocol::kHeaderSize, response.c_str() + offset, chunkLength);
        bleResponseCharacteristic->setValue(frame.data(), frame.size());
        bleResponseCharacteristic->indicate();
        offset += chunkLength;
        ++chunkIndex;
    } while (offset < response.length() && bleClientConnected);
}

void writeUInt16LE(uint8_t* output, uint16_t value) {
    output[0] = static_cast<uint8_t>(value & 0xFF);
    output[1] = static_cast<uint8_t>(value >> 8);
}

void sendBLETelemetry() {
    if (!bleClientConnected || bleTelemetryCharacteristic == nullptr) return;
    static uint32_t lastRx = 0;
    static uint32_t lastModified = 0;
    static uint32_t lastErrors = 0;
    static uint8_t sequence = 0;
    uint32_t now = millis();
    bool vehicleSpeedValid = cfg.lastVehicleSpeedMillis != 0
        && now - cfg.lastVehicleSpeedMillis <= VEHICLE_SPEED_STALE_MS;
    bool canActive = cfg.lastRxMillis != 0 && now - cfg.lastRxMillis <= CAN_ACTIVITY_WINDOW_MS;
    uint16_t flags = 0;
    if (cfg.canOK) flags |= 1 << 0;
    if (canActive) flags |= 1 << 1;
    if (cfg.fsdTriggered) flags |= 1 << 2;
    if (cfg.fsdEnable) flags |= 1 << 3;
    if (apStarted) flags |= 1 << 4;
    if (WiFi.status() == WL_CONNECTED) flags |= 1 << 5;
    if (natEnabled) flags |= 1 << 6;
    if (isThermalProtectionActive()) flags |= 1 << 7;
    if (vehicleSpeedValid) flags |= 1 << 8;
    if (cfg.profileModeAuto) flags |= 1 << 9;
    if (dnsCfg.enabled) flags |= 1 << 10;

    uint8_t packet[20] = {};
    packet[0] = ble_protocol::kVersion;
    writeUInt16LE(packet + 1, flags);
    packet[3] = cfg.hwMode;
    packet[4] = cfg.speedProfile;
    writeUInt16LE(packet + 5, cfg.vehicleSpeedCentiKph);
    uint16_t detectedLimit = cfg.detectedSpeedLimitKph;
    packet[7] = static_cast<uint8_t>(std::min<uint16_t>(detectedLimit, 255));
    packet[8] = cfg.appliedSpeedOffsetKph;
    int16_t temp = std::isfinite(thermalStatus.currentC)
        ? static_cast<int16_t>(thermalStatus.currentC * 10.0f)
        : INT16_MIN;
    writeUInt16LE(packet + 9, static_cast<uint16_t>(temp));
    packet[11] = static_cast<uint8_t>(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127);
    packet[12] = static_cast<uint8_t>(std::min<int32_t>(WiFi.softAPgetStationNum(), 255));
    writeUInt16LE(packet + 13, static_cast<uint16_t>(std::min<uint32_t>(cfg.rxCount - lastRx, 65535)));
    writeUInt16LE(packet + 15, static_cast<uint16_t>(std::min<uint32_t>(cfg.modifiedCount - lastModified, 65535)));
    packet[17] = static_cast<uint8_t>(std::min<uint32_t>(cfg.errorCount - lastErrors, 255));
    packet[18] = sequence++;
    lastRx = cfg.rxCount;
    lastModified = cfg.modifiedCount;
    lastErrors = cfg.errorCount;

    bleTelemetryCharacteristic->setValue(packet, sizeof(packet));
    bleTelemetryCharacteristic->notify();
}

class ControllerBLESecurityCallbacks : public BLESecurityCallbacks {
public:
    uint32_t onPassKeyRequest() override { return deriveBLEPasskey(); }
    void onPassKeyNotify(uint32_t) override {}
    bool onSecurityRequest() override { return true; }
    void onAuthenticationComplete(esp_ble_auth_cmpl_t result) override {
        ESP_LOGI(TAG, "ble auth %s reason=%u", result.success ? "ok" : "failed", result.fail_reason);
    }
    bool onConfirmPIN(uint32_t) override { return true; }
};

class ControllerBLEServerCallbacks : public BLEServerCallbacks {
public:
    void onConnect(BLEServer*) override {
        ++bleConnectionGeneration;
        bleClientConnected = true;
        bleRequestAssembler.reset();
        ESP_LOGI(TAG, "ble client connected");
    }
    void onDisconnect(BLEServer* server) override {
        ++bleConnectionGeneration;
        bleClientConnected = false;
        bleRequestBusy = false;
        bleRequestAssembler.reset();
        server->startAdvertising();
        ESP_LOGI(TAG, "ble client disconnected");
    }
};

bool enqueueBLECommand(BLECommand* command) {
    if (command == nullptr || bleCommandQueue == nullptr
        || xQueueSend(bleCommandQueue, &command, 0) != pdTRUE) {
        delete command;
        return false;
    }
    return true;
}

class ControllerBLECommandCallbacks : public BLECharacteristicCallbacks {
public:
    void onWrite(BLECharacteristic* characteristic) override {
        std::string value = characteristic->getValue();
        if (value.empty()) return;

        ble_protocol::FrameHeader header;
        ble_protocol::readHeader(
            reinterpret_cast<const uint8_t*>(value.data()),
            value.size(),
            header
        );
        std::string framingError;
        auto result = bleRequestAssembler.push(
            reinterpret_cast<const uint8_t*>(value.data()),
            value.size(),
            millis(),
            ble_protocol::kMaxRequestBytes,
            framingError
        );
        if (result == ble_protocol::AssembleResult::Partial) return;
        if (bleRequestBusy) {
            auto* command = new (std::nothrow) BLECommand();
            if (command != nullptr) {
                command->messageId = header.messageId;
                command->connectionGeneration = bleConnectionGeneration;
                command->errorCode = "request-busy";
                command->framingError = "已有请求正在执行";
                enqueueBLECommand(command);
            }
            bleRequestAssembler.reset();
            return;
        }

        auto* command = new (std::nothrow) BLECommand();
        if (command == nullptr) return;
        command->messageId = header.messageId;
        command->connectionGeneration = bleConnectionGeneration;
        if (result == ble_protocol::AssembleResult::Error) {
            command->framingError = framingError;
        } else {
            command->payload.assign(
                bleRequestAssembler.payload().begin(),
                bleRequestAssembler.payload().end()
            );
        }
        bleRequestAssembler.reset();
        bleRequestBusy = enqueueBLECommand(command);
    }
};

void bleControlTask(void*) {
    uint32_t lastTelemetryMillis = 0;
    for (;;) {
        BLECommand* command = nullptr;
        if (xQueueReceive(bleCommandQueue, &command, pdMS_TO_TICKS(50)) == pdTRUE && command != nullptr) {
            String response;
            bool isError = !command->framingError.empty();
            if (isError) {
                response = buildBLEError(
                    command->messageId,
                    command->errorCode.empty() ? "invalid-frame" : command->errorCode.c_str(),
                    command->framingError.c_str()
                );
            } else {
                response = dispatchBLECommand(command->messageId, command->payload);
                isError = response.indexOf("\"ok\":false") >= 0;
            }
            bool currentConnection = command->connectionGeneration == bleConnectionGeneration;
            if (currentConnection) sendBLEResponse(command->messageId, response, isError);
            delete command;
            if (currentConnection) {
                bleRequestBusy = uxQueueMessagesWaiting(bleCommandQueue) > 0;
            }
        }

        uint32_t now = millis();
        if (bleClientConnected && now - lastTelemetryMillis >= BLE_TELEMETRY_INTERVAL_MS) {
            lastTelemetryMillis = now;
            sendBLETelemetry();
        }
    }
}

void setupBLEControl() {
    uint64_t chipId = ESP.getEfuseMac();
    char deviceName[32];
    snprintf(deviceName, sizeof(deviceName), "FSD-Controller-%04X", static_cast<unsigned>(chipId & 0xFFFF));

    BLEDevice::init(deviceName);
    BLEDevice::setMTU(185);
    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);
    BLEDevice::setSecurityCallbacks(new ControllerBLESecurityCallbacks());
    bleSecurity = new BLESecurity();
    updateBLEPasskey();
    bleSecurity->setCapability(ESP_IO_CAP_OUT);
    bleSecurity->setKeySize(16);
    bleSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    bleSecurity->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

    bleServer = BLEDevice::createServer();
    bleServer->setCallbacks(new ControllerBLEServerCallbacks());
    BLEService* service = bleServer->createService(BLE_SERVICE_UUID);
    BLECharacteristic* command = service->createCharacteristic(
        BLE_COMMAND_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    command->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
    command->setCallbacks(new ControllerBLECommandCallbacks());

    bleResponseCharacteristic = service->createCharacteristic(
        BLE_RESPONSE_UUID,
        BLECharacteristic::PROPERTY_INDICATE
    );
    bleResponseCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);
    auto* responseDescriptor = new BLE2902();
    responseDescriptor->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);
    bleResponseCharacteristic->addDescriptor(responseDescriptor);

    bleTelemetryCharacteristic = service->createCharacteristic(
        BLE_TELEMETRY_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    bleTelemetryCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);
    auto* telemetryDescriptor = new BLE2902();
    telemetryDescriptor->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);
    bleTelemetryCharacteristic->addDescriptor(telemetryDescriptor);

    service->start();
    BLEAdvertising* advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(BLE_SERVICE_UUID);
    advertising->setScanResponse(true);
    advertising->setMinInterval(0xA0);
    advertising->setMaxInterval(0xF0);
    advertising->setMinPreferred(0x18);
    advertising->setMaxPreferred(0x30);
    BLEDevice::startAdvertising();
    ESP_LOGI(TAG, "ble control ready name=%s", deviceName);
}

// ═══════════════════════════════════════════
//  Web Server Setup (runs on Core 0)
// ═══════════════════════════════════════════

void setupWebServer() {
    // Serve UI
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(
            200,
            "text/html; charset=utf-8",
            reinterpret_cast<const uint8_t*>(INDEX_HTML),
            sizeof(INDEX_HTML) - 1
        );
    });

    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildStatusJson());
    });

    server.on("/api/ota/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildOTAStatusJson());
    });

    server.on("/api/debug/log", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!debugLogReady) {
            req->send(503, "text/plain; charset=utf-8", "debug log storage unavailable");
            return;
        }
        req->send(200, "text/plain; charset=utf-8", readDebugLogText());
    });

    server.on("/api/debug/log/clear", HTTP_GET, [](AsyncWebServerRequest* req) {
        clearDebugLogStorage();
        req->send(200, "text/plain; charset=utf-8", "OK");
    });

    server.on("/api/dns/blocked/clear", HTTP_GET, [](AsyncWebServerRequest* req) {
        dnsServer.clearBlockedRequests();
        req->send(200, "text/plain", "OK");
    });

    server.on("/api/upstream/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        int statusCode = 200;
        String error;
        String json = buildUpstreamScanJson(statusCode, error);
        if (statusCode != 200) req->send(statusCode, "text/plain", error);
        else req->send(200, "application/json", json);
    });

    server.on("/api/upstream/add", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("ssid")) {
            req->send(400, "text/plain", "缺少热点名称");
            return;
        }

        String ssid = req->getParam("ssid")->value();
        String pass = req->hasParam("pass") ? req->getParam("pass")->value() : "";
        bool overwritePass = req->hasParam("pass");
        String error;
        if (!saveUpstreamNetwork(ssid, pass, overwritePass, error)) {
            req->send(400, "text/plain", error);
            return;
        }
        req->send(200, "text/plain", "OK");
    });

    server.on("/api/upstream/delete", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("ssid")) {
            req->send(400, "text/plain", "缺少热点名称");
            return;
        }

        String error;
        if (!deleteUpstreamNetwork(req->getParam("ssid")->value(), error)) {
            req->send(error == "热点不存在" ? 404 : 400, "text/plain", error);
            return;
        }
        req->send(200, "text/plain", "OK");
    });

    server.on("/api/set", HTTP_GET, [](AsyncWebServerRequest* req) {
        ConfigPatch patch;
        if (req->hasParam("fsdEnable")) {
            patch.hasFsdEnable = true;
            patch.fsdEnable = req->getParam("fsdEnable")->value().toInt() != 0;
        }
        if (req->hasParam("hwMode")) {
            patch.hasHwMode = true;
            patch.hwMode = req->getParam("hwMode")->value().toInt();
        }
        if (req->hasParam("speedProfile")) {
            patch.hasSpeedProfile = true;
            patch.speedProfile = req->getParam("speedProfile")->value().toInt();
        }
        if (req->hasParam("profileMode")) {
            patch.hasProfileMode = true;
            patch.profileModeAuto = req->getParam("profileMode")->value().toInt() != 0;
        }
        if (req->hasParam("isaChime")) {
            patch.hasIsaChime = true;
            patch.isaChime = req->getParam("isaChime")->value().toInt() != 0;
        }
        if (req->hasParam("emergencyDet")) {
            patch.hasEmergencyDetection = true;
            patch.emergencyDetection = req->getParam("emergencyDet")->value().toInt() != 0;
        }
        if (req->hasParam("chinaMode")) {
            patch.hasChinaMode = true;
            patch.chinaMode = req->getParam("chinaMode")->value().toInt() != 0;
        }
        if (req->hasParam("apSSID")) {
            patch.hasAPSSID = true;
            patch.apSSID = req->getParam("apSSID")->value();
        }
        if (req->hasParam("apPass")) {
            patch.hasAPPass = true;
            patch.apPass = req->getParam("apPass")->value();
        }
        if (req->hasParam("upstreamEnable")) {
            patch.hasUpstreamEnable = true;
            patch.upstreamEnable = req->getParam("upstreamEnable")->value().toInt() != 0;
        }
        if (req->hasParam("dnsWhitelistEnable")) {
            patch.hasDNSWhitelistEnable = true;
            patch.dnsWhitelistEnable = req->getParam("dnsWhitelistEnable")->value().toInt() != 0;
        }
        if (req->hasParam("dnsAllowlist")) {
            patch.hasDNSAllowlist = true;
            patch.dnsAllowlist = req->getParam("dnsAllowlist")->value();
        }
        if (req->hasParam("dnsBlocklist")) {
            patch.hasDNSBlocklist = true;
            patch.dnsBlocklist = req->getParam("dnsBlocklist")->value();
        }
        String error;
        if (!applyConfigPatch(patch, error)) {
            req->send(400, "text/plain", error);
            return;
        }
        req->send(200, "text/plain", "OK");
    });

    server.on("/api/ota", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            bool ok = !Update.hasError() && otaStatus.phase == OTAStatusPhase::SuccessRebooting;
            req->send(ok ? 200 : 500, "application/json", buildOTAResponseJson(ok));
            if (ok) otaPendingRestart = true;
        },
        [](AsyncWebServerRequest* req, const String& filename,
           size_t index, uint8_t* data, size_t len, bool final) {
            size_t totalBytes = req->contentLength();

            if (index == 0) {
                if (Update.isRunning()) Update.abort();
                Update.clearError();
                otaPendingRestart = false;
                setOTAStatus(
                    OTAStatusPhase::Uploading,
                    filename,
                    0,
                    totalBytes,
                    UPDATE_ERROR_OK,
                    "正在接收固件...",
                    false
                );
                Serial.printf("OTA start: %s\n", filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    uint8_t errorCode = Update.getError();
                    String errorMessage = buildOTAErrorMessage("无法开始 OTA", errorCode);
                    Update.printError(Serial);
                    setOTAStatus(
                        OTAStatusPhase::FailedBegin,
                        filename,
                        0,
                        totalBytes,
                        errorCode,
                        errorMessage,
                        false
                    );
                    return;
                }
            }

            if (Update.isRunning()) {
                size_t written = Update.write(data, len);
                size_t bytesReceived = index + written;
                if (written != len) {
                    uint8_t errorCode = Update.getError();
                    String errorMessage = buildOTAErrorMessage("固件写入失败", errorCode);
                    Update.printError(Serial);
                    setOTAStatus(
                        OTAStatusPhase::FailedWrite,
                        filename,
                        bytesReceived,
                        totalBytes,
                        errorCode,
                        errorMessage,
                        false
                    );
                    return;
                }

                setOTAStatus(
                    final ? OTAStatusPhase::Finishing : OTAStatusPhase::Uploading,
                    filename,
                    index + len,
                    totalBytes,
                    UPDATE_ERROR_OK,
                    final ? "固件已接收，正在完成校验..." : "正在接收固件...",
                    false
                );
            }

            if (final) {
                if (!Update.isRunning()) return;

                if (Update.end(true)) {
                    Serial.printf("OTA done: %u bytes\n", (unsigned)(index + len));
                    setOTAStatus(
                        OTAStatusPhase::SuccessRebooting,
                        filename,
                        index + len,
                        totalBytes,
                        UPDATE_ERROR_OK,
                        "固件写入完成，设备准备重启。",
                        true
                    );
                } else {
                    uint8_t errorCode = Update.getError();
                    String errorMessage = buildOTAErrorMessage("固件校验失败", errorCode);
                    Update.printError(Serial);
                    setOTAStatus(
                        OTAStatusPhase::FailedEnd,
                        filename,
                        index + len,
                        totalBytes,
                        errorCode,
                        errorMessage,
                        false
                    );
                }
            }
        }
    );

    server.begin();
    ESP_LOGI(TAG, "web server started root=/ status=/api/status uiBytes=%u", static_cast<unsigned>(sizeof(INDEX_HTML) - 1));
    Serial.println("Web server started");
}

// ═══════════════════════════════════════════
//  CAN Task (pinned to Core 1)
// ═══════════════════════════════════════════

void canTask(void* param) {
    CanFrame frame;
    for (;;) {
        bool activity = false;
        while (canDriver.read(frame)) {
            cfg.canOK = true;
            activity = true;
            handleMessage(frame, canDriver);
        }
        if (activity) cfg.lastRxMillis = millis();
        // LED: on during activity, off when idle
        digitalWrite(PIN_LED, activity ? HIGH : LOW);
        // Yield to avoid starving watchdog
        vTaskDelay(1);
    }
}

void dnsTask(void* param) {
    uint32_t lastPolicyMillis = 0;
    for (;;) {
        bool upstreamReady = WiFi.status() == WL_CONNECTED;
        dnsServer.processNextRequest(dnsCfg, upstreamReady);

        uint32_t now = millis();
        if (lastPolicyMillis == 0 || now - lastPolicyMillis >= 250) {
            lastPolicyMillis = now;
            dnsIpPolicyService(
                dnsCfg.allowlist,
                dnsCfg.blocklist,
                dnsCfg.enabled ? 1 : 0,
                upstreamReady ? 1 : 0
            );
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void diagnosticTask(void* param) {
    for (;;) {
        logRuntimeHeartbeat();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

// ═══════════════════════════════════════════
//  Arduino setup / loop
// ═══════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== FSD Controller ===");
    bootResetReason = esp_reset_reason();
    ESP_LOGI(TAG, "boot start reset=%s", getResetReasonName(bootResetReason));

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    debugLogMutex = xSemaphoreCreateMutex();
    configMutex = xSemaphoreCreateMutex();
    bleCommandQueue = xQueueCreate(2, sizeof(BLECommand*));
    loadConfig();
    debugLogReady = debugLogMutex != nullptr;
    if (debugLogReady) {
        String bootLine;
        bootLine.reserve(48);
        bootLine += "logger ready reset=";
        bootLine += getResetReasonName(bootResetReason);
        appendDebugLogRecord("BOOT", bootLine);
    }
    ESP_LOGI(
        TAG,
        "config loaded hw=%u profile=%u china=%d apSSID=%s upstreamEnabled=%d upstreamSaved=%u",
        static_cast<unsigned>(cfg.hwMode),
        static_cast<unsigned>(cfg.speedProfile),
        static_cast<int>(cfg.chinaMode),
        apCfg.ssid,
        static_cast<int>(wifiCfg.enabled),
        static_cast<unsigned>(wifiCfg.networkCount)
    );
    if (debugLogReady) {
        String bootConfig;
        bootConfig.reserve(96);
        bootConfig += "hw=";
        bootConfig += String(static_cast<unsigned>(cfg.hwMode));
        bootConfig += " profile=";
        bootConfig += String(static_cast<unsigned>(cfg.speedProfile));
        bootConfig += " china=";
        bootConfig += String(static_cast<int>(cfg.chinaMode));
        bootConfig += " fsdEn=";
        bootConfig += String(static_cast<int>(cfg.fsdEnable));
        appendDebugLogRecord("CONFIG", bootConfig);
    }
    Serial.printf("Config loaded: HW=%d, Profile=%d\n", cfg.hwMode, cfg.speedProfile);

    cfg.uptimeStart = millis();

    if (canDriver.init()) {
        cfg.canOK = true;
        ESP_LOGI(TAG, "TWAI ready bitrate=500k txPin=%d rxPin=%d", static_cast<int>(TWAI_TX_PIN), static_cast<int>(TWAI_RX_PIN));
        if (debugLogReady) appendDebugLogRecord("CAN", "twai init ok");
        Serial.println("ESP32 TWAI ready @ 500k");
    } else {
        cfg.canOK = false;
        ESP_LOGE(TAG, "TWAI init failed txPin=%d rxPin=%d", static_cast<int>(TWAI_TX_PIN), static_cast<int>(TWAI_RX_PIN));
        if (debugLogReady) appendDebugLogRecord("CAN", "twai init failed");
        Serial.println("CAN init failed!");
    }

    WiFi.onEvent(logWiFiEvent);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_AP_STA);
    ESP_LOGI(TAG, "wifi mode set to AP+STA");
    applyLocalAPConfig(true);
    requestUpstreamApply();
    dnsServer.begin();
    ESP_LOGI(TAG, "dns server started port=53");

    setupWebServer();
    if (bleCommandQueue == nullptr) {
        ESP_LOGE(TAG, "ble command queue unavailable");
    }
    xTaskCreatePinnedToCore(dnsTask, "DNS", 8192, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(diagnosticTask, "DIAG", 6144, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(canTask, "CAN", 8192, NULL, 2, NULL, 1);
}

void loop() {
    if (otaPendingRestart) {
        delay(OTA_RESTART_DELAY_MS);  // let response finish sending
        ESP.restart();
    }

    serviceLocalAP();
    serviceThermalStatus();
    serviceUpstreamWiFi();
    syncNATState();
    if (!bleControlStarted
        && bleCommandQueue != nullptr
        && (!wifiCfg.enabled
            || !hasUpstreamCredentials()
            || WiFi.status() == WL_CONNECTED
            || millis() >= UPSTREAM_CONNECT_TIMEOUT_MS)) {
        WiFi.setSleep(true);
        setupBLEControl();
        xTaskCreatePinnedToCore(bleControlTask, "BLE", 12288, NULL, 1, NULL, 0);
        bleControlStarted = true;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
}
