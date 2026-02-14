#include <Arduino.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include "WebUI.h"
#include "WebUI_gz.h"

// External variables and functions from main (.ino) – ESP32 RTSP Mic for BirdNET-Go
extern WiFiServer rtspServer;
extern WiFiClient rtspClient;
extern volatile bool isStreaming;
extern uint16_t rtpSequence;
extern uint32_t rtpTimestamp;
extern unsigned long lastStatsReset;
extern unsigned long lastRtspPlayMs;
extern uint32_t rtspPlayCount;
extern unsigned long lastRtspClientConnectMs;
extern unsigned long bootTime;
extern unsigned long lastRTSPActivity;
extern unsigned long lastWiFiCheck;
extern unsigned long lastTempCheck;
extern uint32_t minFreeHeap;
extern float maxTemperature;
extern bool rtspServerEnabled;
extern uint32_t audioPacketsSent;
extern uint32_t currentSampleRate;
extern float currentGainFactor;
extern uint16_t currentBufferSize;
extern uint8_t i2sShiftBits;
extern uint32_t minAcceptableRate;
extern uint32_t performanceCheckInterval;
extern bool autoRecoveryEnabled;
extern uint8_t cpuFrequencyMhz;
extern wifi_power_t currentWifiPowerLevel;
extern void resetToDefaultSettings();
extern bool autoThresholdEnabled;
extern uint32_t computeRecommendedMinRate();
extern bool scheduledResetEnabled;
extern uint32_t resetIntervalHours;
extern void scheduleReboot(bool factoryReset, uint32_t delayMs);
extern uint16_t lastPeakAbs16;
extern uint32_t audioClipCount;
extern bool audioClippedLastBlock;
extern uint16_t peakHoldAbs16;
extern bool overheatProtectionEnabled;
extern float overheatShutdownC;
extern bool overheatLockoutActive;
extern float overheatTripTemp;
extern unsigned long overheatTriggeredAt;
extern String overheatLastReason;
extern String overheatLastTimestamp;
extern bool overheatSensorFault;
extern float lastTemperatureC;
extern bool lastTemperatureValid;
extern bool overheatLatched;

// Local helper: snap requested Wi‑Fi TX power (dBm) to nearest supported step
static float snapWifiTxDbm(float dbm) {
    static const float steps[] = {-1.0f, 2.0f, 5.0f, 7.0f, 8.5f, 11.0f, 13.0f, 15.0f, 17.0f, 18.5f, 19.0f, 19.5f};
    float best = steps[0];
    float bestd = fabsf(dbm - steps[0]);
    for (size_t i=1;i<sizeof(steps)/sizeof(steps[0]);++i){
        float d = fabsf(dbm - steps[i]);
        if (d < bestd){ bestd = d; best = steps[i]; }
    }
    return best;
}

static const uint32_t OH_MIN = 30;
static const uint32_t OH_MAX = 95;
static const uint32_t OH_STEP = 5;
static const char* UI_MUTATION_HEADER = "X-ESP32MIC-CSRF";
static const char* UI_MUTATION_TOKEN = "1";

// Helper functions in main
extern float wifiPowerLevelToDbm(wifi_power_t lvl);
extern String formatUptime(unsigned long seconds);
extern String formatSince(unsigned long eventMs);
extern void restartI2S();
extern void saveAudioSettings();
extern void applyWifiTxPower(bool log);
extern const char* FW_VERSION_STR;
extern bool timeSynced;
extern unsigned long lastTimeSyncSuccess;
extern int32_t timeOffsetMinutes;
extern bool timeSyncEnabled;
extern bool mdnsEnabled;
extern bool mdnsRunning;
extern bool streamScheduleEnabled;
extern uint16_t streamScheduleStartMin;
extern uint16_t streamScheduleStopMin;
extern bool deepSleepScheduleEnabled;
extern String deepSleepStatusCode;
extern uint32_t deepSleepNextSleepSec;
extern bool mqttEnabled;
extern String mqttHost;
extern uint16_t mqttPort;
extern String mqttUser;
extern String mqttPassword;
extern String mqttTopicPrefix;
extern String mqttDiscoveryPrefix;
extern String mqttClientId;
extern uint16_t mqttPublishIntervalSec;
extern bool mqttConnected;
extern String mqttLastError;
extern bool isStreamScheduleAllowedNow(bool* timeValidOut);
extern const char* MDNS_HOSTNAME;
extern bool attemptTimeSync(bool logResult, bool quickMode);
extern String formatDateTime();
extern void configureTimeService(bool enableNtp);
extern void applyMdnsSetting();
extern void mqttRequestReconnect(bool forceDiscovery);
extern void mqttPublishDiscoverySoon();

