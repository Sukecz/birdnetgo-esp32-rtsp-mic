#include <Arduino.h>
#include <math.h>
#include <time.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include "WebUI.h"

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

// Async reboot/factory-reset task to avoid restarting from HTTP context
static void rebootTask(void* arg){
    bool doFactory = ((uintptr_t)arg) != 0;
    if (doFactory) {
        resetToDefaultSettings();
    }
    vTaskDelay(pdMS_TO_TICKS(600));
    ESP.restart();
    vTaskDelete(NULL);
}

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
extern bool isStreamScheduleAllowedNow(bool* timeValidOut);
extern const char* MDNS_HOSTNAME;
extern bool attemptTimeSync(bool logResult, bool quickMode);
extern String formatDateTime();
extern const char* NTP_SERVER_1;
extern const char* NTP_SERVER_2;
extern void applyMdnsSetting();

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

// HTML UI
static String htmlIndex() {
    String ip = WiFi.localIP().toString();
    String h;
    // Best-effort preallocation only. Large contiguous blocks may fail on ESP32
    // even when total free heap is sufficient, so avoid noisy warning logs here.
    h.reserve(32768);
    h += F(
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta name='color-scheme' content='light dark'>"
        "<title>ESP32 RTSP Mic for BirdNET-Go</title>"
        "<style>"
        ":root{color-scheme:light dark;--bg:#f6f7fb;--bg2:#eef2f8;--fg:#0f172a;--muted:#526079;--card:rgba(255,255,255,.88);--card2:rgba(255,255,255,.72);--border:rgba(15,23,42,.12);--acc:#0ea5e9;--acc2:#10b981;--warn:#f59e0b;--bad:#ef4444;--shadow:0 14px 34px rgba(15,23,42,.10);--shadow2:0 8px 18px rgba(15,23,42,.08);--row:rgba(15,23,42,.03);--row2:rgba(15,23,42,.05);--field:rgba(255,255,255,.70)}"
        "@media (prefers-color-scheme:dark){:root{--bg:#070a14;--bg2:#0b1632;--fg:#e8eefc;--muted:#a7b3cb;--card:rgba(15,23,42,.70);--card2:rgba(15,23,42,.55);--border:rgba(226,232,240,.14);--shadow:0 18px 44px rgba(0,0,0,.55);--shadow2:0 10px 22px rgba(0,0,0,.40);--row:rgba(255,255,255,.04);--row2:rgba(255,255,255,.06);--field:rgba(255,255,255,.04)}}"
        ":root[data-theme='light']{color-scheme:light;--bg:#f6f7fb;--bg2:#eef2f8;--fg:#0f172a;--muted:#526079;--card:rgba(255,255,255,.88);--card2:rgba(255,255,255,.72);--border:rgba(15,23,42,.12);--shadow:0 14px 34px rgba(15,23,42,.10);--shadow2:0 8px 18px rgba(15,23,42,.08);--row:rgba(15,23,42,.03);--row2:rgba(15,23,42,.05);--field:rgba(255,255,255,.70)}"
        ":root[data-theme='dark']{color-scheme:dark;--bg:#070a14;--bg2:#0b1632;--fg:#e8eefc;--muted:#a7b3cb;--card:rgba(15,23,42,.70);--card2:rgba(15,23,42,.55);--border:rgba(226,232,240,.14);--shadow:0 18px 44px rgba(0,0,0,.55);--shadow2:0 10px 22px rgba(0,0,0,.40);--row:rgba(255,255,255,.04);--row2:rgba(255,255,255,.06);--field:rgba(255,255,255,.04)}"
        "*{box-sizing:border-box}"
        "body{font-family:ui-rounded,\"SF Pro Rounded\",\"Segoe UI Variable\",\"Segoe UI\",system-ui,-apple-system,sans-serif;margin:0;color:var(--fg);background:radial-gradient(1100px 700px at 12% -10%,rgba(14,165,233,.18),transparent 60%),radial-gradient(900px 700px at 90% -20%,rgba(16,185,129,.16),transparent 55%),linear-gradient(180deg,var(--bg),var(--bg2));min-height:100vh}"
        "a{color:inherit}"
        ".page{max-width:1100px;margin:0 auto;padding:18px 16px 24px}"
        ".card{background:var(--card);border:1px solid var(--border);border-radius:16px;padding:14px;margin:0;box-shadow:var(--shadow2);backdrop-filter:blur(10px)}"
        ".page>.card{margin-bottom:14px}"
        ".top{position:sticky;top:0;z-index:50}"
        ".hero{display:flex;justify-content:space-between;align-items:flex-start;gap:12px;margin:0;flex-wrap:wrap}"
        ".hero>div:first-child{min-width:0;flex:1 1 360px}"
        ".brand{display:flex;align-items:flex-start;gap:12px;flex-wrap:wrap}"
        ".title{font-weight:850;font-size:19px;letter-spacing:.2px;line-height:1.1}"
        ".subtitle{color:var(--muted);font-size:12.5px;line-height:1.4;margin-top:6px;overflow-wrap:anywhere;word-break:break-word}"
        ".subtitle a{color:var(--acc);text-decoration:none;border-bottom:1px solid transparent;overflow-wrap:anywhere;word-break:break-word}"
        ".subtitle a:hover{border-bottom-color:rgba(14,165,233,.55)}"
        ".badge{display:inline-flex;align-items:center;gap:6px;border:1px solid var(--border);background:var(--row);color:var(--muted);padding:4px 10px;border-radius:999px;font-size:12px}"
        ".row{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:14px}"
        "h2{font-size:12px;margin:0 0 10px;color:var(--muted);font-weight:800;letter-spacing:.14em;text-transform:uppercase}"
        "details.card>summary.sum{list-style:none;cursor:pointer;display:flex;align-items:center;justify-content:space-between;gap:10px;padding:0 2px 10px;margin:0 0 12px;border-bottom:1px solid var(--border)}"
        "details.card>summary.sum::-webkit-details-marker{display:none}"
        "details.card>summary.sum h2{margin:0}"
        ".chev{width:26px;height:26px;border-radius:999px;border:1px solid var(--border);display:inline-flex;align-items:center;justify-content:center;color:var(--muted);background:var(--row);font-weight:900;line-height:1}"
        ".chev:before{content:'v'} details[open]>summary.sum .chev:before{content:'^'}"
        "summary.sum:hover .chev{border-color:rgba(14,165,233,.55);background:var(--row2)}"
        "table{width:100%;border-collapse:separate;border-spacing:0 8px}"
        "td{padding:10px 10px;background:var(--row);border-top:1px solid var(--border);border-bottom:1px solid var(--border)}"
        "tr td:first-child{border-left:1px solid var(--border);border-top-left-radius:12px;border-bottom-left-radius:12px}"
        "tr td:last-child{border-right:1px solid var(--border);border-top-right-radius:12px;border-bottom-right-radius:12px}"
        "td.k{color:var(--muted);width:44%;font-size:12.5px}"
        "td.v{font-weight:750;font-size:13px}"
        "button,select,input{font:inherit;padding:9px 11px;border-radius:12px;border:1px solid var(--border);background:var(--field);color:var(--fg)}"
        // Some browsers render the option list with a white background but still inherit the
        // select's text color. Force readable contrast in the dropdown list.
        "option{background:#fff;color:#0f172a}"
        "button{background:var(--row);cursor:pointer;font-weight:800}"
        "button:hover{border-color:rgba(14,165,233,.55);background:var(--row2)}"
        "button:disabled{opacity:.55;cursor:not-allowed}"
        "button.active{background:rgba(14,165,233,.18);border-color:rgba(14,165,233,.55);color:var(--fg)}"
        "button.danger{background:rgba(239,68,68,.12);border-color:rgba(239,68,68,.35)}"
        "button.danger:hover{border-color:rgba(239,68,68,.55);background:rgba(239,68,68,.16)}"
        "button:focus-visible,select:focus-visible,input:focus-visible,a:focus-visible{outline:3px solid rgba(14,165,233,.35);outline-offset:2px}"
        ".actions{display:flex;flex-wrap:wrap;gap:8px;margin-top:10px;align-items:center}"
        "#adv{flex-basis:100%;font-size:12.5px;color:var(--muted);padding-top:4px;line-height:1.35}"
        ".ok{color:var(--acc2)} .warn{color:var(--warn)} .bad{color:var(--bad)}"
        "span.ok,span.warn,span.bad{display:inline-flex;align-items:center;padding:2px 10px;border-radius:999px;border:1px solid var(--border);background:var(--row);font-size:12px;font-weight:800}"
        ".mono{font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,\"Liberation Mono\",\"Courier New\",monospace}"
        ".subtitle .mono,td.v .mono{overflow-wrap:anywhere;word-break:break-word}"
        "input[type=number]{width:130px} select{min-width:110px} .muted{color:var(--muted)}"
        ".field{display:flex;align-items:center;gap:8px;flex-wrap:wrap}"
        ".unit{color:var(--muted);font-size:12px}"
        ".help{display:inline-flex;align-items:center;justify-content:center;width:20px;height:20px;border:1px solid rgba(14,165,233,.55);border-radius:999px;font-size:12px;color:var(--fg);margin-left:8px;background:rgba(14,165,233,.08);cursor:pointer}"
        ".help:hover{filter:brightness(1.05)}"
        ".hint{margin-top:8px;padding:10px 12px;border:1px solid var(--border);border-radius:14px;background:var(--card2);color:var(--fg);font-size:12.5px;line-height:1.4;box-shadow:0 10px 22px rgba(0,0,0,.08)}"
        ".dirty{border-color:rgba(239,68,68,.65)!important;box-shadow:0 0 0 3px rgba(239,68,68,.16);background:rgba(239,68,68,.10)}"
        ".gh{margin-right:10px;color:var(--acc);text-decoration:none;border:1px solid var(--border);background:var(--row);padding:6px 10px;border-radius:999px;font-size:12.5px}"
        ".gh:hover{border-color:rgba(14,165,233,.55);background:var(--row2)}"
        ".lang{display:flex;align-items:center;gap:8px;white-space:nowrap;color:var(--muted);font-size:12.5px;flex-wrap:wrap;justify-content:flex-end}"
        "@media (max-width:760px){.page{padding:12px 10px 18px}.lang{width:100%;justify-content:flex-start;white-space:normal}.lang select{min-width:92px}.gh{margin-right:0}}"
        "pre{white-space:pre-wrap;word-break:break-word;background:var(--row);border:1px solid var(--border);border-radius:14px;padding:12px;overflow:auto;box-shadow:inset 0 1px 0 rgba(255,255,255,.05)} pre#logs{height:45vh}"
        ".overlay{position:fixed;inset:0;display:none;align-items:center;justify-content:center;background:rgba(0,0,0,.55);z-index:9999}"
        ".overlay .box{background:var(--card);border:1px solid var(--border);padding:16px 20px;border-radius:16px;color:var(--fg);text-align:center;min-width:260px;box-shadow:var(--shadow)}"
        "</style></head><body>"
        "<div id='ovr' class='overlay'><div class='box' id='ovr_msg'>Restarting…</div></div>"
        "<div class='page'>"
        "<div class='card top'><div class='hero'><div><div class='brand'><div class='title' id='t_title'>ESP32 RTSP Mic for BirdNET-Go</div><span class='badge' id='fwv'></span></div><div class='subtitle'><span id='t_rtsp_label'>RTSP:</span> <a id='rtsp_ip' class='mono' href='rtsp://");
    h += ip;
    h += F(
        ":8554/audio' target='_blank'>rtsp://");
    h += ip;
    h += F(
        ":8554/audio</a><span id='rtsp_mdns_wrap' style='margin-left:8px'> <span id='t_or'>or</span> <a id='rtsp_mdns' class='mono' href='rtsp://esp32mic.local:8554/audio' target='_blank'>rtsp://esp32mic.local:8554/audio</a></span></div></div>"
        "<div class='lang'><a href='https://github.com/Sukecz/birdnetgo-esp32-rtsp-mic' target='_blank' class='gh'>GitHub</a>"
        "<span class='muted' id='t_theme'>Theme:</span><select id='themeSel'><option id='opt_theme_auto' value='auto'>Auto</option><option id='opt_theme_light' value='light'>Light</option><option id='opt_theme_dark' value='dark'>Dark</option></select>"
        "</div></div></div>"
        "<div class='row'>"
        "<div class='card'><h2 id='t_status'>Status</h2><table>"
        "<tr><td class='k' id='t_ip'>IP Address</td><td class='v' id='ip'></td></tr>"
        "<tr><td class='k' id='t_wifi_rssi'>WiFi RSSI</td><td class='v' id='rssi'></td></tr>"
        "<tr><td class='k' id='t_wifi_tx'>WiFi TX Power</td><td class='v' id='wtx'></td></tr>"
        "<tr><td class='k' id='t_heap'>Free Heap (min)</td><td class='v' id='heap'></td></tr>"
        "<tr><td class='k' id='t_uptime'>Uptime</td><td class='v' id='uptime'></td></tr>"
        "<tr><td class='k' id='t_rtsp_server'>RTSP Server</td><td class='v' id='srv'></td></tr>"
        "<tr><td class='k' id='t_client'>Client</td><td class='v' id='client'></td></tr>"
        "<tr><td class='k' id='t_streaming'>Streaming</td><td class='v' id='stream'></td></tr>"
        "<tr><td class='k' id='t_pkt_rate'>Packet Rate</td><td class='v' id='rate'></td></tr>"
        "<tr><td class='k' id='t_last_connect'>Last RTSP Connect</td><td class='v' id='lcon'></td></tr>"
        "<tr><td class='k' id='t_last_play'>Last Stream Start</td><td class='v' id='lplay'></td></tr>"
        "</table><div class='actions'>"
        "<button onclick=\"act('server_start')\" id='b_srv_on'>Server ON</button>"
        "<button onclick=\"act('server_stop')\" id='b_srv_off'>Server OFF</button>"
        "<button onclick=\"act('reset_i2s')\" id='b_reset'>Reset I2S</button>"
        "<button onclick=\"rebootNow()\" id='b_reboot'>Reboot</button>"
        "<button onclick=\"defaultsNow()\" id='b_defaults' class='danger'>Defaults</button>"
        "<div id='adv' class='footer muted'></div></div>"

        "<div class='card'><h2 id='t_time'>Time & Network</h2><table>"
        "<tr><td class='k'><span id='t_time_sync_en'>Time Sync</span><span class='help' id='h_time_sync'>?</span></td><td class='v'><div class='field'><select id='sel_time_sync'><option value='on'>ON</option><option value='off'>OFF</option></select><button id='btn_time_sync_set' onclick=\"setv('time_sync',sel_time_sync.value)\">Set</button></div></td></tr>"
        "<tr id='row_time_sync_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_time_sync_hint'></div></td></tr>"
        "<tr><td class='k' id='t_time_sync'>Sync Status</td><td class='v' id='time_sync'></td></tr>"
        "<tr><td class='k' id='t_last_sync'>Last Sync</td><td class='v' id='last_sync'></td></tr>"
        "<tr><td class='k' id='t_local_time'>Device Local Time</td><td class='v mono' id='local_time'></td></tr>"
        "<tr><td class='k' id='t_utc_time'>UTC Time</td><td class='v mono' id='utc_time'></td></tr>"
        "<tr><td class='k'><span id='t_offset'>Time Offset</span><span class='help' id='h_offset'>?</span></td><td class='v'><div class='field'><select id='sel_offset'><option>-12</option><option>-11</option><option>-10</option><option>-9</option><option>-8</option><option>-7</option><option>-6</option><option>-5</option><option>-4</option><option>-3</option><option>-2</option><option>-1</option><option selected>0</option><option>1</option><option>2</option><option>3</option><option>4</option><option>5</option><option>6</option><option>7</option><option>8</option><option>9</option><option>10</option><option>11</option><option>12</option></select><span class='unit'>h</span><button id='btn_offset_set' onclick=\"setOffset()\">Set</button></div></td></tr>"
        "<tr id='row_offset_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_offset_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_stream_sched_en'>Stream Schedule</span><span class='help' id='h_stream_sched_en'>?</span></td><td class='v'><div class='field'><select id='sel_stream_sched_en'><option value='on'>ON</option><option value='off'>OFF</option></select><button id='btn_stream_sched_en_set' onclick=\"setv('stream_sched',sel_stream_sched_en.value)\">Set</button></div></td></tr>"
        "<tr id='row_stream_sched_en_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_stream_sched_en_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_stream_start'>Stream Start</span><span class='help' id='h_stream_start'>?</span></td><td class='v'><div class='field'><input id='in_stream_start' type='time' step='60'><button id='btn_stream_start_set' onclick=\"setStreamScheduleTime('start')\">Set</button></div></td></tr>"
        "<tr id='row_stream_start_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_stream_start_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_stream_stop'>Stream Stop</span><span class='help' id='h_stream_stop'>?</span></td><td class='v'><div class='field'><input id='in_stream_stop' type='time' step='60'><button id='btn_stream_stop_set' onclick=\"setStreamScheduleTime('stop')\">Set</button></div></td></tr>"
        "<tr id='row_stream_stop_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_stream_stop_hint'></div></td></tr>"
        "<tr><td class='k' id='t_stream_sched_state'>Schedule Status</td><td class='v' id='stream_sched_state'></td></tr>"
        "<tr><td class='k'><span id='t_deep_sleep_sched'>Deep Sleep (Outside Window)</span><span class='help' id='h_deep_sleep_sched'>?</span></td><td class='v'><div class='field'><select id='sel_deep_sleep_sched'><option value='on'>ON</option><option value='off'>OFF</option></select><button id='btn_deep_sleep_sched_set' onclick=\"setDeepSleepSched()\">Set</button></div></td></tr>"
        "<tr id='row_deep_sleep_sched_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_deep_sleep_sched_hint'></div></td></tr>"
        "<tr><td class='k' id='t_deep_sleep_state'>Deep Sleep Status</td><td class='v' id='deep_sleep_state'></td></tr>"
        "<tr><td class='k' id='t_mdns'>mDNS</td><td class='v'><div class='field'><select id='sel_mdns'><option value='on'>ON</option><option value='off'>OFF</option></select><button id='btn_mdns_set' onclick=\"setv('mdns_enable',sel_mdns.value)\">Set</button></div></td></tr>"
        "<tr><td class='k' id='t_stream_ip'>RTSP (IP)</td><td class='v'><a id='url_ip' class='mono' target='_blank'></a></td></tr>"
        "<tr><td class='k' id='t_stream_mdns'>RTSP (mDNS)</td><td class='v'><span id='url_mdns_wrap'><a id='url_mdns' class='mono' target='_blank'></a></span></td></tr>"
        "</table><div class='actions'>"
        "<button onclick=\"act('time_sync')\" id='b_sync_now'>Sync Time Now</button>"
        "<button onclick=\"confirmNetReset()\" id='b_net_reset' class='danger'>Reset Wi-Fi</button>"
        "</div></div>"

        "<div class='card'><h2 id='t_audio'>Audio</h2><table>"
        "<tr><td class='k'><span id='t_rate'>Sample Rate</span><span class='help' id='h_rate'>?</span><div class='hint' id='rate_hint' style='display:none'></div></td><td class='v'><div class='field'><input id='in_rate' type='number' step='1000' min='8000' max='96000'><span class='unit'>Hz</span><button id='btn_rate_set' onclick=\"setv('rate',in_rate.value)\">Set</button></div></td></tr>"
        "<tr id='row_rate_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_rate_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_gain'>Gain</span><span class='help' id='h_gain'>?</span></td><td class='v'><div class='field'><input id='in_gain' type='number' step='0.1' min='0.1' max='100'><span class='unit'>×</span><button id='btn_gain_set' onclick=\"setv('gain',in_gain.value)\">Set</button></div></td></tr>"
        "<tr id='row_gain_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_gain_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_hpf'>High-pass</span><span class='help' id='h_hpf'>?</span></td><td class='v'><div class='field'><select id='sel_hp'><option value='off'>OFF</option><option value='on'>ON</option></select><button id='btn_hp_set' onclick=\"setv('hp_enable',sel_hp.value)\">Set</button></div></td></tr>"
        "<tr id='row_hpf_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_hpf_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_hpf_cut'>HPF Cutoff</span><span class='help' id='h_hpf_cut'>?</span></td><td class='v'><div class='field'><input id='in_hp_cutoff' type='number' step='10' min='10' max='10000'><span class='unit'>Hz</span><button id='btn_hpf_cut_set' onclick=\"setv('hp_cutoff',in_hp_cutoff.value)\">Set</button></div></td></tr>"
        "<tr id='row_hpf_cut_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_hpf_cut_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_buf'>Buffer Size</span><span class='help' id='h_buf'>?</span></td><td class='v'><div class='field'>"
        "<select id='sel_buf'><option>256</option><option>512</option><option selected>1024</option><option>2048</option><option>4096</option><option>8192</option></select>"
        "<span class='unit'>samples</span><button id='btn_buf_set' onclick=\"setv('buffer',sel_buf.value)\">Set</button></div></td></tr>"
        "<tr id='row_buf_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_buf_hint'></div></td></tr>"
        "<tr><td class='k' id='t_latency'>Latency</td><td class='v' id='lat'></td></tr>"
        "<tr><td class='k'><span id='t_level'>Signal Level</span><span class='help' id='h_level'>?</span></td><td class='v' id='level'></td></tr>"
        "<tr id='row_level_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_level_hint'></div></td></tr>"
        "<tr><td class='k' id='t_profile'>Profile</td><td class='v' id='profile'></td></tr>"
        "</table></div>"

        "<div class='card'><h2 id='t_perf'>Reliability</h2><table>"
        "<tr><td class='k'><span id='t_auto'>Auto Recovery</span><span class='help' id='h_auto'>?</span></td><td class='v'><div class='field'><select id='in_auto'><option value='on'>ON</option><option value='off'>OFF</option></select><button id='btn_auto_set' onclick=\"setv('auto_recovery',in_auto.value)\">Set</button></div></td></tr>"
        "<tr id='row_auto_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_auto_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_thr_mode'>Threshold Mode</span><span class='help' id='h_thr_mode'>?</span></td><td class='v'><div class='field'><select id='in_thr_mode'><option value='auto'>Auto</option><option value='manual'>Manual</option></select><button id='btn_thrmode_set' onclick=\"setv('thr_mode',in_thr_mode.value)\">Set</button></div></td></tr>"
        "<tr id='row_thrmode_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_thr_mode_hint'></div></td></tr>"
        "<tr id='row_thr_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_thr_hint'></div></td></tr>"
        "<tr id='row_min_rate'><td class='k'><span id='t_thr'>Restart Threshold</span><span class='help' id='h_thr'>?</span></td><td class='v'><div class='field'><input id='in_thr' type='number' step='1' min='5' max='200'><span class='unit'>pkt/s</span><button id='btn_thr_set' onclick=\"setv('min_rate',in_thr.value)\">Set</button></div></td></tr>"
        "<tr><td class='k'><span id='t_sched'>Scheduled Reset</span><span class='help' id='h_sched'>?</span></td><td class='v'><div class='field'><select id='in_sched'><option value='on'>ON</option><option value='off' selected>OFF</option></select><button id='btn_sched_set' onclick=\"setv('sched_reset',in_sched.value)\">Set</button></div></td></tr>"
        "<tr id='row_sched_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_sched_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_hours'>Reset After</span><span class='help' id='h_hours'>?</span></td><td class='v'><div class='field'><input id='in_hours' type='number' step='1' min='1' max='168'><span class='unit'>h</span><button id='btn_hours_set' onclick=\"setv('reset_hours',in_hours.value)\">Set</button></div></td></tr>"
        "<tr id='row_hours_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_hours_hint'></div></td></tr>"
        "</table></div>"

        ""

        "<div class='card'><h2 id='t_thermal'>Thermal</h2><table>"
        "<tr><td class='k'><span id='t_therm_protect'>Overheat Protection</span><span class='help' id='h_therm_protect'>?</span></td><td class='v'><div class='field'><select id='sel_oh_enable'><option value='on'>ON</option><option value='off'>OFF</option></select><button id='btn_oh_enable' onclick=\"setv('oh_enable',sel_oh_enable.value)\">Set</button></div></td></tr>"
        "<tr id='row_therm_hint_protect' style='display:none'><td colspan='2'><div class='hint' id='txt_therm_hint_protect'></div></td></tr>"
        "<tr><td class='k'><span id='t_therm_limit'>Shutdown Limit</span><span class='help' id='h_therm_limit'>?</span></td><td class='v'><div class='field'><select id='sel_oh_limit'><option>30</option><option>35</option><option>40</option><option>45</option><option>50</option><option>55</option><option>60</option><option>65</option><option>70</option><option>75</option><option selected>80</option><option>85</option><option>90</option><option>95</option></select><span class='unit'>&deg;C</span><button id='btn_oh_limit' onclick=\"setv('oh_limit',sel_oh_limit.value)\">Set</button></div></td></tr>"
        "<tr id='row_therm_hint_limit' style='display:none'><td colspan='2'><div class='hint' id='txt_therm_hint_limit'></div></td></tr>"
        "<tr><td class='k' id='t_therm_status'>Status</td><td class='v' id='therm_status'></td></tr>"
        "<tr><td class='k' id='t_therm_now'>Current Temp</td><td class='v' id='therm_now'></td></tr>"
        "<tr><td class='k' id='t_therm_max'>Peak Temp</td><td class='v' id='therm_max'></td></tr>"
"<tr><td class='k' id='t_therm_cpu'>CPU Clock</td><td class='v' id='therm_cpu'></td></tr>"
"<tr><td class='k'><span id='t_therm_last'>Last Shutdown</span></td><td class='v'><div id='therm_last' class='hint'></div></td></tr>"
"<tr id='row_therm_latch' style='display:none'><td colspan='2'><div class='hint warn' id='txt_therm_latch'></div><div class='field' style='margin-top:8px'><button id='btn_therm_clear' class='danger' onclick=\"clearThermalLatch()\"></button></div></td></tr>"
"</table></div>"

        "<details id='advsec' class='card'><summary class='sum'><h2 id='t_advanced_settings'>Advanced Settings</h2><span class='chev' aria-hidden='true'></span></summary><table>"
        "<tr><td class='k'><span id='t_shift'>I2S Shift</span><span class='help' id='h_shift'>?</span></td><td class='v'><div class='field'><input id='in_shift' type='number' step='1' min='0' max='24'><span class='unit'>bits</span><button id='btn_shift_set' onclick=\"setv('shift',in_shift.value)\">Set</button></div></td></tr>"
        "<tr id='row_shift_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_shift_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_chk'>Check Interval</span><span class='help' id='h_chk'>?</span></td><td class='v'><div class='field'><input id='in_chk' type='number' step='1' min='1' max='60'><span class='unit'>min</span><button id='btn_chk_set' onclick=\"setv('check_interval',in_chk.value)\">Set</button></div></td></tr>"
        "<tr id='row_chk_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_chk_hint'></div></td></tr>"
        "<tr id='row_tx_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_tx_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_wifi_tx2'>TX Power</span><span class='help' id='h_tx'>?</span></td><td class='v'><div class='field'>"
        "<select id='sel_tx'><option>-1.0</option><option>2.0</option><option>5.0</option><option>7.0</option><option>8.5</option><option>11.0</option><option>13.0</option><option selected>15.0</option><option>17.0</option><option>18.5</option><option>19.0</option><option>19.5</option></select>"
        "<span class='unit'>dBm</span><button id='btn_tx_set' onclick=\"setv('wifi_tx',sel_tx.value)\">Set</button></div></td></tr>"
        "<tr><td class='k'><span id='t_cpu'>CPU Frequency</span><span class='help' id='h_cpu'>?</span></td><td class='v'><div class='field'>"
        "<select id='sel_cpu'><option>80</option><option>120</option><option selected>160</option></select><span class='unit'>MHz</span><button id='btn_cpu_set' onclick=\"setv('cpu_freq',sel_cpu.value)\">Set</button></div></td></tr>"
        "<tr id='row_cpu_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_cpu_hint'></div></td></tr>"
        "</table></details>"

        "<details class='card' open><summary class='sum'><h2 id='t_logs'>Logs</h2><span class='chev' aria-hidden='true'></span></summary><div class='actions'><button onclick=\"downloadLogs()\" id='b_log_dl'>Download Logs</button></div><pre id='logs' class='mono'></pre></details>"

        "</div>"
        "</div>"
        "<script>"
"const T={en:{title:'ESP32 RTSP Mic for BirdNET-Go',status:'Status',ip:'IP Address',wifi_rssi:'WiFi RSSI',wifi_tx:'WiFi TX Power',heap:'Free Heap (min)',uptime:'Uptime',rtsp_server:'RTSP Server',client:'Client',streaming:'Streaming',pkt_rate:'Packet Rate',last_connect:'Last RTSP Connect',last_play:'Last Stream Start',time:'Time & Network',time_sync:'Sync Status',time_sync_en:'Time Sync',last_sync:'Last Sync',local_time:'Device Local Time',utc_time:'UTC Time',time_unavailable:'Unavailable (unsynced)',offset:'Time Offset',mdns:'mDNS',stream_ip:'RTSP (IP)',stream_mdns:'RTSP (mDNS)',sync_now:'Sync Time Now',net_reset:'Reset Wi-Fi',net_reset_confirm:'Reset Wi-Fi credentials and reboot? You will need to reconfigure via the setup AP.',download_logs:'Download Logs',rtsp_label:'RTSP',or:'or',theme:'Theme',theme_auto:'Auto',theme_light:'Light',theme_dark:'Dark',yes:'YES',no:'NO',audio:'Audio',rate:'Sample Rate',gain:'Gain',buf:'Buffer Size',latency:'Latency',profile:'Profile',perf:'Reliability',auto:'Auto Recovery',wifi:'WiFi',wifi_tx2:'TX Power (dBm)',thermal:'Thermal',logs:'Logs',bsrvon:'Server ON',bsrvoff:'Server OFF',breset:'Reset I2S',breboot:'Reboot',bdefaults:'Defaults',restarting:'Restarting device...',resetting:'Restoring defaults and rebooting...',advanced_settings:'Advanced Settings',shift:'I2S Shift',thr:'Restart Threshold',chk:'Check Interval',thr_mode:'Threshold Mode',sched:'Scheduled Reset',hours:'Reset After',cpu:'CPU Frequency',set:'Set',profile_ultra:'Ultra-Low Latency (Higher CPU, may have dropouts)',profile_balanced:'Balanced (Moderate CPU, good stability)',profile_stable:'Stable Streaming (Lower CPU, excellent stability)',profile_high:'High Stability (Lowest CPU, max stability)',adv_buf512:'If stream drops, try Buffer 512 or higher.',adv_buf1024:'For unstable Wi-Fi, Buffer 1024 is usually best.',adv_gain:'High Gain can clip. Lower Gain or increase I2S Shift.',help_rate:'Higher sample rate gives more detail but uses more bandwidth.',help_gain:'Amplifies audio after I2S shift; too high will clip.',help_buf:'More samples per packet means more stability and higher latency.',help_auto:'Auto-restarts the audio pipeline when packet rate collapses.',help_tx:'Wi-Fi TX power; lowering can reduce RF noise.',help_shift:'Digital right shift before gain scaling.',help_thr:'Minimum packet rate before auto-recovery triggers.',help_chk:'How often performance is checked.',help_sched:'Periodic device restart for long-term stability.',help_hours:'Interval between scheduled restarts.',help_cpu:'Lower MHz runs cooler but can increase latency.',help_offset:'Manual timezone/clock correction in hours. Negative = west (UTC-), positive = east (UTC+).',help_time_sync:'If internet time is unavailable, disable Time Sync to avoid repeated NTP retries.',therm_protect:'Overheat Protection',therm_limit:'Shutdown Limit',therm_status:'Status',therm_now:'Current Temp',therm_max:'Peak Temp',therm_cpu:'CPU Clock',therm_last:'Last Shutdown',therm_status_ready:'Protection ready',therm_status_disabled:'Protection disabled',therm_status_latched:'Cooling required - restart manually',therm_status_sensor_fault:'Sensor unavailable - protection paused',therm_status_latched_persist:'Protection latched - acknowledge to re-enable',therm_last_none:'No shutdown recorded yet.',therm_last_fmt:'Stopped at %TEMP% C (limit %LIMIT% C) after %TIME% uptime (%AGO%).',therm_last_sensor_fault:'Thermal protection disabled: temperature sensor unavailable.',therm_latch_notice:'Thermal shutdown latched the RTSP server. Confirm only after hardware cools down.',therm_clear_btn:'Acknowledge and re-enable RTSP',therm_time_unknown:'unknown time',therm_time_ago_unknown:'just now',help_therm_protect:'Stops streaming when ESP32 exceeds the limit to protect board and mic preamp.',help_therm_limit:'Temperature threshold for shutdown. 80 C is a safe default.'}};"
        "const HELP_EXT_EN={hpf:'High-pass',hpf_cut:'HPF Cutoff',help_hpf:'Cuts low-frequency rumble.',help_hpf_cut:'High-pass cutoff in Hz.',help_thr_mode:'Auto = computed threshold; Manual = your value.',level:'Signal Level',help_level:'Keep peaks below clipping.',clip_ok:'OK',clip_warn:'High level - close to clipping.',clip_bad:'CLIPPING! Reduce Gain or raise I2S Shift.'};"
        "Object.assign(T.en, HELP_EXT_EN);"
        "let lang='en'; let theme=localStorage.getItem('theme')||'auto'; const $=id=>document.getElementById(id);"
        "function applyTheme(){ const root=document.documentElement; if(theme==='light'||theme==='dark'){ root.setAttribute('data-theme', theme); } else { root.removeAttribute('data-theme'); theme='auto'; } const ts=$('themeSel'); if(ts) ts.value=theme; }"
        "const SCHED_TXT={en:{stream_sched_en:'Stream Schedule',stream_start:'Stream Start',stream_stop:'Stream Stop',stream_sched_state:'Schedule Status',help_stream_sched_en:'Automatically turns RTSP server ON/OFF by local device time. If time is unavailable, fail-open keeps stream allowed.',help_stream_start:'Start of the allowed streaming window in local device time. Start and stop must differ.',help_stream_stop:'End of the allowed streaming window in local device time. Cross-midnight windows are supported (for example 22:00-06:00). If start equals stop, the window is empty and stream is blocked.',state_off:'Schedule disabled',state_wait:'Time unavailable - fail-open (stream allowed)',state_allow:'Inside allowed window',state_block:'Outside allowed window',state_invalid:'Invalid window (start = stop) - stream blocked',deep_sleep_sched:'Deep Sleep (Outside Window)',deep_sleep_state:'Deep Sleep Status',help_deep_sleep_sched:'Uses deep sleep only outside the stream window. Requires valid synchronized time and no active client/stream. While sleeping, RTSP, Web UI, API, and OTA are unavailable.',ds_confirm_table:'Deep Sleep confirmation:\\n\\nPower-saving mode outside the allowed stream window.\\n\\nImportant behavior:\\n- RTSP, Web UI, API, and OTA are unreachable while sleeping\\n- Any active stream/client is stopped before entering sleep\\n- Sleep is allowed only with valid synchronized time\\n- If time is unsynced, deep sleep is blocked and device stays awake\\n- Timer wake only; one cycle is capped to 8 hours\\n- Device wakes about 5 minutes before next stream window\\n\\nEnable only if temporary offline periods are acceptable.',ds_confirm_1:'Enable Deep Sleep (Outside Window) now?',ds_confirm_2:'Second confirmation: enable deep sleep and allow temporary offline periods?',ds_disabled:'Deep sleep disabled',ds_need_sched:'Enable Stream Schedule first',ds_invalid:'Invalid schedule window (start = stop) - deep sleep blocked',ds_need_time:'Time not synchronized - deep sleep blocked',ds_inside:'Inside stream window - device stays awake',ds_grace:'Startup grace period - waiting',ds_wait:'Outside window detected - waiting for stability',ds_client:'Client connected - sleep postponed',ds_streaming:'Streaming active - sleep postponed',ds_reboot:'Reboot pending - sleep postponed',ds_soon:'Next stream window is soon - no sleep',ds_ready:'Outside window - deep sleep armed'}};"
        "function minsToHHMM(mins){ mins=parseInt(mins,10); if(isNaN(mins)) mins=0; mins=((mins%1440)+1440)%1440; const h=Math.floor(mins/60), m=mins%60; return String(h).padStart(2,'0')+':'+String(m).padStart(2,'0'); }"
        "function hhmmToMins(v){ if(!v || v.indexOf(':')<0) return null; const p=v.split(':'); if(p.length!==2) return null; const h=parseInt(p[0],10), m=parseInt(p[1],10); if(isNaN(h)||isNaN(m)||h<0||h>23||m<0||m>59) return null; return h*60+m; }"
        "function fmtSecondsBrief(sec){ sec=parseInt(sec,10); if(!isFinite(sec)||sec<0) sec=0; const h=Math.floor(sec/3600), m=Math.floor((sec%3600)/60), s=sec%60; if(h>0) return h+'h '+m+'m'; if(m>0) return m+'m '+s+'s'; return s+'s'; }"
        "function setStreamScheduleTime(kind){ const id=(kind==='start')?'in_stream_start':'in_stream_stop'; const key=(kind==='start')?'stream_start_min':'stream_stop_min'; const el=$(id); if(!el) return; const mins=hhmmToMins(el.value); if(mins===null) return; setv(key,String(mins)); }"
        "function setDeepSleepSched(){ const el=$('sel_deep_sleep_sched'); if(!el) return; const v=(el.value==='on')?'on':'off'; const L=(SCHED_TXT[lang]||SCHED_TXT.en); if(v==='on'){ const table=L.ds_confirm_table||L.ds_confirm_1||''; if(!confirm(table)) return; if(!confirm(L.ds_confirm_2)) return; } setv('deep_sleep_sched',v); }"
        "function deepSleepStatusHTML(code,nextSec,L){ let cls='muted', txt=L.ds_disabled||'Deep sleep disabled'; if(code==='schedule_disabled'){ cls='bad'; txt=L.ds_need_sched; } else if(code==='schedule_invalid'){ cls='bad'; txt=L.ds_invalid; } else if(code==='time_invalid'){ cls='warn'; txt=L.ds_need_time; } else if(code==='inside_window'){ cls='ok'; txt=L.ds_inside; } else if(code==='grace_boot'){ cls='muted'; txt=L.ds_grace; } else if(code==='outside_stabilizing'){ cls='muted'; txt=L.ds_wait; } else if(code==='client_connected'){ cls='warn'; txt=L.ds_client; } else if(code==='streaming_active'){ cls='warn'; txt=L.ds_streaming; } else if(code==='reboot_pending'){ cls='warn'; txt=L.ds_reboot; } else if(code==='next_window_soon'){ cls='muted'; txt=L.ds_soon; } else if(code==='ready'){ cls='warn'; txt=L.ds_ready; } if(nextSec>0){ txt += ' ('+fmtSecondsBrief(nextSec)+')'; } return '<span class='+cls+'>'+txt+'</span>'; }"
        "function applyScheduleLang(){ const L=(SCHED_TXT[lang]||SCHED_TXT.en); const st=(id,t)=>{const e=$(id); if(e) e.textContent=t}; const tt=(id,t)=>{const e=$(id); if(e) e.setAttribute('title',t)}; st('t_stream_sched_en',L.stream_sched_en); st('t_stream_start',L.stream_start); st('t_stream_stop',L.stream_stop); st('t_stream_sched_state',L.stream_sched_state); st('txt_stream_sched_en_hint',L.help_stream_sched_en); st('txt_stream_start_hint',L.help_stream_start); st('txt_stream_stop_hint',L.help_stream_stop); st('t_deep_sleep_sched',L.deep_sleep_sched); st('t_deep_sleep_state',L.deep_sleep_state); st('txt_deep_sleep_sched_hint',L.help_deep_sleep_sched); tt('h_stream_sched_en',L.help_stream_sched_en); tt('h_stream_start',L.help_stream_start); tt('h_stream_stop',L.help_stream_stop); tt('h_deep_sleep_sched',L.help_deep_sleep_sched); const setTxt=(T[lang]&&T[lang].set)?T[lang].set:'Set'; st('btn_stream_sched_en_set',setTxt); st('btn_stream_start_set',setTxt); st('btn_stream_stop_set',setTxt); st('btn_deep_sleep_sched_set',setTxt); }"
"function applyLang(){const L=T[lang]; const st=(id,t)=>{const e=$(id); if(e) e.textContent=t}; const help=(k)=>{const b=L[k]||''; return b}; st('t_title',L.title); st('t_status',L.status); st('t_ip',L.ip); st('t_wifi_rssi',L.wifi_rssi); st('t_wifi_tx',L.wifi_tx); st('t_heap',L.heap); st('t_uptime',L.uptime); st('t_rtsp_label',L.rtsp_label); st('t_or',L.or); st('t_theme',L.theme); st('opt_theme_auto',L.theme_auto); st('opt_theme_light',L.theme_light); st('opt_theme_dark',L.theme_dark); st('t_rtsp_server',L.rtsp_server); st('t_client',L.client); st('t_streaming',L.streaming); st('t_pkt_rate',L.pkt_rate); st('t_last_connect',L.last_connect); st('t_last_play',L.last_play); st('t_time',L.time); st('t_time_sync',L.time_sync); st('t_time_sync_en',L.time_sync_en); st('t_last_sync',L.last_sync); st('t_local_time',L.local_time); st('t_utc_time',L.utc_time); st('t_offset',L.offset); st('t_mdns',L.mdns); st('t_stream_ip',L.stream_ip); st('t_stream_mdns',L.stream_mdns); st('t_audio',L.audio); st('t_rate',L.rate); st('t_gain',L.gain); st('t_buf',L.buf); st('t_latency',L.latency); st('t_level',L.level); st('t_profile',L.profile); st('t_perf',L.perf); st('t_auto',L.auto); st('t_wifi',L.wifi); st('t_wifi_tx2',L.wifi_tx2); st('t_thermal',L.thermal); st('t_therm_protect',L.therm_protect); st('t_therm_limit',L.therm_limit); st('t_therm_status',L.therm_status); st('t_therm_now',L.therm_now); st('t_therm_max',L.therm_max); st('t_therm_cpu',L.therm_cpu); st('t_therm_last',L.therm_last); st('t_logs',L.logs); st('b_srv_on',L.bsrvon); st('b_srv_off',L.bsrvoff); st('b_reset',L.breset); st('b_reboot',L.breboot); st('b_defaults',L.bdefaults); st('b_sync_now',L.sync_now); st('b_net_reset',L.net_reset); st('b_log_dl',L.download_logs); st('t_advanced_settings',L.advanced_settings); st('t_shift',L.shift); st('t_thr',L.thr); st('t_chk',L.chk); st('t_thr_mode',L.thr_mode); st('t_sched',L.sched); st('t_hours',L.hours); st('t_cpu',L.cpu); const hm=(id,k)=>{const e=$(id); if(e) e.setAttribute('title',help(k))}; hm('h_rate','help_rate'); hm('h_gain','help_gain'); hm('h_hpf','help_hpf'); hm('h_hpf_cut','help_hpf_cut'); hm('h_buf','help_buf'); hm('h_auto','help_auto'); hm('h_tx','help_tx'); hm('h_thr','help_thr'); hm('h_chk','help_chk'); hm('h_shift','help_shift'); hm('h_sched','help_sched'); hm('h_hours','help_hours'); hm('h_cpu','help_cpu'); hm('h_thr_mode','help_thr_mode'); hm('h_level','help_level'); hm('h_therm_protect','help_therm_protect'); hm('h_therm_limit','help_therm_limit'); hm('h_offset','help_offset'); hm('h_time_sync','help_time_sync'); st('btn_rate_set',L.set); st('btn_gain_set',L.set); st('btn_buf_set',L.set); st('btn_auto_set',L.set); st('btn_thrmode_set',L.set); st('btn_thr_set',L.set); st('btn_sched_set',L.set); st('btn_hours_set',L.set); st('btn_shift_set',L.set); st('btn_chk_set',L.set); st('btn_tx_set',L.set); st('btn_cpu_set',L.set); st('btn_oh_enable',L.set); st('btn_oh_limit',L.set); st('btn_offset_set',L.set); st('btn_time_sync_set',L.set); st('btn_mdns_set',L.set); st('btn_hp_set',L.set); st('btn_hpf_cut_set',L.set); const sht=(id,k)=>{const e=$(id); if(e) e.textContent=help(k)}; sht('txt_rate_hint','help_rate'); sht('txt_gain_hint','help_gain'); sht('txt_hpf_hint','help_hpf'); sht('txt_hpf_cut_hint','help_hpf_cut'); sht('txt_buf_hint','help_buf'); sht('txt_auto_hint','help_auto'); sht('txt_thr_hint','help_thr'); sht('txt_thr_mode_hint','help_thr_mode'); sht('txt_sched_hint','help_sched'); sht('txt_hours_hint','help_hours'); sht('txt_shift_hint','help_shift'); sht('txt_chk_hint','help_chk'); sht('txt_tx_hint','help_tx'); sht('txt_cpu_hint','help_cpu'); sht('txt_level_hint','help_level'); sht('txt_therm_hint_protect','help_therm_protect'); sht('txt_therm_hint_limit','help_therm_limit'); sht('txt_offset_hint','help_offset'); sht('txt_time_sync_hint','help_time_sync'); st('t_hpf',L.hpf); st('t_hpf_cut',L.hpf_cut); document.title=L.title;}"
        "function profileText(buf){const L=T[lang]; buf=parseInt(buf,10)||0; if(buf<=256) return L.profile_ultra; if(buf<=512) return L.profile_balanced; if(buf<=1024) return L.profile_stable; return L.profile_high;}"
"function fmtBool(b){const L=T[lang]; return b?`<span class=ok>${L.yes||'YES'}</span>`:`<span class=bad>${L.no||'NO'}</span>`}"
        "function fmtSrv(b){return b?'<span class=ok>ENABLED</span>':'<span class=bad>DISABLED</span>'}"
        "function showOverlay(msg){ $('ovr_msg').textContent=msg; $('ovr').style.display='flex'; }"
        "function rebootSequence(kind){ const L=T[lang]; const msg=(kind==='factory_reset')?L.resetting:L.restarting; showOverlay(msg); function tick(){ fetch('/api/status',{cache:'no-store'}).then(r=>{ if(r.ok){ location.reload(); } else { setTimeout(tick,2000); } }).catch(()=>setTimeout(tick,2000)); } setTimeout(tick,4000); }"
        "function act(a){fetch('/api/action/'+a,{cache:'no-store'}).then(r=>r.json()).then(loadAll)}"
        "function rebootNow(){ rebootSequence('reboot'); act('reboot'); }"
        "function defaultsNow(){ rebootSequence('factory_reset'); act('factory_reset'); }"
        "const locks={}; const edits={};"
        "const LOG_STICKY_PX=24; let logAutoScroll=true;"
        "function isLogNearBottom(el){ return (el.scrollHeight - el.scrollTop - el.clientHeight) <= LOG_STICKY_PX; }"
        "function bindLogScroll(){ const lg=$('logs'); if(!lg || lg.dataset.bound==='1') return; lg.dataset.bound='1'; lg.addEventListener('scroll',()=>{ logAutoScroll=isLogNearBottom(lg); }); }"
        "function setv(k,v){v=String(v??'').trim().replace(',', '.'); if(v==='')return; locks[k]=Date.now()+5000; delete edits[k]; fetch('/api/set?key='+encodeURIComponent(k)+'&value='+encodeURIComponent(v),{cache:'no-store'}).then(r=>r.json()).then(loadAll)}"
        "function bindSaver(el,key){if(!el)return; el.addEventListener('keydown',e=>{if(e.key==='Enter'){setv(key,el.value)}})}"
        "function trackEdit(el,key){if(!el)return; const bump=()=>{edits[key]=Date.now()+10000; toggleDirty(el,key)}; el.addEventListener('input',bump); el.addEventListener('change',bump)}"
        "function toggleDirty(el,key){ if(!el)return; const now=Date.now(); const d=(edits[key]&&now<edits[key]); el.classList.toggle('dirty', !!d); if(!d){ delete edits[key]; } }"
        "function setToggleState(on){const onb=$('b_srv_on'), offb=$('b_srv_off'); if(onb&&offb){onb.classList.toggle('active',on); offb.classList.toggle('active',!on); onb.disabled=on; offb.disabled=!on;}}"
        "function renderTimeRows(j){ const L=T[lang]||T.en; const lm=$('local_time'), um=$('utc_time'); const m=parseInt(j.time_offset_min||0,10)||0; const s=(m>=0)?'+':'-'; const a=Math.abs(m); const oh=String(Math.floor(a/60)).padStart(2,'0'); const om=String(a%60).padStart(2,'0'); const off='UTC'+s+oh+':'+om; const synced=!!j.time_synced; if(lm){ if(synced && j.local_time && j.local_time!=='unavailable'){ lm.innerHTML='<span class=ok>'+j.local_time+' ('+off+')</span>'; } else { lm.innerHTML='<span class=warn>'+(L.time_unavailable||'Unavailable (unsynced)')+'</span>'; } } if(um){ if(synced && j.utc_time && j.utc_time!=='unavailable'){ um.innerHTML='<span class=ok>'+j.utc_time+'</span>'; } else { um.innerHTML='<span class=warn>'+(L.time_unavailable||'Unavailable (unsynced)')+'</span>'; } } }"
        "function loadStreamSchedule(j){ const now=Date.now(); const L=(SCHED_TXT[lang]||SCHED_TXT.en); const en=$('sel_stream_sched_en'); if(en){ const editing=(edits['stream_sched']&&now<edits['stream_sched']); if(!(locks['stream_sched']&&now<locks['stream_sched']) && !editing) en.value=j.stream_schedule_enabled?'on':'off'; toggleDirty(en,'stream_sched'); } const st=$('in_stream_start'); if(st){ const editing=(edits['stream_start_min']&&now<edits['stream_start_min']); if(!(locks['stream_start_min']&&now<locks['stream_start_min']) && !editing) st.value=minsToHHMM(j.stream_schedule_start_min||0); toggleDirty(st,'stream_start_min'); } const sp=$('in_stream_stop'); if(sp){ const editing=(edits['stream_stop_min']&&now<edits['stream_stop_min']); if(!(locks['stream_stop_min']&&now<locks['stream_stop_min']) && !editing) sp.value=minsToHHMM(j.stream_schedule_stop_min||0); toggleDirty(sp,'stream_stop_min'); } const ds=$('sel_deep_sleep_sched'); if(ds){ const editing=(edits['deep_sleep_sched']&&now<edits['deep_sleep_sched']); if(!(locks['deep_sleep_sched']&&now<locks['deep_sleep_sched']) && !editing) ds.value=j.deep_sleep_sched_enabled?'on':'off'; toggleDirty(ds,'deep_sleep_sched'); } const state=$('stream_sched_state'); if(state){ const start=(j.stream_schedule_start_min||0), stop=(j.stream_schedule_stop_min||0); const win=' ('+minsToHHMM(start)+'-'+minsToHHMM(stop)+')'; if(!j.stream_schedule_enabled){ state.innerHTML='<span class=muted>'+L.state_off+'</span>'; } else if(start===stop){ state.innerHTML='<span class=bad>'+L.state_invalid+win+'</span>'; } else if(!j.stream_schedule_time_valid){ state.innerHTML='<span class=warn>'+L.state_wait+win+'</span>'; } else if(j.stream_schedule_allow_now){ state.innerHTML='<span class=ok>'+L.state_allow+win+'</span>'; } else { state.innerHTML='<span class=bad>'+L.state_block+win+'</span>'; } } const dsState=$('deep_sleep_state'); if(dsState){ const code=String(j.deep_sleep_status_code||'disabled'); const next=parseInt(j.deep_sleep_next_sec||0,10)||0; dsState.innerHTML=deepSleepStatusHTML(code,next,L); } }"
"function loadStatus(){fetch('/api/status',{cache:'no-store'}).then(r=>r.json()).then(j=>{ $('ip').textContent=j.ip; $('rssi').textContent=j.wifi_rssi+' dBm'; $('wtx').textContent=j.wifi_tx_dbm.toFixed(1)+' dBm'; $('heap').textContent=j.free_heap_kb+' KB ('+j.min_free_heap_kb+' KB)'; $('uptime').textContent=j.uptime; $('srv').innerHTML=fmtSrv(j.rtsp_server_enabled); setToggleState(j.rtsp_server_enabled); $('client').textContent=j.client || 'Waiting...'; $('stream').innerHTML=fmtBool(j.streaming); $('rate').textContent=j.current_rate_pkt_s+' pkt/s'; $('lcon').textContent=j.last_rtsp_connect; $('lplay').textContent=j.last_stream_start; $('time_sync').innerHTML=fmtBool(j.time_synced); $('last_sync').textContent=j.last_time_sync; renderTimeRows(j); const now=Date.now(); const stx=$('sel_tx'); if(stx){ const editing=(edits['wifi_tx']&&now<edits['wifi_tx']); if(!(locks['wifi_tx']&&now<locks['wifi_tx']) && !editing) stx.value=j.wifi_tx_dbm.toFixed(1); toggleDirty(stx,'wifi_tx'); } const md=$('sel_mdns'); if(md){ const editing=(edits['mdns_enable']&&now<edits['mdns_enable']); if(!(locks['mdns_enable']&&now<locks['mdns_enable']) && !editing) md.value=j.mdns_enabled?'on':'off'; toggleDirty(md,'mdns_enable'); } const tse=$('sel_time_sync'); if(tse){ const editing=(edits['time_sync']&&now<edits['time_sync']); if(!(locks['time_sync']&&now<locks['time_sync']) && !editing) tse.value=j.time_sync_enabled?'on':'off'; toggleDirty(tse,'time_sync'); } const offSel=$('sel_offset'); if(offSel){ const editing=(edits['time_offset']&&now<edits['time_offset']); const hours=Math.round((j.time_offset_min||0)/60); if(!(locks['time_offset']&&now<locks['time_offset']) && !editing) offSel.value=String(hours); toggleDirty(offSel,'time_offset'); } const fv=$('fwv'); if(fv && j.fw_version){ fv.textContent='v'+j.fw_version; } const ui=$('url_ip'); if(ui){ ui.textContent=j.stream_url_ip; ui.href=j.stream_url_ip; } const um=$('url_mdns'); const umw=$('url_mdns_wrap'); if(um){ if(j.mdns_enabled){ um.textContent=j.stream_url_mdns; um.href=j.stream_url_mdns; um.style.opacity='1'; if(umw) umw.style.display=''; } else { um.textContent='—'; um.removeAttribute('href'); um.style.opacity='0.6'; if(umw) umw.style.display='none'; } } const topIp=$('rtsp_ip'); if(topIp){ topIp.textContent=j.stream_url_ip; topIp.href=j.stream_url_ip; } const topMdns=$('rtsp_mdns'); const wrap=$('rtsp_mdns_wrap'); if(topMdns){ if(j.mdns_enabled){ topMdns.textContent=j.stream_url_mdns; topMdns.href=j.stream_url_mdns; if(wrap) wrap.style.display='inline'; } else { topMdns.textContent=''; topMdns.removeAttribute('href'); if(wrap) wrap.style.display='none'; } } loadStreamSchedule(j); })}"
        "function loadAudio(){fetch('/api/audio_status',{cache:'no-store'}).then(r=>r.json()).then(j=>{ const r=$('in_rate'); const g=$('in_gain'); const sb=$('sel_buf'); const s=$('in_shift'); const hp=$('sel_hp'); const hpc=$('in_hp_cutoff'); const now=Date.now(); if(r){ const editing=(edits['rate']&&now<edits['rate']); if(!(locks['rate']&&now<locks['rate']) && !editing) r.value=j.sample_rate; toggleDirty(r,'rate'); } if(g){ const editing=(edits['gain']&&now<edits['gain']); if(!(locks['gain']&&now<locks['gain']) && !editing) g.value=j.gain.toFixed(2); toggleDirty(g,'gain'); } if(sb){ const editing=(edits['buffer']&&now<edits['buffer']); if(!(locks['buffer']&&now<locks['buffer']) && !editing) sb.value=j.buffer_size; toggleDirty(sb,'buffer'); } if(s){ const editing=(edits['shift']&&now<edits['shift']); if(!(locks['shift']&&now<locks['shift']) && !editing) s.value=j.i2s_shift; toggleDirty(s,'shift'); } if(hp){ const editing=(edits['hp_enable']&&now<edits['hp_enable']); if(!(locks['hp_enable']&&now<locks['hp_enable']) && !editing) hp.value=j.hp_enable?'on':'off'; toggleDirty(hp,'hp_enable'); } if(hpc){ const editing=(edits['hp_cutoff']&&now<edits['hp_cutoff']); if(!(locks['hp_cutoff']&&now<locks['hp_cutoff']) && !editing) hpc.value=j.hp_cutoff_hz; toggleDirty(hpc,'hp_cutoff'); } $('lat').textContent=j.latency_ms.toFixed(1)+' ms'; $('profile').textContent=profileText(j.buffer_size); const L=T[lang]; const lvl=$('level'); if(lvl){ const pct=j.peak_pct||0, db=j.peak_dbfs||-90, clip=j.clip, cc=j.clip_count||0; if(clip){ lvl.innerHTML = `<span class='bad'>${L.clip_bad}</span> Peak ${pct.toFixed(0)}% (${db.toFixed(1)} dBFS), clips: ${cc}`; } else if(pct>=90){ lvl.innerHTML = `<span class='warn'>${L.clip_warn}</span> Peak ${pct.toFixed(0)}% (${db.toFixed(1)} dBFS)`; } else { lvl.textContent = `Peak ${pct.toFixed(0)}% (${db.toFixed(1)} dBFS) — ${L.clip_ok}`; } } updateAdvice(j); })}"
        "function updateAdvice(a){const L=T[lang]; let tips=[]; if(a.buffer_size<512) tips.push(L.adv_buf512); if(a.buffer_size<1024) tips.push(L.adv_buf1024); if(a.gain>20) tips.push(L.adv_gain); $('adv').textContent=tips.join(' ');}"
        "function loadPerf(){fetch('/api/perf_status',{cache:'no-store'}).then(r=>r.json()).then(j=>{ const el=$('in_auto'); if(el) el.value=j.auto_recovery?'on':'off'; const thr=$('in_thr'); const chk=$('in_chk'); const mode=$('in_thr_mode'); const sch=$('in_sched'); const hrs=$('in_hours'); const now=Date.now(); if(mode){ const editing=(edits['thr_mode']&&now<edits['thr_mode']); if(!(locks['thr_mode']&&now<locks['thr_mode']) && !editing) mode.value=j.auto_threshold?'auto':'manual'; toggleDirty(mode,'thr_mode'); } if(thr){ const editing=(edits['min_rate']&&now<edits['min_rate']); if(!(locks['min_rate']&&now<locks['min_rate']) && !editing) thr.value=j.restart_threshold_pkt_s; toggleDirty(thr,'min_rate'); } if(chk){ const editing=(edits['check_interval']&&now<edits['check_interval']); if(!(locks['check_interval']&&now<locks['check_interval']) && !editing) chk.value=j.check_interval_min; toggleDirty(chk,'check_interval'); } if(sch){ const editing=(edits['sched_reset']&&now<edits['sched_reset']); if(!(locks['sched_reset']&&now<locks['sched_reset']) && !editing) sch.value=j.scheduled_reset?'on':'off'; toggleDirty(sch,'sched_reset'); } if(hrs){ const editing=(edits['reset_hours']&&now<edits['reset_hours']); if(!(locks['reset_hours']&&now<locks['reset_hours']) && !editing) hrs.value=j.reset_hours; toggleDirty(hrs,'reset_hours'); } $('row_min_rate').style.display=j.auto_threshold?'none':''; })}"
"function loadTherm(){fetch('/api/thermal',{cache:'no-store'}).then(r=>r.json()).then(j=>{ const now=Date.now(); const L=T[lang]; const en=$('sel_oh_enable'); if(en){ const editing=(edits['oh_enable']&&now<edits['oh_enable']); if(!(locks['oh_enable']&&now<locks['oh_enable']) && !editing) en.value=j.protection_enabled?'on':'off'; toggleDirty(en,'oh_enable'); } const lim=$('sel_oh_limit'); if(lim){ const editing=(edits['oh_limit']&&now<edits['oh_limit']); if(!(locks['oh_limit']&&now<locks['oh_limit']) && !editing) lim.value=(Number(j.shutdown_c)||80).toFixed(0); toggleDirty(lim,'oh_limit'); } const sc=$('sel_cpu'); if(sc && !(locks['cpu_freq']&&now<locks['cpu_freq'])){ sc.value=j.cpu_mhz; } const currentValid=(j.current_valid&&typeof j.current_c==='number'&&isFinite(j.current_c)); const cur=$('therm_now'); if(cur) cur.textContent=currentValid?j.current_c.toFixed(1)+' °C':'N/A'; const max=$('therm_max'); if(max){ const maxValid=(typeof j.max_c==='number'&&isFinite(j.max_c)); max.textContent=maxValid?j.max_c.toFixed(1)+' °C':'N/A'; } const cpu=$('therm_cpu'); if(cpu) cpu.textContent=j.cpu_mhz+' MHz'; const status=$('therm_status'); if(status){ if(j.sensor_fault){ status.innerHTML='<span class=warn>'+L.therm_status_sensor_fault+'</span>'; } else if(j.latched_persist){ status.innerHTML='<span class=warn>'+L.therm_status_latched_persist+'</span>'; } else if(!j.protection_enabled){ status.innerHTML='<span class=bad>'+L.therm_status_disabled+'</span>'; } else if(j.manual_restart || j.latched){ status.innerHTML='<span class=warn>'+L.therm_status_latched+'</span>'; } else { status.innerHTML='<span class=ok>'+L.therm_status_ready+'</span>'; } } const latchRow=$('row_therm_latch'); const latchMsg=$('txt_therm_latch'); const latchBtn=$('btn_therm_clear'); if(latchRow){ if(j.latched_persist){ latchRow.style.display=''; if(latchMsg) latchMsg.textContent=L.therm_latch_notice; if(latchBtn){ latchBtn.textContent=L.therm_clear_btn; latchBtn.disabled=false; } } else { latchRow.style.display='none'; if(latchBtn){ latchBtn.disabled=true; } } } const last=$('therm_last'); if(last){ if(j.sensor_fault){ last.textContent=L.therm_last_sensor_fault; } else if(j.last_trip_ts && j.last_trip_ts.length){ let msg=L.therm_last_fmt; const temp=(typeof j.last_trip_c==='number'&&isFinite(j.last_trip_c)&&j.last_trip_c>0)?j.last_trip_c.toFixed(1):'0'; const limit=(Number(j.shutdown_c)||0).toFixed(0); const ts=j.last_trip_ts||L.therm_time_unknown; const ago=j.last_trip_since||L.therm_time_ago_unknown; msg=msg.replace('%TEMP%',temp).replace('%LIMIT%',limit).replace('%TIME%',ts).replace('%AGO%',ago); last.textContent=msg; if(j.latched_persist){ last.textContent+=' — '+L.therm_status_latched_persist; } else if(j.manual_restart){ last.textContent+=' — '+L.therm_status_latched; } } else if(j.last_reason && j.last_reason.length){ last.textContent=j.last_reason; } else { last.textContent=L.therm_last_none; } } })}"
"function loadLogs(){ const lg=$('logs'); if(!lg) return; bindLogScroll(); fetch('/api/logs',{cache:'no-store'}).then(r=>r.text()).then(t=>{ const pinBottom = logAutoScroll || isLogNearBottom(lg); const bottomOffset = lg.scrollHeight - lg.scrollTop; lg.textContent=t; if(pinBottom){ lg.scrollTop=lg.scrollHeight; } else { lg.scrollTop=Math.max(0, lg.scrollHeight-bottomOffset); } })}"
"function downloadLogs(){ fetch('/api/logs?download=1',{cache:'no-store'}).then(r=>r.blob()).then(b=>{ const url=URL.createObjectURL(b); const a=document.createElement('a'); a.href=url; a.download='esp32mic-log.txt'; a.click(); setTimeout(()=>URL.revokeObjectURL(url),2000); }); }"
"function setOffset(){ const sel=$('sel_offset'); if(!sel) return; const h=parseInt(sel.value,10); if(isNaN(h)) return; setv('time_offset', h*60); }"
"function confirmNetReset(){ const L=T[lang]; if(confirm(L.net_reset_confirm||'Reset Wi-Fi settings and reboot?')){ act('network_reset'); } }"
"function loadAll(){loadStatus();loadAudio();loadPerf();loadTherm();loadLogs()}"
"function clearThermalLatch(){ const btn=$('btn_therm_clear'); if(btn) btn.disabled=true; fetch('/api/thermal/clear',{method:'POST',cache:'no-store'}).then(r=>r.json()).then(j=>{ if(!j.ok){ console.warn('Thermal latch clear rejected'); } loadAll(); }).catch(()=>loadAll());}"
"setInterval(loadAll,3000);"
        "const tsel=$('themeSel'); if(tsel){ tsel.value=theme; tsel.onchange=()=>{theme=tsel.value; localStorage.setItem('theme',theme); applyTheme();}; } applyTheme();"
        "applyLang(); applyScheduleLang();"
        "bindSaver($('in_rate'),'rate'); bindSaver($('in_gain'),'gain'); bindSaver($('in_shift'),'shift'); bindSaver($('in_thr'),'min_rate'); bindSaver($('in_chk'),'check_interval'); bindSaver($('in_hours'),'reset_hours'); bindSaver($('in_hp_cutoff'),'hp_cutoff');"
        "trackEdit($('in_rate'),'rate'); trackEdit($('in_gain'),'gain'); trackEdit($('in_shift'),'shift'); trackEdit($('in_thr'),'min_rate'); trackEdit($('in_chk'),'check_interval'); trackEdit($('in_hours'),'reset_hours'); trackEdit($('in_hp_cutoff'),'hp_cutoff');"
"trackEdit($('in_auto'),'auto_recovery'); trackEdit($('in_thr_mode'),'thr_mode'); trackEdit($('in_sched'),'sched_reset'); trackEdit($('sel_buf'),'buffer'); trackEdit($('sel_tx'),'wifi_tx'); trackEdit($('sel_hp'),'hp_enable'); trackEdit($('sel_cpu'),'cpu_freq'); trackEdit($('sel_oh_enable'),'oh_enable'); trackEdit($('sel_oh_limit'),'oh_limit');"
"trackEdit($('sel_mdns'),'mdns_enable'); trackEdit($('sel_offset'),'time_offset'); trackEdit($('sel_time_sync'),'time_sync'); trackEdit($('sel_stream_sched_en'),'stream_sched'); trackEdit($('in_stream_start'),'stream_start_min'); trackEdit($('in_stream_stop'),'stream_stop_min'); trackEdit($('sel_deep_sleep_sched'),'deep_sleep_sched');"
        "const H=(hid,rid)=>{const h=$(hid), r=$(rid); if(h&&r){ h.onclick=()=>{ r.style.display = (r.style.display==='none')?'':'none'; }; }};"
"H('h_rate','row_rate_hint'); H('h_gain','row_gain_hint'); H('h_hpf','row_hpf_hint'); H('h_hpf_cut','row_hpf_cut_hint'); H('h_buf','row_buf_hint'); H('h_auto','row_auto_hint'); H('h_thr','row_thr_hint'); H('h_thr_mode','row_thrmode_hint'); H('h_chk','row_chk_hint'); H('h_sched','row_sched_hint'); H('h_hours','row_hours_hint'); H('h_tx','row_tx_hint'); H('h_shift','row_shift_hint'); H('h_cpu','row_cpu_hint'); H('h_level','row_level_hint'); H('h_therm_protect','row_therm_hint_protect'); H('h_therm_limit','row_therm_hint_limit'); H('h_offset','row_offset_hint'); H('h_time_sync','row_time_sync_hint'); H('h_stream_sched_en','row_stream_sched_en_hint'); H('h_stream_start','row_stream_start_hint'); H('h_stream_stop','row_stream_stop_hint'); H('h_deep_sleep_sched','row_deep_sleep_sched_hint');"
        "loadAll();"
        "</script></body></html>");
    return h;
}

