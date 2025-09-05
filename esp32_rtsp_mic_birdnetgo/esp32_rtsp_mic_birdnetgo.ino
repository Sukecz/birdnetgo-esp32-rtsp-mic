#include <WiFi.h>
#include <WiFiManager.h>
#include "driver/i2s.h"
#include <arpa/inet.h>      // kept; no longer using htons/htonl directly for headers
#include <ArduinoOTA.h>
#include <Preferences.h>
#include "WebUI.h"

// ================== SETTINGS (ESP32 RTSP Mic for BirdNET-Go) ==================
#define FW_VERSION "1.1.0"

// OTA password (optional):
// - For production, set a strong password to protect OTA updates.
// - You can leave it undefined to disable password protection.
// - Example placeholder (edit as needed):
// #define OTA_PASSWORD "1234"  // Optional: change or leave undefined

// -- DEFAULT PARAMETERS (configurable via Web UI / API)
#define DEFAULT_SAMPLE_RATE 48000
#define DEFAULT_GAIN_FACTOR 0.8f
#define DEFAULT_BUFFER_SIZE 1024   // Stable streaming profile by default
#define DEFAULT_WIFI_TX_DBM 19.5f  // Default WiFi TX power in dBm

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

// -- Scheduled reset
bool scheduledResetEnabled = false;
uint32_t resetIntervalHours = 24; // Default 24 hours

// -- Configurable thresholds
uint32_t minAcceptableRate = 50;        // Minimum acceptable packet rate (restart below this)
uint32_t performanceCheckInterval = 15; // Check interval in minutes
uint8_t cpuFrequencyMhz = 120;          // CPU frequency (default 120 MHz – balance)

// -- WiFi TX power (configurable)
float wifiTxPowerDbm = DEFAULT_WIFI_TX_DBM;
wifi_power_t currentWifiPowerLevel = WIFI_POWER_19_5dBm;

// -- RTSP connect/PLAY statistics
unsigned long lastRtspClientConnectMs = 0;
unsigned long lastRtspPlayMs = 0;
uint32_t rtspConnectCount = 0;
uint32_t rtspPlayCount = 0;

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

// Temperature monitoring
void checkTemperature() {
    float temp = temperatureRead(); // ESP32 internal sensor (approximate)
    if (temp > maxTemperature) {
        maxTemperature = temp;
    }

    // Only warn occasionally on high temperature; no periodic logging
    static unsigned long lastTempWarn = 0;
    if (temp > 80.0f && (millis() - lastTempWarn) > 600000UL) { // 10 min cooldown
        simplePrintln("WARNING: High temperature detected! Consider cooling.");
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
    if (WiFi.status() != WL_CONNECTED) {
        simplePrintln("WiFi disconnected! Reconnecting...");
        WiFi.reconnect();
    }

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
    cpuFrequencyMhz = audioPrefs.getUChar("cpuFreq", 120);
    wifiTxPowerDbm = audioPrefs.getFloat("wifiTxDbm", DEFAULT_WIFI_TX_DBM);
    audioPrefs.end();

    if (autoThresholdEnabled) {
        minAcceptableRate = computeRecommendedMinRate();
    }
    // Log the configured TX dBm (not the current enum), snapped for clarity
    float txShown = wifiPowerLevelToDbm(pickWifiPowerLevel(wifiTxPowerDbm));
    simplePrintln("Loaded settings: Rate=" + String(currentSampleRate) +
                  ", Gain=" + String(currentGainFactor, 1) +
                  ", Buffer=" + String(currentBufferSize) +
                  ", WiFiTX=" + String(txShown, 1) + "dBm" +
                  ", shiftBits=" + String(i2sShiftBits));
}

// Save settings to flash
void saveAudioSettings() {
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
    audioPrefs.end();

    simplePrintln("Settings saved to flash");
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
    cpuFrequencyMhz = 120;
    wifiTxPowerDbm = DEFAULT_WIFI_TX_DBM;

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
    maxPacketRate = 0;
    minPacketRate = 0xFFFFFFFF;
    simplePrintln("I2S restarted successfully");
}

// Minimal print helpers: Serial + buffered for Web UI
void simplePrint(String message) {
    Serial.print(message);
}

void simplePrintln(String message) {
    Serial.println(message);
    webui_pushLog(message);
}

// OTA setup
void setupOTA() {
    ArduinoOTA.setHostname("ESP32-RTSP-Mic");
#ifdef OTA_PASSWORD
    ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
    ArduinoOTA.begin();
}

// I2S setup
void setup_i2s_driver() {
    i2s_driver_uninstall(I2S_NUM_0);

    uint16_t dma_buf_len = (currentBufferSize > 512) ? 512 : currentBufferSize;

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = currentSampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = dma_buf_len,
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK_PIN,
        .ws_io_num = I2S_LRCLK_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_DOUT_PIN
    };

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);

    // (5) log i2sShiftBits for easier debugging
    simplePrintln("I2S ready: " + String(currentSampleRate) + "Hz, gain " +
                  String(currentGainFactor, 1) + ", buffer " + String(currentBufferSize) +
                  ", shiftBits " + String(i2sShiftBits));
}