// Web server and in-memory log ring buffer
static WebServer web(80);
static const size_t LOG_CAP = 120;
static String logBuffer[LOG_CAP];
static size_t logHead = 0;
static size_t logCount = 0;

void webui_pushLog(const String &line) {
    logBuffer[logHead] = line;
    logHead = (logHead + 1) % LOG_CAP;
    if (logCount < LOG_CAP) logCount++;
}

static String jsonEscape(const String &s) {
    String o; o.reserve(s.length()+8);
    for (size_t i=0;i<s.length();++i){char c=s[i]; if(c=='"'||c=='\\'){o+='\\';o+=c;} else if(c=='\n'){o+="\\n";} else {o+=c;}}
    return o;
}

static String formatLocalDateTimeSafe() {
    time_t now = time(nullptr);
    if (now <= 1672531200) return F("unavailable");
    struct tm tmNow;
    if (!localtime_r(&now, &tmNow)) return F("unavailable");
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmNow);
    return String(buf);
}

static String formatUtcDateTimeSafe() {
    time_t now = time(nullptr);
    if (now <= 1672531200) return F("unavailable");
    struct tm tmUtc;
    if (!gmtime_r(&now, &tmUtc)) return F("unavailable");
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmUtc);
    return String(buf);
}

static String profileName(uint16_t buf) {
    // Server-side fallback (English). UI localizes on client by buffer size.
    if (buf <= 256) return F("Ultra-Low Latency (Higher CPU, May have dropouts)");
    if (buf <= 512) return F("Balanced (Moderate CPU, Good stability)");
    if (buf <= 1024) return F("Stable Streaming (Lower CPU, Excellent stability)");
    return F("High Stability (Lowest CPU, Maximum stability)");
}

static void apiSendJSON(const String &json) {
    web.sendHeader("Cache-Control", "no-cache");
    web.send(200, "application/json", json);
}

static bool requireMutationAuth() {
    if (web.hasHeader(UI_MUTATION_HEADER)) {
        String token = web.header(UI_MUTATION_HEADER);
        token.trim();
        if (token == UI_MUTATION_TOKEN) {
            return true;
        }
    }
    web.sendHeader("Cache-Control", "no-cache");
    web.send(403, "application/json", "{\"ok\":false,\"error\":\"forbidden\"}");
    return false;
}

// HTML UI (gzip-compressed in PROGMEM)
static void httpIndex() {
    // Avoid stale UI after firmware updates (browser caches).
    web.sendHeader("Cache-Control", "no-store");
    web.sendHeader("Content-Encoding", "gzip");
    web.sendHeader("Vary", "Accept-Encoding");
    web.send_P(
        200,
        PSTR("text/html; charset=utf-8"),
        reinterpret_cast<PGM_P>(WEBUI_INDEX_GZ),
        WEBUI_INDEX_GZ_LEN
    );
}

// HTTP handlery

