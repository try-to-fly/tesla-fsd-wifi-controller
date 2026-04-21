/*
 * Tesla Open CAN Mod — ESP32 Web Edition
 * Core 0: WiFi AP+STA + AsyncWebServer + OTA
 * Core 1: CAN bus read/modify/write (TWAI)
 *
 * GPLv3 — Based on tesla-open-can-mod
 */

#include <Arduino.h>
#include <cctype>
#include <cmath>
#include <cstring>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <esp_log.h>
#include <freertos/semphr.h>

#include "lwip/lwip_napt.h"

#include "can_frame_types.h"
#include "debug_log.h"
#include "dns_whitelist.h"
#include "dns_ip_blocker.h"
#include "drivers/twai_driver.h"
#include "handlers.h"
#include "web_ui.h"

// ── WiFi AP config ──
static const char* DEFAULT_AP_SSID = "FSD-Controller";
static const char* DEFAULT_AP_PASS = "12345678";   // min 8 chars
static const IPAddress AP_IP(9, 9, 9, 9);
static const IPAddress AP_GATEWAY(9, 9, 9, 9);
static const IPAddress AP_SUBNET(255, 255, 255, 0);
static constexpr uint32_t AP_CONFIG_APPLY_DELAY_MS = 800;
static constexpr uint32_t UPSTREAM_RETRY_MS = 15000;
static constexpr uint32_t UPSTREAM_FAILURE_RETRY_MS = 3000;
static constexpr uint32_t UPSTREAM_RETRY_THROTTLED_MS = 60000;
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
static constexpr uint32_t DEBUG_HEARTBEAT_PERSIST_MS = 10000;
static constexpr size_t DEBUG_LOG_MAX_BYTES = 48 * 1024;
static constexpr size_t DEBUG_LOG_RETAIN_BYTES = 24 * 1024;
static constexpr const char* DEBUG_LOG_PATH = "/debug.log";
static constexpr uint32_t OTA_STATUS_STALE_MS = 8000;
static constexpr uint32_t OTA_RESTART_DELAY_MS = 1500;

// ── Globals ──
static TWAIDriver     canDriver;
static AsyncWebServer server(80);
static Preferences    prefs;
static DNSWhitelistServer dnsServer;
static volatile bool  otaPendingRestart = false;
static bool           natEnabled = false;

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

static UpstreamWiFiConfig wifiCfg;
static volatile bool upstreamScanInProgress = false;
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

