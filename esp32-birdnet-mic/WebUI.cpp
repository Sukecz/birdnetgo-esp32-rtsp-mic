#include <Arduino.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include "WebUI.h"
#include "WebUI_gz.h"

// External variables and functions from main (.ino) – ESP32 RTSP Mic for BirdNET-Go / BirdNET-Pi
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
extern uint32_t audioI2SErrorCount;
extern uint32_t audioRingBufferDropCount;
extern uint32_t audioRingBufferChunkCount;
extern uint32_t audioRingBufferFlushCount;
extern uint32_t rtspWriteStallCount;
extern uint32_t rtspWriteTimeoutCount;
extern size_t audioRingBufferCapacityBytes;
extern volatile bool audioProducerRunning;
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
extern void scheduleWifiReconnect(const uint8_t *bssid, uint32_t delayMs);
extern uint16_t lastPeakAbs16;
extern uint32_t audioClipCount;
extern bool audioClippedLastBlock;
extern uint16_t peakHoldAbs16;
extern bool overheatProtectionEnabled;
extern float overheatShutdownC;
extern bool overheatLockoutActive;
extern uint8_t overheatConsecutiveSamples;
extern float overheatTripTemp;
extern unsigned long overheatTriggeredAt;
extern String overheatLastReason;
extern String overheatLastTimestamp;
extern bool overheatSensorFault;
extern float lastTemperatureC;
extern bool lastTemperatureValid;
extern bool overheatLatched;
struct StreamProfileConfig { uint8_t target; };
extern StreamProfileConfig streamProfiles[2];
extern bool streamEnabled[2];
extern uint8_t maxActiveClients;
struct StreamStats { uint8_t clientCount; bool streaming; uint32_t packetsSent; unsigned long statsResetMs; unsigned long lastConnectMs; unsigned long lastPlayMs; };
extern StreamStats streamStats[2];
extern void getStreamClientCounts(uint8_t &s1, uint8_t &s2);
extern String getRtspClientSummary();
extern void stopAllRtspClients(const char* reason);

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
static const char* OFFICIAL_OTA_HOST = "esp32mic.msmeteo.cz";
static const uint32_t OTA_VERSION_CACHE_MS = 10UL * 60UL * 1000UL;
static const uint32_t OTA_VERSION_ERROR_CACHE_MS = 60UL * 60UL * 1000UL;
static bool otaUploadOk = false;
static String otaUploadError;
static bool otaVersionChecked = false;
static unsigned long otaVersionCheckedAtMs = 0;
static String otaLatestVersion;
static String otaVersionCheckError;
static SemaphoreHandle_t otaVersionMutex = nullptr;
static volatile bool otaVersionCheckRunning = false;

static bool fetchLatestOtaVersion(bool forceRefresh, String &latestOut, String &errorOut);
static bool startOtaVersionCheckAsync();
static void getOtaVersionSnapshot(bool &checkedOut, unsigned long &checkedAtOut,
                                  String &latestOut, String &errorOut, bool &runningOut);
static bool otaVersionCacheFresh(bool checked, unsigned long checkedAt, const String &error);
static bool isNewerFirmwareAvailable(const String &latestVersion);

// Helper functions in main
extern float wifiPowerLevelToDbm(wifi_power_t lvl);
extern String formatUptime(unsigned long seconds);
extern String formatSince(unsigned long eventMs);
extern bool restartI2S();
extern bool applyAudioConfig(uint32_t newRate, float newGain, uint16_t newBuffer, uint8_t newShift);
extern uint16_t maxHighpassCutoffForRate(uint32_t sampleRate);
extern void saveAudioSettings();
extern void applyWifiTxPower(bool log);
extern const char* FW_VERSION_STR;
extern const char* FW_BOARD_ID_STR;
extern const char* FW_BOARD_NAME_STR;
extern const char* FW_CHIP_FAMILY_STR;
extern const char* FW_OTA_ARTIFACT_STR;
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

static String getDefaultOtaUrl() {
    if (!FW_OTA_ARTIFACT_STR || FW_OTA_ARTIFACT_STR[0] == '\0') return String();
    return String("http://") + OFFICIAL_OTA_HOST + "/" + FW_OTA_ARTIFACT_STR;
}

static String getOtaVersionUrl() {
    return String("http://") + OFFICIAL_OTA_HOST + "/ota-version.txt";
}
extern uint16_t mqttPublishIntervalSec;
extern bool mqttConnected;
extern String mqttLastError;
extern bool isStreamScheduleAllowedNow(bool* timeValidOut);
extern String mdnsHostname;
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

static bool hasMutationAuth() {
    if (web.hasHeader(UI_MUTATION_HEADER)) {
        String token = web.header(UI_MUTATION_HEADER);
        token.trim();
        if (token == UI_MUTATION_TOKEN) {
            return true;
        }
    }
    return false;
}

static bool requireMutationAuth() {
    if (hasMutationAuth()) return true;

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

static String htmlEscape(const String &s) {
    String o;
    o.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c == '&') o += F("&amp;");
        else if (c == '<') o += F("&lt;");
        else if (c == '>') o += F("&gt;");
        else if (c == '"') o += F("&quot;");
        else o += c;
    }
    return o;
}

