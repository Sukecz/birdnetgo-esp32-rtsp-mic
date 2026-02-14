#include <WiFi.h>
#include <WiFiManager.h>
#ifndef CONFIG_I2S_SUPPRESS_DEPRECATE_WARN
#define CONFIG_I2S_SUPPRESS_DEPRECATE_WARN 1
#endif
#include "driver/i2s.h"
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <time.h>
#include <math.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include "WebUI.h"

// ================== SETTINGS (ESP32 RTSP Mic for BirdNET-Go) ==================
#define FW_VERSION "1.6.0"
// Expose FW version as a global C string for WebUI/API
const char* FW_VERSION_STR = FW_VERSION;
// Build timestamp for diagnostics (compile time)
const char* FW_BUILD_DATE_STR = __DATE__ " " __TIME__;

// Time / NTP
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";
static const unsigned long NTP_SYNC_INTERVAL_SYNCED_MS = 6UL * 60UL * 60UL * 1000UL;   // 6 hours
static const unsigned long NTP_SYNC_INTERVAL_UNSYNCED_MS = 60UL * 60UL * 1000UL;        // 1 hour
bool timeSyncEnabled = true;
bool timeSynced = false;                 // true after the first successful NTP sync
unsigned long lastTimeSyncAttempt = 0;   // millis() of last attempt
unsigned long lastTimeSyncSuccess = 0;   // millis() of last success
int32_t timeOffsetMinutes = 0;           // user-set offset applied to displayed time

// mDNS
const char* MDNS_HOSTNAME = "esp32mic"; // results in esp32mic.local
bool mdnsEnabled = true;
bool mdnsRunning = false;

// OTA password (optional):
// - For production, set a strong password to protect OTA updates.
// - You can leave it undefined to disable password protection.
// - Example placeholder (edit as needed):
// #define OTA_PASSWORD "1234"  // Optional: change or leave undefined

// -- DEFAULT PARAMETERS (configurable via Web UI / API)
#define DEFAULT_SAMPLE_RATE 48000
#define DEFAULT_GAIN_FACTOR 1.2f
#define DEFAULT_BUFFER_SIZE 1024   // Stable streaming profile by default
#define DEFAULT_WIFI_TX_DBM 19.5f  // Default WiFi TX power in dBm
#define DEFAULT_MQTT_PORT 1883
#define DEFAULT_MQTT_PUBLISH_INTERVAL_SEC 60
// High-pass filter defaults (to remove low-frequency rumble)
#define DEFAULT_HPF_ENABLED true
#define DEFAULT_HPF_CUTOFF_HZ 500

// Thermal protection defaults
#define DEFAULT_OVERHEAT_PROTECTION true
#define DEFAULT_OVERHEAT_LIMIT_C 80
#define OVERHEAT_MIN_LIMIT_C 30
#define OVERHEAT_MAX_LIMIT_C 95
#define OVERHEAT_LIMIT_STEP_C 5

// -- Pins
#define I2S_BCLK_PIN    21
#define I2S_LRCLK_PIN   1
#define I2S_DOUT_PIN    2

// -- Servers
WiFiServer rtspServer(8554);
WiFiClient rtspClient;

// -- RTSP Streaming
String rtspSessionId = "";
volatile bool isStreaming = false;
uint16_t rtpSequence = 0;
uint32_t rtpTimestamp = 0;
uint32_t rtpSSRC = 0x43215678;
unsigned long lastRTSPActivity = 0;

// -- Buffers
uint8_t rtspParseBuffer[1024];
int rtspParseBufferPos = 0;
//
int32_t* i2s_32bit_buffer = nullptr;
int16_t* i2s_16bit_buffer = nullptr;

// -- Global state
unsigned long audioPacketsSent = 0;
unsigned long lastStatsReset = 0;
bool rtspServerEnabled = true;

// -- Audio parameters (runtime configurable)
uint32_t currentSampleRate = DEFAULT_SAMPLE_RATE;
float currentGainFactor = DEFAULT_GAIN_FACTOR;
uint16_t currentBufferSize = DEFAULT_BUFFER_SIZE;
uint8_t i2sShiftBits = 12;  // (1) compile-time default respected on first boot

// -- Audio metering / clipping diagnostics
uint16_t lastPeakAbs16 = 0;       // last block peak absolute value (0..32767)
uint32_t audioClipCount = 0;      // total blocks where clipping occurred
bool audioClippedLastBlock = false; // clipping occurred in last processed block
uint16_t peakHoldAbs16 = 0;       // peak hold (recent window)
unsigned long peakHoldUntilMs = 0; // when to clear hold

// -- High-pass filter (biquad) to cut low-frequency rumble
struct Biquad {
    float b0{1.0f}, b1{0.0f}, b2{0.0f}, a1{0.0f}, a2{0.0f};
    float x1{0.0f}, x2{0.0f}, y1{0.0f}, y2{0.0f};
    inline float process(float x) {
        float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
    inline void reset() { x1 = x2 = y1 = y2 = 0.0f; }
};
bool highpassEnabled = DEFAULT_HPF_ENABLED;
uint16_t highpassCutoffHz = DEFAULT_HPF_CUTOFF_HZ;
Biquad hpf;
uint32_t hpfConfigSampleRate = 0;
uint16_t hpfConfigCutoff = 0;

// -- Preferences for persistent settings
Preferences audioPrefs;

// -- Diagnostics, auto-recovery and temperature monitoring
unsigned long lastMemoryCheck = 0;
unsigned long lastPerformanceCheck = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastTempCheck = 0;
uint32_t minFreeHeap = 0xFFFFFFFF;
uint32_t maxPacketRate = 0;
uint32_t minPacketRate = 0xFFFFFFFF;
bool autoRecoveryEnabled = true;
bool autoThresholdEnabled = true; // auto compute minAcceptableRate from sample rate and buffer size
// Deferred reboot scheduling (to restart safely outside HTTP context)
volatile bool scheduledFactoryReset = false;
volatile unsigned long scheduledRebootAt = 0;
unsigned long bootTime = 0;
unsigned long lastI2SReset = 0;
float maxTemperature = 0.0f;
float lastTemperatureC = 0.0f;
bool lastTemperatureValid = false;
bool overheatProtectionEnabled = DEFAULT_OVERHEAT_PROTECTION;
float overheatShutdownC = (float)DEFAULT_OVERHEAT_LIMIT_C;
bool overheatLockoutActive = false;
float overheatTripTemp = 0.0f;
unsigned long overheatTriggeredAt = 0;
String overheatLastReason = "";
String overheatLastTimestamp = "";
bool overheatSensorFault = false;
bool overheatLatched = false;

// -- Scheduled reset
bool scheduledResetEnabled = false;
uint32_t resetIntervalHours = 24; // Default 24 hours

// -- Stream schedule (local clock, minutes from midnight)
bool streamScheduleEnabled = false;
uint16_t streamScheduleStartMin = 0; // 00:00
uint16_t streamScheduleStopMin = 0;  // 00:00 (same as start = empty/blocked window)
unsigned long lastStreamScheduleCheck = 0;
bool lastScheduleAllow = true;
bool lastScheduleTimeValid = false;
unsigned long lastScheduleUnsyncedLog = 0;

// -- Optional deep sleep outside stream schedule window (conservative mode)
bool deepSleepScheduleEnabled = false;
unsigned long deepSleepOutsideSinceMs = 0;
String deepSleepStatusCode = "disabled";
uint32_t deepSleepNextSleepSec = 0;
static const unsigned long DEEP_SLEEP_BOOT_GRACE_MS = 120000UL;      // 2 min after boot
static const unsigned long DEEP_SLEEP_OUTSIDE_STABLE_MS = 30000UL;    // 30 s outside window before sleep
static const uint32_t DEEP_SLEEP_MIN_SEC = 120UL;                     // minimum timer sleep
static const uint32_t DEEP_SLEEP_MAX_SEC = 28800UL;                   // cap one sleep chunk to 8 h
static const uint32_t DEEP_SLEEP_DRIFT_GUARD_SEC = 300UL;             // wake 5 min before window start

// Deep-sleep snapshot retained across deep-sleep reset (for post-wake logging).
static const uint32_t DEEP_SLEEP_SNAPSHOT_MAGIC = 0x44535031UL; // "DSP1"
RTC_DATA_ATTR uint32_t rtcSleepSnapshotMagic = 0;
RTC_DATA_ATTR uint32_t rtcSleepPlannedSec = 0;
RTC_DATA_ATTR uint32_t rtcSleepUntilStartSec = 0;
RTC_DATA_ATTR uint16_t rtcSleepStartMin = 0;
RTC_DATA_ATTR uint16_t rtcSleepStopMin = 0;
RTC_DATA_ATTR uint16_t rtcSleepEnteredMin = 0;
RTC_DATA_ATTR int32_t rtcSleepOffsetMin = 0;
RTC_DATA_ATTR uint32_t rtcSleepCycleCount = 0;

// -- Configurable thresholds
uint32_t minAcceptableRate = 50;        // Minimum acceptable packet rate (restart below this)
uint32_t performanceCheckInterval = 15; // Check interval in minutes
uint8_t cpuFrequencyMhz = 160;          // CPU frequency (default 160 MHz)

// Forward declaration (used by early wake-snapshot logger).
void simplePrintln(String message);
void scheduleReboot(bool factoryReset, uint32_t delayMs);
void mqttRequestReconnect(bool forceDiscovery);
void mqttPublishDiscoverySoon();

// -- WiFi TX power (configurable)
float wifiTxPowerDbm = DEFAULT_WIFI_TX_DBM;
wifi_power_t currentWifiPowerLevel = WIFI_POWER_19_5dBm;

// -- RTSP connect/PLAY statistics
unsigned long lastRtspClientConnectMs = 0;
unsigned long lastRtspPlayMs = 0;
uint32_t rtspConnectCount = 0;
uint32_t rtspPlayCount = 0;
uint32_t wifiReconnectCount = 0;
uint32_t restartCounter = 0;
String rebootReason = "unknown";

// -- MQTT (Home Assistant discovery + telemetry)
bool mqttEnabled = false;
String mqttHost = "";
uint16_t mqttPort = DEFAULT_MQTT_PORT;
String mqttUser = "";
String mqttPassword = "";
String mqttTopicPrefix = "";
String mqttDiscoveryPrefix = "homeassistant";
String mqttClientId = "";
uint16_t mqttPublishIntervalSec = DEFAULT_MQTT_PUBLISH_INTERVAL_SEC;
bool mqttConnected = false;
String mqttLastError = "disabled";
String mqttDeviceId = "";
WiFiClient mqttNetClient;
PubSubClient mqttClient(mqttNetClient);
unsigned long lastMqttReconnectAttempt = 0;
unsigned long lastMqttPublishMs = 0;
unsigned long lastMqttLogMs = 0;
bool mqttDiscoveryPublished = false;
bool mqttForceDiscovery = false;
static const unsigned long MQTT_RECONNECT_INTERVAL_MS = 10000UL;
static const uint16_t MQTT_PUBLISH_INTERVAL_MIN_SEC = 10;
static const uint16_t MQTT_PUBLISH_INTERVAL_MAX_SEC = 3600;

// -- RTSP diagnostics (for clearer disconnect reasons in logs)
unsigned long lastRtspCommandMs = 0;
String lastRtspCommand = "none";
unsigned long streamStartedAtMs = 0;
unsigned long lastRtpPacketMs = 0;
String lastStreamStopReason = "none";
unsigned long lastStreamStopMs = 0;
uint32_t rtspWriteFailCount = 0;
String lastRtspClientIp = "none";

// ===============================================

// Helper: convert WiFi power enum to dBm (for logs)
float wifiPowerLevelToDbm(wifi_power_t lvl) {
    switch (lvl) {
        case WIFI_POWER_19_5dBm:    return 19.5f;
        case WIFI_POWER_19dBm:      return 19.0f;
        case WIFI_POWER_18_5dBm:    return 18.5f;
        case WIFI_POWER_17dBm:      return 17.0f;
        case WIFI_POWER_15dBm:      return 15.0f;
        case WIFI_POWER_13dBm:      return 13.0f;
        case WIFI_POWER_11dBm:      return 11.0f;
        case WIFI_POWER_8_5dBm:     return 8.5f;
        case WIFI_POWER_7dBm:       return 7.0f;
        case WIFI_POWER_5dBm:       return 5.0f;
        case WIFI_POWER_2dBm:       return 2.0f;
        case WIFI_POWER_MINUS_1dBm: return -1.0f;
        default:                    return 19.5f;
    }
}

static String resetReasonToString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_UNKNOWN:   return "unknown";
        case ESP_RST_POWERON:   return "power_on";
        case ESP_RST_EXT:       return "external_pin";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "interrupt_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "other_wdt";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
#ifdef ESP_RST_USB
        case ESP_RST_USB:       return "usb";
#endif
#ifdef ESP_RST_JTAG
        case ESP_RST_JTAG:      return "jtag";
#endif
#ifdef ESP_RST_EFUSE
        case ESP_RST_EFUSE:     return "efuse";
#endif
#ifdef ESP_RST_PWR_GLITCH
        case ESP_RST_PWR_GLITCH:return "power_glitch";
#endif
#ifdef ESP_RST_CPU_LOCKUP
        case ESP_RST_CPU_LOCKUP:return "cpu_lockup";
#endif
        default:                return "other";
    }
}

