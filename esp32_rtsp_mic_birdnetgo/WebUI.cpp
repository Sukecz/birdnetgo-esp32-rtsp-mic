#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
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

// Web server and in-memory log ring buffer
static WebServer web(80);
static const size_t LOG_CAP = 80;
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
    h += F(
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32 RTSP Mic for BirdNET-Go</title>"
        "<style>:root{--bg:#0b1020;--fg:#e7ebf2;--muted:#9aa3b2;--card:#121a2e;--border:#1b2745;--acc:#4ea1f3;--acc2:#36d399;--warn:#f59e0b;--bad:#ef4444}"
        "body{font-family:system-ui,Segoe UI,Roboto,Arial,sans-serif;margin:0;background:linear-gradient(180deg,#0b1020 0%,#0f1530 100%);color:var(--fg)}"
        ".page{max-width:1000px;margin:0 auto;padding:16px}"
        ".hero{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px}"
        ".title{font-weight:700;font-size:18px;letter-spacing:.2px} .subtitle{color:var(--muted);font-size:13px}"
        ".card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:12px;margin-bottom:12px;box-shadow:0 1px 1px rgba(0,0,0,.2)}"
        ".row{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:12px} h1{font-size:20px;margin:0 0 4px} h2{font-size:15px;margin:4px 0 10px;color:var(--muted);font-weight:600;letter-spacing:.2px}"
        "table{width:100%;border-collapse:collapse} td{padding:8px 6px;border-bottom:1px solid var(--border)} td.k{color:var(--muted);width:44%} td.v{font-weight:600}"
        "button,select,input{font:inherit;padding:8px 10px;border-radius:10px;border:1px solid var(--border);background:#0d1427;color:var(--fg)}"
        "button{background:#0e152a} button:hover{border-color:var(--acc)} button.active{background:var(--acc);color:#061120;border-color:#2a7dd4}"
        ".actions{display:flex;flex-wrap:wrap;gap:8px;margin-top:8px} .ok{color:var(--acc2)} .warn{color:var(--warn)} .bad{color:var(--bad)} .lang{float:right} .mono{font-family:ui-monospace,Consolas,Menlo,monospace}"
        "input[type=number]{width:130px} select{min-width:110px} .muted{color:var(--muted)}"
        ".field{display:flex;align-items:center;gap:8px} .unit{color:var(--muted);font-size:12px} .help{display:inline-flex;align-items:center;justify-content:center;width:16px;height:16px;border:1px solid var(--acc);border-radius:50%;font-size:12px;color:var(--fg);margin-left:6px;background:#0a1224;cursor:pointer} .help:hover{filter:brightness(1.1)} .hint{margin-top:6px;padding:8px;border:1px solid var(--border);border-radius:8px;background:#0d162c;color:var(--fg);font-size:12px;line-height:1.35}"
        "pre{white-space:pre-wrap;word-break:break-word;background:#0c1325;border:1px solid var(--border);border-radius:10px;padding:10px;overflow:auto} pre#logs{height:45vh}"
        ".overlay{position:fixed;inset:0;display:none;align-items:center;justify-content:center;background:rgba(0,0,0,.6);z-index:9999} .overlay .box{background:var(--card);border:1px solid var(--border);padding:16px 20px;border-radius:12px;color:var(--fg);text-align:center;min-width:260px}"
        "</style></head><body>"
        "<div id='ovr' class='overlay'><div class='box' id='ovr_msg'>Restarting…</div></div>"
        "<div class='page'>"
        "<div class='card'><div class='hero'><div><div class='title' id='t_title'>ESP32 RTSP Mic for BirdNET-Go</div><div class='subtitle'>RTSP: rtsp://");
    h += ip;
    h += F(
        ":8554/audio</div></div>"
        "<div class='lang'>Lang: <select id='langSel'><option value='en'>English</option><option value='cs'>Čeština</option></select></div></div></div>"
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
        "<button onclick=\"defaultsNow()\" id='b_defaults'>Defaults</button>"
        "<div id='adv' class='footer muted'></div></div>"

        "<div class='card'><h2 id='t_audio'>Audio</h2><table>"
        "<tr><td class='k'><span id='t_rate'>Sample Rate</span><span class='help' id='h_rate'>?</span><div class='hint' id='rate_hint' style='display:none'></div></td><td class='v'><div class='field'><input id='in_rate' type='number' step='1000' min='8000' max='96000'><span class='unit'>Hz</span><button id='btn_rate_set' onclick=\"setv('rate',in_rate.value)\">Set</button></div></td></tr>"
        "<tr id='row_rate_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_rate_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_gain'>Gain</span><span class='help' id='h_gain'>?</span></td><td class='v'><div class='field'><input id='in_gain' type='number' step='0.1' min='0.1' max='100'><span class='unit'>×</span><button id='btn_gain_set' onclick=\"setv('gain',in_gain.value)\">Set</button></div></td></tr>"
        "<tr id='row_gain_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_gain_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_buf'>Buffer Size</span><span class='help' id='h_buf'>?</span></td><td class='v'><div class='field'>"
        "<select id='sel_buf'><option>256</option><option>512</option><option selected>1024</option><option>2048</option><option>4096</option><option>8192</option></select>"
        "<span class='unit'>samples</span><button id='btn_buf_set' onclick=\"setv('buffer',sel_buf.value)\">Set</button></div></td></tr>"
        "<tr id='row_buf_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_buf_hint'></div></td></tr>"
        "<tr><td class='k' id='t_latency'>Latency</td><td class='v' id='lat'></td></tr>"
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

        "<div class='card'><h2 id='t_thermal'>Thermal</h2><div id='therm'></div></div>"

        "<div id='advsec'>"
        "<div class='card'><h2 id='t_advanced_settings'>Advanced Settings</h2><table>"
        "<tr><td class='k'><span id='t_shift'>I2S Shift</span><span class='help' id='h_shift'>?</span></td><td class='v'><div class='field'><input id='in_shift' type='number' step='1' min='0' max='24'><span class='unit'>bits</span><button id='btn_shift_set' onclick=\"setv('shift',in_shift.value)\">Set</button></div></td></tr>"
        "<tr id='row_shift_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_shift_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_chk'>Check Interval</span><span class='help' id='h_chk'>?</span></td><td class='v'><div class='field'><input id='in_chk' type='number' step='1' min='1' max='60'><span class='unit'>min</span><button id='btn_chk_set' onclick=\"setv('check_interval',in_chk.value)\">Set</button></div></td></tr>"
        "<tr id='row_chk_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_chk_hint'></div></td></tr>"
        "<tr id='row_tx_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_tx_hint'></div></td></tr>"
        "<tr><td class='k'><span id='t_wifi_tx2'>TX Power</span><span class='help' id='h_tx'>?</span></td><td class='v'><div class='field'>"
        "<select id='sel_tx'><option>-1.0</option><option>2.0</option><option>5.0</option><option>7.0</option><option>8.5</option><option>11.0</option><option>13.0</option><option selected>15.0</option><option>17.0</option><option>18.5</option><option>19.0</option><option>19.5</option></select>"
        "<span class='unit'>dBm</span><button id='btn_tx_set' onclick=\"setv('wifi_tx',sel_tx.value)\">Set</button></div></td></tr>"
        "<tr><td class='k'><span id='t_cpu'>CPU Frequency</span><span class='help' id='h_cpu'>?</span></td><td class='v'><div class='field'>"
        "<select id='sel_cpu'><option>80</option><option selected>120</option><option>160</option></select><span class='unit'>MHz</span><button id='btn_cpu_set' onclick=\"setv('cpu_freq',sel_cpu.value)\">Set</button></div></td></tr>"
        "<tr id='row_cpu_hint' style='display:none'><td colspan='2'><div class='hint' id='txt_cpu_hint'></div></td></tr>"
        "</table></div>"
        "</div>"

        "<div class='card'><h2 id='t_logs'>Logs</h2><pre id='logs' class='mono'></pre></div>"

        "</div>"
        "</div>"
        "<script>"
        "const T={en:{title:'ESP32 RTSP Mic for BirdNET-Go',status:'Status',ip:'IP Address',wifi_rssi:'WiFi RSSI',wifi_tx:'WiFi TX Power',heap:'Free Heap (min)',uptime:'Uptime',rtsp_server:'RTSP Server',client:'Client',streaming:'Streaming',pkt_rate:'Packet Rate',last_connect:'Last RTSP Connect',last_play:'Last Stream Start',audio:'Audio',rate:'Sample Rate',gain:'Gain',buf:'Buffer Size',latency:'Latency',profile:'Profile',perf:'Reliability',auto:'Auto Recovery',wifi:'WiFi',wifi_tx2:'TX Power (dBm)',thermal:'Thermal',logs:'Logs',bsrvon:'Server ON',bsrvoff:'Server OFF',breset:'Reset I2S',breboot:'Reboot',bdefaults:'Defaults',confirm_reboot:'Restart device now?',confirm_reset:'Reset to defaults and reboot?',restarting:'Restarting device…',resetting:'Restoring defaults and rebooting…',advanced_settings:'Advanced Settings',shift:'I2S Shift',thr:'Restart Threshold',chk:'Check Interval',thr_mode:'Threshold Mode',auto_m:'Auto',manual_m:'Manual',sched:'Scheduled Reset',hours:'Reset After',cpu:'CPU Frequency',set:'Set',profile_ultra:'Ultra-Low Latency (Higher CPU, May have dropouts)',profile_balanced:'Balanced (Moderate CPU, Good stability)',profile_stable:'Stable Streaming (Lower CPU, Excellent stability)',profile_high:'High Stability (Lowest CPU, Maximum stability)',help_rate:'Higher sample-rate = more detail, more bandwidth.',help_gain:'Amplifies audio after I²S shift; too high clips.',help_buf:'More samples per packet = higher latency, more stability.',help_auto:'Auto-restarts the pipeline when packet-rate collapses.',help_tx:'Wi‑Fi TX power; lowering can reduce RF noise.',help_shift:'Digital right shift applied before scaling.',help_thr:'Minimum packet-rate before auto-recovery triggers.',help_chk:'How often performance is checked.',help_sched:'Periodic device restart for stability.',help_hours:'Interval between scheduled restarts.',help_cpu:'Lower MHz = cooler, higher latency possible.'},cs:{title:'ESP32 RTSP Mic pro BirdNET-Go',status:'Stav',ip:'IP adresa',wifi_rssi:'WiFi RSSI',wifi_tx:'WiFi výkon',heap:'Volná RAM (min)',uptime:'Doba běhu',rtsp_server:'RTSP server',client:'Klient',streaming:'Streamování',pkt_rate:'Rychlost paketů',last_connect:'Poslední RTSP připojení',last_play:'Poslední start streamu',audio:'Audio',rate:'Vzorkovací frekvence',gain:'Zisk',buf:'Velikost bufferu',latency:'Latence',profile:'Profil',perf:'Spolehlivost',auto:'Automatická obnova',wifi:'WiFi',wifi_tx2:'TX výkon (dBm)',thermal:'Teplota',logs:'Logy',bsrvon:'Server ZAP',bsrvoff:'Server VYP',breset:'Reset I2S',breboot:'Restart',bdefaults:'Výchozí',confirm_reboot:'Restartovat zařízení nyní?',confirm_reset:'Obnovit výchozí nastavení a restartovat?',restarting:'Zařízení se restartuje…',resetting:'Obnovuji výchozí nastavení a restartuji…',advanced_settings:'Pokročilá nastavení',shift:'I2S posun',thr:'Prahová hodnota restartu',chk:'Interval kontroly',thr_mode:'Režim prahu',auto_m:'Automaticky',manual_m:'Manuálně',sched:'Plánovaný restart',hours:'Po kolika hodinách',cpu:'Frekvence CPU',set:'Nastavit',profile_ultra:'Ultra nízká latence (vyšší zátěž CPU, možné výpadky)',profile_balanced:'Vyvážené (střední zátěž CPU, dobrá stabilita)',profile_stable:'Stabilní stream (nižší zátěž CPU, výborná stabilita)',profile_high:'Vysoká stabilita (nejnižší zátěž CPU, max. stabilita)',help_rate:'Vyšší frekvence = více detailů, větší datový tok.',help_gain:'Zesílení po I²S posunu; příliš vysoké klipuje.',help_buf:'Více vzorků v paketu = vyšší latence, větší stabilita.',help_auto:'Při poklesu rychlosti paketů dojde k obnově.',help_tx:'Výkon vysílače Wi‑Fi; snížení může zlepšit šum.',help_shift:'Digitální bitový posun před škálováním.',help_thr:'Minimální rychlost paketů pro spuštění obnovy.',help_chk:'Jak často se provádí kontrola výkonu.',help_sched:'Pravidelný restart zařízení kvůli stabilitě.',help_hours:'Interval mezi plánovanými restarty.',help_cpu:'Nižší MHz = chladnější, může přidat latenci.'}};"
        "const HELP_EXT_EN={help_rate:'How many audio samples per second are captured. Higher rates increase detail and bandwidth and CPU usage. 48 kHz is a safe default; 44.1 kHz is also fine. Very high rates may stress Wi‑Fi and processing.',help_gain:'Software amplification after the I2S shift. Use to boost loudness. Too high causes clipping (distortion). Start around 0.8–1.5 and adjust while watching the stream.',help_buf:'Samples per network packet. Bigger buffer increases latency but improves stability on weak Wi‑Fi; smaller buffer lowers latency but may drop packets. 1024 is a good balance.',help_auto:'When enabled, the device restarts the audio pipeline if packet rate drops below the threshold. Helps recover from glitches without manual intervention.',help_tx:'Wi‑Fi transmit power in dBm. Lower values can reduce RF self-noise near the microphone and power draw, but reduce range. Only specific steps are supported by the radio. Change carefully if your signal is weak.',help_shift:'Right bit-shift applied to 32‑bit I2S samples before converting to 16‑bit. Higher shift lowers volume and avoids clipping; lower shift raises volume but may clip.',help_thr:'Minimum packet rate (packets per second) considered healthy while streaming. If measured rate stays below this at a check, auto recovery restarts I2S. In Auto mode this comes from sample rate and buffer size (about 70% of expected).',help_chk:'How often performance is checked (minutes). Shorter intervals react faster with small CPU cost; longer intervals reduce checks.',help_sched:'Optional periodic device reboot for long-term stability on problematic networks. Leave OFF unless you need it.',help_hours:'Number of hours between scheduled reboots. Applies only when Scheduled Reset is ON.',help_cpu:'Processor clock. Lower MHz reduces heat and power; higher MHz can help under heavy load. 120 MHz is a balanced default.',help_thr_mode:'Auto: Threshold is computed from Sample Rate and Buffer; recommended for most users. Manual: You set the exact minimum packet rate; use if you know your network and latency constraints.'};"
        "const HELP_EXT_CS={help_rate:'Kolik vzorků za sekundu se pořizuje. Vyšší frekvence zvyšuje detail i nároky na šířku pásma a CPU. 48 kHz je bezpečné výchozí nastavení; 44,1 kHz je také v pořádku. Velmi vysoké frekvence mohou zatěžovat Wi‑Fi a zpracování.',help_gain:'Softwarové zesílení po I2S posunu. Slouží ke zvýšení hlasitosti. Příliš vysoká hodnota způsobí ořez (zkreslení). Začněte okolo 0,8–1,5 a dolaďte podle poslechu streamu.',help_buf:'Počet vzorků v jednom síťovém paketu. Větší buffer zvyšuje latenci a zlepšuje stabilitu na slabším Wi‑Fi; menší buffer snižuje latenci, ale může zvyšovat ztráty paketů. 1024 je dobrý kompromis.',help_auto:'Při poklesu rychlosti odchozích paketů pod práh zařízení automaticky restartuje audio pipeline. Pomáhá zotavit se z výpadků bez zásahu.',help_tx:'Vysílací výkon Wi‑Fi v dBm. Snížení může omezit vlastní RF šum u mikrofonu a spotřebu, ale zmenší dosah. Čip podporuje jen určité kroky. Pokud máte slabý signál, měňte opatrně.',help_shift:'Pravý bitový posun na 32bitových I2S vzorcích před převodem na 16bit audio. Vyšší posun snižuje hlasitost a brání klipování; nižší posun zvyšuje hlasitost, ale může klipovat.',help_thr:'Minimální rychlost paketů (paketů za sekundu), považovaná při streamování za zdravou. Pokud při kontrole klesne pod tuto hodnotu, automatická obnova restartuje I2S. V režimu Auto se práh odvozuje z frekvence a bufferu (asi 70 % očekávané hodnoty).',help_chk:'Jak často se kontroluje výkon (minuty). Kratší interval reaguje rychleji s malou zátěží CPU; delší interval snižuje počet kontrol.',help_sched:'Volitelný pravidelný restart zařízení pro dlouhodobou stabilitu na problematických sítích. Nechte VYP, pokud není nutné.',help_hours:'Počet hodin mezi plánovanými restarty. Platí pouze pokud je Plánovaný restart ZAP.',help_cpu:'Frekvence procesoru. Nižší MHz snižuje zahřívání a spotřebu; vyšší MHz pomůže při zátěži. 120 MHz je vyvážené výchozí nastavení.',help_thr_mode:'Auto: Práh restartu se počítá z Vzorkovací frekvence a Bufferu; doporučeno pro většinu uživatelů. Manuálně: Nastavíte přesný minimální počet paketů za sekundu; použijte, pokud znáte svou síť a požadavky na latenci.'};"
        "Object.assign(T.en, HELP_EXT_EN); Object.assign(T.cs, HELP_EXT_CS);"
        "let lang=localStorage.getItem('lang')||'en'; const $=id=>document.getElementById(id);"
        "function applyLang(){const L=T[lang]; const st=(id,t)=>{const e=$(id); if(e) e.textContent=t}; const help=(k)=>{const b=L[k]||''; return b}; st('t_title',L.title); st('t_status',L.status); st('t_ip',L.ip); st('t_wifi_rssi',L.wifi_rssi); st('t_wifi_tx',L.wifi_tx); st('t_heap',L.heap); st('t_uptime',L.uptime); st('t_rtsp_server',L.rtsp_server); st('t_client',L.client); st('t_streaming',L.streaming); st('t_pkt_rate',L.pkt_rate); st('t_last_connect',L.last_connect); st('t_last_play',L.last_play); st('t_audio',L.audio); st('t_rate',L.rate); st('t_gain',L.gain); st('t_buf',L.buf); st('t_latency',L.latency); st('t_profile',L.profile); st('t_perf',L.perf); st('t_auto',L.auto); st('t_wifi',L.wifi); st('t_wifi_tx2',L.wifi_tx2); st('t_thermal',L.thermal); st('t_logs',L.logs); st('b_srv_on',L.bsrvon); st('b_srv_off',L.bsrvoff); st('b_reset',L.breset); st('b_reboot',L.breboot); st('b_defaults',L.bdefaults); st('t_advanced_settings',L.advanced_settings); st('t_shift',L.shift); st('t_thr',L.thr); st('t_chk',L.chk); st('t_thr_mode',L.thr_mode); st('t_sched',L.sched); st('t_hours',L.hours); st('t_cpu',L.cpu); const hm=(id,k)=>{const e=$(id); if(e) e.setAttribute('title',help(k))}; hm('h_rate','help_rate'); hm('h_gain','help_gain'); hm('h_buf','help_buf'); hm('h_auto','help_auto'); hm('h_tx','help_tx'); hm('h_thr','help_thr'); hm('h_chk','help_chk'); hm('h_shift','help_shift'); hm('h_sched','help_sched'); hm('h_hours','help_hours'); hm('h_cpu','help_cpu'); hm('h_thr_mode','help_thr_mode'); st('btn_rate_set',L.set); st('btn_gain_set',L.set); st('btn_buf_set',L.set); st('btn_auto_set',L.set); st('btn_thrmode_set',L.set); st('btn_thr_set',L.set); st('btn_sched_set',L.set); st('btn_hours_set',L.set); st('btn_shift_set',L.set); st('btn_chk_set',L.set); st('btn_tx_set',L.set); st('btn_cpu_set',L.set); const sht=(id,k)=>{const e=$(id); if(e) e.textContent=help(k)}; sht('txt_rate_hint','help_rate'); sht('txt_gain_hint','help_gain'); sht('txt_buf_hint','help_buf'); sht('txt_auto_hint','help_auto'); sht('txt_thr_hint','help_thr'); sht('txt_thr_mode_hint','help_thr_mode'); sht('txt_sched_hint','help_sched'); sht('txt_hours_hint','help_hours'); sht('txt_shift_hint','help_shift'); sht('txt_chk_hint','help_chk'); sht('txt_tx_hint','help_tx'); sht('txt_cpu_hint','help_cpu'); document.title=L.title;}"
        "function profileText(buf){const L=T[lang]; buf=parseInt(buf,10)||0; if(buf<=256) return L.profile_ultra; if(buf<=512) return L.profile_balanced; if(buf<=1024) return L.profile_stable; return L.profile_high;}"
        "function fmtBool(b){return b?'<span class=ok>YES</span>':'<span class=bad>NO</span>'}"
        "function fmtSrv(b){return b?'<span class=ok>ENABLED</span>':'<span class=bad>DISABLED</span>'}"
        "function showOverlay(msg){ $('ovr_msg').textContent=msg; $('ovr').style.display='flex'; }"
        "function rebootSequence(kind){ const L=T[lang]; const msg=(kind==='factory_reset')?L.resetting:L.restarting; showOverlay(msg); function tick(){ fetch('/api/status',{cache:'no-store'}).then(r=>{ if(r.ok){ location.reload(); } else { setTimeout(tick,2000); } }).catch(()=>setTimeout(tick,2000)); } setTimeout(tick,4000); }"
        "function act(a){fetch('/api/action/'+a,{cache:'no-store'}).then(r=>r.json()).then(loadAll)}"
        "function rebootNow(){ rebootSequence('reboot'); act('reboot'); }"
        "function defaultsNow(){ rebootSequence('factory_reset'); act('factory_reset'); }"
        "const locks={};"
        "function setv(k,v){v=String(v??'').trim().replace(',', '.'); if(v==='')return; locks[k]=Date.now()+1500; fetch('/api/set?key='+encodeURIComponent(k)+'&value='+encodeURIComponent(v),{cache:'no-store'}).then(r=>r.json()).then(loadAll)}"
        "function bindSaver(el,key){if(!el)return; el.addEventListener('keydown',e=>{if(e.key==='Enter'){setv(key,el.value)}}); el.addEventListener('blur',()=>{setv(key,el.value)})}"
        "function setToggleState(on){const onb=$('b_srv_on'), offb=$('b_srv_off'); if(onb&&offb){onb.classList.toggle('active',on); offb.classList.toggle('active',!on); onb.disabled=on; offb.disabled=!on;}}"
        "function loadStatus(){fetch('/api/status',{cache:'no-store'}).then(r=>r.json()).then(j=>{ $('ip').textContent=j.ip; $('rssi').textContent=j.wifi_rssi+' dBm'; $('wtx').textContent=j.wifi_tx_dbm.toFixed(1)+' dBm'; $('heap').textContent=j.free_heap_kb+' KB ('+j.min_free_heap_kb+' KB)'; $('uptime').textContent=j.uptime; $('srv').innerHTML=fmtSrv(j.rtsp_server_enabled); setToggleState(j.rtsp_server_enabled); $('client').textContent=j.client || 'Waiting...'; $('stream').innerHTML=fmtBool(j.streaming); $('rate').textContent=j.current_rate_pkt_s+' pkt/s'; $('lcon').textContent=j.last_rtsp_connect; $('lplay').textContent=j.last_stream_start; const stx=$('sel_tx'); const now=Date.now(); if(stx && !(locks['wifi_tx']&&now<locks['wifi_tx'])){ stx.value=j.wifi_tx_dbm.toFixed(1);} })}"
        "function loadAudio(){fetch('/api/audio_status',{cache:'no-store'}).then(r=>r.json()).then(j=>{ const r=$('in_rate'); const g=$('in_gain'); const sb=$('sel_buf'); const s=$('in_shift'); const now=Date.now(); if(r && !(locks['rate']&&now<locks['rate'])) r.value=j.sample_rate; if(g && !(locks['gain']&&now<locks['gain'])) g.value=j.gain.toFixed(2); if(sb && !(locks['buffer']&&now<locks['buffer'])) sb.value=j.buffer_size; if(s && !(locks['shift']&&now<locks['shift'])) s.value=j.i2s_shift; $('lat').textContent=j.latency_ms.toFixed(1)+' ms'; $('profile').textContent=profileText(j.buffer_size); updateAdvice(j); })}"
        "function updateAdvice(a){const L=T[lang]; let tips=[]; if(a.buffer_size<512) tips.push(L.adv_buf512); if(a.buffer_size<1024) tips.push(L.adv_buf1024); if(a.gain>20) tips.push(L.adv_gain); $('adv').textContent=tips.join(' ');}"
        "function loadPerf(){fetch('/api/perf_status',{cache:'no-store'}).then(r=>r.json()).then(j=>{ const el=$('in_auto'); if(el) el.value=j.auto_recovery?'on':'off'; const thr=$('in_thr'); const chk=$('in_chk'); const mode=$('in_thr_mode'); const sch=$('in_sched'); const hrs=$('in_hours'); const now=Date.now(); if(mode) mode.value=j.auto_threshold?'auto':'manual'; if(thr && thr!==document.activeElement && !(locks['min_rate']&&now<locks['min_rate'])) thr.value=j.restart_threshold_pkt_s; if(chk && chk!==document.activeElement && !(locks['check_interval']&&now<locks['check_interval'])) chk.value=j.check_interval_min; if(sch && !(locks['sched_reset']&&now<locks['sched_reset'])) sch.value=j.scheduled_reset?'on':'off'; if(hrs && hrs!==document.activeElement && !(locks['reset_hours']&&now<locks['reset_hours'])) hrs.value=j.reset_hours; $('row_min_rate').style.display=j.auto_threshold?'none':''; })}"
        "function loadTherm(){fetch('/api/thermal',{cache:'no-store'}).then(r=>r.json()).then(j=>{ $('therm').innerHTML='Current: '+j.current_c.toFixed(1)+'°C, Max: '+j.max_c.toFixed(1)+'°C, CPU: '+j.cpu_mhz+' MHz'; const sc=$('sel_cpu'); const now=Date.now(); if(sc && !(locks['cpu_freq']&&now<locks['cpu_freq'])){ sc.value=j.cpu_mhz; } })}"
        "function loadLogs(){fetch('/api/logs',{cache:'no-store'}).then(r=>r.text()).then(t=>{ const lg=$('logs'); lg.textContent=t; lg.scrollTop=lg.scrollHeight; })}"
        "function loadAll(){loadStatus();loadAudio();loadPerf();loadTherm();loadLogs()}"
        "setInterval(loadAll,3000);"
        "const sel=document.getElementById('langSel'); sel.value=lang; sel.onchange=()=>{lang=sel.value;localStorage.setItem('lang',lang);applyLang()}; applyLang();"
        "bindSaver($('in_rate'),'rate'); bindSaver($('in_gain'),'gain'); bindSaver($('in_shift'),'shift'); bindSaver($('in_thr'),'min_rate'); bindSaver($('in_chk'),'check_interval'); bindSaver($('in_hours'),'reset_hours'); const ia=$('in_auto'); if(ia) ia.onchange=()=>setv('auto_recovery',$('in_auto').value); const im=$('in_thr_mode'); if(im) im.onchange=()=>setv('thr_mode',$('in_thr_mode').value); const isr=$('in_sched'); if(isr) isr.onchange=()=>setv('sched_reset',$('in_sched').value); const sb=$('sel_buf'); if(sb) sb.onchange=()=>setv('buffer',sb.value); const st=$('sel_tx'); if(st) st.onchange=()=>setv('wifi_tx',st.value); const sc=$('sel_cpu'); if(sc) sc.onchange=()=>setv('cpu_freq',sc.value);"
        "const H=(hid,rid)=>{const h=$(hid), r=$(rid); if(h&&r){ h.onclick=()=>{ r.style.display = (r.style.display==='none'||!r.style.display)?'block':'none'; }; }};"
        "H('h_rate','row_rate_hint'); H('h_gain','row_gain_hint'); H('h_buf','row_buf_hint'); H('h_auto','row_auto_hint'); H('h_thr','row_thr_hint'); H('h_thr_mode','row_thrmode_hint'); H('h_chk','row_chk_hint'); H('h_sched','row_sched_hint'); H('h_hours','row_hours_hint'); H('h_tx','row_tx_hint'); H('h_shift','row_shift_hint'); H('h_cpu','row_cpu_hint');"
        "loadAll();"
        "</script></body></html>");
    return h;
}