static bool writeAll(WiFiClient &client, const uint8_t* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int w = client.write(data + off, len - off);
        if (w <= 0) return false;
        off += (size_t)w;
    }
    return true;
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

    if (!writeAll(client, inter, sizeof(inter)) ||
        !writeAll(client, header, sizeof(header)) ||
        !writeAll(client, (uint8_t*)audioData, payloadSize)) {
        isStreaming = false;
        return;
    }

    rtpSequence++;
    rtpTimestamp += (uint32_t)numSamples;
    audioPacketsSent++;
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

        for (int i = 0; i < samplesRead; i++) {
            float sample = (float)(i2s_32bit_buffer[i] >> i2sShiftBits);
            float amplified = sample * currentGainFactor;
            if (amplified > 32767.0f) amplified = 32767.0f;
            if (amplified < -32768.0f) amplified = -32768.0f;
            i2s_16bit_buffer[i] = (int16_t)amplified;
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

    lastRTSPActivity = millis();

    if (request.startsWith("OPTIONS")) {
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n\r\n");

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
        simplePrintln("STREAMING STARTED");

    } else if (request.startsWith("TEARDOWN")) {
        client.print("RTSP/1.0 200 OK\r\n");
        client.print("CSeq: " + cseq + "\r\n");
        client.print("Session: " + rtspSessionId + "\r\n\r\n");
        isStreaming = false;
        simplePrintln("STREAMING STOPPED");
    }
}

// RTSP processing
void processRTSP(WiFiClient &client) {
    if (!client.connected()) return;

    if (client.available()) {
        int available = client.available();

        if (rtspParseBufferPos + available >= (int)sizeof(rtspParseBuffer)) {
            available = sizeof(rtspParseBuffer) - rtspParseBufferPos - 1;
            if (available <= 0) {
                simplePrintln("RTSP buffer overflow - resetting");
                rtspParseBufferPos = 0;
                return;
            }
        }

        client.read(rtspParseBuffer + rtspParseBufferPos, available);
        rtspParseBufferPos += available;

        char* endOfHeader = strstr((char*)rtspParseBuffer, "\r\n\r\n");
        if (endOfHeader != nullptr) {
            *endOfHeader = '\0';
            String request = String((char*)rtspParseBuffer);

            handleRTSPCommand(client, request);

            int headerLen = (endOfHeader - (char*)rtspParseBuffer) + 4;
            memmove(rtspParseBuffer, rtspParseBuffer + headerLen, rtspParseBufferPos - headerLen);
            rtspParseBufferPos -= headerLen;
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

    // Enable external antenna (for XIAO ESP32-C6)
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

    setupOTA();
    setup_i2s_driver();

    rtspServer.begin();
    rtspServer.setNoDelay(true);
    // Web UI
    webui_begin();

    lastStatsReset = millis();
    lastRTSPActivity = millis();
    lastMemoryCheck = millis();
    lastPerformanceCheck = millis();
    lastWiFiCheck = millis();
    minFreeHeap = ESP.getFreeHeap();
    maxTemperature = temperatureRead();

    setCpuFrequencyMhz(cpuFrequencyMhz);
    simplePrintln("CPU frequency set to " + String(cpuFrequencyMhz) + " MHz for optimal thermal/performance balance");

    simplePrintln("RTSP server ready on port 8554");
    simplePrintln("Web UI: http://" + WiFi.localIP().toString() + "/");
    simplePrintln("RTSP URL: rtsp://" + WiFi.localIP().toString() + ":8554/audio");
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

    checkScheduledReset();

    // RTSP client management
    if (rtspServerEnabled) {
        if (rtspClient && !rtspClient.connected()) {
            rtspClient.stop();
            isStreaming = false;
            simplePrintln("RTSP client disconnected");
        }

        // Timeout for RTSP clients (30 seconds of inactivity)
        if (rtspClient && rtspClient.connected() && !isStreaming) {
            if (millis() - lastRTSPActivity > 30000) {
                rtspClient.stop();
                simplePrintln("RTSP client timeout - disconnected");
            }
        }

        if (!rtspClient || !rtspClient.connected()) {
            WiFiClient newClient = rtspServer.available();
            if (newClient) {
                rtspClient = newClient;
                rtspClient.setNoDelay(true);
                rtspParseBufferPos = 0;
                lastRTSPActivity = millis();
                lastRtspClientConnectMs = millis();
                rtspConnectCount++;
                simplePrintln("New RTSP client connected from: " + rtspClient.remoteIP().toString());
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
            rtspClient.stop();
            isStreaming = false;
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