static void loadBootMetadata() {
    rebootReason = resetReasonToString(esp_reset_reason());
    audioPrefs.begin("audio", false);
    restartCounter = audioPrefs.getUInt("bootCount", 0);
    if (restartCounter < 0xFFFFFFFFUL) restartCounter++;
    audioPrefs.putUInt("bootCount", restartCounter);
    audioPrefs.end();
}

// Helper: pick the highest power level not exceeding requested dBm
static wifi_power_t pickWifiPowerLevel(float dbm) {
    if (dbm <= -1.0f) return WIFI_POWER_MINUS_1dBm;
    if (dbm <= 2.0f)  return WIFI_POWER_2dBm;
    if (dbm <= 5.0f)  return WIFI_POWER_5dBm;
    if (dbm <= 7.0f)  return WIFI_POWER_7dBm;
    if (dbm <= 8.5f)  return WIFI_POWER_8_5dBm;
    if (dbm <= 11.0f) return WIFI_POWER_11dBm;
    if (dbm <= 13.0f) return WIFI_POWER_13dBm;
    if (dbm <= 15.0f) return WIFI_POWER_15dBm;
    if (dbm <= 17.0f) return WIFI_POWER_17dBm;
    if (dbm <= 18.5f) return WIFI_POWER_18_5dBm;
    if (dbm <= 19.0f) return WIFI_POWER_19dBm;
    return WIFI_POWER_19_5dBm;
}

// Apply WiFi TX power
// Logs only when changed; can be muted with log=false
void applyWifiTxPower(bool log = true) {
    wifi_power_t desired = pickWifiPowerLevel(wifiTxPowerDbm);
    if (desired != currentWifiPowerLevel) {
        WiFi.setTxPower(desired);
        currentWifiPowerLevel = desired;
        if (log) {
            simplePrintln("WiFi TX power set to " + String(wifiPowerLevelToDbm(currentWifiPowerLevel), 1) + " dBm");
        }
    }
}

static String mqttJsonEscape(const String &s) {
    String o;
    o.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') { o += "\\n"; }
        else { o += c; }
    }
    return o;
}

static String buildMqttMacSuffix() {
    uint64_t mac = ESP.getEfuseMac();
    char buf[13];
    snprintf(
        buf,
        sizeof(buf),
        "%04X%08X",
        static_cast<unsigned int>((mac >> 32) & 0xFFFFu),
        static_cast<unsigned int>(mac & 0xFFFFFFFFu)
    );
    return String(buf);
}

static bool isMqttTokenChar(char c) {
    return ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.');
}

static String sanitizeMqttTopicPath(const String &input, const String &fallback) {
    String out;
    out.reserve(input.length() + 4);
    bool prevSlash = false;
    for (size_t i = 0; i < input.length(); ++i) {
        char c = input[i];
        if (c == ' ' || c == '\t') c = '_';
        if (c == '/') {
            if (out.length() == 0 || prevSlash) continue;
            out += '/';
            prevSlash = true;
            continue;
        }
        prevSlash = false;
        if (!isMqttTokenChar(c)) c = '_';
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out += c;
    }
    while (out.length() && out[0] == '/') out.remove(0, 1);
    while (out.length() && out[out.length() - 1] == '/') out.remove(out.length() - 1, 1);
    if (out.length() == 0) return fallback;
    return out;
}

static String sanitizeMqttClientId(const String &input, const String &fallback) {
    String out;
    out.reserve(input.length() + 4);
    for (size_t i = 0; i < input.length(); ++i) {
        char c = input[i];
        if (c == ' ' || c == '\t') c = '_';
        if (!isMqttTokenChar(c)) c = '_';
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out += c;
    }
    if (out.length() == 0) return fallback;
    return out;
}

static String mqttDefaultTopicPrefix() {
    return String("esp32mic/") + mqttDeviceId;
}

static String mqttDefaultClientId() {
    return String("esp32mic-") + mqttDeviceId;
}

static String mqttStateTopic() {
    return mqttTopicPrefix + "/state";
}

static String mqttAvailabilityTopic() {
    return mqttTopicPrefix + "/availability";
}

static String mqttCmdRtspTopic() {
    return mqttTopicPrefix + "/cmd/rtsp_server";
}

static String mqttCmdRebootTopic() {
    return mqttTopicPrefix + "/cmd/reboot";
}

static void mqttNormalizeSettings() {
    mqttHost.trim();
    mqttUser.trim();
    mqttClientId.trim();
    mqttTopicPrefix.trim();
    mqttDiscoveryPrefix.trim();
    if (mqttPort == 0) mqttPort = DEFAULT_MQTT_PORT;
    if (mqttDeviceId.length() == 0) {
        mqttDeviceId = sanitizeMqttClientId(String("esp32mic_") + buildMqttMacSuffix(), "esp32mic");
    }
    mqttTopicPrefix = sanitizeMqttTopicPath(mqttTopicPrefix, mqttDefaultTopicPrefix());
    mqttDiscoveryPrefix = sanitizeMqttTopicPath(mqttDiscoveryPrefix, "homeassistant");
    mqttClientId = sanitizeMqttClientId(mqttClientId, mqttDefaultClientId());
    if (mqttPublishIntervalSec < MQTT_PUBLISH_INTERVAL_MIN_SEC) mqttPublishIntervalSec = MQTT_PUBLISH_INTERVAL_MIN_SEC;
    if (mqttPublishIntervalSec > MQTT_PUBLISH_INTERVAL_MAX_SEC) mqttPublishIntervalSec = MQTT_PUBLISH_INTERVAL_MAX_SEC;
}

static String mqttBuildDeviceJson() {
    String json = "{";
    json += "\"ids\":[\"" + mqttJsonEscape(mqttDeviceId) + "\"],";
    json += "\"name\":\"ESP32 RTSP Mic\",";
    json += "\"mdl\":\"XIAO ESP32-C6\",";
    json += "\"mf\":\"Sukecz\",";
    json += "\"sw\":\"" + mqttJsonEscape(String(FW_VERSION_STR)) + "\",";
    json += "\"cu\":\"http://" + mqttJsonEscape(WiFi.localIP().toString()) + "/\"";
    json += "}";
    return json;
}

static String mqttBuildStateJson() {
    unsigned long nowMs = millis();
    unsigned long uptimeSeconds = (nowMs - bootTime) / 1000;
    unsigned long runtime = nowMs - lastStatsReset;
    uint32_t currentRate = (isStreaming && runtime > 1000) ? (audioPacketsSent * 1000) / runtime : 0;
    uint32_t streamUptimeSeconds = (isStreaming && streamStartedAtMs > 0 && nowMs >= streamStartedAtMs)
                                       ? (uint32_t)((nowMs - streamStartedAtMs) / 1000UL)
                                       : 0;
    uint8_t clientCount = (rtspClient && rtspClient.connected()) ? 1 : 0;

    String json = "{";
    json += "\"fw_version\":\"" + mqttJsonEscape(String(FW_VERSION_STR)) + "\",";
    json += "\"fw_build\":\"" + mqttJsonEscape(String(FW_BUILD_DATE_STR)) + "\",";
    json += "\"reboot_reason\":\"" + mqttJsonEscape(rebootReason) + "\",";
    json += "\"restart_counter\":" + String(restartCounter) + ",";
    json += "\"ip\":\"" + mqttJsonEscape(WiFi.localIP().toString()) + "\",";
    json += "\"wifi_ssid\":\"" + mqttJsonEscape(WiFi.SSID()) + "\",";
    json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"wifi_reconnect_count\":" + String(wifiReconnectCount) + ",";
    json += "\"wifi_tx_dbm\":" + String(wifiPowerLevelToDbm(currentWifiPowerLevel), 1) + ",";
    json += "\"free_heap_kb\":" + String(ESP.getFreeHeap() / 1024) + ",";
    json += "\"min_free_heap_kb\":" + String(minFreeHeap / 1024) + ",";
    json += "\"uptime_s\":" + String(uptimeSeconds) + ",";
    json += "\"rtsp_server_enabled\":" + String(rtspServerEnabled ? "true" : "false") + ",";
    json += "\"streaming\":" + String(isStreaming ? "true" : "false") + ",";
    json += "\"stream_uptime_s\":" + String(streamUptimeSeconds) + ",";
    json += "\"client_count\":" + String((uint32_t)clientCount) + ",";
    json += "\"current_rate_pkt_s\":" + String(currentRate) + ",";
    json += "\"sample_rate\":" + String(currentSampleRate) + ",";
    json += "\"audio_format\":\"L16/mono\",";
    json += "\"buffer_size\":" + String(currentBufferSize) + ",";
    json += "\"gain\":" + String(currentGainFactor, 2) + ",";
    if (rtspClient && rtspClient.connected()) {
        json += "\"client\":\"" + mqttJsonEscape(rtspClient.remoteIP().toString()) + "\",";
    } else {
        json += "\"client\":\"\",";
    }
    if (lastTemperatureValid) json += "\"temperature_c\":" + String(lastTemperatureC, 1) + ",";
    else json += "\"temperature_c\":null,";
    json += "\"temperature_valid\":" + String(lastTemperatureValid ? "true" : "false") + ",";
    json += "\"max_temperature_c\":" + String(maxTemperature, 1) + ",";
    json += "\"overheat_latched\":" + String(overheatLatched ? "true" : "false") + ",";
    json += "\"mdns_enabled\":" + String(mdnsEnabled ? "true" : "false") + ",";
    json += "\"time_synced\":" + String(timeSynced ? "true" : "false");
    json += "}";
    return json;
}

static bool mqttPublishDiscoveryConfig(const String &component, const String &objectId, const String &payload) {
    String topic = mqttDiscoveryPrefix + "/" + component + "/" + mqttDeviceId + "/" + objectId + "/config";
    return mqttClient.publish(topic.c_str(), payload.c_str(), true);
}

static bool mqttPublishState(bool force) {
    if (!mqttClient.connected()) return false;
    unsigned long now = millis();
    unsigned long intervalMs = (unsigned long)mqttPublishIntervalSec * 1000UL;
    if (!force && (now - lastMqttPublishMs) < intervalMs) return true;
    String topic = mqttStateTopic();
    String payload = mqttBuildStateJson();
    bool ok = mqttClient.publish(topic.c_str(), payload.c_str(), false);
    if (ok) lastMqttPublishMs = now;
    return ok;
}

