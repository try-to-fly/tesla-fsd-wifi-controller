/*
 * Tesla Open CAN Mod — ESP32 Web Edition
 * Core 0: WiFi AP+STA + AsyncWebServer + OTA
 * Core 1: CAN bus read/modify/write (TWAI)
 *
 * GPLv3 — Based on tesla-open-can-mod
 */

#include <Arduino.h>
#include <cctype>
#include <cstring>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <Preferences.h>

#include "lwip/lwip_napt.h"

#include "can_frame_types.h"
#include "dns_whitelist.h"
#include "drivers/twai_driver.h"
#include "handlers.h"
#include "web_ui.h"

// ── WiFi AP config ──
static const char* AP_SSID = "FSD-Controller";
static const char* AP_PASS = "12345678";   // min 8 chars
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_GATEWAY(192, 168, 4, 1);
static const IPAddress AP_SUBNET(255, 255, 255, 0);
static constexpr uint32_t UPSTREAM_RETRY_MS = 15000;

// ── Globals ──
static TWAIDriver     canDriver;
static AsyncWebServer server(80);
static Preferences    prefs;
static DNSWhitelistServer dnsServer;
static volatile bool  otaPendingRestart = false;
static bool           natEnabled = false;

struct UpstreamWiFiConfig {
    bool     enabled             = false;
    char     ssid[33]            = {};
    char     pass[65]            = {};
    volatile bool applyRequested = false;
    uint32_t lastAttemptMillis   = 0;
};

static UpstreamWiFiConfig wifiCfg;

static DNSFilterConfig dnsCfg;

#ifndef PIN_LED
#define PIN_LED 2   // ESP32 DevKit onboard LED
#endif

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

bool hasUpstreamCredentials() {
    return wifiCfg.ssid[0] != '\0';
}