const char* getTwaiStateName(twai_state_t state) {
    switch (state) {
        case TWAI_STATE_STOPPED:    return "STOPPED";
        case TWAI_STATE_RUNNING:    return "RUNNING";
        case TWAI_STATE_BUS_OFF:    return "BUS_OFF";
        case TWAI_STATE_RECOVERING: return "RECOVERING";
        default:                    return "UNKNOWN";
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

void trimDebugLogLocked() {
    File file = SPIFFS.open(DEBUG_LOG_PATH, FILE_READ);
    if (!file) return;

    size_t size = file.size();
    if (size <= DEBUG_LOG_MAX_BYTES) {
        file.close();
        return;
    }

    size_t start = size > DEBUG_LOG_RETAIN_BYTES ? size - DEBUG_LOG_RETAIN_BYTES : 0;
    if (start > 0) {
        file.seek(start);
        while (file.available()) {
            if (file.read() == '\n') break;
        }
    }

    String tail;
    tail.reserve(DEBUG_LOG_RETAIN_BYTES + 64);
    while (file.available()) {
        tail += static_cast<char>(file.read());
    }
    file.close();

    File out = SPIFFS.open(DEBUG_LOG_PATH, FILE_WRITE);
    if (!out) return;
    out.print(tail);
    out.close();
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

    File file = SPIFFS.open(DEBUG_LOG_PATH, FILE_APPEND);
    if (file) {
        file.print(line);
        file.close();
        trimDebugLogLocked();
    }

    unlockDebugLog();
}

size_t getDebugLogSizeBytes() {
    if (!debugLogReady || !lockDebugLog()) return 0;

    size_t size = 0;
    File file = SPIFFS.open(DEBUG_LOG_PATH, FILE_READ);
    if (file) {
        size = file.size();
        file.close();
    }

    unlockDebugLog();
    return size;
}

String readDebugLogText() {
    if (!debugLogReady || !lockDebugLog()) return "";

    String content;
    File file = SPIFFS.open(DEBUG_LOG_PATH, FILE_READ);
    if (file) {
        size_t size = file.size();
        content.reserve(size + 1);
        while (file.available()) {
            content += static_cast<char>(file.read());
        }
        file.close();
    }

    unlockDebugLog();
    return content;
}

void clearDebugLogStorage() {
    if (!debugLogReady || !lockDebugLog()) return;
    SPIFFS.remove(DEBUG_LOG_PATH);
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

void logWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_AP_START:
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
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            ESP_LOGW(
                TAG,
                "wifi STA disconnected reason=%d",
                static_cast<int>(info.wifi_sta_disconnected.reason)
            );
            break;
        default:
            ESP_LOGI(TAG, "wifi event=%s", WiFi.eventName(event));
            break;
    }
}

void logRuntimeHeartbeat() {
    static uint32_t lastBeat = 0;
    static uint32_t lastPersistBeat = 0;
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
        "beat rx=%lu mod=%lu err=%lu fsdTrig=%d fsdEn=%d hw=%u china=%d apClients=%u apIP=%s sta=%d staSSID=%s staIP=%s twai=%s rxErr=%lu txErr=%lu busErr=%lu rxMiss=%lu txFail=%lu",
        static_cast<unsigned long>(cfg.rxCount),
        static_cast<unsigned long>(cfg.modifiedCount),
        static_cast<unsigned long>(cfg.errorCount),
        static_cast<int>(cfg.fsdTriggered),
        static_cast<int>(cfg.fsdEnable),
        static_cast<unsigned>(cfg.hwMode),
        static_cast<int>(cfg.chinaMode),
        static_cast<unsigned>(WiFi.softAPgetStationNum()),
        WiFi.softAPIP().toString().c_str(),
        static_cast<int>(staStatus),
        staSSID.isEmpty() ? "-" : staSSID.c_str(),
        staIP.c_str(),
        getTwaiStateName(twaiStatus.state),
        static_cast<unsigned long>(twaiStatus.rx_error_counter),
        static_cast<unsigned long>(twaiStatus.tx_error_counter),
        static_cast<unsigned long>(twaiStatus.bus_error_count),
        static_cast<unsigned long>(twaiStatus.rx_missed_count),
        static_cast<unsigned long>(twaiStatus.tx_failed_count)
    );

    if (debugLogReady && (lastPersistBeat == 0 || (now - lastPersistBeat) >= DEBUG_HEARTBEAT_PERSIST_MS)) {
        lastPersistBeat = now;
        String body;
        body.reserve(160);
        body += "rx=";
        body += String(static_cast<unsigned long>(cfg.rxCount));
        body += " mod=";
        body += String(static_cast<unsigned long>(cfg.modifiedCount));
        body += " err=";
        body += String(static_cast<unsigned long>(cfg.errorCount));
        body += " fsdTrig=";
        body += String(static_cast<int>(cfg.fsdTriggered));
        body += " fsdEn=";
        body += String(static_cast<int>(cfg.fsdEnable));
        body += " hw=";
        body += String(static_cast<unsigned>(cfg.hwMode));
        body += " twai=";
        body += getTwaiStateName(twaiStatus.state);
        body += " rxErr=";
        body += String(static_cast<unsigned long>(twaiStatus.rx_error_counter));
        body += " txErr=";
        body += String(static_cast<unsigned long>(twaiStatus.tx_error_counter));
        body += " busErr=";
        body += String(static_cast<unsigned long>(twaiStatus.bus_error_count));
        body += " rxMiss=";
        body += String(static_cast<unsigned long>(twaiStatus.rx_missed_count));
        body += " txFail=";
        body += String(static_cast<unsigned long>(twaiStatus.tx_failed_count));
        appendDebugLogRecord("HEARTBEAT", body);
    }
}

int performUpstreamScan(UpstreamScanResult* results, size_t maxResults) {
    if (upstreamScanInProgress) return -1;
    upstreamScanInProgress = true;

    WiFi.scanDelete();
    int foundCount = WiFi.scanNetworks(false, true);
    int uniqueCount = 0;

    if (foundCount > 0) {
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
    }

    WiFi.scanDelete();
    upstreamScanInProgress = false;
    return uniqueCount;
}