static bool mqttPublishDiscovery() {
    if (!mqttClient.connected()) return false;

    String dev = mqttBuildDeviceJson();
    String st = mqttStateTopic();
    String av = mqttAvailabilityTopic();
    String cmdRtsp = mqttCmdRtspTopic();
    String cmdReboot = mqttCmdRebootTopic();
    bool ok = true;

    String p;

    p = "{\"name\":\"WiFi RSSI\",\"uniq_id\":\"" + mqttDeviceId + "_wifi_rssi\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.wifi_rssi }}\",\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\",\"stat_cla\":\"measurement\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "wifi_rssi", p);

    p = "{\"name\":\"Free Heap\",\"uniq_id\":\"" + mqttDeviceId + "_heap_kb\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.free_heap_kb }}\",\"unit_of_meas\":\"KB\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "heap_kb", p);

    p = "{\"name\":\"Packet Rate\",\"uniq_id\":\"" + mqttDeviceId + "_pkt_rate\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.current_rate_pkt_s }}\",\"unit_of_meas\":\"pkt/s\",\"stat_cla\":\"measurement\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "packet_rate", p);

    p = "{\"name\":\"Temperature\",\"uniq_id\":\"" + mqttDeviceId + "_temp_c\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.temperature_c }}\",\"unit_of_meas\":\"C\",\"dev_cla\":\"temperature\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "temperature_c", p);

    p = "{\"name\":\"Uptime\",\"uniq_id\":\"" + mqttDeviceId + "_uptime_s\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.uptime_s }}\",\"unit_of_meas\":\"s\",\"dev_cla\":\"duration\",\"stat_cla\":\"total_increasing\",\"ent_cat\":\"diagnostic\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "uptime_s", p);

    p = "{\"name\":\"Streaming\",\"uniq_id\":\"" + mqttDeviceId + "_streaming\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ 'ON' if value_json.streaming else 'OFF' }}\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"dev_cla\":\"running\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("binary_sensor", "streaming", p);

    p = "{\"name\":\"RTSP Server\",\"uniq_id\":\"" + mqttDeviceId + "_rtsp_server\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ 'ON' if value_json.rtsp_server_enabled else 'OFF' }}\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"cmd_t\":\"" + cmdRtsp + "\",\"ic\":\"mdi:radio-tower\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("switch", "rtsp_server", p);

    p = "{\"name\":\"RTSP Client\",\"uniq_id\":\"" + mqttDeviceId + "_rtsp_client\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.client }}\",\"ent_cat\":\"diagnostic\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "rtsp_client", p);

    p = "{\"name\":\"Firmware\",\"uniq_id\":\"" + mqttDeviceId + "_fw_version\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.fw_version }}\",\"ent_cat\":\"diagnostic\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "fw_version", p);

    p = "{\"name\":\"Build Date\",\"uniq_id\":\"" + mqttDeviceId + "_fw_build\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.fw_build }}\",\"ent_cat\":\"diagnostic\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "fw_build", p);

    p = "{\"name\":\"Reboot Reason\",\"uniq_id\":\"" + mqttDeviceId + "_reboot_reason\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.reboot_reason }}\",\"ent_cat\":\"diagnostic\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "reboot_reason", p);

    p = "{\"name\":\"Restart Counter\",\"uniq_id\":\"" + mqttDeviceId + "_restart_counter\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.restart_counter }}\",\"stat_cla\":\"total_increasing\",\"ent_cat\":\"diagnostic\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "restart_counter", p);

    p = "{\"name\":\"WiFi SSID\",\"uniq_id\":\"" + mqttDeviceId + "_wifi_ssid\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.wifi_ssid }}\",\"ent_cat\":\"diagnostic\",\"ic\":\"mdi:wifi\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "wifi_ssid", p);

    p = "{\"name\":\"WiFi Reconnects\",\"uniq_id\":\"" + mqttDeviceId + "_wifi_reconnect_count\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.wifi_reconnect_count }}\",\"stat_cla\":\"total_increasing\",\"ent_cat\":\"diagnostic\",\"ic\":\"mdi:wifi-refresh\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "wifi_reconnect_count", p);

    p = "{\"name\":\"Stream Uptime\",\"uniq_id\":\"" + mqttDeviceId + "_stream_uptime_s\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.stream_uptime_s }}\",\"unit_of_meas\":\"s\",\"dev_cla\":\"duration\",\"stat_cla\":\"measurement\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "stream_uptime_s", p);

    p = "{\"name\":\"RTSP Client Count\",\"uniq_id\":\"" + mqttDeviceId + "_client_count\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.client_count }}\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\",\"ic\":\"mdi:account-multiple\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "client_count", p);

    p = "{\"name\":\"Sample Rate\",\"uniq_id\":\"" + mqttDeviceId + "_sample_rate_hz\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.sample_rate }}\",\"unit_of_meas\":\"Hz\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "sample_rate_hz", p);

    p = "{\"name\":\"Audio Format\",\"uniq_id\":\"" + mqttDeviceId + "_audio_format\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.audio_format }}\",\"ent_cat\":\"diagnostic\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "audio_format", p);

    p = "{\"name\":\"Reboot Device\",\"uniq_id\":\"" + mqttDeviceId + "_reboot\",\"cmd_t\":\"" + cmdReboot + "\",\"pl_prs\":\"PRESS\",\"ent_cat\":\"config\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("button", "reboot", p);

    if (ok) {
        mqttDiscoveryPublished = true;
        mqttForceDiscovery = false;
    }
    return ok;
}

static void mqttMessageCallback(char* topic, byte* payload, unsigned int length) {
    String t = topic ? String(topic) : String("");
    String msg;
    msg.reserve(length + 1);
    for (unsigned int i = 0; i < length; ++i) msg += (char)payload[i];
    msg.trim();
    String up = msg;
    up.toUpperCase();

    if (t == mqttCmdRtspTopic()) {
        if (up == "ON") {
            if (overheatLatched) {
                simplePrintln("MQTT command ignored: RTSP ON blocked by thermal latch.");
            } else if (!rtspServerEnabled) {
                rtspServer.begin();
                rtspServer.setNoDelay(true);
                rtspServerEnabled = true;
                simplePrintln("MQTT command: RTSP server enabled.");
            }
        } else if (up == "OFF") {
            rtspServerEnabled = false;
            if (rtspClient && rtspClient.connected()) rtspClient.stop();
            isStreaming = false;
            rtspServer.stop();
            simplePrintln("MQTT command: RTSP server disabled.");
        }
        mqttPublishState(true);
        return;
    }

    if (t == mqttCmdRebootTopic()) {
        if (up == "PRESS" || up == "REBOOT") {
            simplePrintln("MQTT command: reboot requested.");
            scheduleReboot(false, 600);
        }
        return;
    }
}

static void mqttApplyClientSettings(bool logResult) {
    mqttNormalizeSettings();
    mqttClient.setServer(mqttHost.c_str(), mqttPort);
    mqttClient.setCallback(mqttMessageCallback);
    mqttClient.setKeepAlive(30);
    mqttClient.setBufferSize(1536);
    if (logResult) {
        simplePrintln("MQTT config: " + String(mqttEnabled ? "enabled" : "disabled") +
                      ", host=" + (mqttHost.length() ? mqttHost : String("(empty)")) +
                      ", port=" + String(mqttPort) +
                      ", topic=" + mqttTopicPrefix +
                      ", discovery=" + mqttDiscoveryPrefix +
                      ", clientId=" + mqttClientId);
    }
}

static bool mqttConnectNow() {
    mqttApplyClientSettings(false);
    if (!mqttEnabled) {
        mqttConnected = false;
        mqttLastError = "disabled";
        return false;
    }
    if (mqttHost.length() == 0) {
        mqttConnected = false;
        mqttLastError = "missing_host";
        return false;
    }

    String availTopic = mqttAvailabilityTopic();
    bool ok = false;
    if (mqttUser.length()) {
        ok = mqttClient.connect(mqttClientId.c_str(), mqttUser.c_str(), mqttPassword.c_str(), availTopic.c_str(), 0, true, "offline");
    } else {
        ok = mqttClient.connect(mqttClientId.c_str(), availTopic.c_str(), 0, true, "offline");
    }
    if (!ok) {
        mqttConnected = false;
        mqttLastError = String("connect_failed_") + String(mqttClient.state());
        return false;
    }

    mqttConnected = true;
    mqttLastError = "ok";
    mqttClient.publish(availTopic.c_str(), "online", true);
    mqttClient.subscribe(mqttCmdRtspTopic().c_str());
    mqttClient.subscribe(mqttCmdRebootTopic().c_str());
    mqttDiscoveryPublished = false;
    if (!mqttPublishDiscovery()) {
        mqttLastError = "discovery_publish_failed";
    }
    mqttPublishState(true);
    simplePrintln("MQTT connected to " + mqttHost + ":" + String(mqttPort));
    return true;
}

void mqttRequestReconnect(bool forceDiscovery) {
    if (forceDiscovery) mqttForceDiscovery = true;
    mqttDiscoveryPublished = false;
    lastMqttReconnectAttempt = 0;
    lastMqttPublishMs = 0;
    if (mqttClient.connected()) {
        String availTopic = mqttAvailabilityTopic();
        mqttClient.publish(availTopic.c_str(), "offline", true);
        mqttClient.disconnect();
    }
    mqttConnected = false;
}

void mqttPublishDiscoverySoon() {
    mqttForceDiscovery = true;
    if (mqttClient.connected()) {
        if (!mqttPublishDiscovery()) mqttLastError = "discovery_publish_failed";
    } else {
        mqttRequestReconnect(true);
    }
}