uint8_t getDNSAllowlistCount() {
    uint8_t count = 0;
    const char* cursor = dnsCfg.allowlist;

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

const char* getUpstreamStatusText() {
    if (!wifiCfg.enabled)          return "未启用";
    if (!hasUpstreamCredentials()) return "未配置热点";

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

void requestUpstreamApply() {
    wifiCfg.applyRequested = true;
}

void startUpstreamConnect() {
    if (!wifiCfg.enabled || !hasUpstreamCredentials()) return;

    Serial.printf("Connecting upstream WiFi: %s\n", wifiCfg.ssid);
    WiFi.begin(wifiCfg.ssid, wifiCfg.pass);
    wifiCfg.lastAttemptMillis = millis();
}

void applyUpstreamWiFiConfig() {
    if (!wifiCfg.enabled || !hasUpstreamCredentials()) {
        WiFi.disconnect(false, true);
        wifiCfg.lastAttemptMillis = 0;
        Serial.println("Upstream WiFi disabled");
        return;
    }

    WiFi.disconnect(false, true);
    startUpstreamConnect();
}

void serviceUpstreamWiFi() {
    if (wifiCfg.applyRequested) {
        wifiCfg.applyRequested = false;
        applyUpstreamWiFiConfig();
        return;
    }

    if (!wifiCfg.enabled || !hasUpstreamCredentials()) return;
    if (WiFi.status() == WL_CONNECTED) return;

    uint32_t now = millis();
    if (wifiCfg.lastAttemptMillis == 0 || now - wifiCfg.lastAttemptMillis >= UPSTREAM_RETRY_MS) {
        startUpstreamConnect();
    }
}

void syncNATState() {
    bool shouldEnable = WiFi.status() == WL_CONNECTED;
    if (natEnabled == shouldEnable) return;

    ip_napt_enable(static_cast<u32_t>(AP_IP), shouldEnable ? 1 : 0);
    natEnabled = shouldEnable;
    Serial.printf("NAPT %s on %s\n", natEnabled ? "enabled" : "disabled", AP_IP.toString().c_str());
}

// ═══════════════════════════════════════════
//  Config persistence (NVS)
// ═══════════════════════════════════════════

void loadConfig() {
    prefs.begin("fsd", true);  // read-only
    cfg.fsdEnable          = prefs.getBool("fsdEn", true);
    cfg.hwMode             = prefs.getUChar("hwMode", 2);
    cfg.speedProfile       = prefs.getUChar("spPro", 1);
    cfg.profileModeAuto    = prefs.getBool("proAuto", true);
    cfg.isaChimeSuppress   = prefs.getBool("isaChm", false);
    cfg.emergencyDetection = prefs.getBool("emDet", true);
    cfg.chinaMode          = prefs.getBool("cnMode", false);
    wifiCfg.enabled        = prefs.getBool("upEn", false);
    copyStringToBuffer(wifiCfg.ssid, sizeof(wifiCfg.ssid), prefs.getString("upSsid", ""));
    copyStringToBuffer(wifiCfg.pass, sizeof(wifiCfg.pass), prefs.getString("upPass", ""));
    dnsCfg.enabled         = prefs.getBool("dnsEn", false);
    copyStringToBuffer(dnsCfg.allowlist, sizeof(dnsCfg.allowlist), prefs.getString("dnsList", ""));
    prefs.end();

    // Clamp values
    if (cfg.hwMode > 2)       cfg.hwMode = 2;
    if (cfg.speedProfile > 4) cfg.speedProfile = 1;
}

void saveConfig() {
    prefs.begin("fsd", false);  // read-write
    prefs.putBool("fsdEn",   cfg.fsdEnable);
    prefs.putUChar("hwMode", cfg.hwMode);
    prefs.putUChar("spPro",  cfg.speedProfile);
    prefs.putBool("proAuto", cfg.profileModeAuto);
    prefs.putBool("isaChm",  cfg.isaChimeSuppress);
    prefs.putBool("emDet",   cfg.emergencyDetection);
    prefs.putBool("cnMode",  cfg.chinaMode);
    prefs.putBool("upEn",    wifiCfg.enabled);
    prefs.putString("upSsid", wifiCfg.ssid);
    prefs.putString("upPass", wifiCfg.pass);
    prefs.putBool("dnsEn",   dnsCfg.enabled);
    prefs.putString("dnsList", dnsCfg.allowlist);
    prefs.end();
}

// ═══════════════════════════════════════════
//  Web Server Setup (runs on Core 0)
// ═══════════════════════════════════════════

void setupWebServer() {
    // Serve UI
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", INDEX_HTML);
    });

    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        uint32_t uptime = (millis() - cfg.uptimeStart) / 1000;
        bool upstreamConnected = WiFi.status() == WL_CONNECTED;
        String upstreamSSID = jsonEscape(String(wifiCfg.ssid));
        String apSSID = jsonEscape(String(AP_SSID));
        String upstreamIP = upstreamConnected ? WiFi.localIP().toString() : "";
        String apIP = WiFi.softAPIP().toString();
        String upstreamStatus = jsonEscape(String(getUpstreamStatusText()));
        String dnsAllowlist = jsonEscape(String(dnsCfg.allowlist));
        char buf[1400];

        snprintf(buf, sizeof(buf),
            "{\"rx\":%u,\"modified\":%u,\"errors\":%u,\"uptime\":%u,"
            "\"canOK\":%s,\"fsdTriggered\":%s,"
            "\"fsdEnable\":%d,\"hwMode\":%d,\"speedProfile\":%d,"
            "\"profileMode\":%d,\"isaChime\":%d,\"emergencyDet\":%d,\"chinaMode\":%d,"
            "\"upstreamEnable\":%d,\"upstreamConfigured\":%d,\"upstreamConnected\":%s,"
            "\"upstreamSSID\":\"%s\",\"upstreamPassSet\":%d,\"upstreamStatus\":\"%s\",\"upstreamIP\":\"%s\","
            "\"apSSID\":\"%s\",\"apIP\":\"%s\","
            "\"dnsWhitelistEnable\":%d,\"dnsWhitelistCount\":%u,\"dnsAllowlist\":\"%s\","
            "\"natEnabled\":%d,\"natStatus\":\"%s\"}",
            (unsigned)cfg.rxCount, (unsigned)cfg.modifiedCount,
            (unsigned)cfg.errorCount, (unsigned)uptime,
            cfg.canOK ? "true" : "false",
            cfg.fsdTriggered ? "true" : "false",
            (int)cfg.fsdEnable,
            (int)cfg.hwMode,
            (int)cfg.speedProfile,
            (int)cfg.profileModeAuto,
            (int)cfg.isaChimeSuppress,
            (int)cfg.emergencyDetection,
            (int)cfg.chinaMode,
            (int)wifiCfg.enabled,
            hasUpstreamCredentials() ? 1 : 0,
            upstreamConnected ? "true" : "false",
            upstreamSSID.c_str(),
            wifiCfg.pass[0] != '\0' ? 1 : 0,
            upstreamStatus.c_str(),
            upstreamIP.c_str(),
            apSSID.c_str(),
            apIP.c_str(),
            (int)dnsCfg.enabled,
            (unsigned)getDNSAllowlistCount(),
            dnsAllowlist.c_str(),
            natEnabled ? 1 : 0,
            getNATStatusText()
        );
        req->send(200, "application/json", buf);
    });

    server.on("/api/set", HTTP_GET, [](AsyncWebServerRequest* req) {
        bool changed = false;
        bool wifiChanged = false;

        if (req->hasParam("fsdEnable")) {
            cfg.fsdEnable = req->getParam("fsdEnable")->value().toInt() != 0;
            changed = true;
        }
        if (req->hasParam("hwMode")) {
            uint8_t v = req->getParam("hwMode")->value().toInt();
            if (v <= 2) { cfg.hwMode = v; changed = true; }
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
        if (req->hasParam("upstreamEnable")) {
            wifiCfg.enabled = req->getParam("upstreamEnable")->value().toInt() != 0;
            changed = true;
            wifiChanged = true;
        }
        if (req->hasParam("upstreamSSID")) {
            String value = req->getParam("upstreamSSID")->value();
            value.trim();
            if (value.length() <= 32) {
                copyStringToBuffer(wifiCfg.ssid, sizeof(wifiCfg.ssid), value);
                changed = true;
                wifiChanged = true;
            }
        }
        if (req->hasParam("upstreamPass")) {
            String value = req->getParam("upstreamPass")->value();
            if (value.length() <= 63) {
                copyStringToBuffer(wifiCfg.pass, sizeof(wifiCfg.pass), value);
                changed = true;
                wifiChanged = true;
            }
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

        if (changed) saveConfig();
        if (wifiChanged) requestUpstreamApply();
        req->send(200, "text/plain", "OK");
    });

    server.on("/api/ota", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            bool ok = !Update.hasError();
            req->send(200, "text/plain", ok ? "OK" : "FAIL");
            if (ok) otaPendingRestart = true;
        },
        [](AsyncWebServerRequest* req, const String& filename,
           size_t index, uint8_t* data, size_t len, bool final) {
            if (index == 0) {
                Serial.printf("OTA start: %s\n", filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    Update.printError(Serial);
                }
            }
            if (Update.isRunning()) {
                if (Update.write(data, len) != len) {
                    Update.printError(Serial);
                }
            }
            if (final) {
                if (Update.end(true)) {
                    Serial.printf("OTA done: %u bytes\n", (unsigned)(index + len));
                } else {
                    Update.printError(Serial);
                }
            }
        }
    );

    server.begin();
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

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    loadConfig();
    Serial.printf("Config loaded: HW=%d, Profile=%d\n", cfg.hwMode, cfg.speedProfile);

    cfg.uptimeStart = millis();

    if (canDriver.init()) {
        cfg.canOK = true;
        Serial.println("ESP32 TWAI ready @ 500k");
    } else {
        cfg.canOK = false;
        Serial.println("CAN init failed!");
    }

    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("WiFi AP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
    requestUpstreamApply();
    dnsServer.begin();

    setupWebServer();
    xTaskCreatePinnedToCore(dnsTask, "DNS", 4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(canTask, "CAN", 8192, NULL, 2, NULL, 1);
}

void loop() {
    if (otaPendingRestart) {
        delay(1000);  // let response finish sending
        ESP.restart();
    }

    serviceUpstreamWiFi();
    syncNATState();
    vTaskDelay(1000);
}