int selectNextUpstreamNetworkIndex() {
    if (!hasUpstreamCredentials()) return -1;

    UpstreamScanResult results[MAX_SCAN_RESULTS];
    int scanCount = performUpstreamScan(results, MAX_SCAN_RESULTS);

    if (scanCount > 0) {
        for (int i = 0; i < scanCount; ++i) {
            int index = findSavedUpstreamNetwork(String(results[i].ssid));
            if (index >= 0) return index;
        }
    }

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

    switch (WiFi.status()) {
        case WL_CONNECTED:       return "已连接";
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

bool applyLocalAPConfig() {
    WiFi.softAPdisconnect(false);
    delay(100);
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    bool ok = WiFi.softAP(apCfg.ssid, apCfg.pass);
    if (ok) {
        ESP_LOGI(TAG, "local AP ready ssid=%s ip=%s", apCfg.ssid, WiFi.softAPIP().toString().c_str());
        Serial.printf("WiFi AP: %s  IP: %s\n", apCfg.ssid, WiFi.softAPIP().toString().c_str());
    } else {
        ESP_LOGE(TAG, "local AP start failed ssid=%s", apCfg.ssid);
        Serial.printf("WiFi AP restart failed for SSID: %s\n", apCfg.ssid);
    }
    return ok;
}

void startUpstreamConnect() {
    if (!wifiCfg.enabled || !hasUpstreamCredentials()) return;

    int networkIndex = selectNextUpstreamNetworkIndex();
    if (networkIndex < 0 || networkIndex >= wifiCfg.networkCount) {
        wifiCfg.activeIndex = -1;
        wifiCfg.lastAttemptMillis = millis();
        ESP_LOGW(TAG, "no upstream hotspot available");
        Serial.println("No upstream hotspot available");
        return;
    }

    wifiCfg.activeIndex = networkIndex;
    wifiCfg.nextTryIndex = (networkIndex + 1) % wifiCfg.networkCount;

    const SavedUpstreamNetwork& network = wifiCfg.networks[networkIndex];
    ESP_LOGI(TAG, "connecting upstream WiFi ssid=%s", network.ssid);
    Serial.printf("Connecting upstream WiFi: %s\n", network.ssid);
    WiFi.disconnect(false, false);
    if (network.pass[0] != '\0') {
        WiFi.begin(network.ssid, network.pass);
    } else {
        WiFi.begin(network.ssid);
    }
    wifiCfg.lastAttemptMillis = millis();
}

void applyUpstreamWiFiConfig() {
    if (isThermalProtectionActive()) {
        WiFi.disconnect(false, true);
        wifiCfg.activeIndex = -1;
        wifiCfg.nextTryIndex = 0;
        wifiCfg.lastAttemptMillis = 0;
        ESP_LOGW(TAG, "upstream WiFi paused by thermal protection");
        Serial.println("Upstream WiFi paused by thermal protection");
        return;
    }

    if (!wifiCfg.enabled || !hasUpstreamCredentials()) {
        WiFi.disconnect(false, true);
        wifiCfg.activeIndex = -1;
        wifiCfg.nextTryIndex = 0;
        wifiCfg.lastAttemptMillis = 0;
        ESP_LOGI(TAG, "upstream WiFi disabled enabled=%d saved=%u", static_cast<int>(wifiCfg.enabled), static_cast<unsigned>(wifiCfg.networkCount));
        Serial.println("Upstream WiFi disabled");
        return;
    }

    WiFi.disconnect(false, true);
    wifiCfg.activeIndex = -1;
    wifiCfg.nextTryIndex = 0;
    startUpstreamConnect();
}

void serviceUpstreamWiFi() {
    if (wifiCfg.applyRequested) {
        wifiCfg.applyRequested = false;
        applyUpstreamWiFiConfig();
        return;
    }

    if (!wifiCfg.enabled || !hasUpstreamCredentials()) return;
    if (isThermalProtectionActive()) {
        if (WiFi.status() == WL_CONNECTED) {
            WiFi.disconnect(false, true);
            wifiCfg.activeIndex = -1;
            wifiCfg.lastAttemptMillis = 0;
            ESP_LOGW(TAG, "upstream WiFi disconnected due to thermal protection");
            Serial.println("Upstream WiFi disconnected due to thermal protection");
        }
        return;
    }
    if (WiFi.status() == WL_CONNECTED) {
        int connectedIndex = findSavedUpstreamNetwork(WiFi.SSID());
        if (connectedIndex >= 0) {
            wifiCfg.activeIndex = connectedIndex;
        }
        return;
    }

    uint32_t now = millis();
    wl_status_t status = WiFi.status();
    bool shouldRetry = wifiCfg.lastAttemptMillis == 0;

    if (!shouldRetry) {
        if (status == WL_NO_SSID_AVAIL || status == WL_CONNECT_FAILED || status == WL_CONNECTION_LOST) {
            shouldRetry = now - wifiCfg.lastAttemptMillis >= UPSTREAM_FAILURE_RETRY_MS;
        } else {
            shouldRetry = now - wifiCfg.lastAttemptMillis >= UPSTREAM_RETRY_MS;
        }
        if (!shouldRetry && isThermalThrottleActive()) {
            shouldRetry = now - wifiCfg.lastAttemptMillis >= UPSTREAM_RETRY_THROTTLED_MS;
        }
    } else if (isThermalThrottleActive()) {
        shouldRetry = now - wifiCfg.lastAttemptMillis >= UPSTREAM_RETRY_THROTTLED_MS;
    }

    if (shouldRetry) {
        startUpstreamConnect();
    }
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

String buildStatusJson() {
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
    size_t debugLogBytes = getDebugLogSizeBytes();
    String json;

    json.reserve(7400);
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
    json += ",\"apPassword\":\"";
    json += apPass;
    json += "\"";
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
    json += ",\"natEnabled\":";
    json += String(natEnabled ? 1 : 0);
    json += ",\"natStatus\":\"";
    json += natStatus;
    json += "\"";
    json += "}";
    return json;
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
        UpstreamScanResult results[MAX_SCAN_RESULTS];
        int resultCount = performUpstreamScan(results, MAX_SCAN_RESULTS);

        if (resultCount < 0) {
            req->send(409, "text/plain", "热点搜索忙，请稍后再试");
            return;
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
        req->send(200, "application/json", json);
    });

    server.on("/api/upstream/add", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("ssid")) {
            req->send(400, "text/plain", "缺少热点名称");
            return;
        }

        String ssid = req->getParam("ssid")->value();
        String pass = req->hasParam("pass") ? req->getParam("pass")->value() : "";
        ssid.trim();

        if (ssid.isEmpty()) {
            req->send(400, "text/plain", "热点名称不能为空");
            return;
        }
        if (ssid.length() > 32 || pass.length() > 63) {
            req->send(400, "text/plain", "热点名称或密码长度不合法");
            return;
        }
        if (isReservedUpstreamSSID(ssid)) {
            req->send(400, "text/plain", "不能保存本机发射的热点");
            return;
        }
        if (findSavedUpstreamNetwork(ssid) < 0 && wifiCfg.networkCount >= MAX_UPSTREAM_NETWORKS) {
            req->send(400, "text/plain", "已达到可保存热点上限");
            return;
        }

        bool overwritePass = req->hasParam("pass");
        if (!addOrUpdateSavedUpstreamNetwork(ssid, pass, overwritePass)) {
            req->send(400, "text/plain", "保存热点失败");
            return;
        }

        saveConfig();
        requestUpstreamApply();
        req->send(200, "text/plain", "OK");
    });

    server.on("/api/upstream/delete", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("ssid")) {
            req->send(400, "text/plain", "缺少热点名称");
            return;
        }

        String ssid = req->getParam("ssid")->value();
        ssid.trim();
        if (!removeSavedUpstreamNetwork(ssid)) {
            req->send(404, "text/plain", "热点不存在");
            return;
        }

        saveConfig();
        requestUpstreamApply();
        req->send(200, "text/plain", "OK");
    });

    server.on("/api/set", HTTP_GET, [](AsyncWebServerRequest* req) {
        bool changed = false;
        bool wifiChanged = false;
        bool apChanged = false;

        if (req->hasParam("fsdEnable")) {
            cfg.fsdEnable = req->getParam("fsdEnable")->value().toInt() != 0;
            if (!cfg.fsdEnable) cfg.appliedSpeedOffsetKph = 0;
            changed = true;
        }
        if (req->hasParam("hwMode")) {
            uint8_t v = req->getParam("hwMode")->value().toInt();
            if (v <= 2) {
                if (v != cfg.hwMode && (v == 1 || cfg.hwMode == 1)) {
                    resetHW3SpeedLimitState();
                }
                cfg.hwMode = v;
                changed = true;
            }
        }
        if (req->hasParam("speedProfile")) {
            uint8_t v = req->getParam("speedProfile")->value().toInt();
            if (v <= 4) { cfg.speedProfile = v; changed = true; }
        }
        if (req->hasParam("profileMode")) {
            cfg.profileModeAuto = req->getParam("profileMode")->value().toInt() != 0;
            changed = true;
        }
        if (req->hasParam("isaChime")) {
            cfg.isaChimeSuppress = req->getParam("isaChime")->value().toInt() != 0;
            changed = true;
        }
        if (req->hasParam("emergencyDet")) {
            cfg.emergencyDetection = req->getParam("emergencyDet")->value().toInt() != 0;
            changed = true;
        }
        if (req->hasParam("chinaMode")) {
            cfg.chinaMode = req->getParam("chinaMode")->value().toInt() != 0;
            changed = true;
        }
        if (req->hasParam("apSSID") || req->hasParam("apPass")) {
            String ssid = req->hasParam("apSSID") ? req->getParam("apSSID")->value() : String(apCfg.ssid);
            String pass = req->hasParam("apPass") ? req->getParam("apPass")->value() : String(apCfg.pass);
            ssid.trim();

            if (ssid.isEmpty() || ssid.length() > 32) {
                req->send(400, "text/plain", "热点名称长度必须为 1-32 个字符");
                return;
            }
            if (pass.length() < 8 || pass.length() > 63) {
                req->send(400, "text/plain", "热点密码长度必须为 8-63 个字符");
                return;
            }

            if (ssid != String(apCfg.ssid) || pass != String(apCfg.pass)) {
                copyStringToBuffer(apCfg.ssid, sizeof(apCfg.ssid), ssid);
                copyStringToBuffer(apCfg.pass, sizeof(apCfg.pass), pass);
                changed = true;
                apChanged = true;
            }
        }
        if (req->hasParam("upstreamEnable")) {
            wifiCfg.enabled = req->getParam("upstreamEnable")->value().toInt() != 0;
            changed = true;
            wifiChanged = true;
        }
        if (req->hasParam("dnsWhitelistEnable")) {
            dnsCfg.enabled = req->getParam("dnsWhitelistEnable")->value().toInt() != 0;
            changed = true;
        }
        if (req->hasParam("dnsAllowlist")) {
            String value = req->getParam("dnsAllowlist")->value();
            value.trim();
            if (value.length() < static_cast<int>(sizeof(dnsCfg.allowlist))) {
                copyStringToBuffer(dnsCfg.allowlist, sizeof(dnsCfg.allowlist), value);
                changed = true;
            }
        }
        if (req->hasParam("dnsBlocklist")) {
            String value = req->getParam("dnsBlocklist")->value();
            value.trim();
            if (value.length() < static_cast<int>(sizeof(dnsCfg.blocklist))) {
                copyStringToBuffer(dnsCfg.blocklist, sizeof(dnsCfg.blocklist), value);
                changed = true;
            }
        }

        if (changed) saveConfig();
        if (apChanged) requestLocalAPApply();
        if (wifiChanged) requestUpstreamApply();
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
        logRuntimeHeartbeat();
        // LED: on during activity, off when idle
        digitalWrite(PIN_LED, activity ? HIGH : LOW);
        // Yield to avoid starving watchdog
        vTaskDelay(1);
    }
}