void checkMqtt() {
    if (!mqttEnabled) {
        if (mqttClient.connected()) {
            String availTopic = mqttAvailabilityTopic();
            mqttClient.publish(availTopic.c_str(), "offline", true);
            mqttClient.disconnect();
        }
        mqttConnected = false;
        mqttLastError = "disabled";
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {
        if (mqttClient.connected()) mqttClient.disconnect();
        mqttConnected = false;
        mqttLastError = "wifi_disconnected";
        return;
    }

    if (!mqttClient.connected()) {
        unsigned long now = millis();
        if ((now - lastMqttReconnectAttempt) < MQTT_RECONNECT_INTERVAL_MS) return;
        lastMqttReconnectAttempt = now;
        bool ok = mqttConnectNow();
        if (!ok && (now - lastMqttLogMs) > 60000UL) {
            simplePrintln("MQTT connect failed: " + mqttLastError);
            lastMqttLogMs = now;
        }
        return;
    }

    mqttConnected = true;
    mqttClient.loop();
    if (mqttForceDiscovery || !mqttDiscoveryPublished) {
        if (!mqttPublishDiscovery()) mqttLastError = "discovery_publish_failed";
    }
    if (!mqttPublishState(false)) {
        mqttLastError = "state_publish_failed";
    }
}

// Recompute HPF coefficients (2nd-order Butterworth high-pass)
void updateHighpassCoeffs() {
    if (!highpassEnabled) {
        hpf.reset();
        hpfConfigSampleRate = currentSampleRate;
        hpfConfigCutoff = highpassCutoffHz;
        return;
    }
    float fs = (float)currentSampleRate;
    float fc = (float)highpassCutoffHz;
    if (fc < 10.0f) fc = 10.0f;
    if (fc > fs * 0.45f) fc = fs * 0.45f; // keep reasonable

    const float pi = 3.14159265358979323846f;
    float w0 = 2.0f * pi * (fc / fs);
    float cosw0 = cosf(w0);
    float sinw0 = sinf(w0);
    float Q = 0.70710678f; // Butterworth-like
    float alpha = sinw0 / (2.0f * Q);

    float b0 =  (1.0f + cosw0) * 0.5f;
    float b1 = -(1.0f + cosw0);
    float b2 =  (1.0f + cosw0) * 0.5f;
    float a0 =  1.0f + alpha;
    float a1 = -2.0f * cosw0;
    float a2 =  1.0f - alpha;

    hpf.b0 = b0 / a0;
    hpf.b1 = b1 / a0;
    hpf.b2 = b2 / a0;
    hpf.a1 = a1 / a0;
    hpf.a2 = a2 / a0;
    hpf.reset();

    hpfConfigSampleRate = currentSampleRate;
    hpfConfigCutoff = (uint16_t)fc;
}

// Uptime -> "Xd Yh Zm Ts"
String formatUptime(unsigned long seconds) {
    unsigned long days = seconds / 86400;
    seconds %= 86400;
    unsigned long hours = seconds / 3600;
    seconds %= 3600;
    unsigned long minutes = seconds / 60;
    seconds %= 60;

    String result = "";
    if (days > 0) result += String(days) + "d ";
    if (hours > 0 || days > 0) result += String(hours) + "h ";
    if (minutes > 0 || hours > 0 || days > 0) result += String(minutes) + "m ";
    result += String(seconds) + "s";
    return result;
}

// Format "X ago" for events based on millis()
String formatSince(unsigned long eventMs) {
    if (eventMs == 0) return String("never");
    unsigned long seconds = (millis() - eventMs) / 1000;
    return formatUptime(seconds) + " ago";
}

static const char* wifiStatusToString(wl_status_t status) {
    switch (status) {
        case WL_IDLE_STATUS:     return "IDLE";
        case WL_NO_SSID_AVAIL:   return "NO_SSID";
        case WL_SCAN_COMPLETED:  return "SCAN_DONE";
        case WL_CONNECTED:       return "CONNECTED";
        case WL_CONNECT_FAILED:  return "CONNECT_FAILED";
        case WL_CONNECTION_LOST: return "CONNECTION_LOST";
        case WL_DISCONNECTED:    return "DISCONNECTED";
        default:                 return "UNKNOWN";
    }
}

static String buildRtspDiag(WiFiClient &client) {
    unsigned long nowMs = millis();
    unsigned long idleMs = nowMs - lastRTSPActivity;
    String diag = "idle=" + String(idleMs) + "ms";
    diag += ", lastCmd=" + lastRtspCommand;
    if (lastRtspCommandMs > 0) {
        diag += " (" + String(nowMs - lastRtspCommandMs) + "ms ago)";
    } else {
        diag += " (never)";
    }
    if (lastStreamStopMs > 0) {
        diag += ", lastStop=" + lastStreamStopReason + " (" + String(nowMs - lastStreamStopMs) + "ms ago)";
    } else {
        diag += ", lastStop=none";
    }
    if (streamStartedAtMs > 0) {
        diag += ", streamAge=" + String(nowMs - streamStartedAtMs) + "ms";
    }
    if (lastRtpPacketMs > 0) {
        diag += ", rtpIdle=" + String(nowMs - lastRtpPacketMs) + "ms";
    }
    diag += ", packets=" + String(audioPacketsSent);
    diag += ", wifi=" + String(wifiStatusToString(WiFi.status()));
    diag += ", rssi=" + String(WiFi.RSSI()) + "dBm";
    if (client && client.connected()) {
        diag += ", client=" + client.remoteIP().toString();
    } else {
        diag += ", client=" + lastRtspClientIp;
    }
    return diag;
}

// Return true if we have a plausible current time (epoch after 2023-01-01)
static bool hasValidTime() {
    time_t now = time(nullptr);
    return now > 1672531200; // 2023-01-01 00:00:00 UTC
}

// Apply local time offset and optionally enable/disable NTP source servers.
// With enableNtp=false we keep local offset handling, but no network sync is configured.
void configureTimeService(bool enableNtp) {
    if (enableNtp) {
        configTime(timeOffsetMinutes * 60, 0, NTP_SERVER_1, NTP_SERVER_2);
    } else {
        configTime(timeOffsetMinutes * 60, 0, nullptr, nullptr);
    }
}

static String formatClockHHMM(uint16_t mins) {
    mins %= 1440;
    uint8_t hh = (uint8_t)(mins / 60);
    uint8_t mm = (uint8_t)(mins % 60);
    char buf[6];
    snprintf(buf, sizeof(buf), "%02u:%02u", hh, mm);
    return String(buf);
}

// Return true when current local time falls inside the [start, stop) window.
// If start == stop, window is treated as empty (always blocked).
static bool isScheduleWindowActive(uint16_t nowMin, uint16_t startMin, uint16_t stopMin) {
    if (startMin == stopMin) return false;
    if (startMin < stopMin) return (nowMin >= startMin && nowMin < stopMin);
    return (nowMin >= startMin || nowMin < stopMin); // overnight window
}

// Schedule policy: when local time is not valid, fail-open (do not block RTSP).
bool isStreamScheduleAllowedNow(bool* timeValidOut = nullptr) {
    bool validTime = hasValidTime();
    if (timeValidOut) *timeValidOut = validTime;
    if (!streamScheduleEnabled) return true;
    // Equal start/stop is an explicit empty window (always blocked), independent of time sync.
    if (streamScheduleStartMin == streamScheduleStopMin) return false;
    if (!validTime) return true;

    time_t now = time(nullptr);
    struct tm tmNow;
    if (!localtime_r(&now, &tmNow)) return true; // fail-open on conversion issue

    uint16_t nowMin = (uint16_t)(tmNow.tm_hour * 60 + tmNow.tm_min);
    return isScheduleWindowActive(nowMin, streamScheduleStartMin, streamScheduleStopMin);
}

static uint32_t secondsUntilScheduleStart(const struct tm& tmNow, uint16_t startMin) {
    uint16_t nowMin = (uint16_t)(tmNow.tm_hour * 60 + tmNow.tm_min);
    uint16_t deltaMin = (uint16_t)((startMin + 1440 - nowMin) % 1440);
    uint32_t sec = (uint32_t)deltaMin * 60UL;
    if (sec == 0) return 1; // schedule start is essentially now
    if (tmNow.tm_sec > 0) {
        uint32_t used = (uint32_t)tmNow.tm_sec;
        sec = (sec > used) ? (sec - used) : 1;
    }
    return sec;
}

static void recordDeepSleepSnapshot(uint32_t sleepSec, uint32_t untilStartSec, const struct tm& tmNow) {
    rtcSleepPlannedSec = sleepSec;
    rtcSleepUntilStartSec = untilStartSec;
    rtcSleepStartMin = streamScheduleStartMin;
    rtcSleepStopMin = streamScheduleStopMin;
    rtcSleepEnteredMin = (uint16_t)(tmNow.tm_hour * 60 + tmNow.tm_min);
    rtcSleepOffsetMin = timeOffsetMinutes;
    rtcSleepCycleCount++;
    rtcSleepSnapshotMagic = DEEP_SLEEP_SNAPSHOT_MAGIC;
}

static void logDeepSleepWakeSnapshotIfAny() {
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) return;
    if (rtcSleepSnapshotMagic != DEEP_SLEEP_SNAPSHOT_MAGIC) {
        simplePrintln("Deep sleep wake: timer wake detected (no retained snapshot).");
        return;
    }
    simplePrintln("Deep sleep wake: cycle #" + String(rtcSleepCycleCount) +
                  ", planned " + String(rtcSleepPlannedSec) + " s, entered " +
                  formatClockHHMM(rtcSleepEnteredMin) + ", schedule " +
                  formatClockHHMM(rtcSleepStartMin) + "-" + formatClockHHMM(rtcSleepStopMin) +
                  ", until start at sleep " + String(rtcSleepUntilStartSec) +
                  " s, offset " + String(rtcSleepOffsetMin) + " min.");
    // Consume snapshot after first successful boot log to avoid repeated replay.
    rtcSleepSnapshotMagic = 0;
}

void checkStreamSchedule() {
    if (!streamScheduleEnabled) {
        lastScheduleAllow = true;
        lastScheduleTimeValid = hasValidTime();
        return;
    }

    bool timeValid = false;
    bool allowNow = isStreamScheduleAllowedNow(&timeValid);
    bool invalidWindow = (streamScheduleStartMin == streamScheduleStopMin);
    unsigned long nowMs = millis();
    bool transitioned = (allowNow != lastScheduleAllow) || (timeValid != lastScheduleTimeValid);

    if (!timeValid) {
        if (lastScheduleUnsyncedLog == 0 || (nowMs - lastScheduleUnsyncedLog) > 600000UL) {
            simplePrintln("Stream schedule: local time unavailable, fail-open mode (RTSP allowed).");
            lastScheduleUnsyncedLog = nowMs;
        }
    }

    if (!allowNow) {
        if (rtspClient && rtspClient.connected()) {
            rtspClient.stop();
        }
        if (isStreaming) {
            isStreaming = false;
            lastStreamStopReason = "Stream schedule window closed";
            lastStreamStopMs = millis();
        }
        if (rtspServerEnabled) {
            rtspServerEnabled = false;
            rtspServer.stop();
        }
        if (transitioned) {
            if (invalidWindow) {
                simplePrintln("Stream schedule: invalid/empty window (" +
                              formatClockHHMM(streamScheduleStartMin) + "-" +
                              formatClockHHMM(streamScheduleStopMin) +
                              "). RTSP server paused.");
            } else {
                simplePrintln("Stream schedule: outside allowed window " +
                              formatClockHHMM(streamScheduleStartMin) + "-" +
                              formatClockHHMM(streamScheduleStopMin) +
                              ". RTSP server paused.");
            }
            mqttPublishState(true);
        }
    } else if (transitioned) {
        if (overheatLatched) {
            simplePrintln("Stream schedule: window opened, but thermal latch keeps RTSP paused.");
        } else if (!rtspServerEnabled) {
            rtspServer.begin();
            rtspServer.setNoDelay(true);
            rtspServerEnabled = true;
            simplePrintln("Stream schedule: allowed window " +
                          formatClockHHMM(streamScheduleStartMin) + "-" +
                          formatClockHHMM(streamScheduleStopMin) +
                          ". RTSP server resumed.");
        }
        mqttPublishState(true);
    }

    lastScheduleAllow = allowNow;
    lastScheduleTimeValid = timeValid;
}