static void httpStatus() {
    unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
    String uptimeStr = formatUptime(uptimeSeconds);
    String localTimeStr = formatLocalDateTimeSafe();
    String utcTimeStr = formatUtcDateTimeSafe();
    unsigned long runtime = millis() - lastStatsReset;
    uint32_t currentRate = (isStreaming && runtime > 1000) ? (audioPacketsSent * 1000) / runtime : 0;
    String json = "{";
    json += "\"fw_version\":\"" + String(FW_VERSION_STR) + "\",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"stream_url_ip\":\"rtsp://" + WiFi.localIP().toString() + ":8554/audio\",";
    json += "\"stream_url_mdns\":\"rtsp://" + String(MDNS_HOSTNAME) + ".local:8554/audio\",";
    json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"wifi_tx_dbm\":" + String(wifiPowerLevelToDbm(currentWifiPowerLevel),1) + ",";
    json += "\"free_heap_kb\":" + String(ESP.getFreeHeap()/1024) + ",";
    json += "\"min_free_heap_kb\":" + String(minFreeHeap/1024) + ",";
    json += "\"uptime\":\"" + uptimeStr + "\",";
    json += "\"time_synced\":" + String(timeSynced?"true":"false") + ",";
    json += "\"time_sync_enabled\":" + String(timeSyncEnabled?"true":"false") + ",";
    json += "\"last_time_sync\":\"" + jsonEscape(timeSynced ? formatSince(lastTimeSyncSuccess) : String("never")) + "\",";
    json += "\"local_time\":\"" + jsonEscape(localTimeStr) + "\",";
    json += "\"utc_time\":\"" + jsonEscape(utcTimeStr) + "\",";
    json += "\"time_offset_min\":" + String(timeOffsetMinutes) + ",";
    json += "\"mdns_enabled\":" + String(mdnsEnabled?"true":"false") + ",";
    json += "\"mqtt_enabled\":" + String(mqttEnabled?"true":"false") + ",";
    json += "\"mqtt_connected\":" + String(mqttConnected?"true":"false") + ",";
    json += "\"mqtt_host\":\"" + jsonEscape(mqttHost) + "\",";
    json += "\"mqtt_port\":" + String((uint32_t)mqttPort) + ",";
    json += "\"mqtt_user\":\"" + jsonEscape(mqttUser) + "\",";
    json += "\"mqtt_topic\":\"" + jsonEscape(mqttTopicPrefix) + "\",";
    json += "\"mqtt_discovery\":\"" + jsonEscape(mqttDiscoveryPrefix) + "\",";
    json += "\"mqtt_client_id\":\"" + jsonEscape(mqttClientId) + "\",";
    json += "\"mqtt_interval_sec\":" + String((uint32_t)mqttPublishIntervalSec) + ",";
    json += "\"mqtt_last_error\":\"" + jsonEscape(mqttLastError) + "\",";
    bool schedTimeValid = false;
    bool schedAllowNow = isStreamScheduleAllowedNow(&schedTimeValid);
    json += "\"stream_schedule_enabled\":" + String(streamScheduleEnabled?"true":"false") + ",";
    json += "\"stream_schedule_start_min\":" + String(streamScheduleStartMin) + ",";
    json += "\"stream_schedule_stop_min\":" + String(streamScheduleStopMin) + ",";
    json += "\"stream_schedule_allow_now\":" + String(schedAllowNow?"true":"false") + ",";
    json += "\"stream_schedule_time_valid\":" + String(schedTimeValid?"true":"false") + ",";
    json += "\"deep_sleep_sched_enabled\":" + String(deepSleepScheduleEnabled?"true":"false") + ",";
    json += "\"deep_sleep_status_code\":\"" + jsonEscape(deepSleepStatusCode) + "\",";
    json += "\"deep_sleep_next_sec\":" + String(deepSleepNextSleepSec) + ",";
    json += "\"rtsp_server_enabled\":" + String(rtspServerEnabled?"true":"false") + ",";
    if (rtspClient && rtspClient.connected()) json += "\"client\":\"" + rtspClient.remoteIP().toString() + "\","; else json += "\"client\":\"\",";
    json += "\"streaming\":" + String(isStreaming?"true":"false") + ",";
    json += "\"current_rate_pkt_s\":" + String(currentRate) + ",";
    json += "\"last_rtsp_connect\":\"" + jsonEscape(formatSince(lastRtspClientConnectMs)) + "\",";
    json += "\"last_stream_start\":\"" + jsonEscape(formatSince(lastRtspPlayMs)) + "\"";
    json += "}";
    apiSendJSON(json);
}