// HTTP handlery
static void httpIndex() { web.send(200, "text/html; charset=utf-8", htmlIndex()); }

static void httpStatus() {
    unsigned long uptimeSeconds = (millis() - bootTime) / 1000;
    String uptimeStr = formatUptime(uptimeSeconds);
    unsigned long runtime = millis() - lastStatsReset;
    uint32_t currentRate = (isStreaming && runtime > 1000) ? (audioPacketsSent * 1000) / runtime : 0;
    String json = "{";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"wifi_tx_dbm\":" + String(wifiPowerLevelToDbm(currentWifiPowerLevel),1) + ",";
    json += "\"free_heap_kb\":" + String(ESP.getFreeHeap()/1024) + ",";
    json += "\"min_free_heap_kb\":" + String(minFreeHeap/1024) + ",";
    json += "\"uptime\":\"" + uptimeStr + "\",";
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
    json += "\"profile\":\"" + jsonEscape(profileName(currentBufferSize)) + "\"";
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
    float t = temperatureRead();
    String json = "{";
    json += "\"current_c\":" + String(t,1) + ",";
    json += "\"max_c\":" + String(maxTemperature,1) + ",";
    json += "\"cpu_mhz\":" + String(getCpuFrequencyMhz()) + "}";
    apiSendJSON(json);
}