void checkDeepSleepSchedule() {
    deepSleepNextSleepSec = 0;

    if (!deepSleepScheduleEnabled) {
        deepSleepOutsideSinceMs = 0;
        deepSleepStatusCode = "disabled";
        return;
    }
    if (!streamScheduleEnabled) {
        deepSleepOutsideSinceMs = 0;
        deepSleepStatusCode = "schedule_disabled";
        return;
    }
    if (streamScheduleStartMin == streamScheduleStopMin) {
        // Invalid/empty schedule window: avoid accidental deep-sleep-only mode.
        deepSleepOutsideSinceMs = 0;
        deepSleepStatusCode = "schedule_invalid";
        return;
    }

    bool timeValid = false;
    bool allowNow = isStreamScheduleAllowedNow(&timeValid);
    if (!timeValid) {
        deepSleepOutsideSinceMs = 0;
        deepSleepStatusCode = "time_invalid";
        return;
    }
    if (allowNow) {
        deepSleepOutsideSinceMs = 0;
        deepSleepStatusCode = "inside_window";
        return;
    }
    if (scheduledRebootAt != 0) {
        deepSleepStatusCode = "reboot_pending";
        return;
    }
    if (rtspClient && rtspClient.connected()) {
        deepSleepStatusCode = "client_connected";
        return;
    }
    if (isStreaming) {
        deepSleepStatusCode = "streaming_active";
        return;
    }
    if (millis() < DEEP_SLEEP_BOOT_GRACE_MS) {
        deepSleepStatusCode = "grace_boot";
        return;
    }

    unsigned long nowMs = millis();
    if (deepSleepOutsideSinceMs == 0) deepSleepOutsideSinceMs = nowMs;
    if ((nowMs - deepSleepOutsideSinceMs) < DEEP_SLEEP_OUTSIDE_STABLE_MS) {
        deepSleepStatusCode = "outside_stabilizing";
        return;
    }

    time_t now = time(nullptr);
    struct tm tmNow;
    if (!localtime_r(&now, &tmNow)) {
        deepSleepStatusCode = "time_invalid";
        return;
    }

    uint32_t untilStartSec = secondsUntilScheduleStart(tmNow, streamScheduleStartMin);
    deepSleepNextSleepSec = untilStartSec;
    // If the next stream window is soon, stay awake and avoid edge flapping near boundary.
    if (untilStartSec <= (DEEP_SLEEP_MIN_SEC + DEEP_SLEEP_DRIFT_GUARD_SEC + 15UL)) {
        deepSleepStatusCode = "next_window_soon";
        return;
    }

    // Sleep only part of the remaining time so wake-up happens before the next window starts.
    uint32_t targetSleepSec = untilStartSec - DEEP_SLEEP_DRIFT_GUARD_SEC;
    uint32_t sleepSec = (targetSleepSec > DEEP_SLEEP_MAX_SEC)
                            ? DEEP_SLEEP_MAX_SEC
                            : targetSleepSec;
    if (sleepSec < DEEP_SLEEP_MIN_SEC) {
        deepSleepStatusCode = "next_window_soon";
        return;
    }

    deepSleepStatusCode = "ready";
    deepSleepNextSleepSec = sleepSec;
    recordDeepSleepSnapshot(sleepSec, untilStartSec, tmNow);

    simplePrintln("Deep sleep schedule: outside allowed stream window " +
                  formatClockHHMM(streamScheduleStartMin) + "-" +
                  formatClockHHMM(streamScheduleStopMin) +
                  ", sleeping for " + String(sleepSec) +
                  " s (wake guard " + String(DEEP_SLEEP_DRIFT_GUARD_SEC) + " s).");
    if (rtspClient && rtspClient.connected()) {
        rtspClient.stop();
    }
    isStreaming = false;
    if (rtspServerEnabled) {
        rtspServerEnabled = false;
        rtspServer.stop();
    }
    lastStreamStopReason = "Deep sleep outside stream schedule";
    lastStreamStopMs = millis();
    mqttPublishState(true);
    delay(40);

    esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
    Serial.flush();
    delay(30);
    esp_deep_sleep_start();
}

// Format current local date/time (uses NTP + offset), fallback to uptime
String formatDateTime() {
    time_t now = time(nullptr);
    if (hasValidTime()) {
        struct tm tmNow;
        localtime_r(&now, &tmNow);
        char buf[24];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmNow);
        return String(buf);
    }
    unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
    return String("uptime ") + formatUptime(uptimeSeconds);
}

// Timestamp for log lines (short)
String formatLogTimestamp() {
    time_t now = time(nullptr);
    if (hasValidTime()) {
        struct tm tmNow;
        localtime_r(&now, &tmNow);
        char buf[24];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmNow);
        return String(buf);
    }
    unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
    return String("uptime ") + formatUptime(uptimeSeconds);
}

// Attempt NTP sync if Wi-Fi is up; returns success
bool attemptTimeSync(bool logResult, bool quickMode /*prefer short, single shot*/ = false) {
    lastTimeSyncAttempt = millis();
    bool wasUnsynced = !timeSynced;
    if (WiFi.status() != WL_CONNECTED) {
        if (logResult) simplePrintln("NTP skipped: WiFi not connected");
        return false;
    }
    configureTimeService(true);
    struct tm tmNow;
    bool ok = false;
    // Keep attempts very short when quickMode (during streaming).
    int tries = quickMode ? 1 : 3;
    int perTryTimeoutMs = quickMode ? 150 : 200;
    for (int i = 0; i < tries; ++i) {
        if (getLocalTime(&tmNow, perTryTimeoutMs)) { ok = true; break; }
        if (!quickMode) delay(60);
    }
    if (ok) {
        timeSynced = true;
        lastTimeSyncSuccess = millis();
        if (logResult || wasUnsynced) {
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmNow);
            simplePrintln(String("Time synchronized via NTP: ") + String(buf));
        }
    } else {
        if (!hasValidTime()) timeSynced = false;
        if (logResult) simplePrintln("NTP sync failed (no response)");
    }
    return ok;
}

// Periodic NTP sync scheduler
void checkTimeSync() {
    if (!timeSyncEnabled) return;
    unsigned long now = millis();
    bool dueUnsynced = (!timeSynced && (now - lastTimeSyncAttempt) > NTP_SYNC_INTERVAL_UNSYNCED_MS);
    bool dueSynced = (timeSynced && (now - lastTimeSyncAttempt) > NTP_SYNC_INTERVAL_SYNCED_MS);
    if (dueUnsynced || dueSynced) {
        bool quick = isStreaming; // během streamu jen krátce
        attemptTimeSync(false, quick);
    }
}

static bool isTemperatureValid(float temp) {
    if (isnan(temp) || isinf(temp)) return false;
    if (temp < -20.0f || temp > 130.0f) return false;
    return true;
}

// Format current local time, fallback to uptime when no RTC/NTP time available
static void persistOverheatNote() {
    audioPrefs.begin("audio", false);
    audioPrefs.putString("ohReason", overheatLastReason);
    audioPrefs.putString("ohStamp", overheatLastTimestamp);
    audioPrefs.putFloat("ohTripC", overheatTripTemp);
    audioPrefs.putBool("ohLatched", overheatLatched);
    audioPrefs.end();
}

void recordOverheatTrip(float temp) {
    unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
    overheatTripTemp = temp;
    overheatTriggeredAt = millis();
    overheatLastTimestamp = formatUptime(uptimeSeconds);
    overheatLastReason = String("Thermal shutdown: ") + String(temp, 1) + " C reached (limit " +
                         String(overheatShutdownC, 1) + " C). Stream disabled; acknowledge in UI.";
    overheatLatched = true;
    simplePrintln("THERMAL PROTECTION: " + overheatLastReason);
    simplePrintln("TIP: Improve cooling or lower WiFi TX power/CPU MHz if overheating persists.");
    persistOverheatNote();
}

// Temperature monitoring + thermal protection
void checkTemperature() {
    float temp = temperatureRead(); // ESP32 internal sensor (approximate)
    bool tempValid = isTemperatureValid(temp);
    if (!tempValid) {
        lastTemperatureValid = false;
        if (!overheatSensorFault) {
            overheatSensorFault = true;
            overheatLastReason = "Thermal protection disabled: temperature sensor unavailable.";
            overheatLastTimestamp = "";
            overheatTripTemp = 0.0f;
            overheatTriggeredAt = 0;
            persistOverheatNote();
            simplePrintln("WARNING: Temperature sensor unavailable. Thermal protection paused.");
        }
        return;
    }

    lastTemperatureC = temp;
    lastTemperatureValid = true;

    if (overheatSensorFault) {
        overheatSensorFault = false;
        overheatLastReason = "Thermal protection restored: temperature sensor reading valid.";
        overheatLastTimestamp = formatUptime((millis() - bootTime) / 1000);
        persistOverheatNote();
        simplePrintln("Temperature sensor restored. Thermal protection active again.");
    }

    if (temp > maxTemperature) {
        maxTemperature = temp;
    }

    bool protectionActive = overheatProtectionEnabled && !overheatSensorFault;
    if (protectionActive) {
        if (!overheatLockoutActive && temp >= overheatShutdownC) {
            overheatLockoutActive = true;
            recordOverheatTrip(temp);
            // Disable streaming until user restarts manually
            if (rtspClient && rtspClient.connected()) {
                rtspClient.stop();
            }
            if (isStreaming) {
                isStreaming = false;
            }
            rtspServerEnabled = false;
            rtspServer.stop();
            mqttPublishState(true);
        } else if (overheatLockoutActive && temp <= (overheatShutdownC - OVERHEAT_LIMIT_STEP_C)) {
            // Allow re-arming after we cool down by at least one step
            overheatLockoutActive = false;
        }
    } else {
        overheatLockoutActive = false;
    }

    // Only warn occasionally on high temperature; no periodic logging
    static unsigned long lastTempWarn = 0;
    float warnThreshold = max(overheatShutdownC - 5.0f, (float)OVERHEAT_MIN_LIMIT_C);
    if (temp > warnThreshold && (millis() - lastTempWarn) > 600000UL) { // 10 min cooldown
        simplePrintln("WARNING: High temperature detected (" + String(temp, 1) + " C). Approaching shutdown limit.");
        lastTempWarn = millis();
    }
}

// Performance diagnostics
void checkPerformance() {
    uint32_t currentHeap = ESP.getFreeHeap();
    if (currentHeap < minFreeHeap) {
        minFreeHeap = currentHeap;
    }

    if (isStreaming && (millis() - lastStatsReset) > 30000) {
        uint32_t runtime = millis() - lastStatsReset;
        uint32_t currentRate = (audioPacketsSent * 1000) / runtime;

        if (currentRate > maxPacketRate) maxPacketRate = currentRate;
        if (currentRate < minPacketRate) minPacketRate = currentRate;

        if (currentRate < minAcceptableRate) {
            simplePrintln("PERFORMANCE DEGRADATION DETECTED!");
            simplePrintln("Rate " + String(currentRate) + " < minimum " + String(minAcceptableRate) + " pkt/s");

            if (autoRecoveryEnabled) {
                simplePrintln("AUTO-RECOVERY: Restarting I2S...");
                restartI2S();
                audioPacketsSent = 0;
                lastStatsReset = millis();
                lastI2SReset = millis();
            }
        }
    }
}

// WiFi health check
void checkWiFiHealth() {
    static wl_status_t lastStatus = WL_IDLE_STATUS;
    static bool initialized = false;
    wl_status_t cur = WiFi.status();
    if (!initialized) {
        lastStatus = cur;
        initialized = true;
    }
    if (cur != WL_CONNECTED) {
        simplePrintln("WiFi disconnected! Reconnecting...");
        WiFi.reconnect();
    } else if (lastStatus != WL_CONNECTED) {
        wifiReconnectCount++;
        simplePrintln("WiFi reconnected: " + WiFi.localIP().toString() +
                      " (count " + String(wifiReconnectCount) + ")");
        applyMdnsSetting();
        if (timeSyncEnabled) {
            attemptTimeSync(false);
        }
        mqttRequestReconnect(true);
        mqttPublishState(true);
    }
    lastStatus = cur;

    // Re-apply TX power WITHOUT logging (prevent periodic log spam)
    applyWifiTxPower(false);

    int32_t rssi = WiFi.RSSI();
    if (rssi < -85) {
        simplePrintln("WARNING: Weak WiFi signal: " + String(rssi) + " dBm");
    }
}

// Scheduled reset
void checkScheduledReset() {
    if (!scheduledResetEnabled) return;

    unsigned long uptimeHours = (millis() - bootTime) / 3600000;
    if (uptimeHours >= resetIntervalHours) {
        simplePrintln("SCHEDULED RESET: " + String(resetIntervalHours) + " hours reached");
        delay(1000);
        ESP.restart();
    }
}

// Load only time settings early so the very first boot logs use the persisted local offset.
static void preloadTimeSettingsForEarlyLogs() {
    Preferences bootPrefs;
    if (bootPrefs.begin("audio", true)) {
        timeOffsetMinutes = bootPrefs.getInt("timeOffset", 0);
        timeSyncEnabled = bootPrefs.getBool("timeSyncEn", true);
        bootPrefs.end();
    }
    configureTimeService(timeSyncEnabled);
}