static void httpAudioStatus() {
    float latency_ms = (float)currentBufferSize / currentSampleRate * 1000.0f;
    String json = "{";
    json += "\"sample_rate\":" + String(currentSampleRate) + ",";
    json += "\"gain\":" + String(currentGainFactor,2) + ",";
    json += "\"buffer_size\":" + String(currentBufferSize) + ",";
    json += "\"i2s_shift\":" + String(i2sShiftBits) + ",";
    json += "\"latency_ms\":" + String(latency_ms,1) + ",";
    extern bool highpassEnabled; extern uint16_t highpassCutoffHz;
    json += "\"profile\":\"" + jsonEscape(profileName(currentBufferSize)) + "\",";
    json += "\"hp_enable\":" + String(highpassEnabled?"true":"false") + ",";
    json += "\"hp_cutoff_hz\":" + String((uint32_t)highpassCutoffHz) + ",";
    // Metering/clipping
    uint16_t p = (peakHoldAbs16 > 0) ? peakHoldAbs16 : lastPeakAbs16;
    float peak_pct = (p <= 0) ? 0.0f : (100.0f * (float)p / 32767.0f);
    float peak_dbfs = (p <= 0) ? -90.0f : (20.0f * log10f((float)p / 32767.0f));
    json += "\"peak_pct\":" + String(peak_pct,1) + ",";
    json += "\"peak_dbfs\":" + String(peak_dbfs,1) + ",";
    json += "\"clip\":" + String(audioClippedLastBlock?"true":"false") + ",";
    json += "\"clip_count\":" + String(audioClipCount);
    json += "}";
    apiSendJSON(json);
}

static void httpPerfStatus() {
    String json = "{";
    json += "\"restart_threshold_pkt_s\":" + String(minAcceptableRate) + ",";
    json += "\"check_interval_min\":" + String(performanceCheckInterval) + ",";
    json += "\"auto_recovery\":" + String(autoRecoveryEnabled?"true":"false") + ",";
    json += "\"auto_threshold\":" + String(autoThresholdEnabled?"true":"false") + ",";
    json += "\"recommended_min_rate\":" + String(computeRecommendedMinRate()) + ",";
    json += "\"scheduled_reset\":" + String(scheduledResetEnabled?"true":"false") + ",";
    json += "\"reset_hours\":" + String(resetIntervalHours) + "}";
    apiSendJSON(json);
}

static void httpThermal() {
    String since = "";
    if (overheatTripTemp > 0.0f && overheatTriggeredAt != 0) {
        since = formatSince(overheatTriggeredAt);
    }
    bool manualRequired = overheatLatched || (!rtspServerEnabled && overheatProtectionEnabled && overheatTripTemp > 0.0f);
    String json = "{";
    if (lastTemperatureValid) {
        json += "\"current_c\":" + String(lastTemperatureC,1) + ",";
    } else {
        json += "\"current_c\":null,";
    }
    json += "\"current_valid\":" + String(lastTemperatureValid?"true":"false") + ",";
    json += "\"max_c\":" + String(maxTemperature,1) + ",";
    json += "\"cpu_mhz\":" + String(getCpuFrequencyMhz()) + ",";
    json += "\"protection_enabled\":" + String(overheatProtectionEnabled?"true":"false") + ",";
    json += "\"shutdown_c\":" + String(overheatShutdownC,0) + ",";
    json += "\"latched\":" + String(overheatLockoutActive?"true":"false") + ",";
    json += "\"latched_persist\":" + String(overheatLatched?"true":"false") + ",";
    json += "\"sensor_fault\":" + String(overheatSensorFault?"true":"false") + ",";
    json += "\"last_trip_c\":" + String(overheatTripTemp,1) + ",";
    json += "\"last_reason\":\"" + jsonEscape(overheatLastReason) + "\",";
    json += "\"last_trip_ts\":\"" + jsonEscape(overheatLastTimestamp) + "\",";
    json += "\"last_trip_since\":\"" + jsonEscape(since) + "\",";
    json += "\"manual_restart\":" + String(manualRequired?"true":"false");
    json += "}";
    apiSendJSON(json);
}