void dnsTask(void* param) {
    for (;;) {
        dnsServer.processNextRequest(dnsCfg, WiFi.status() == WL_CONNECTED);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ═══════════════════════════════════════════
//  Arduino setup / loop
// ═══════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== FSD Controller ===");
    ESP_LOGI(TAG, "boot start");

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    debugLogMutex = xSemaphoreCreateMutex();
    loadConfig();
    debugLogReady = SPIFFS.begin(true);
    if (debugLogReady) {
        appendDebugLogRecord("BOOT", "logger ready");
    } else {
        ESP_LOGE(TAG, "SPIFFS mount failed, debug log disabled");
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
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_AP_STA);
    ESP_LOGI(TAG, "wifi mode set to AP+STA");
    applyLocalAPConfig();
    requestUpstreamApply();
    dnsServer.begin();
    ESP_LOGI(TAG, "dns server started port=53");

    setupWebServer();
    xTaskCreatePinnedToCore(dnsTask, "DNS", 4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(canTask, "CAN", 8192, NULL, 2, NULL, 1);
}

void loop() {
    if (otaPendingRestart) {
        delay(OTA_RESTART_DELAY_MS);  // let response finish sending
        ESP.restart();
    }

    if (apCfg.applyRequested && static_cast<int32_t>(millis() - apCfg.applyAtMillis) >= 0) {
        apCfg.applyRequested = false;
        applyLocalAPConfig();
    }

    serviceThermalStatus();
    serviceUpstreamWiFi();
    dnsIpPolicyService(
        dnsCfg.allowlist,
        dnsCfg.blocklist,
        dnsCfg.enabled ? 1 : 0,
        WiFi.status() == WL_CONNECTED ? 1 : 0
    );
    syncNATState();
    vTaskDelay(1000);
}