// Load settings from flash
void loadAudioSettings() {
    audioPrefs.begin("audio", false);
    currentSampleRate = audioPrefs.getUInt("sampleRate", DEFAULT_SAMPLE_RATE);
    currentGainFactor = audioPrefs.getFloat("gainFactor", DEFAULT_GAIN_FACTOR);
    currentBufferSize = audioPrefs.getUShort("bufferSize", DEFAULT_BUFFER_SIZE);
    // (1) respect compile-time default 12 on first boot
    i2sShiftBits = audioPrefs.getUChar("shiftBits", i2sShiftBits);
    autoRecoveryEnabled = audioPrefs.getBool("autoRecovery", true);
    scheduledResetEnabled = audioPrefs.getBool("schedReset", false);
    resetIntervalHours = audioPrefs.getUInt("resetHours", 24);
    minAcceptableRate = audioPrefs.getUInt("minRate", 50);
    performanceCheckInterval = audioPrefs.getUInt("checkInterval", 15);
    autoThresholdEnabled = audioPrefs.getBool("thrAuto", true);
    cpuFrequencyMhz = audioPrefs.getUChar("cpuFreq", 160);
    wifiTxPowerDbm = audioPrefs.getFloat("wifiTxDbm", DEFAULT_WIFI_TX_DBM);
    highpassEnabled = audioPrefs.getBool("hpEnable", DEFAULT_HPF_ENABLED);
    highpassCutoffHz = (uint16_t)audioPrefs.getUInt("hpCutoff", DEFAULT_HPF_CUTOFF_HZ);
    overheatProtectionEnabled = audioPrefs.getBool("ohEnable", DEFAULT_OVERHEAT_PROTECTION);
    timeOffsetMinutes = audioPrefs.getInt("timeOffset", 0);
    timeSyncEnabled = audioPrefs.getBool("timeSyncEn", true);
    mdnsEnabled = audioPrefs.getBool("mdnsEn", true);
    streamScheduleEnabled = audioPrefs.getBool("strSchedEn", false);
    streamScheduleStartMin = (uint16_t)audioPrefs.getUInt("strSchStart", 0);
    streamScheduleStopMin = (uint16_t)audioPrefs.getUInt("strSchStop", 0);
    deepSleepScheduleEnabled = audioPrefs.getBool("deepSchSlp", false);
    mqttEnabled = audioPrefs.getBool("mqttEn", false);
    mqttHost = audioPrefs.getString("mqttHost", "");
    mqttPort = (uint16_t)audioPrefs.getUInt("mqttPort", DEFAULT_MQTT_PORT);
    mqttUser = audioPrefs.getString("mqttUser", "");
    mqttPassword = audioPrefs.getString("mqttPass", "");
    mqttTopicPrefix = audioPrefs.getString("mqttTop", "");
    mqttDiscoveryPrefix = audioPrefs.getString("mqttDisc", "homeassistant");
    mqttClientId = audioPrefs.getString("mqttCid", "");
    mqttPublishIntervalSec = (uint16_t)audioPrefs.getUInt("mqttIntSec", DEFAULT_MQTT_PUBLISH_INTERVAL_SEC);
    if (streamScheduleStartMin > 1439) streamScheduleStartMin = 0;
    if (streamScheduleStopMin > 1439) streamScheduleStopMin = 0;
    uint32_t ohLimit = audioPrefs.getUInt("ohThresh", DEFAULT_OVERHEAT_LIMIT_C);
    if (ohLimit < OVERHEAT_MIN_LIMIT_C) ohLimit = OVERHEAT_MIN_LIMIT_C;
    if (ohLimit > OVERHEAT_MAX_LIMIT_C) ohLimit = OVERHEAT_MAX_LIMIT_C;
    ohLimit = OVERHEAT_MIN_LIMIT_C + ((ohLimit - OVERHEAT_MIN_LIMIT_C) / OVERHEAT_LIMIT_STEP_C) * OVERHEAT_LIMIT_STEP_C;
    overheatShutdownC = (float)ohLimit;
    overheatLastReason = audioPrefs.getString("ohReason", "");
    overheatLastTimestamp = audioPrefs.getString("ohStamp", "");
    overheatTripTemp = audioPrefs.getFloat("ohTripC", 0.0f);
    overheatLatched = audioPrefs.getBool("ohLatched", false);
    audioPrefs.end();
    mqttApplyClientSettings(false);

    // Apply timezone/offset immediately after loading persisted settings so that
    // *all* early boot logs (including "Loaded settings") use the correct local time.
    // This does not block; NTP sync is attempted later when Wi-Fi is available.
    configureTimeService(timeSyncEnabled);

    if (autoThresholdEnabled) {
        minAcceptableRate = computeRecommendedMinRate();
    }
    if (overheatLatched) {
        rtspServerEnabled = false;
    }
    // Log the configured TX dBm (not the current enum), snapped for clarity
    float txShown = wifiPowerLevelToDbm(pickWifiPowerLevel(wifiTxPowerDbm));
    simplePrintln("Loaded settings: Rate=" + String(currentSampleRate) +
                  ", Gain=" + String(currentGainFactor, 1) +
                  ", Buffer=" + String(currentBufferSize) +
                  ", WiFiTX=" + String(txShown, 1) + "dBm" +
                  ", shiftBits=" + String(i2sShiftBits) +
                  ", HPF=" + String(highpassEnabled?"on":"off") +
                  ", HPFcut=" + String(highpassCutoffHz) + "Hz");
}

// Save settings to flash
void saveAudioSettings() {
    mqttApplyClientSettings(false);
    audioPrefs.begin("audio", false);
    audioPrefs.putUInt("sampleRate", currentSampleRate);
    audioPrefs.putFloat("gainFactor", currentGainFactor);
    audioPrefs.putUShort("bufferSize", currentBufferSize);
    audioPrefs.putUChar("shiftBits", i2sShiftBits);
    audioPrefs.putBool("autoRecovery", autoRecoveryEnabled);
    audioPrefs.putBool("schedReset", scheduledResetEnabled);
    audioPrefs.putUInt("resetHours", resetIntervalHours);
    audioPrefs.putUInt("minRate", minAcceptableRate);
    audioPrefs.putUInt("checkInterval", performanceCheckInterval);
    audioPrefs.putBool("thrAuto", autoThresholdEnabled);
    audioPrefs.putUChar("cpuFreq", cpuFrequencyMhz);
    audioPrefs.putFloat("wifiTxDbm", wifiTxPowerDbm);
    audioPrefs.putBool("hpEnable", highpassEnabled);
    audioPrefs.putUInt("hpCutoff", (uint32_t)highpassCutoffHz);
    audioPrefs.putBool("ohEnable", overheatProtectionEnabled);
    uint32_t ohLimit = (uint32_t)(overheatShutdownC + 0.5f);
    if (ohLimit < OVERHEAT_MIN_LIMIT_C) ohLimit = OVERHEAT_MIN_LIMIT_C;
    if (ohLimit > OVERHEAT_MAX_LIMIT_C) ohLimit = OVERHEAT_MAX_LIMIT_C;
    audioPrefs.putUInt("ohThresh", ohLimit);
    audioPrefs.putString("ohReason", overheatLastReason);
    audioPrefs.putString("ohStamp", overheatLastTimestamp);
    audioPrefs.putFloat("ohTripC", overheatTripTemp);
    audioPrefs.putBool("ohLatched", overheatLatched);
    audioPrefs.putInt("timeOffset", timeOffsetMinutes);
    audioPrefs.putBool("timeSyncEn", timeSyncEnabled);
    audioPrefs.putBool("mdnsEn", mdnsEnabled);
    audioPrefs.putBool("strSchedEn", streamScheduleEnabled);
    audioPrefs.putUInt("strSchStart", streamScheduleStartMin);
    audioPrefs.putUInt("strSchStop", streamScheduleStopMin);
    audioPrefs.putBool("deepSchSlp", deepSleepScheduleEnabled);
    audioPrefs.putBool("mqttEn", mqttEnabled);
    audioPrefs.putString("mqttHost", mqttHost);
    audioPrefs.putUInt("mqttPort", (uint32_t)mqttPort);
    audioPrefs.putString("mqttUser", mqttUser);
    audioPrefs.putString("mqttPass", mqttPassword);
    audioPrefs.putString("mqttTop", mqttTopicPrefix);
    audioPrefs.putString("mqttDisc", mqttDiscoveryPrefix);
    audioPrefs.putString("mqttCid", mqttClientId);
    audioPrefs.putUInt("mqttIntSec", (uint32_t)mqttPublishIntervalSec);
    audioPrefs.end();

    simplePrintln("Settings saved to flash");
}

// mDNS management
void applyMdnsSetting() {
    if (!mdnsEnabled) {
        if (mdnsRunning) {
            MDNS.end();
            mdnsRunning = false;
            simplePrintln("mDNS disabled");
        }
        return;
    }
    if (mdnsRunning) return;
    if (!MDNS.begin(MDNS_HOSTNAME)) {
        simplePrintln("mDNS start failed");
        mdnsRunning = false;
        return;
    }
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("rtsp", "tcp", 8554);
    mdnsRunning = true;
    simplePrintln("mDNS ready: http://" + String(MDNS_HOSTNAME) + ".local/ ; rtsp://" + String(MDNS_HOSTNAME) + ".local:8554/audio");
}

// Schedule a safe reboot (optionally with factory reset) after delayMs
void scheduleReboot(bool factoryReset, uint32_t delayMs) {
    scheduledFactoryReset = factoryReset;
    scheduledRebootAt = millis() + delayMs;
}

// Compute recommended minimum packet-rate threshold based on current sample rate and buffer size
uint32_t computeRecommendedMinRate() {
    uint32_t buf = max((uint16_t)1, currentBufferSize);
    float expectedPktPerSec = (float)currentSampleRate / (float)buf;
    uint32_t rec = (uint32_t)(expectedPktPerSec * 0.7f + 0.5f); // 70% safety margin
    if (rec < 5) rec = 5;
    return rec;
}

// Restore application settings to safe defaults and persist
void resetToDefaultSettings() {
    simplePrintln("FACTORY RESET: Restoring default settings...");

    // Clear persisted settings in our namespace
    audioPrefs.begin("audio", false);
    audioPrefs.clear();
    audioPrefs.end();

    // Reset runtime variables to defaults
    currentSampleRate = DEFAULT_SAMPLE_RATE;
    currentGainFactor = DEFAULT_GAIN_FACTOR;
    currentBufferSize = DEFAULT_BUFFER_SIZE;
    i2sShiftBits = 12;  // compile-time default respected

    autoRecoveryEnabled = true;
    autoThresholdEnabled = true;
    scheduledResetEnabled = false;
    resetIntervalHours = 24;
    minAcceptableRate = computeRecommendedMinRate();
    performanceCheckInterval = 15;
    cpuFrequencyMhz = 160;
    wifiTxPowerDbm = DEFAULT_WIFI_TX_DBM;
    highpassEnabled = DEFAULT_HPF_ENABLED;
    highpassCutoffHz = DEFAULT_HPF_CUTOFF_HZ;
    overheatProtectionEnabled = DEFAULT_OVERHEAT_PROTECTION;
    overheatShutdownC = (float)DEFAULT_OVERHEAT_LIMIT_C;
    overheatLockoutActive = false;
    overheatTripTemp = 0.0f;
    overheatTriggeredAt = 0;
    overheatLastReason = "";
    overheatLastTimestamp = "";
    overheatSensorFault = false;
    overheatLatched = false;
    lastTemperatureC = 0.0f;
    lastTemperatureValid = false;
    timeOffsetMinutes = 0;
    mdnsEnabled = true;
    streamScheduleEnabled = false;
    streamScheduleStartMin = 0;
    streamScheduleStopMin = 0;
    deepSleepScheduleEnabled = false;
    deepSleepOutsideSinceMs = 0;
    deepSleepStatusCode = "disabled";
    deepSleepNextSleepSec = 0;
    timeSynced = false;
    lastTimeSyncAttempt = 0;
    lastTimeSyncSuccess = 0;
    mqttEnabled = false;
    mqttHost = "";
    mqttPort = DEFAULT_MQTT_PORT;
    mqttUser = "";
    mqttPassword = "";
    mqttTopicPrefix = mqttDefaultTopicPrefix();
    mqttDiscoveryPrefix = "homeassistant";
    mqttClientId = mqttDefaultClientId();
    mqttPublishIntervalSec = DEFAULT_MQTT_PUBLISH_INTERVAL_SEC;
    mqttConnected = false;
    mqttLastError = "disabled";
    mqttDiscoveryPublished = false;
    mqttForceDiscovery = false;

    isStreaming = false;

    saveAudioSettings();

    simplePrintln("Defaults applied. Device will reboot.");
}