static void sendOtaPage(const String &message = String(), bool ok = true) {
    String deviceUrl = "http://" + WiFi.localIP().toString() + "/ota";
    String defaultOtaUrl = getDefaultOtaUrl();
    bool versionChecked = false;
    bool versionChecking = false;
    unsigned long versionCheckedAt = 0;
    String latestVersion;
    String versionError;
    getOtaVersionSnapshot(versionChecked, versionCheckedAt, latestVersion, versionError, versionChecking);
    if (!otaVersionCacheFresh(versionChecked, versionCheckedAt, versionError)) {
        startOtaVersionCheckAsync();
        getOtaVersionSnapshot(versionChecked, versionCheckedAt, latestVersion, versionError, versionChecking);
    }
    bool versionCheckOk = versionChecked && versionError.length() == 0;
    bool updateAvailable = versionCheckOk && isNewerFirmwareAvailable(latestVersion);
    String html;
    html.reserve(8000);
    html += F("<!doctype html><html><head><meta charset='utf-8'>");
    html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
    html += F("<title>Firmware update</title><style>");
    html += F(":root{--bg:#f6f7fb;--fg:#0f172a;--muted:#526079;--card:#fff;--border:#d7dee9;--acc:#0ea5e9;--ok:#10b981;--warn:#f59e0b;--bad:#ef4444}");
    html += F("@media(prefers-color-scheme:dark){:root{--bg:#07101f;--fg:#e8eefc;--muted:#a7b3cb;--card:#111a2c;--border:#26344f}}");
    html += F("*{box-sizing:border-box}body{margin:0;background:radial-gradient(900px 600px at 10% -10%,rgba(14,165,233,.18),transparent),var(--bg);color:var(--fg);font-family:system-ui,-apple-system,Segoe UI,sans-serif}.wrap{max-width:760px;margin:0 auto;padding:24px 14px}.card{background:var(--card);border:1px solid var(--border);border-radius:16px;padding:18px;margin:0 0 14px;box-shadow:0 12px 34px rgba(0,0,0,.10)}h1{font-size:26px;margin:0 0 8px}.muted{color:var(--muted);line-height:1.45}.msg,.version-state{border-radius:12px;padding:12px 14px;margin:0 0 14px;border:1px solid var(--border)}.version-state.update{background:rgba(245,158,11,.16);border-color:var(--warn);font-size:17px;font-weight:850}.version-state.current{background:rgba(16,185,129,.10);border-color:var(--ok)}.ok{color:var(--ok)}.bad{color:var(--bad)}input{width:100%;padding:11px;border-radius:12px;border:1px solid var(--border);background:transparent;color:var(--fg);font:inherit;margin:8px 0 12px}button,a.btn{display:inline-block;border:1px solid var(--border);background:linear-gradient(120deg,var(--acc),#f59e0b);color:#082f49;border-radius:12px;padding:11px 14px;font-weight:800;text-decoration:none;cursor:pointer}button.secondary,a.secondary{background:transparent;color:var(--fg)}button:disabled{opacity:.55;cursor:not-allowed}code{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;word-break:break-all}.ota-progress{position:fixed;inset:0;z-index:9999;display:none;place-items:center;padding:20px;background:rgba(2,6,23,.78);backdrop-filter:blur(4px)}.ota-progress.active{display:grid}.ota-progress-box{width:min(430px,100%);padding:26px;text-align:center;background:var(--card);border:1px solid var(--border);border-radius:18px;box-shadow:0 24px 70px rgba(0,0,0,.4)}.spinner{width:48px;height:48px;margin:0 auto 18px;border:5px solid var(--border);border-top-color:var(--acc);border-radius:50%;animation:spin .9s linear infinite}@keyframes spin{to{transform:rotate(360deg)}}.ota-progress h2{margin:0 0 9px}.ota-progress p{margin:0;color:var(--muted);line-height:1.5}</style></head><body><div id='ota-progress' class='ota-progress' role='alert' aria-live='assertive'><div class='ota-progress-box'><div class='spinner'></div><h2 id='ota-progress-title'>Installing firmware…</h2><p id='ota-progress-text'>Do not disconnect power or close this page.</p></div></div><div class='wrap'>");
    html += F("<div class='card'><h1>Firmware update</h1><p class='muted'>Device: <code>");
    html += htmlEscape(deviceUrl);
    html += F("</code><br>Current firmware: <strong>v");
    html += htmlEscape(String(FW_VERSION_STR));
    html += F("</strong><br>Board: <strong>");
    html += htmlEscape(String(FW_BOARD_NAME_STR));
    html += F("</strong> / ");
    html += htmlEscape(String(FW_CHIP_FAMILY_STR));
    html += F("</p><p><a class='btn secondary' href='/'>Back to Web UI</a></p></div>");
    if (message.length()) {
        html += F("<div class='msg ");
        html += ok ? F("ok") : F("bad");
        html += F("'>");
        html += htmlEscape(message);
        html += F("</div>");
    }
    html += F("<div class='card'><h2>Automatic update</h2><p class='muted'>Use this when the device has internet access. It downloads the latest board-specific app build from the project web flasher and installs it automatically.</p>");
    if (defaultOtaUrl.length()) {
        if (versionChecking) {
            html += F("<div class='version-state current'>Checking for firmware updates in the background…</div>");
        } else if (!versionCheckOk) {
            html += F("<div class='version-state bad'>Update check failed: ");
            html += htmlEscape(versionError);
            html += F("</div>");
        } else if (updateAvailable) {
            html += F("<div class='version-state update'>New firmware available: v");
            html += htmlEscape(latestVersion);
            html += F("</div><form id='ota-install-form' method='post' action='/ota/install'><button type='submit'>Download and install v");
            html += htmlEscape(latestVersion);
            html += F("</button></form>");
        } else {
            html += F("<div class='version-state current'><strong>Firmware is up to date.</strong><br><span class='muted'>Latest available: v");
            html += htmlEscape(latestVersion);
            html += F("</span></div>");
        }
        html += F("<p><a class='btn secondary' href='/ota?refresh=1'>Check again</a></p><p class='muted'>Firmware file: <code>");
        html += htmlEscape(defaultOtaUrl);
        html += F("</code></p>");
    } else {
        html += F("<p class='bad'>Automatic update is unavailable for this board profile. Upload a matching app-only firmware file manually.</p>");
    }
    html += F("</div>");
    html += F("<div class='card'><h2>Upload compiled file</h2><p class='muted'>Use this when the device has no internet access. Select the matching app-only file such as <code>firmware-app-c3.bin</code>, <code>firmware-app-s3.bin</code>, <code>firmware-app-c5.bin</code>, or <code>firmware-app-c6.bin</code>. Do not upload the USB <code>firmware.bin</code> merged image here.</p>");
    html += F("<form id='ota-upload-form' method='post' action='/ota/upload' enctype='multipart/form-data'>");
    html += F("<input type='file' name='firmware' accept='.bin,application/octet-stream' required><button type='submit'>Upload and install file</button></form></div>");
    html += F("<script>(function(){var busy=false,allowLeave=false,overlay=document.getElementById('ota-progress'),title=document.getElementById('ota-progress-title'),detail=document.getElementById('ota-progress-text');function setText(t,d){title.textContent=t;detail.textContent=d;}function begin(t,d){busy=true;setText(t,d);overlay.classList.add('active');document.querySelectorAll('button,input,a.btn').forEach(function(el){el.disabled=true;el.setAttribute('aria-disabled','true');});}window.addEventListener('beforeunload',function(e){if(busy&&!allowLeave){e.preventDefault();e.returnValue='Firmware update is in progress.';}});function replaceWithResponse(r){busy=false;allowLeave=true;return r.text().then(function(t){document.open();document.write(t);document.close();});}function waitForDevice(){setText('Firmware installed','The device is rebooting. This page will reconnect automatically…');var tries=0;setTimeout(function poll(){fetch('/api/status?ota_reconnect='+Date.now(),{cache:'no-store'}).then(function(r){if(!r.ok)throw new Error('not ready');allowLeave=true;location.replace('/ota');}).catch(function(){tries++;if(tries<120)setTimeout(poll,1500);else{busy=false;setText('Reconnection is taking longer than expected','The firmware was installed. Reopen the device page when it is back online.');}});},4000);}function handleResponse(r){if(r.headers.get('X-ESP32MIC-OTA-Reboot')==='1'){return r.text().then(waitForDevice);}return replaceWithResponse(r);}function fail(e){busy=false;overlay.classList.remove('active');document.querySelectorAll('button,input,a.btn').forEach(function(el){el.disabled=false;el.removeAttribute('aria-disabled');});alert('Update request failed: '+e);}var install=document.getElementById('ota-install-form');if(install){install.addEventListener('submit',function(e){e.preventDefault();if(!confirm('Install firmware update now? The stream will stop and the device will reboot.'))return;begin('Downloading and installing firmware…','Do not disconnect power or close this page. The stream will stop temporarily.');fetch('/ota/install',{method:'POST',cache:'no-store',headers:{'X-ESP32MIC-CSRF':'1'}}).then(handleResponse).catch(fail);});}var upload=document.getElementById('ota-upload-form');if(upload){upload.addEventListener('submit',function(e){e.preventDefault();if(!confirm('Upload and install selected firmware now?'))return;begin('Uploading and installing firmware…','Do not disconnect power or close this page. The stream will stop temporarily.');fetch('/ota/upload',{method:'POST',cache:'no-store',headers:{'X-ESP32MIC-CSRF':'1'},body:new FormData(upload)}).then(handleResponse).catch(fail);});}");
    if (versionChecking) {
        html += F("var tries=0;function poll(){fetch('/api/ota_status',{cache:'no-store'}).then(function(r){return r.json();}).then(function(j){if(j&&j.checking&&tries++<10){setTimeout(poll,750);}else{location.replace('/ota');}}).catch(function(){if(tries++<10)setTimeout(poll,750);});}setTimeout(poll,750);");
    }
    html += F("})();</script>");
    html += F("</div></body></html>");
    web.sendHeader("Cache-Control", "no-store");
    web.send(200, "text/html; charset=utf-8", html);
}