static void httpThermalClear() {
    if (!requireMutationAuth()) return;

    if (overheatLatched) {
        overheatLatched = false;
        overheatLockoutActive = false;
        overheatTripTemp = 0.0f;
        overheatTriggeredAt = 0;
        overheatLastReason = String("Thermal latch cleared manually.");
        overheatLastTimestamp = String("");
        if (!rtspServerEnabled) {
            rtspServer.begin();
            rtspServer.setNoDelay(true);
            rtspServerEnabled = true;
        }
        saveAudioSettings();
        webui_pushLog(F("UI action: thermal_latch_clear"));
        apiSendJSON(F("{\"ok\":true}"));
    } else {
        apiSendJSON(F("{\"ok\":false}"));
    }
}

static void httpLogs() {
    String out;
    for (size_t i=0;i<logCount;i++){
        size_t idx = (logHead + LOG_CAP - logCount + i) % LOG_CAP;
        out += logBuffer[idx]; out += '\n';
    }
    if (web.hasArg("download")) {
        web.sendHeader("Content-Disposition", "attachment; filename=\"esp32mic-log.txt\"");
    }
    web.sendHeader("Cache-Control", "no-cache");
    web.send(200, "text/plain; charset=utf-8", out);
}

static void httpActionServerStart(){
    if (!requireMutationAuth()) return;

    if (overheatLatched) {
        webui_pushLog(F("Server start blocked: thermal protection latched"));
        apiSendJSON(F("{\"ok\":false,\"error\":\"thermal_latched\"}"));
        return;
    }
    if (!rtspServerEnabled) {
        rtspServerEnabled=true; rtspServer.begin(); rtspServer.setNoDelay(true);
        overheatLockoutActive = false;
    }
    webui_pushLog(F("UI action: server_start"));
    apiSendJSON(F("{\"ok\":true}"));
}
static void httpActionServerStop(){
    if (!requireMutationAuth()) return;

    rtspServerEnabled=false; if (rtspClient && rtspClient.connected()) rtspClient.stop(); isStreaming=false; rtspServer.stop();
    webui_pushLog(F("UI action: server_stop"));
    apiSendJSON(F("{\"ok\":true}"));
}
static void httpActionResetI2S(){
    if (!requireMutationAuth()) return;

    webui_pushLog(F("UI action: reset_i2s"));
    restartI2S(); apiSendJSON(F("{\"ok\":true}"));
}