// HTTP handlery
static void httpIndex() {
    // Avoid stale UI after firmware updates (browser caches).
    web.sendHeader("Cache-Control", "no-store");
    web.send(200, "text/html; charset=utf-8", htmlIndex());
}

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
    rtspServerEnabled=false; if (rtspClient && rtspClient.connected()) rtspClient.stop(); isStreaming=false; rtspServer.stop();
    webui_pushLog(F("UI action: server_stop"));
    apiSendJSON(F("{\"ok\":true}"));
}
static void httpActionResetI2S(){
    webui_pushLog(F("UI action: reset_i2s"));
    restartI2S(); apiSendJSON(F("{\"ok\":true}"));
}

static void httpActionTimeSync(){
    bool ok = attemptTimeSync(true, true);
    apiSendJSON(String("{\"ok\":") + (ok ? "true" : "false") + "}");
}

static void httpActionNetworkReset(){
    webui_pushLog(F("UI action: network_reset (clearing Wi-Fi and rebooting)"));
    WiFiManager wm;
    wm.resetSettings();
    apiSendJSON(F("{\"ok\":true}"));
    scheduleReboot(false, 800);
}

static inline bool argToFloat(float &out) { if (!web.hasArg("value")) return false; out = web.arg("value").toFloat(); return true; }
static inline bool argToUInt(uint32_t &out) { if (!web.hasArg("value")) return false; out = (uint32_t) web.arg("value").toInt(); return true; }
static inline bool argToUShort(uint16_t &out) { if (!web.hasArg("value")) return false; out = (uint16_t) web.arg("value").toInt(); return true; }
static inline bool argToUChar(uint8_t &out) { if (!web.hasArg("value")) return false; out = (uint8_t) web.arg("value").toInt(); return true; }
static inline bool argToInt(int32_t &out) { if (!web.hasArg("value")) return false; out = (int32_t) web.arg("value").toInt(); return true; }