static bool isLikelyMergedFirmwareName(String filename) {
    filename.toLowerCase();
    int slash = filename.lastIndexOf('/');
    if (slash >= 0) filename = filename.substring(slash + 1);
    slash = filename.lastIndexOf('\\');
    if (slash >= 0) filename = filename.substring(slash + 1);
    if (filename == "firmware.bin") return true;
    if (filename.startsWith("firmware-") && !filename.startsWith("firmware-app-")) return true;
    return false;
}

static bool validateOtaImageSize(size_t imageSize, String &errorOut) {
    if (imageSize == 0) {
        errorOut = F("No firmware data received.");
        return false;
    }
    if (imageSize == 4194304UL || imageSize == 8388608UL) {
        errorOut = F("This looks like a merged USB firmware image. OTA needs the matching app-only firmware-app-*.bin file.");
        return false;
    }
    uint32_t freeSketchSpace = ESP.getFreeSketchSpace();
    if (freeSketchSpace > 0) {
        uint32_t maxSketchSpace = freeSketchSpace > 0x1000UL
            ? (freeSketchSpace - 0x1000UL) & 0xFFFFF000UL
            : 0;
        if (maxSketchSpace > 0 && imageSize > maxSketchSpace) {
            errorOut = F("Firmware image is larger than the OTA app partition. Use the matching app-only firmware-app-*.bin file.");
            return false;
        }
    }
    return true;
}