static void httpActionTimeSync(){
    if (!requireMutationAuth()) return;

    bool ok = attemptTimeSync(true, true);
    apiSendJSON(String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

static void httpActionNetworkReset(){
    if (!requireMutationAuth()) return;

    webui_pushLog(F("UI action: network_reset (clearing Wi-Fi and rebooting)"));
    WiFiManager wm;
    wm.resetSettings();
    apiSendJSON(F("{\"ok\":true}"));
    scheduleReboot(false, 800);
}

static void httpActionMqttDiscovery(){
    if (!requireMutationAuth()) return;

    webui_pushLog(F("UI action: mqtt_discovery"));
    mqttPublishDiscoverySoon();
    apiSendJSON(F("{\"ok\":true}"));
}

static bool valueArgTrimmed(String& out) {
    if (!web.hasArg("value")) return false;
    out = web.arg("value");
    out.trim();
    return out.length() > 0;
}

static bool parseUInt32Strict(const String& input, uint32_t& out) {
    const char* s = input.c_str();
    size_t i = 0;
    if (s[i] == '\0') return false;
    for (; s[i] != '\0'; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    errno = 0;
    char* end = nullptr;
    unsigned long v = strtoul(s, &end, 10);
    if (errno == ERANGE || end == s || *end != '\0') return false;
    out = (uint32_t)v;
    return true;
}

static bool parseInt32Strict(const String& input, int32_t& out) {
    const char* s = input.c_str();
    size_t i = 0;
    if (s[i] == '+' || s[i] == '-') ++i;
    if (s[i] == '\0') return false;
    for (; s[i] != '\0'; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    errno = 0;
    char* end = nullptr;
    long v = strtol(s, &end, 10);
    if (errno == ERANGE || end == s || *end != '\0') return false;
    if (v < (long)INT32_MIN || v > (long)INT32_MAX) return false;
    out = (int32_t)v;
    return true;
}

static bool parseFloatStrict(const String& input, float& out) {
    const char* s = input.c_str();
    if (*s == '\0') return false;
    errno = 0;
    char* end = nullptr;
    float v = strtof(s, &end);
    if (errno == ERANGE || end == s || *end != '\0') return false;
    if (!isfinite(v)) return false;
    out = v;
    return true;
}

static inline bool argToFloat(float &out) {
    String v;
    if (!valueArgTrimmed(v)) return false;
    return parseFloatStrict(v, out);
}
static inline bool argToUInt(uint32_t &out) {
    String v;
    if (!valueArgTrimmed(v)) return false;
    return parseUInt32Strict(v, out);
}
static inline bool argToUShort(uint16_t &out) {
    uint32_t v = 0;
    if (!argToUInt(v) || v > 65535u) return false;
    out = (uint16_t)v;
    return true;
}
static inline bool argToUChar(uint8_t &out) {
    uint32_t v = 0;
    if (!argToUInt(v) || v > 255u) return false;
    out = (uint8_t)v;
    return true;
}
static inline bool argToInt(int32_t &out) {
    String v;
    if (!valueArgTrimmed(v)) return false;
    return parseInt32Strict(v, out);
}

static void httpSet() {
    if (!requireMutationAuth()) return;

    if (!web.hasArg("key")) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"missing_key\"}"));
        return;
    }

    String key = web.arg("key");
    String val = web.hasArg("value") ? web.arg("value") : String("");
    if (key == "mqtt_pass") {
        webui_pushLog(F("UI set: mqtt_pass=<hidden>"));
    } else if (val.length()) {
        webui_pushLog(String("UI set: ")+key+"="+val);
    }

    bool handled = false;
    bool applied = false;

    if (key == "gain") {
        handled = true;
        float v;
        if (argToFloat(v) && v >= 0.1f && v <= 100.0f) { currentGainFactor = v; saveAudioSettings(); restartI2S(); applied = true; }
    }
    else if (key == "rate") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 8000 && v <= 96000) { currentSampleRate = v; if (autoThresholdEnabled) { minAcceptableRate = computeRecommendedMinRate(); } saveAudioSettings(); restartI2S(); applied = true; }
    }
    else if (key == "buffer") {
        handled = true;
        uint16_t v;
        if (argToUShort(v) && v >= 256 && v <= 8192) { currentBufferSize = v; if (autoThresholdEnabled) { minAcceptableRate = computeRecommendedMinRate(); } saveAudioSettings(); restartI2S(); applied = true; }
    }
    else if (key == "shift") {
        handled = true;
        uint8_t v;
        if (argToUChar(v) && v <= 24) { i2sShiftBits = v; saveAudioSettings(); restartI2S(); applied = true; }
    }
    else if (key == "wifi_tx") {
        handled = true;
        float v;
        if (argToFloat(v) && v >= -1.0f && v <= 19.5f) { extern float wifiTxPowerDbm; wifiTxPowerDbm = snapWifiTxDbm(v); applyWifiTxPower(true); saveAudioSettings(); applied = true; }
    }
    else if (key == "auto_recovery") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") { autoRecoveryEnabled = (v == "on"); saveAudioSettings(); applied = true; }
    }
    else if (key == "thr_mode") {
        handled = true;
        String v = web.arg("value");
        if (v == "auto") { autoThresholdEnabled = true; minAcceptableRate = computeRecommendedMinRate(); saveAudioSettings(); applied = true; }
        else if (v == "manual") { autoThresholdEnabled = false; saveAudioSettings(); applied = true; }
    }
    else if (key == "min_rate") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 5 && v <= 200) { minAcceptableRate = v; saveAudioSettings(); applied = true; }
    }
    else if (key == "check_interval") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 1 && v <= 60) { performanceCheckInterval = v; saveAudioSettings(); applied = true; }
    }
    else if (key == "sched_reset") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") { extern bool scheduledResetEnabled; scheduledResetEnabled = (v == "on"); saveAudioSettings(); applied = true; }
    }
    else if (key == "reset_hours") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 1 && v <= 168) { extern uint32_t resetIntervalHours; resetIntervalHours = v; saveAudioSettings(); applied = true; }
    }
    else if (key == "cpu_freq") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 40 && v <= 160) { cpuFrequencyMhz = (uint8_t)v; setCpuFrequencyMhz(cpuFrequencyMhz); saveAudioSettings(); applied = true; }
    }
    else if (key == "hp_enable") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") { extern bool highpassEnabled; highpassEnabled = (v == "on"); extern void updateHighpassCoeffs(); updateHighpassCoeffs(); saveAudioSettings(); applied = true; }
    }
    else if (key == "hp_cutoff") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 10 && v <= 10000) { extern uint16_t highpassCutoffHz; highpassCutoffHz = (uint16_t)v; extern void updateHighpassCoeffs(); updateHighpassCoeffs(); saveAudioSettings(); applied = true; }
    }
    else if (key == "oh_enable") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") { overheatProtectionEnabled = (v == "on"); if (!overheatProtectionEnabled) { overheatLockoutActive = false; } saveAudioSettings(); applied = true; }
    }
    else if (key == "oh_limit") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= OH_MIN && v <= OH_MAX) { uint32_t snapped = OH_MIN + ((v - OH_MIN) / OH_STEP) * OH_STEP; overheatShutdownC = (float)snapped; overheatLockoutActive = false; saveAudioSettings(); applied = true; }
    }
    else if (key == "time_offset") {
        handled = true;
        int32_t v;
        if (argToInt(v) && v >= -720 && v <= 840) { timeOffsetMinutes = v; configureTimeService(timeSyncEnabled); saveAudioSettings(); applied = true; }
    }
    else if (key == "time_sync") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") {
            timeSyncEnabled = (v == "on");
            configureTimeService(timeSyncEnabled);
            if (timeSyncEnabled) {
                attemptTimeSync(false, true);
            }
            saveAudioSettings();
            applied = true;
        }
    }
    else if (key == "stream_sched") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") { streamScheduleEnabled = (v == "on"); saveAudioSettings(); applied = true; }
    }
    else if (key == "stream_start_min") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v <= 1439) { streamScheduleStartMin = (uint16_t)v; saveAudioSettings(); applied = true; }
    }
    else if (key == "stream_stop_min") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v <= 1439) { streamScheduleStopMin = (uint16_t)v; saveAudioSettings(); applied = true; }
    }
    else if (key == "deep_sleep_sched") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") {
            deepSleepScheduleEnabled = (v == "on");
            if (!deepSleepScheduleEnabled) {
                deepSleepStatusCode = "disabled";
                deepSleepNextSleepSec = 0;
            }
            saveAudioSettings();
            applied = true;
        }
    }
    else if (key == "mdns_enable") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") { mdnsEnabled = (v == "on"); applyMdnsSetting(); saveAudioSettings(); applied = true; }
    }
    else if (key == "mqtt_enable") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") {
            mqttEnabled = (v == "on");
            saveAudioSettings();
            mqttRequestReconnect(true);
            applied = true;
        }
    }
    else if (key == "mqtt_host") {
        handled = true;
        String v = web.arg("value");
        v.trim();
        if (v.length() <= 96) {
            mqttHost = v;
            saveAudioSettings();
            mqttRequestReconnect(true);
            applied = true;
        }
    }
    else if (key == "mqtt_port") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 1 && v <= 65535) {
            mqttPort = (uint16_t)v;
            saveAudioSettings();
            mqttRequestReconnect(true);
            applied = true;
        }
    }
    else if (key == "mqtt_user") {
        handled = true;
        String v = web.arg("value");
        v.trim();
        if (v.length() <= 96) {
            mqttUser = v;
            saveAudioSettings();
            mqttRequestReconnect(true);
            applied = true;
        }
    }
    else if (key == "mqtt_pass") {
        handled = true;
        String v = web.arg("value");
        if (v.length() <= 128) {
            mqttPassword = v;
            saveAudioSettings();
            mqttRequestReconnect(true);
            applied = true;
        }
    }
    else if (key == "mqtt_topic") {
        handled = true;
        String v = web.arg("value");
        v.trim();
        if (v.length() <= 128) {
            mqttTopicPrefix = v;
            saveAudioSettings();
            mqttRequestReconnect(true);
            applied = true;
        }
    }
    else if (key == "mqtt_discovery") {
        handled = true;
        String v = web.arg("value");
        v.trim();
        if (v.length() <= 128) {
            mqttDiscoveryPrefix = v;
            saveAudioSettings();
            mqttRequestReconnect(true);
            applied = true;
        }
    }
    else if (key == "mqtt_client_id") {
        handled = true;
        String v = web.arg("value");
        v.trim();
        if (v.length() <= 96) {
            mqttClientId = v;
            saveAudioSettings();
            mqttRequestReconnect(true);
            applied = true;
        }
    }
    else if (key == "mqtt_interval") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 10 && v <= 3600) {
            mqttPublishIntervalSec = (uint16_t)v;
            saveAudioSettings();
            applied = true;
        }
    }

    if (!handled) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"unknown_key\"}"));
        return;
    }
    if (!applied) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"invalid_value\"}"));
        return;
    }
    apiSendJSON(F("{\"ok\":true}"));
}