// Restart I2S with new parameters
void restartI2S() {
    simplePrintln("Restarting I2S with new parameters...");
    isStreaming = false;

    if (i2s_32bit_buffer) { free(i2s_32bit_buffer); i2s_32bit_buffer = nullptr; }
    if (i2s_16bit_buffer) { free(i2s_16bit_buffer); i2s_16bit_buffer = nullptr; }

    i2s_32bit_buffer = (int32_t*)malloc(currentBufferSize * sizeof(int32_t));
    i2s_16bit_buffer = (int16_t*)malloc(currentBufferSize * sizeof(int16_t));
    if (!i2s_32bit_buffer || !i2s_16bit_buffer) {
        simplePrintln("FATAL: Memory allocation failed after parameter change!");
        ESP.restart();
    }

    setup_i2s_driver();
    // Refresh HPF with current parameters
    updateHighpassCoeffs();
    maxPacketRate = 0;
    minPacketRate = 0xFFFFFFFF;
    simplePrintln("I2S restarted successfully");
}

// Minimal print helpers: Serial + buffered for Web UI
void simplePrint(String message) {
    Serial.print(message);
}

void simplePrintln(String message) {
    String line = "[" + formatLogTimestamp() + "] " + message;
    Serial.println(line);
    webui_pushLog(line);
}

// OTA setup
void setupOTA() {
    // Keep OTA hostname aligned with our mDNS hostname to avoid multiple .local names
    ArduinoOTA.setHostname(MDNS_HOSTNAME);
#ifdef OTA_PASSWORD
    ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
    ArduinoOTA.begin();
}

// I2S setup
void setup_i2s_driver() {
    i2s_driver_uninstall(I2S_NUM_0);

    uint16_t dma_buf_len = (currentBufferSize > 512) ? 512 : currentBufferSize;

    i2s_config_t i2s_config = {};
    i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    i2s_config.sample_rate = currentSampleRate;
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_config.dma_desc_num = 8;
    i2s_config.dma_frame_num = dma_buf_len;

    i2s_pin_config_t pin_config = {};
    pin_config.bck_io_num = I2S_BCLK_PIN;
    pin_config.ws_io_num = I2S_LRCLK_PIN;
    pin_config.data_out_num = I2S_PIN_NO_CHANGE;
    pin_config.data_in_num = I2S_DOUT_PIN;

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);

    // (5) log i2sShiftBits for easier debugging
    simplePrintln("I2S ready: " + String(currentSampleRate) + "Hz, gain " +
                  String(currentGainFactor, 1) + ", buffer " + String(currentBufferSize) +
                  ", shiftBits " + String(i2sShiftBits));
}

static const uint8_t RTSP_WRITE_RETRY_MAX = 8;
static const uint32_t RTSP_WRITE_TIMEOUT_MS = 30UL;

// Write helper with short retry window to tolerate brief TCP backpressure spikes.
static bool writeAll(WiFiClient &client, const uint8_t* data, size_t len) {
    size_t off = 0;
    uint8_t retries = 0;
    unsigned long startMs = millis();
    while (off < len) {
        if (!client.connected()) return false;

        size_t chunk = len - off;
        int avail = client.availableForWrite();
        if (avail > 0 && (size_t)avail < chunk) {
            chunk = (size_t)avail;
        }

        int w = client.write(data + off, chunk);
        if (w > 0) {
            off += (size_t)w;
            retries = 0;
            continue;
        }

        if ((millis() - startMs) > RTSP_WRITE_TIMEOUT_MS || retries >= RTSP_WRITE_RETRY_MAX) {
            return false;
        }
        retries++;
        delay(1);
    }
    return true;
}

static void stopStreamOnWriteFailure(WiFiClient &client, const char* reason) {
    isStreaming = false;
    rtspWriteFailCount++;
    lastStreamStopReason = reason;
    lastStreamStopMs = millis();

    // Drop socket immediately so client can reconnect without waiting for inactivity timeout.
    rtspParseBufferPos = 0;
    rtspParseBuffer[0] = '\0';
    client.stop();

    simplePrintln("STREAMING STOPPED: " + lastStreamStopReason + " | " + buildRtspDiag(client));
    mqttPublishState(true);
}

void sendRTPPacket(WiFiClient &client, int16_t* audioData, int numSamples) {
    if (!client.connected()) return;

    const uint16_t payloadSize = (uint16_t)(numSamples * (int)sizeof(int16_t));
    const uint16_t packetSize = (uint16_t)(12 + payloadSize);

    // RTSP interleaved header: '$' 0x24, channel 0, length
    uint8_t inter[4];
    inter[0] = 0x24;
    inter[1] = 0x00;
    inter[2] = (uint8_t)((packetSize >> 8) & 0xFF);
    inter[3] = (uint8_t)(packetSize & 0xFF);

    // RTP header (12 bytes)
    uint8_t header[12];
    header[0] = 0x80;      // V=2, P=0, X=0, CC=0
    header[1] = 96;        // M=0, PT=96 (dynamic)
    // (3) safe byte-wise filling (no unaligned writes)
    header[2] = (uint8_t)((rtpSequence >> 8) & 0xFF);
    header[3] = (uint8_t)(rtpSequence & 0xFF);
    header[4] = (uint8_t)((rtpTimestamp >> 24) & 0xFF);
    header[5] = (uint8_t)((rtpTimestamp >> 16) & 0xFF);
    header[6] = (uint8_t)((rtpTimestamp >> 8) & 0xFF);
    header[7] = (uint8_t)(rtpTimestamp & 0xFF);
    header[8]  = (uint8_t)((rtpSSRC >> 24) & 0xFF);
    header[9]  = (uint8_t)((rtpSSRC >> 16) & 0xFF);
    header[10] = (uint8_t)((rtpSSRC >> 8) & 0xFF);
    header[11] = (uint8_t)(rtpSSRC & 0xFF);

    // Host->network: per-sample byte-swap (16bit PCM L16 big-endian)
    for (int i = 0; i < numSamples; ++i) {
        uint16_t s = (uint16_t)audioData[i];
        s = (uint16_t)((s << 8) | (s >> 8)); // htons without dependency
        audioData[i] = (int16_t)s;
    }

    if (!writeAll(client, inter, sizeof(inter))) {
        stopStreamOnWriteFailure(client, "RTP write failed (interleaved header)");
        return;
    }
    if (!writeAll(client, header, sizeof(header))) {
        stopStreamOnWriteFailure(client, "RTP write failed (RTP header)");
        return;
    }
    if (!writeAll(client, (uint8_t*)audioData, payloadSize)) {
        stopStreamOnWriteFailure(client, "RTP write failed (audio payload)");
        return;
    }

    rtpSequence++;
    rtpTimestamp += (uint32_t)numSamples;
    audioPacketsSent++;
    lastRtpPacketMs = millis();
}

// Audio streaming
void streamAudio(WiFiClient &client) {
    if (!isStreaming || !client.connected()) return;

    size_t bytesRead = 0;
    esp_err_t result = i2s_read(I2S_NUM_0, i2s_32bit_buffer,
                                currentBufferSize * sizeof(int32_t),
                                &bytesRead, 50 / portTICK_PERIOD_MS);

    if (result == ESP_OK && bytesRead > 0) {
        int samplesRead = bytesRead / sizeof(int32_t);

        // If HPF params changed dynamically, recompute
        if (highpassEnabled && (hpfConfigSampleRate != currentSampleRate || hpfConfigCutoff != highpassCutoffHz)) {
            updateHighpassCoeffs();
        }

        bool clipped = false;
        float peakAbs = 0.0f;
        for (int i = 0; i < samplesRead; i++) {
            float sample = (float)(i2s_32bit_buffer[i] >> i2sShiftBits);
            if (highpassEnabled) sample = hpf.process(sample);
            float amplified = sample * currentGainFactor;
            float aabs = fabsf(amplified);
            if (aabs > peakAbs) peakAbs = aabs;
            if (aabs > 32767.0f) clipped = true;
            if (amplified > 32767.0f) amplified = 32767.0f;
            if (amplified < -32768.0f) amplified = -32768.0f;
            i2s_16bit_buffer[i] = (int16_t)amplified;
        }
        // Update metering after processing the block
        if (peakAbs > 32767.0f) peakAbs = 32767.0f;
        lastPeakAbs16 = (uint16_t)peakAbs;
        audioClippedLastBlock = clipped;
        if (clipped) audioClipCount++;

        // Update peak hold for a short window (~3 s) to match UI polling cadence
        if (lastPeakAbs16 > peakHoldAbs16) {
            peakHoldAbs16 = lastPeakAbs16;
            peakHoldUntilMs = millis() + 3000UL;
        } else if (peakHoldAbs16 > 0 && millis() > peakHoldUntilMs) {
            peakHoldAbs16 = 0;
        }

        sendRTPPacket(client, i2s_16bit_buffer, samplesRead);
    }
}

// RTSP handling
void handleRTSPCommand(WiFiClient &client, String request) {
    String cseq = "1";
    int cseqPos = request.indexOf("CSeq: ");
    if (cseqPos >= 0) {
        cseq = request.substring(cseqPos + 6, request.indexOf("\r", cseqPos));
        cseq.trim();
    }

    int methodEnd = request.indexOf(' ');
    if (methodEnd <= 0) methodEnd = request.length();
    lastRtspCommand = request.substring(0, methodEnd);
    lastRtspCommandMs = millis();
    lastRTSPActivity = lastRtspCommandMs;

    if (request.startsWith("OPTIONS")) {
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n\r\n");

    } else if (request.startsWith("DESCRIBE")) {
        String ip = WiFi.localIP().toString();
        String sdp = "v=0\r\n";
        sdp += "o=- 0 0 IN IP4 " + ip + "\r\n";
        sdp += "s=ESP32 RTSP Mic (" + String(currentSampleRate) + "Hz, 16-bit PCM)\r\n";
        // better compatibility: include actual IP
        sdp += "c=IN IP4 " + ip + "\r\n";
        sdp += "t=0 0\r\n";
        sdp += "m=audio 0 RTP/AVP 96\r\n";
        sdp += "a=rtpmap:96 L16/" + String(currentSampleRate) + "/1\r\n";
        sdp += "a=control:track1\r\n";

        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Content-Type: application/sdp\r\n");
        client.print("Content-Base: rtsp://" + ip + ":8554/audio/\r\n");
        client.print("Content-Length: " + String(sdp.length()) + "\r\n\r\n");
        client.print(sdp);

    } else if (request.startsWith("SETUP")) {
        rtspSessionId = String(random(100000000, 999999999));
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Session: " + rtspSessionId + "\r\n");
        client.print("Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");

    } else if (request.startsWith("PLAY")) {
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Session: " + rtspSessionId + "\r\n");
        client.print("Range: npt=0.000-\r\n\r\n");

        isStreaming = true;
        rtpSequence = 0;
        rtpTimestamp = 0;
        audioPacketsSent = 0;
        lastStatsReset = millis();
        lastRtspPlayMs = millis();
        rtspPlayCount++;
        streamStartedAtMs = millis();
        lastRtpPacketMs = streamStartedAtMs;
        lastStreamStopReason = "none";
        lastStreamStopMs = 0;
        simplePrintln("STREAMING STARTED");
        mqttPublishState(true);

    } else if (request.startsWith("TEARDOWN")) {
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Session: " + rtspSessionId + "\r\n\r\n");
        isStreaming = false;
        lastStreamStopReason = "RTSP TEARDOWN";
        lastStreamStopMs = millis();
        simplePrintln("STREAMING STOPPED (" + lastStreamStopReason + ")");
        mqttPublishState(true);
    } else if (request.startsWith("GET_PARAMETER")) {
        // Many RTSP clients send GET_PARAMETER as keep-alive.
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n\r\n");
    } else {
        client.print("RTSP/1.0 501 Not Implemented\r\n");
        client.print("CSeq: " + cseq + "\r\n\r\n");
        simplePrintln("RTSP unsupported command: " + lastRtspCommand);
    }
}