static bool isSafeOtaUrl(const String &url) {
    String expected = getDefaultOtaUrl();
    return expected.length() > 0 && url == expected;
}

static bool isFirmwareContentType(String contentType) {
    int separator = contentType.indexOf(';');
    if (separator >= 0) contentType = contentType.substring(0, separator);
    contentType.trim();
    contentType.toLowerCase();
    return contentType == "application/octet-stream" ||
           contentType == "application/macbinary" ||
           contentType == "binary/octet-stream";
}

static bool parseFirmwareVersion(String version, uint32_t &majorOut, uint32_t &minorOut) {
    version.trim();
    if (version.startsWith("v") || version.startsWith("V")) version.remove(0, 1);
    int dot = version.indexOf('.');
    if (dot <= 0 || dot != version.lastIndexOf('.') || dot >= (int)version.length() - 1) return false;

    String majorPart = version.substring(0, dot);
    String minorPart = version.substring(dot + 1);
    for (size_t i = 0; i < majorPart.length(); i++) {
        if (!isDigit((unsigned char)majorPart[i])) return false;
    }
    for (size_t i = 0; i < minorPart.length(); i++) {
        if (!isDigit((unsigned char)minorPart[i])) return false;
    }

    errno = 0;
    char* end = nullptr;
    unsigned long majorValue = strtoul(majorPart.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0' || majorValue > UINT32_MAX) return false;
    errno = 0;
    end = nullptr;
    unsigned long minorValue = strtoul(minorPart.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0' || minorValue > UINT32_MAX) return false;

    majorOut = (uint32_t)majorValue;
    minorOut = (uint32_t)minorValue;
    return true;
}

static int compareFirmwareVersions(const String &left, const String &right, bool &validOut) {
    uint32_t leftMajor = 0, leftMinor = 0, rightMajor = 0, rightMinor = 0;
    validOut = parseFirmwareVersion(left, leftMajor, leftMinor) &&
               parseFirmwareVersion(right, rightMajor, rightMinor);
    if (!validOut) return 0;
    if (leftMajor != rightMajor) return leftMajor > rightMajor ? 1 : -1;
    if (leftMinor != rightMinor) return leftMinor > rightMinor ? 1 : -1;
    return 0;
}

static void ensureOtaVersionMutex() {
    if (!otaVersionMutex) otaVersionMutex = xSemaphoreCreateMutex();
}

static void getOtaVersionSnapshot(bool &checkedOut, unsigned long &checkedAtOut,
                                  String &latestOut, String &errorOut, bool &runningOut) {
    ensureOtaVersionMutex();
    if (otaVersionMutex && xSemaphoreTake(otaVersionMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        checkedOut = otaVersionChecked;
        checkedAtOut = otaVersionCheckedAtMs;
        latestOut = otaLatestVersion;
        errorOut = otaVersionCheckError;
        runningOut = otaVersionCheckRunning;
        xSemaphoreGive(otaVersionMutex);
        return;
    }
    checkedOut = false;
    checkedAtOut = 0;
    latestOut = "";
    errorOut = F("Firmware update status is temporarily busy.");
    runningOut = true;
}

static bool otaVersionCacheFresh(bool checked, unsigned long checkedAt, const String &error) {
    if (!checked) return false;
    uint32_t cacheMs = error.length() == 0 ? OTA_VERSION_CACHE_MS : OTA_VERSION_ERROR_CACHE_MS;
    return (millis() - checkedAt) < cacheMs;
}

static void storeOtaVersionResult(const String &latest, const String &error) {
    ensureOtaVersionMutex();
    if (!otaVersionMutex || xSemaphoreTake(otaVersionMutex, portMAX_DELAY) != pdTRUE) return;
    otaVersionChecked = true;
    otaVersionCheckedAtMs = millis();
    otaLatestVersion = latest;
    otaVersionCheckError = error;
    otaVersionCheckRunning = false;
    xSemaphoreGive(otaVersionMutex);
}

static bool fetchOtaVersionFromNetwork(String &latestOut, String &errorOut) {
    latestOut = "";
    errorOut = "";
    if (!FW_OTA_ARTIFACT_STR || FW_OTA_ARTIFACT_STR[0] == '\0') {
        errorOut = F("Automatic update is unavailable for this board profile.");
    } else {
        WiFiClient client;
        HTTPClient http;
        http.setConnectTimeout(2000);
        http.setTimeout(3000);
        if (!http.begin(client, getOtaVersionUrl())) {
            errorOut = F("Could not open the firmware version URL.");
        } else {
            int code = http.GET();
            if (code != HTTP_CODE_OK) {
                errorOut = String("Firmware version check failed, HTTP ") + String(code);
            } else if (http.getSize() > 32) {
                errorOut = F("Firmware server returned an invalid version response.");
            } else {
                String version = http.getString();
                version.trim();
                uint32_t majorValue = 0, minorValue = 0;
                if (version.length() == 0 || version.length() > 24 ||
                    !parseFirmwareVersion(version, majorValue, minorValue)) {
                    errorOut = F("Firmware server returned an invalid version.");
                } else {
                    latestOut = version;
                }
            }
            http.end();
        }
    }
    return errorOut.length() == 0;
}

static bool fetchLatestOtaVersion(bool forceRefresh, String &latestOut, String &errorOut) {
    bool checked = false;
    bool running = false;
    unsigned long checkedAt = 0;
    getOtaVersionSnapshot(checked, checkedAt, latestOut, errorOut, running);
    if (!forceRefresh && otaVersionCacheFresh(checked, checkedAt, errorOut)) {
        return errorOut.length() == 0;
    }

    bool ok = fetchOtaVersionFromNetwork(latestOut, errorOut);
    storeOtaVersionResult(latestOut, errorOut);
    return ok;
}

static void otaVersionCheckTask(void* /*arg*/) {
    String latest;
    String error;
    fetchOtaVersionFromNetwork(latest, error);
    storeOtaVersionResult(latest, error);
    vTaskDelete(nullptr);
}

static bool startOtaVersionCheckAsync() {
    ensureOtaVersionMutex();
    if (!otaVersionMutex || xSemaphoreTake(otaVersionMutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    if (otaVersionCheckRunning) {
        xSemaphoreGive(otaVersionMutex);
        return true;
    }
    otaVersionCheckRunning = true;
    xSemaphoreGive(otaVersionMutex);

    BaseType_t created = xTaskCreate(otaVersionCheckTask, "ota_check", 5120, nullptr, 1, nullptr);
    if (created == pdPASS) return true;
    storeOtaVersionResult("", F("Could not start the firmware update check."));
    return false;
}

static bool isNewerFirmwareAvailable(const String &latestVersion) {
    bool valid = false;
    int comparison = compareFirmwareVersions(latestVersion, String(FW_VERSION_STR), valid);
    return valid && comparison > 0;
}

static bool streamUpdate(NetworkClient &stream, int contentLength, String &errorOut) {
    if (contentLength <= 0) {
        errorOut = F("Firmware server did not provide a valid Content-Length.");
        return false;
    }
    if (!Update.begin((size_t)contentLength)) {
        errorOut = String("Update begin failed: ") + Update.errorString();
        return false;
    }

    uint8_t buffer[1024];
    size_t writtenTotal = 0;
    int remaining = contentLength;
    unsigned long lastDataMs = millis();

    while (remaining > 0) {
        size_t available = stream.available();
        if (available == 0) {
            if (!stream.connected()) {
                errorOut = String("Firmware connection closed after ") + writtenTotal +
                           " of " + contentLength + " bytes.";
                Update.abort();
                return false;
            }
            if (millis() - lastDataMs > 15000) {
                errorOut = F("Update download timed out.");
                Update.abort();
                return false;
            }
            delay(1);
            continue;
        }
        lastDataMs = millis();
        size_t toRead = available;
        if (toRead > sizeof(buffer)) toRead = sizeof(buffer);
        if (toRead > (size_t)remaining) toRead = (size_t)remaining;
        int readLen = stream.readBytes(buffer, toRead);
        if (readLen <= 0) continue;
        size_t written = Update.write(buffer, (size_t)readLen);
        if (written != (size_t)readLen) {
            errorOut = String("Flash write failed: ") + Update.errorString();
            Update.abort();
            return false;
        }
        writtenTotal += written;
        remaining -= readLen;
        yield();
    }

    if (writtenTotal != (size_t)contentLength) {
        errorOut = F("Firmware download size did not match Content-Length.");
        Update.abort();
        return false;
    }
    if (!validateOtaImageSize(writtenTotal, errorOut)) {
        Update.abort();
        return false;
    }
    if (!Update.end()) {
        errorOut = String("Update end failed: ") + Update.errorString();
        return false;
    }
    if (!Update.isFinished()) {
        errorOut = F("Update was not fully written.");
        return false;
    }
    return true;
}

static bool installOtaFromUrl(const String &url, String &errorOut) {
    if (!isSafeOtaUrl(url)) {
        errorOut = F("No matching official automatic OTA image is available for this board profile.");
        return false;
    }

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(15000);
    if (!http.begin(client, url)) {
        errorOut = F("Could not open firmware URL.");
        return false;
    }
    const char* headerKeys[] = {"Content-Type"};
    http.collectHeaders(headerKeys, 1);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        errorOut = String("Firmware download failed, HTTP ") + String(code);
        http.end();
        return false;
    }

    if (!isFirmwareContentType(http.header("Content-Type"))) {
        errorOut = F("Firmware server returned an unexpected Content-Type.");
        http.end();
        return false;
    }
    int contentLength = http.getSize();
    if (contentLength <= 0) {
        errorOut = F("Firmware server did not provide a valid Content-Length.");
        http.end();
        return false;
    }
    if (!validateOtaImageSize((size_t)contentLength, errorOut)) {
        http.end();
        return false;
    }
    NetworkClient* stream = http.getStreamPtr();
    if (!stream) {
        errorOut = F("Firmware response stream is unavailable.");
        http.end();
        return false;
    }

    stopAllRtspClients("OTA update starting");
    webui_pushLog(String("OTA pull update from ") + url);
    bool ok = streamUpdate(*stream, contentLength, errorOut);
    http.end();
    return ok;
}

static void httpOtaPage() {
    if (web.hasArg("refresh") && web.arg("refresh") == "1") {
        startOtaVersionCheckAsync();
    }
    sendOtaPage();
}

static void httpOtaInstall() {
    if (!requireMutationAuth()) return;

    String latestVersion;
    String versionError;
    if (!fetchLatestOtaVersion(true, latestVersion, versionError)) {
        webui_pushLog(String("OTA install blocked: version check failed: ") + versionError);
        sendOtaPage(String("Update blocked: ") + versionError, false);
        return;
    }
    if (!isNewerFirmwareAvailable(latestVersion)) {
        String reason = String("No newer firmware is available. Installed v") + FW_VERSION_STR +
                        ", latest v" + latestVersion + ".";
        webui_pushLog(String("OTA install blocked: ") + reason);
        sendOtaPage(reason, false);
        return;
    }

    String url = getDefaultOtaUrl();
    String error;
    bool ok = installOtaFromUrl(url, error);
    if (ok) {
        web.sendHeader("X-ESP32MIC-OTA-Reboot", "1");
        sendOtaPage(F("Firmware installed. Device will reboot now."), true);
        scheduleReboot(false, 700);
    } else {
        webui_pushLog(String("OTA pull failed: ") + error);
        sendOtaPage(String("Update failed: ") + error, false);
    }
}

static void httpOtaStatus() {
    bool checked = false;
    bool checking = false;
    unsigned long checkedAt = 0;
    String latestVersion;
    String error;
    getOtaVersionSnapshot(checked, checkedAt, latestVersion, error, checking);
    if (!otaVersionCacheFresh(checked, checkedAt, error)) {
        startOtaVersionCheckAsync();
        getOtaVersionSnapshot(checked, checkedAt, latestVersion, error, checking);
    }
    bool checkOk = checked && error.length() == 0;
    bool updateAvailable = checkOk && isNewerFirmwareAvailable(latestVersion);
    String status = checking ? F("checking")
                             : (checkOk ? (updateAvailable ? F("update_available") : F("up_to_date"))
                                        : F("check_failed"));
    uint32_t checkAgeSec = checked
                               ? (uint32_t)((millis() - checkedAt) / 1000UL)
                               : 0;

    String json;
    json.reserve(320);
    json += F("{\"ok\":");
    json += checkOk ? F("true") : F("false");
    json += F(",\"status\":\"");
    json += status;
    json += F("\",\"current_version\":\"");
    json += jsonEscape(String(FW_VERSION_STR));
    json += F("\",\"latest_version\":\"");
    json += jsonEscape(latestVersion);
    json += F("\",\"update_available\":");
    json += updateAvailable ? F("true") : F("false");
    json += F(",\"checking\":");
    json += checking ? F("true") : F("false");
    json += F(",\"check_age_sec\":");
    json += String(checkAgeSec);
    json += F(",\"error\":\"");
    json += jsonEscape(error);
    json += F("\"}");
    apiSendJSON(json);
}

static void httpOtaUploadDone() {
    if (!requireMutationAuth()) return;

    if (otaUploadOk) {
        web.sendHeader("X-ESP32MIC-OTA-Reboot", "1");
        sendOtaPage(F("Firmware uploaded and installed. Device will reboot now."), true);
        scheduleReboot(false, 700);
    } else {
        sendOtaPage(String("Upload failed: ") + otaUploadError, false);
    }
}

static void httpOtaUploadChunk() {
    HTTPUpload &upload = web.upload();
    if (upload.status == UPLOAD_FILE_START) {
        otaUploadOk = false;
        otaUploadError = "";
        if (!hasMutationAuth()) {
            otaUploadError = F("forbidden");
            Update.abort();
            return;
        }
        if (isLikelyMergedFirmwareName(upload.filename)) {
            otaUploadError = F("This looks like a merged USB firmware image. OTA needs the matching app-only firmware-app-*.bin file.");
            Update.abort();
            return;
        }
        stopAllRtspClients("OTA upload starting");
        webui_pushLog(String("OTA upload start: ") + upload.filename);
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            otaUploadError = String("Update begin failed: ") + Update.errorString();
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (otaUploadError.length() == 0) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                otaUploadError = String("Flash write failed: ") + Update.errorString();
                Update.abort();
            }
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (otaUploadError.length() == 0) {
            if (!validateOtaImageSize(upload.totalSize, otaUploadError)) {
                Update.abort();
            } else if (!Update.end(true)) {
                otaUploadError = String("Update end failed: ") + Update.errorString();
            } else if (!Update.isFinished()) {
                otaUploadError = F("Update was not fully written.");
            } else {
                otaUploadOk = true;
                webui_pushLog(String("OTA upload installed, bytes=") + String(upload.totalSize));
            }
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        otaUploadError = F("Upload aborted.");
        Update.abort();
    }
}

static void httpStatus() {
    unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
    String uptimeStr = formatUptime(uptimeSeconds);
    String localTimeStr = formatLocalDateTimeSafe();
    String utcTimeStr = formatUtcDateTimeSafe();
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < minFreeHeap) minFreeHeap = freeHeap;
    unsigned long runtime = millis() - lastStatsReset;
    uint32_t currentRate = (isStreaming && runtime > 1000)
        ? (uint32_t)(((uint64_t)audioPacketsSent * 1000ULL) / runtime)
        : 0;
    String json = "{";
    json.reserve(1800);
    json += "\"fw_version\":\"" + String(FW_VERSION_STR) + "\",";
    json += "\"board_id\":\"" + jsonEscape(String(FW_BOARD_ID_STR)) + "\",";
    json += "\"board_name\":\"" + jsonEscape(String(FW_BOARD_NAME_STR)) + "\",";
    json += "\"chip_family\":\"" + jsonEscape(String(FW_CHIP_FAMILY_STR)) + "\",";
    json += "\"ota_artifact\":\"" + jsonEscape(String(FW_OTA_ARTIFACT_STR)) + "\",";
    json += "\"ota_auto_supported\":" + String(FW_OTA_ARTIFACT_STR[0] ? "true" : "false") + ",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"stream_url_ip\":\"rtsp://" + WiFi.localIP().toString() + ":8554/audio1\",";
    json += "\"stream_url_mdns\":\"rtsp://" + mdnsHostname + ".local:8554/audio1\",";
    json += "\"stream1_url_ip\":\"rtsp://" + WiFi.localIP().toString() + ":8554/audio1\",";
    json += "\"stream2_url_ip\":\"rtsp://" + WiFi.localIP().toString() + ":8554/audio2\",";
    json += "\"stream1_url_mdns\":\"rtsp://" + mdnsHostname + ".local:8554/audio1\",";
    json += "\"stream2_url_mdns\":\"rtsp://" + mdnsHostname + ".local:8554/audio2\",";
    json += "\"stream1_target\":" + String((uint32_t)streamProfiles[0].target) + ",";
    json += "\"stream2_target\":" + String((uint32_t)streamProfiles[1].target) + ",";
    json += "\"stream1_enabled\":" + String(streamEnabled[0]?"true":"false") + ",";
    json += "\"stream2_enabled\":" + String(streamEnabled[1]?"true":"false") + ",";
    json += "\"max_clients\":" + String((uint32_t)maxActiveClients) + ",";
    // Per-stream client counts
    uint8_t s1clients = 0, s2clients = 0;
    getStreamClientCounts(s1clients, s2clients);
    unsigned long nowMs = millis();
    json += "\"s1_clients\":" + String(s1clients) + ",";
    json += "\"s1_streaming\":" + String(streamStats[0].streaming?"true":"false") + ",";
    json += "\"s1_pkt_rate\":" + String((streamStats[0].streaming && (nowMs - streamStats[0].statsResetMs) > 1000) ? (uint32_t)(((uint64_t)streamStats[0].packetsSent * 1000ULL) / (nowMs - streamStats[0].statsResetMs)) : 0) + ",";
    json += "\"s1_last_play\":\"" + jsonEscape(formatSince(streamStats[0].lastPlayMs)) + "\",";
    json += "\"s2_clients\":" + String(s2clients) + ",";
    json += "\"s2_streaming\":" + String(streamStats[1].streaming?"true":"false") + ",";
    json += "\"s2_pkt_rate\":" + String((streamStats[1].streaming && (nowMs - streamStats[1].statsResetMs) > 1000) ? (uint32_t)(((uint64_t)streamStats[1].packetsSent * 1000ULL) / (nowMs - streamStats[1].statsResetMs)) : 0) + ",";
    json += "\"s2_last_play\":\"" + jsonEscape(formatSince(streamStats[1].lastPlayMs)) + "\",";
    json += "\"mdns_hostname\":\"" + jsonEscape(mdnsHostname) + "\",";
    json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"wifi_tx_dbm\":" + String(wifiPowerLevelToDbm(currentWifiPowerLevel),1) + ",";
    json += "\"free_heap_kb\":" + String(freeHeap/1024) + ",";
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
    json += "\"client\":\"" + jsonEscape(getRtspClientSummary()) + "\",";
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
    json.reserve(360);
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
    json += "\"clip_count\":" + String(audioClipCount) + ",";
    json += "\"producer_running\":" + String(audioProducerRunning?"true":"false") + ",";
    json += "\"i2s_error_count\":" + String(audioI2SErrorCount) + ",";
    json += "\"rb_capacity_bytes\":" + String((uint32_t)audioRingBufferCapacityBytes) + ",";
    json += "\"rb_chunks\":" + String(audioRingBufferChunkCount) + ",";
    json += "\"rb_drops\":" + String(audioRingBufferDropCount) + ",";
    json += "\"rb_flushes\":" + String(audioRingBufferFlushCount) + ",";
    json += "\"rtsp_write_stalls\":" + String(rtspWriteStallCount) + ",";
    json += "\"rtsp_write_timeouts\":" + String(rtspWriteTimeoutCount);
    json += "}";
    apiSendJSON(json);
}

static void httpPerfStatus() {
    String json = "{";
    json.reserve(220);
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
    json.reserve(520);
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
        overheatConsecutiveSamples = 0;
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
    out.reserve(LOG_CAP * 96);
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
        overheatConsecutiveSamples = 0;
    }
    webui_pushLog(F("UI action: server_start"));
    apiSendJSON(F("{\"ok\":true}"));
}
static void httpActionServerStop(){
    if (!requireMutationAuth()) return;

    rtspServerEnabled=false; stopAllRtspClients("Web UI RTSP server disabled"); rtspServer.stop();
    webui_pushLog(F("UI action: server_stop"));
    apiSendJSON(F("{\"ok\":true}"));
}
static void httpActionResetI2S(){
    if (!requireMutationAuth()) return;

    webui_pushLog(F("UI action: reset_i2s"));
    if (restartI2S()) apiSendJSON(F("{\"ok\":true}"));
    else apiSendJSON(F("{\"ok\":false,\"error\":\"i2s_restart_failed\"}"));
}

static void httpActionTimeSync(){
    if (!requireMutationAuth()) return;

    bool ok = attemptTimeSync(true, true);
    apiSendJSON(String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

static bool parseBssidStr(const String &s, uint8_t out[6]) {
    if (s.length() != 17) return false;
    for (int i = 0; i < 6; ++i) {
        int p = i * 3;
        if (i < 5) {
            char sep = s[p + 2];
            if (sep != ':' && sep != '-') return false;
        }
        int nibble[2];
        for (int k = 0; k < 2; ++k) {
            char c = s[p + k];
            if (c >= '0' && c <= '9')      nibble[k] = c - '0';
            else if (c >= 'a' && c <= 'f') nibble[k] = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') nibble[k] = c - 'A' + 10;
            else return false;
        }
        out[i] = (uint8_t)((nibble[0] << 4) | nibble[1]);
    }
    return true;
}

static void httpActionWifiReconnect(){
    if (!requireMutationAuth()) return;

    uint8_t bssid[6];
    bool hasBssid = false;
    if (web.hasArg("bssid")) {
        String v = web.arg("bssid");
        v.trim();
        if (v.length() > 0) {
            if (!parseBssidStr(v, bssid)) {
                apiSendJSON(F("{\"ok\":false,\"error\":\"bad_bssid\"}"));
                return;
            }
            hasBssid = true;
        }
    }
    if (hasBssid) {
        webui_pushLog(F("UI action: wifi_reconnect (BSSID pinned)"));
    } else {
        webui_pushLog(F("UI action: wifi_reconnect"));
    }
    scheduleWifiReconnect(hasBssid ? bssid : nullptr, 300);
    apiSendJSON(F("{\"ok\":true}"));
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

static bool applyAndSaveAudio(uint32_t newRate, float newGain, uint16_t newBuffer, uint8_t newShift) {
    if (applyAudioConfig(newRate, newGain, newBuffer, newShift)) {
        saveAudioSettings();
        return true;
    }
    return false;
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
        if (argToFloat(v) && v >= 0.1f && v <= 100.0f) {
            applied = applyAndSaveAudio(currentSampleRate, v, currentBufferSize, i2sShiftBits);
        }
    }
    else if (key == "rate") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 8000 && v <= 192000) {
            applied = applyAndSaveAudio(v, currentGainFactor, currentBufferSize, i2sShiftBits);
        }
    }
    else if (key == "buffer") {
        handled = true;
        uint16_t v;
        if (argToUShort(v) && v >= 256 && v <= 8192) {
            applied = applyAndSaveAudio(currentSampleRate, currentGainFactor, v, i2sShiftBits);
        }
    }
    else if (key == "shift") {
        handled = true;
        uint8_t v;
        if (argToUChar(v) && v <= 24) {
            applied = applyAndSaveAudio(currentSampleRate, currentGainFactor, currentBufferSize, v);
        }
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
        if (argToUInt(v) && (v == 40 || v == 80 || v == 160)) { cpuFrequencyMhz = (uint8_t)v; setCpuFrequencyMhz(cpuFrequencyMhz); saveAudioSettings(); applied = true; }
    }
    else if (key == "hp_enable") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") { extern bool highpassEnabled; highpassEnabled = (v == "on"); extern void updateHighpassCoeffs(); updateHighpassCoeffs(); saveAudioSettings(); applied = true; }
    }
    else if (key == "hp_cutoff") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 10 && v <= maxHighpassCutoffForRate(currentSampleRate)) { extern uint16_t highpassCutoffHz; highpassCutoffHz = (uint16_t)v; extern void updateHighpassCoeffs(); updateHighpassCoeffs(); saveAudioSettings(); applied = true; }
    }
    else if (key == "oh_enable") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") { overheatProtectionEnabled = (v == "on"); overheatConsecutiveSamples = 0; if (!overheatProtectionEnabled) { overheatLockoutActive = false; } saveAudioSettings(); applied = true; }
    }
    else if (key == "oh_limit") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= OH_MIN && v <= OH_MAX) { uint32_t snapped = OH_MIN + ((v - OH_MIN) / OH_STEP) * OH_STEP; overheatShutdownC = (float)snapped; overheatLockoutActive = false; overheatConsecutiveSamples = 0; saveAudioSettings(); applied = true; }
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
    else if (key == "mdns_hostname") {
        handled = true;
        String v = web.arg("value");
        v.trim();
        extern String sanitizeMdnsHostname(const String &input, const String &fallback);
        String next = sanitizeMdnsHostname(v, mdnsHostname);
        if (next.length() > 0) {
            mdnsHostname = next;
            if (mdnsRunning) {
                MDNS.end();
                mdnsRunning = false;
            }
            applyMdnsSetting();
            saveAudioSettings();
            applied = true;
        }
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
    else if (key == "stream1_target") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v <= 1) { streamProfiles[0].target = (uint8_t)v; saveAudioSettings(); applied = true; }
    }
    else if (key == "stream2_target") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v <= 1) { streamProfiles[1].target = (uint8_t)v; saveAudioSettings(); applied = true; }
    }
    else if (key == "stream1_enabled") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") { streamEnabled[0] = (v == "on"); saveAudioSettings(); applied = true; }
    }
    else if (key == "stream2_enabled") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") { streamEnabled[1] = (v == "on"); saveAudioSettings(); applied = true; }
    }
    else if (key == "max_clients") {
        handled = true;
        uint32_t v;
        if (argToUInt(v) && v >= 1 && v <= 3) { maxActiveClients = (uint8_t)v; saveAudioSettings(); applied = true; }
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
    web.on("/ota", HTTP_GET, httpOtaPage);
    web.on("/ota/install", HTTP_POST, httpOtaInstall);
    web.on("/ota/upload", HTTP_POST, httpOtaUploadDone, httpOtaUploadChunk);
    web.on("/api/ota_status", HTTP_GET, httpOtaStatus);
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
    web.on("/api/action/wifi_reconnect", HTTP_POST, httpActionWifiReconnect);
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
