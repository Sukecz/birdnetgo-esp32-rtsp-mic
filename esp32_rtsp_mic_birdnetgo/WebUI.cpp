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
    String h = F(
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32 RTSP Mic for BirdNET-Go</title>"
        "<style>body{font-family:system-ui,Segoe UI,Roboto,Arial,sans-serif;margin:16px;background:#fafafa;color:#0a0a0a}"
        ".card{background:#fff;border:1px solid #e6e6e6;border-radius:8px;padding:12px;margin-bottom:12px;box-shadow:0 1px 1px rgba(0,0,0,.03)}"
        ".row{display:flex;gap:8px;flex-wrap:wrap} .row>*{flex:1 1 280px} h1{font-size:20px;margin:0 0 8px} h2{font-size:16px;margin:8px 0}"
        "table{width:100%;border-collapse:collapse} td{padding:4px 6px;border-bottom:1px solid #f0f0f0} td.k{color:#555;width:42%} td.v{font-weight:600}"
        "button,select,input{font:inherit;padding:6px 10px;border-radius:6px;border:1px solid #d0d0d0;background:#fff}"
        ".actions button{margin-right:6px} .ok{color:#0a7c2f} .warn{color:#b4690e} .bad{color:#c62828} .lang{float:right} .mono{font-family:ui-monospace,Consolas,Menlo,monospace}"
        "pre{white-space:pre-wrap;word-break:break-word;background:#fcfcfc;border:1px solid #eee;border-radius:6px;padding:8px;max-height:240px;overflow:auto}"
        "</style></head><body>"
        "<div class='card'><h1 id='t_title'>ESP32 RTSP Mic for BirdNET-Go</h1>"
        "<div class='lang'>Lang: <select id='langSel'><option value='en'>English</option><option value='cs'>Čeština</option></select></div>"
        "RTSP: rtsp://"); h += ip; h += F(":8554/audio</div>");
    h += F(
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
        "<button onclick=act('start') id='b_start'>Start</button>"
        "<button onclick=act('stop') id='b_stop'>Stop</button>"
        "<button onclick=act('disconnect') id='b_disconnect'>Disconnect</button>"
        "<button onclick=act('server_start') id='b_srv_on'>Server ON</button>"
        "<button onclick=act('server_stop') id='b_srv_off'>Server OFF</button>"
        "<button onclick=act('reset_i2s') id='b_reset'>Reset I2S</button>"
        "<div id='adv' class='footer'></div></div>"

        "<div class='card'><h2 id='t_audio'>Audio</h2><table>"
        "<tr><td class='k' id='t_rate'>Sample Rate</td><td class='v' id='rate_hz'></td></tr>"
        "<tr><td class='k' id='t_gain'>Gain Factor</td><td class='v' id='gain'></td></tr>"
        "<tr><td class='k' id='t_buf'>Buffer Size</td><td class='v' id='buf'></td></tr>"
        "<tr><td class='k' id='t_shift'>I2S Shift</td><td class='v' id='shift'></td></tr>"
        "<tr><td class='k' id='t_latency'>Latency</td><td class='v' id='lat'></td></tr>"
        "<tr><td class='k' id='t_profile'>Profile</td><td class='v' id='profile'></td></tr>"
        "</table><div class='actions'>"
        "<input id='in_gain' type='number' step='0.1' min='0.1' max='100' placeholder='Gain'>"
        "<button onclick=setv('gain',in_gain.value) id='b_set_gain'>Set Gain</button>"
        "<input id='in_rate' type='number' step='1000' min='8000' max='96000' placeholder='Rate'>"
        "<button onclick=setv('rate',in_rate.value) id='b_set_rate'>Set Rate</button>"
        "<input id='in_buf' type='number' step='32' min='256' max='8192' placeholder='Buffer'>"
        "<button onclick=setv('buffer',in_buf.value) id='b_set_buf'>Set Buffer</button>"
        "<input id='in_shift' type='number' step='1' min='0' max='24' placeholder='Shift'>"
        "<button onclick=setv('shift',in_shift.value) id='b_set_shift'>Set Shift</button>"
        "</div></div>"

        "<div class='card'><h2 id='t_perf'>Performance</h2><table>"
        "<tr><td class='k' id='t_thr'>Restart Threshold</td><td class='v' id='thr'></td></tr>"
        "<tr><td class='k' id='t_chk'>Check Interval</td><td class='v' id='chk'></td></tr>"
        "<tr><td class='k' id='t_auto'>Auto Recovery</td><td class='v' id='auto'></td></tr>"
        "</table><div class='actions'>"
        "<input id='in_thr' type='number' step='1' min='5' max='200' placeholder='Min pkt/s'>"
        "<button onclick=setv('min_rate',in_thr.value) id='b_set_thr'>Set Threshold</button>"
        "<input id='in_chk' type='number' step='1' min='1' max='60' placeholder='Minutes'>"
        "<button onclick=setv('check_interval',in_chk.value) id='b_set_chk'>Set Interval</button>"
        "<select id='in_auto'><option value='on'>ON</option><option value='off'>OFF</option></select>"
        "<button onclick=setv('auto_recovery',in_auto.value) id='b_set_auto'>Set Auto</button>"
        "</div></div>"

        "<div class='card'><h2 id='t_wifi'>WiFi</h2><table>"
        "<tr><td class='k' id='t_wifi_tx2'>TX Power (dBm)</td><td class='v' id='wifi_tx'></td></tr>"
        "</table><div class='actions'>"
        "<input id='in_tx' type='number' step='0.5' min='-1' max='19.5' placeholder='dBm'>"
        "<button onclick=setv('wifi_tx',in_tx.value) id='b_set_tx'>Set TX</button>"
        "</div></div>"

        "<div class='card'><h2 id='t_thermal'>Thermal</h2><div id='therm'></div></div>"

        "<div class='card'><h2 id='t_logs'>Logs</h2><pre id='logs' class='mono'></pre></div>"

        "</div>"
        "<script>"
        "const T={en:{title:'ESP32 RTSP Mic for BirdNET-Go',status:'Status',ip:'IP Address',wifi_rssi:'WiFi RSSI',wifi_tx:'WiFi TX Power',heap:'Free Heap (min)',uptime:'Uptime',rtsp_server:'RTSP Server',client:'Client',streaming:'Streaming',pkt_rate:'Packet Rate',last_connect:'Last RTSP Connect',last_play:'Last Stream Start',audio:'Audio',rate:'Sample Rate',gain:'Gain Factor',buf:'Buffer Size',shift:'I2S Shift',latency:'Latency',profile:'Profile',perf:'Performance',thr:'Restart Threshold',chk:'Check Interval',auto:'Auto Recovery',wifi:'WiFi',wifi_tx2:'TX Power (dBm)',thermal:'Thermal',logs:'Logs',bstart:'Start',bstop:'Stop',bdisc:'Disconnect',bsrvon:'Server ON',bsrvoff:'Server OFF',breset:'Reset I2S',setgain:'Set Gain',setrate:'Set Rate',setbuf:'Set Buffer',setshift:'Set Shift',setthr:'Set Threshold',setchk:'Set Interval',setauto:'Set Auto',settx:'Set TX',adv_buf512:'Increase buffer to ≥512 for fewer dropouts.',adv_buf1024:'1024 buffer = stable streaming, lower CPU.',adv_gain:'High gain may clip; reduce gain or increase shift.'},cs:{title:'ESP32 RTSP Mic pro BirdNET-Go',status:'Stav',ip:'IP adresa',wifi_rssi:'WiFi RSSI',wifi_tx:'WiFi výkon',heap:'Volná RAM (min)',uptime:'Doba běhu',rtsp_server:'RTSP server',client:'Klient',streaming:'Streamování',pkt_rate:'Rychlost paketů',last_connect:'Poslední RTSP připojení',last_play:'Poslední start streamu',audio:'Audio',rate:'Vzorkovací frekvence',gain:'Zisk',buf:'Velikost bufferu',shift:'I2S posun',latence:'Latence',profile:'Profil',perf:'Výkon',thr:'Prahová hodnota restartu',chk:'Interval kontroly',auto:'Automatická obnova',wifi:'WiFi',wifi_tx2:'TX výkon (dBm)',thermal:'Teplota',logs:'Logy',bstart:'Start',bstop:'Stop',bdisc:'Odpojit',bsrvon:'Server ZAP',bsrvoff:'Server VYP',breset:'Reset I2S',setgain:'Nastavit zisk',setrate:'Nastavit frekvenci',setbuf:'Nastavit buffer',setshift:'Nastavit posun',setthr:'Nastavit práh',setchk:'Nastavit interval',setauto:'Nastavit auto',settx:'Nastavit TX',adv_buf512:'Zvyšte buffer na ≥512 pro méně výpadků.',adv_buf1024:'Buffer 1024 = stabilní stream, nižší zátěž CPU.',adv_gain:'Vysoký zisk může klipovat; snižte zisk nebo zvyšte posun.'}};"
        "let lang=localStorage.getItem('lang')||'en'; const $=id=>document.getElementById(id);"
        "function applyLang(){const L=T[lang]; $('t_title').textContent=L.title; $('t_status').textContent=L.status; $('t_ip').textContent=L.ip; $('t_wifi_rssi').textContent=L.wifi_rssi; $('t_wifi_tx').textContent=L.wifi_tx; $('t_heap').textContent=L.heap; $('t_uptime').textContent=L.uptime; $('t_rtsp_server').textContent=L.rtsp_server; $('t_client').textContent=L.client; $('t_streaming').textContent=L.streaming; $('t_pkt_rate').textContent=L.pkt_rate; $('t_last_connect').textContent=L.last_connect; $('t_last_play').textContent=L.last_play; $('t_audio').textContent=L.audio; $('t_rate').textContent=L.rate; $('t_gain').textContent=L.gain; $('t_buf').textContent=L.buf; $('t_shift').textContent=L.shift; $('t_latency').textContent=L.latency; $('t_profile').textContent=L.profile; $('t_perf').textContent=L.perf; $('t_thr').textContent=L.thr; $('t_chk').textContent=L.chk; $('t_auto').textContent=L.auto; $('t_wifi').textContent=L.wifi; $('t_wifi_tx2').textContent=L.wifi_tx2; $('t_thermal').textContent=L.thermal; $('t_logs').textContent=L.logs; $('b_start').textContent=L.bstart; $('b_stop').textContent=L.bstop; $('b_disconnect').textContent=L.bdisc; $('b_srv_on').textContent=L.bsrvon; $('b_srv_off').textContent=L.bsrvoff; $('b_reset').textContent=L.breset; $('b_set_gain').textContent=L.setgain; $('b_set_rate').textContent=L.setrate; $('b_set_buf').textContent=L.setbuf; $('b_set_shift').textContent=L.setshift; $('b_set_thr').textContent=L.setthr; $('b_set_chk').textContent=L.setchk; $('b_set_auto').textContent=L.setauto; $('b_set_tx').textContent=L.settx; document.title=L.title;}"
        "function fmtBool(b){return b?'<span class=ok>YES</span>':'<span class=bad>NO</span>'}"
        "function fmtSrv(b){return b?'<span class=ok>ENABLED</span>':'<span class=bad>DISABLED</span>'}"
        "function act(a){fetch('/api/action/'+a).then(r=>r.json()).then(loadAll)}"
        "function setv(k,v){if(v==='')return; fetch('/api/set?key='+encodeURIComponent(k)+'&value='+encodeURIComponent(v)).then(r=>r.json()).then(loadAll)}"
        "function loadStatus(){fetch('/api/status').then(r=>r.json()).then(j=>{ $('ip').textContent=j.ip; $('rssi').textContent=j.wifi_rssi+' dBm'; $('wtx').textContent=j.wifi_tx_dbm.toFixed(1)+' dBm'; $('heap').textContent=j.free_heap_kb+' KB ('+j.min_free_heap_kb+' KB)'; $('uptime').textContent=j.uptime; $('srv').innerHTML=fmtSrv(j.rtsp_server_enabled); $('client').textContent=j.client || 'Waiting...'; $('stream').innerHTML=fmtBool(j.streaming); $('rate').textContent=j.current_rate_pkt_s+' pkt/s'; $('lcon').textContent=j.last_rtsp_connect; $('lplay').textContent=j.last_stream_start; })}"
        "function loadAudio(){fetch('/api/audio_status').then(r=>r.json()).then(j=>{ $('rate_hz').textContent=j.sample_rate+' Hz'; $('gain').textContent=j.gain.toFixed(2); $('buf').textContent=j.buffer_size+' samples'; $('shift').textContent=j.i2s_shift+' bits'; $('lat').textContent=j.latency_ms.toFixed(1)+' ms'; $('profile').textContent=j.profile; updateAdvice(j); })}"
        "function updateAdvice(a){const L=T[lang]; let tips=[]; if(a.buffer_size<512) tips.push(L.adv_buf512); if(a.buffer_size<1024) tips.push(L.adv_buf1024); if(a.gain>20) tips.push(L.adv_gain); $('adv').textContent=tips.join(' ');}"
        "function loadPerf(){fetch('/api/perf_status').then(r=>r.json()).then(j=>{ $('thr').textContent=j.restart_threshold_pkt_s+' pkt/s'; $('chk').textContent=j.check_interval_min+' min'; $('auto').textContent=j.auto_recovery?'ON':'OFF'; })}"
        "function loadTherm(){fetch('/api/thermal').then(r=>r.json()).then(j=>{ $('therm').innerHTML='Current: '+j.current_c.toFixed(1)+'°C, Max: '+j.max_c.toFixed(1)+'°C, CPU: '+j.cpu_mhz+' MHz'; })}"
        "function loadLogs(){fetch('/api/logs').then(r=>r.text()).then(t=>{ $('logs').textContent=t; })}"
        "function loadAll(){loadStatus();loadAudio();loadPerf();loadTherm();loadLogs()}"
        "setInterval(loadAll,3000);"
        "const sel=document.getElementById('langSel'); sel.value=lang; sel.onchange=()=>{lang=sel.value;localStorage.setItem('lang',lang);applyLang()}; applyLang(); loadAll();"
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
    json += "\"auto_recovery\":" + String(autoRecoveryEnabled?"true":"false") + "}";
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

static void httpActionStart(){ if (!isStreaming && rtspClient && rtspClient.connected()) { isStreaming=true; rtpSequence=0; rtpTimestamp=0; audioPacketsSent=0; lastStatsReset=millis(); lastRtspPlayMs=millis(); rtspPlayCount++; } apiSendJSON(F("{\"ok\":true}")); }
static void httpActionStop(){ if (isStreaming) { isStreaming=false; } apiSendJSON(F("{\"ok\":true}")); }
static void httpActionDisconnect(){ if (rtspClient && rtspClient.connected()) { rtspClient.stop(); isStreaming=false; } apiSendJSON(F("{\"ok\":true}")); }
static void httpActionServerStart(){ if (!rtspServerEnabled) { rtspServerEnabled=true; rtspServer.begin(); rtspServer.setNoDelay(true); } apiSendJSON(F("{\"ok\":true}")); }
static void httpActionServerStop(){ rtspServerEnabled=false; if (rtspClient && rtspClient.connected()) rtspClient.stop(); isStreaming=false; rtspServer.stop(); apiSendJSON(F("{\"ok\":true}")); }
static void httpActionResetI2S(){ restartI2S(); apiSendJSON(F("{\"ok\":true}")); }

static inline bool argToFloat(const String &name, float &out) { if (!web.hasArg("value")) return false; out = web.arg("value").toFloat(); return true; }
static inline bool argToUInt(const String &name, uint32_t &out) { if (!web.hasArg("value")) return false; out = (uint32_t) web.arg("value").toInt(); return true; }
static inline bool argToUShort(const String &name, uint16_t &out) { if (!web.hasArg("value")) return false; out = (uint16_t) web.arg("value").toInt(); return true; }
static inline bool argToUChar(const String &name, uint8_t &out) { if (!web.hasArg("value")) return false; out = (uint8_t) web.arg("value").toInt(); return true; }

static void httpSet() {
    String key = web.arg("key");
    if (key == "gain") { float v; if (argToFloat("value", v) && v>=0.1f && v<=100.0f) { currentGainFactor=v; saveAudioSettings(); restartI2S(); } }
    else if (key == "rate") { uint32_t v; if (argToUInt("value", v) && v>=8000 && v<=96000) { currentSampleRate=v; saveAudioSettings(); restartI2S(); } }
    else if (key == "buffer") { uint16_t v; if (argToUShort("value", v) && v>=256 && v<=8192) { currentBufferSize=v; saveAudioSettings(); restartI2S(); } }
    else if (key == "shift") { uint8_t v; if (argToUChar("value", v) && v<=24) { i2sShiftBits=v; saveAudioSettings(); restartI2S(); } }
    else if (key == "wifi_tx") { float v; if (argToFloat("value", v) && v>=-1.0f && v<=19.5f) { extern float wifiTxPowerDbm; wifiTxPowerDbm=v; applyWifiTxPower(true); saveAudioSettings(); } }
    else if (key == "auto_recovery") { String v=web.arg("value"); if (v=="on"||v=="off") { autoRecoveryEnabled=(v=="on"); saveAudioSettings(); } }
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
    web.on("/api/action/start", httpActionStart);
    web.on("/api/action/stop", httpActionStop);
    web.on("/api/action/disconnect", httpActionDisconnect);
    web.on("/api/action/server_start", httpActionServerStart);
    web.on("/api/action/server_stop", httpActionServerStop);
    web.on("/api/action/reset_i2s", httpActionResetI2S);
    web.on("/api/set", httpSet);
    web.begin();
}

void webui_handleClient() {
    web.handleClient();
}