// RTSP processing
void processRTSP(WiFiClient &client) {
    if (!client.connected()) return;

    while (client.available()) {
        int available = client.available();

        if (rtspParseBufferPos + available >= (int)sizeof(rtspParseBuffer)) {
            available = sizeof(rtspParseBuffer) - rtspParseBufferPos - 1;
            if (available <= 0) {
                simplePrintln("RTSP buffer overflow - resetting");
                rtspParseBufferPos = 0;
                rtspParseBuffer[0] = '\0';
                return;
            }
        }

        int bytesRead = client.read(rtspParseBuffer + rtspParseBufferPos, available);
        if (bytesRead <= 0) return;
        rtspParseBufferPos += bytesRead;
        rtspParseBuffer[rtspParseBufferPos] = '\0';

        while (true) {
            char* endOfHeader = strstr((char*)rtspParseBuffer, "\r\n\r\n");
            if (endOfHeader == nullptr) break;
            *endOfHeader = '\0';
            String request = String((char*)rtspParseBuffer);

            handleRTSPCommand(client, request);

            int headerLen = (endOfHeader - (char*)rtspParseBuffer) + 4;
            if (headerLen > rtspParseBufferPos) {
                rtspParseBufferPos = 0;
                rtspParseBuffer[0] = '\0';
                break;
            }

            int remaining = rtspParseBufferPos - headerLen;
            if (remaining > 0) {
                memmove(rtspParseBuffer, rtspParseBuffer + headerLen, remaining);
            }
            rtspParseBufferPos = remaining;
            rtspParseBuffer[rtspParseBufferPos] = '\0';
        }
    }
}


// Web UI is a separate module (WebUI.*)

void setup() {
    Serial.begin(115200);
    delay(100);

    // (4) seed for random(): combination of time and unique MAC
    randomSeed((uint32_t)micros() ^ (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF));

    bootTime = millis(); // Store boot time
    rtpSSRC = (uint32_t)random(1, 0x7FFFFFFF);
    mqttDeviceId = sanitizeMqttClientId(String("esp32mic_") + buildMqttMacSuffix(), "esp32mic");
    preloadTimeSettingsForEarlyLogs();
    loadBootMetadata();
    simplePrintln("Boot reason: " + rebootReason + ", restart #" + String(restartCounter));

    // Enable external antenna (for XIAO ESP32-C6).
    // NOTE: If you are using a board without the RF switch (or no external antenna), comment out or remove this block.
    pinMode(3, OUTPUT);
    digitalWrite(3, LOW);
    Serial.println("RF switch control enabled (GPIO3 LOW)");
    pinMode(14, OUTPUT);
    digitalWrite(14, HIGH);
    Serial.println("External antenna selected (GPIO14 HIGH)");

    // Load settings from flash
    loadAudioSettings();

    // Allocate buffers with current size
    i2s_32bit_buffer = (int32_t*)malloc(currentBufferSize * sizeof(int32_t));
    i2s_16bit_buffer = (int16_t*)malloc(currentBufferSize * sizeof(int16_t));
    if (!i2s_32bit_buffer || !i2s_16bit_buffer) {
        simplePrintln("FATAL: Memory allocation failed!");
        ESP.restart();
    }

    // WiFi optimization for stable streaming
    WiFi.setSleep(false);

    WiFiManager wm;
    wm.setConnectTimeout(60);
    wm.setConfigPortalTimeout(180);
    if (!wm.autoConnect("ESP32-RTSP-Mic-AP")) {
        simplePrintln("WiFi failed, restarting...");
        ESP.restart();
    }

    simplePrintln("WiFi connected: " + WiFi.localIP().toString());

    // Apply configured WiFi TX power after connect (logs once on change)
    applyWifiTxPower(true);

    // Attempt initial NTP sync (non-blocking). Timezone/offset is configured in loadAudioSettings().
    if (timeSyncEnabled) {
        attemptTimeSync(false);
    }
    applyMdnsSetting();
    logDeepSleepWakeSnapshotIfAny();

    setupOTA();
    setup_i2s_driver();
    updateHighpassCoeffs();

    if (!overheatLatched) {
        rtspServer.begin();
        rtspServer.setNoDelay(true);
        rtspServerEnabled = true;
    } else {
        rtspServerEnabled = false;
        rtspServer.stop();
    }
    bool setupSchedTimeValid = false;
    bool setupSchedAllow = isStreamScheduleAllowedNow(&setupSchedTimeValid);
    if (streamScheduleEnabled && setupSchedTimeValid && !setupSchedAllow && rtspServerEnabled) {
        rtspServerEnabled = false;
        rtspServer.stop();
        simplePrintln("Startup: stream schedule outside window, RTSP server paused.");
    }
    // Web UI
    webui_begin();
    mqttRequestReconnect(true);

    lastStatsReset = millis();
    lastRTSPActivity = millis();
    lastMemoryCheck = millis();
    lastPerformanceCheck = millis();
    lastWiFiCheck = millis();
    minFreeHeap = ESP.getFreeHeap();
    float initialTemp = temperatureRead();
    if (isTemperatureValid(initialTemp)) {
        maxTemperature = initialTemp;
        lastTemperatureC = initialTemp;
        lastTemperatureValid = true;
        overheatSensorFault = false;
    } else {
        maxTemperature = 0.0f;
        lastTemperatureC = 0.0f;
        lastTemperatureValid = false;
        overheatSensorFault = true;
        overheatLastReason = "Thermal protection disabled: temperature sensor unavailable.";
        overheatLastTimestamp = "";
        overheatTripTemp = 0.0f;
        overheatTriggeredAt = 0;
        persistOverheatNote();
        simplePrintln("WARNING: Temperature sensor unavailable at startup. Thermal protection paused.");
    }

    setCpuFrequencyMhz(cpuFrequencyMhz);
    simplePrintln("CPU frequency set to " + String(cpuFrequencyMhz) + " MHz for optimal thermal/performance balance");

    if (!overheatLatched && rtspServerEnabled) {
        simplePrintln("RTSP server ready on port 8554");
        simplePrintln("RTSP URL (IP): rtsp://" + WiFi.localIP().toString() + ":8554/audio");
        if (mdnsEnabled) {
            simplePrintln("RTSP URL (mDNS): rtsp://" + String(MDNS_HOSTNAME) + ".local:8554/audio");
        }
        simplePrintln("You can stream via IP or mDNS (if enabled).");
    } else if (overheatLatched) {
        simplePrintln("RTSP server paused due to thermal latch. Clear via Web UI before resuming streaming.");
    } else {
        simplePrintln("RTSP server paused by stream schedule. It will resume in the next allowed window.");
    }
    simplePrintln("Web UI: http://" + WiFi.localIP().toString() + "/");
    if (mqttEnabled) {
        simplePrintln("MQTT: enabled (" + mqttHost + ":" + String(mqttPort) +
                      "), topic " + mqttTopicPrefix +
                      ", discovery " + mqttDiscoveryPrefix +
                      ", interval " + String((uint32_t)mqttPublishIntervalSec) + "s");
    } else {
        simplePrintln("MQTT: disabled");
    }
}

void loop() {
    ArduinoOTA.handle();

    webui_handleClient();

    if (millis() - lastTempCheck > 60000) { // 1 min
        checkTemperature();
        lastTempCheck = millis();
    }

    if (millis() - lastMemoryCheck > 30000) { // 30 s
        uint32_t currentHeap = ESP.getFreeHeap();
        if (currentHeap < minFreeHeap) minFreeHeap = currentHeap;
        lastMemoryCheck = millis();
    }

    if (millis() - lastPerformanceCheck > (performanceCheckInterval * 60000UL)) {
        checkPerformance();
        lastPerformanceCheck = millis();
    }

    if (millis() - lastWiFiCheck > 30000) { // 30 s
        checkWiFiHealth(); // without TX power log spam
        lastWiFiCheck = millis();
    }

    checkTimeSync();
    if (millis() - lastStreamScheduleCheck > 1000UL) {
        checkStreamSchedule();
        checkDeepSleepSchedule();
        lastStreamScheduleCheck = millis();
    }

    checkScheduledReset();
    checkMqtt();

    // RTSP client management
    if (rtspServerEnabled) {
        if (rtspClient && !rtspClient.connected()) {
            String diag = buildRtspDiag(rtspClient);
            bool wasStreaming = isStreaming;
            bool newDisconnectEvent = false;
            rtspClient.stop();
            isStreaming = false;
            if (lastStreamStopMs == 0) {
                lastStreamStopReason = "TCP client disconnected";
                lastStreamStopMs = millis();
                newDisconnectEvent = true;
            }
            simplePrintln("RTSP client disconnected | " + diag);
            if (newDisconnectEvent || wasStreaming) mqttPublishState(true);
        }

        // Timeout for RTSP clients (30 seconds of inactivity)
        if (rtspClient && rtspClient.connected() && !isStreaming) {
            if (millis() - lastRTSPActivity > 30000) {
                String diag = buildRtspDiag(rtspClient);
                rtspClient.stop();
                if (lastStreamStopMs == 0) {
                    lastStreamStopReason = "RTSP inactivity timeout";
                    lastStreamStopMs = millis();
                }
                simplePrintln("RTSP client timeout - disconnected | " + diag + ", writeFails=" + String(rtspWriteFailCount));
                mqttPublishState(true);
            }
        }

        if (!rtspClient || !rtspClient.connected()) {
            WiFiClient newClient = rtspServer.accept();
            if (newClient) {
                rtspClient = newClient;
                rtspClient.setNoDelay(true);
                rtspParseBufferPos = 0;
                rtspParseBuffer[0] = '\0';
                lastRTSPActivity = millis();
                lastRtspClientConnectMs = millis();
                rtspConnectCount++;
                lastRtspCommand = "none";
                lastRtspCommandMs = 0;
                streamStartedAtMs = 0;
                lastRtpPacketMs = 0;
                lastStreamStopReason = "none";
                lastStreamStopMs = 0;
                lastRtspClientIp = rtspClient.remoteIP().toString();
                simplePrintln("New RTSP client connected from: " + rtspClient.remoteIP().toString());
                mqttPublishState(true);
            }
        }

        if (rtspClient && rtspClient.connected()) {
            if (rtspClient.available()) {
                lastRTSPActivity = millis();
            }
            processRTSP(rtspClient);
            if (isStreaming) {
                streamAudio(rtspClient);
            }
        }
    } else {
        if (rtspClient && rtspClient.connected()) {
            bool wasStreaming = isStreaming;
            rtspClient.stop();
            isStreaming = false;
            if (wasStreaming) mqttPublishState(true);
        }
    }
    // Handle deferred reboot/reset safely here
    if (scheduledRebootAt != 0 && millis() >= scheduledRebootAt) {
        if (scheduledFactoryReset) {
            resetToDefaultSettings();
        }
        delay(50);
        ESP.restart();
    }
}