static void httpActionReboot(){
    if (!requireMutationAuth()) return;

    webui_pushLog(F("UI action: reboot"));
    apiSendJSON(F("{\"ok\":true}"));
    scheduleReboot(false, 600);
}

static void httpActionFactoryReset(){
    if (!requireMutationAuth()) return;

    webui_pushLog(F("UI action: factory_reset"));
    apiSendJSON(F("{\"ok\":true}"));
    scheduleReboot(true, 600);
}

void webui_begin() {
    web.on("/", httpIndex);
    web.on("/api/status", httpStatus);
    web.on("/api/audio_status", httpAudioStatus);
    web.on("/api/perf_status", httpPerfStatus);
    web.on("/api/thermal", httpThermal);
    web.on("/api/thermal/clear", HTTP_POST, httpThermalClear);
    web.on("/api/logs", httpLogs);
    web.on("/api/action/server_start", HTTP_POST, httpActionServerStart);
    web.on("/api/action/server_stop", HTTP_POST, httpActionServerStop);
    web.on("/api/action/reset_i2s", HTTP_POST, httpActionResetI2S);
    web.on("/api/action/time_sync", HTTP_POST, httpActionTimeSync);
    web.on("/api/action/network_reset", HTTP_POST, httpActionNetworkReset);
    web.on("/api/action/mqtt_discovery", HTTP_POST, httpActionMqttDiscovery);
    web.on("/api/action/reboot", HTTP_POST, httpActionReboot);
    web.on("/api/action/factory_reset", HTTP_POST, httpActionFactoryReset);
    web.on("/api/set", HTTP_POST, httpSet);
    static const char* headerKeys[] = { UI_MUTATION_HEADER };
    web.collectHeaders(headerKeys, 1);
    web.begin();
}

void webui_handleClient() {
    web.handleClient();
}