static void httpLogs() {
    String out;
    for (size_t i=0;i<logCount;i++){
        size_t idx = (logHead + LOG_CAP - logCount + i) % LOG_CAP;
        out += logBuffer[idx]; out += '\n';
    }
    web.send(200, "text/plain; charset=utf-8", out);
}

static void httpActionServerStart(){
    if (!rtspServerEnabled) {
        rtspServerEnabled=true; rtspServer.begin(); rtspServer.setNoDelay(true);
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

static inline bool argToFloat(const String &name, float &out) { if (!web.hasArg("value")) return false; out = web.arg("value").toFloat(); return true; }
static inline bool argToUInt(const String &name, uint32_t &out) { if (!web.hasArg("value")) return false; out = (uint32_t) web.arg("value").toInt(); return true; }
static inline bool argToUShort(const String &name, uint16_t &out) { if (!web.hasArg("value")) return false; out = (uint16_t) web.arg("value").toInt(); return true; }
static inline bool argToUChar(const String &name, uint8_t &out) { if (!web.hasArg("value")) return false; out = (uint8_t) web.arg("value").toInt(); return true; }

static void httpSet() {
    String key = web.arg("key");
    String val = web.hasArg("value") ? web.arg("value") : String("");
    if (val.length()) { webui_pushLog(String("UI set: ")+key+"="+val); }
    if (key == "gain") { float v; if (argToFloat("value", v) && v>=0.1f && v<=100.0f) { currentGainFactor=v; saveAudioSettings(); restartI2S(); } }
    else if (key == "rate") { uint32_t v; if (argToUInt("value", v) && v>=8000 && v<=96000) { currentSampleRate=v; if (autoThresholdEnabled) { minAcceptableRate = computeRecommendedMinRate(); } saveAudioSettings(); restartI2S(); } }
    else if (key == "buffer") { uint16_t v; if (argToUShort("value", v) && v>=256 && v<=8192) { currentBufferSize=v; if (autoThresholdEnabled) { minAcceptableRate = computeRecommendedMinRate(); } saveAudioSettings(); restartI2S(); } }
    else if (key == "shift") { uint8_t v; if (argToUChar("value", v) && v<=24) { i2sShiftBits=v; saveAudioSettings(); restartI2S(); } }
    else if (key == "wifi_tx") { float v; if (argToFloat("value", v) && v>=-1.0f && v<=19.5f) { extern float wifiTxPowerDbm; wifiTxPowerDbm = snapWifiTxDbm(v); applyWifiTxPower(true); saveAudioSettings(); } }
    else if (key == "auto_recovery") { String v=web.arg("value"); if (v=="on"||v=="off") { autoRecoveryEnabled=(v=="on"); saveAudioSettings(); } }
    else if (key == "thr_mode") { String v=web.arg("value"); if (v=="auto") { autoThresholdEnabled=true; minAcceptableRate = computeRecommendedMinRate(); saveAudioSettings(); } else if (v=="manual") { autoThresholdEnabled=false; saveAudioSettings(); } }
    else if (key == "min_rate") { uint32_t v; if (argToUInt("value", v) && v>=5 && v<=200) { minAcceptableRate=v; saveAudioSettings(); } }
    else if (key == "check_interval") { uint32_t v; if (argToUInt("value", v) && v>=1 && v<=60) { performanceCheckInterval=v; saveAudioSettings(); } }
    else if (key == "sched_reset") { String v=web.arg("value"); if (v=="on"||v=="off") { extern bool scheduledResetEnabled; scheduledResetEnabled=(v=="on"); saveAudioSettings(); } }
    else if (key == "reset_hours") { uint32_t v; if (argToUInt("value", v) && v>=1 && v<=168) { extern uint32_t resetIntervalHours; resetIntervalHours=v; saveAudioSettings(); } }
    else if (key == "cpu_freq") { uint32_t v; if (argToUInt("value", v) && v>=40 && v<=160) { cpuFrequencyMhz=(uint8_t)v; setCpuFrequencyMhz(cpuFrequencyMhz); saveAudioSettings(); } }
    apiSendJSON(F("{\"ok\":true}"));
}

void webui_begin() {
    web.on("/", httpIndex);
    web.on("/api/status", httpStatus);
    web.on("/api/audio_status", httpAudioStatus);
    web.on("/api/perf_status", httpPerfStatus);
    web.on("/api/thermal", httpThermal);
    web.on("/api/logs", httpLogs);
    web.on("/api/action/server_start", httpActionServerStart);
    web.on("/api/action/server_stop", httpActionServerStop);
    web.on("/api/action/reset_i2s", httpActionResetI2S);
    web.on("/api/action/reboot", [](){ webui_pushLog(F("UI action: reboot")); apiSendJSON(F("{\"ok\":true}")); scheduleReboot(false, 600); });
    web.on("/api/action/factory_reset", [](){ webui_pushLog(F("UI action: factory_reset")); apiSendJSON(F("{\"ok\":true}")); scheduleReboot(true, 600); });
    web.on("/api/set", httpSet);
    web.begin();
}

void webui_handleClient() {
    web.handleClient();
}