static void httpSet() {
    if (!web.hasArg("key")) {
        apiSendJSON(F("{\"ok\":false,\"error\":\"missing_key\"}"));
        return;
    }

    String key = web.arg("key");
    String val = web.hasArg("value") ? web.arg("value") : String("");
    if (val.length()) { webui_pushLog(String("UI set: ")+key+"="+val); }

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
        if (argToInt(v) && v >= -720 && v <= 840) { timeOffsetMinutes = v; configTime(timeOffsetMinutes * 60, 0, NTP_SERVER_1, NTP_SERVER_2); saveAudioSettings(); applied = true; }
    }
    else if (key == "time_sync") {
        handled = true;
        String v = web.arg("value");
        if (v == "on" || v == "off") {
            timeSyncEnabled = (v == "on");
            if (timeSyncEnabled) {
                configTime(timeOffsetMinutes * 60, 0, NTP_SERVER_1, NTP_SERVER_2);
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

void webui_begin() {
    web.on("/", httpIndex);
    web.on("/api/status", httpStatus);
    web.on("/api/audio_status", httpAudioStatus);
    web.on("/api/perf_status", httpPerfStatus);
    web.on("/api/thermal", httpThermal);
    web.on("/api/thermal/clear", HTTP_POST, httpThermalClear);
    web.on("/api/logs", httpLogs);
    web.on("/api/action/server_start", httpActionServerStart);
    web.on("/api/action/server_stop", httpActionServerStop);
    web.on("/api/action/reset_i2s", httpActionResetI2S);
    web.on("/api/action/time_sync", httpActionTimeSync);
    web.on("/api/action/network_reset", httpActionNetworkReset);
    web.on("/api/action/reboot", [](){ webui_pushLog(F("UI action: reboot")); apiSendJSON(F("{\"ok\":true}")); scheduleReboot(false, 600); });
    web.on("/api/action/factory_reset", [](){ webui_pushLog(F("UI action: factory_reset")); apiSendJSON(F("{\"ok\":true}")); scheduleReboot(true, 600); });
    web.on("/api/set", httpSet);
    web.begin();
}

void webui_handleClient() {
    web.handleClient();
}
