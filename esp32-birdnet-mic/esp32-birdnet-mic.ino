#include <WiFi.h>
#include <esp_wifi.h>
#include <WiFiManager.h>
#include "driver/i2s_std.h"
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <time.h>
#include <math.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <errno.h>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "WebUI.h"

// ================== SETTINGS (ESP32 RTSP Mic for BirdNET-Go / BirdNET-Pi) ==================
#define FW_VERSION "1.22"
// Expose FW version as a global C string for WebUI/API
const char* FW_VERSION_STR = FW_VERSION;
// Build timestamp for diagnostics (compile time)
const char* FW_BUILD_DATE_STR = __DATE__ " " __TIME__;

#if defined(M5STACK_ATOM_VOICES3R)
#define XIAO_BOARD_ID "m5stack-atom-voices3r"
#define XIAO_BOARD_NAME "M5Stack Atom VoiceS3R (C126)"
#define XIAO_CHIP_FAMILY "ESP32-S3"
#define XIAO_OTA_ARTIFACT ""
#define XIAO_HAS_RF_SWITCH 0
#elif defined(ARDUINO_XIAO_ESP32C3)
#define XIAO_BOARD_ID "xiao-esp32c3"
#define XIAO_BOARD_NAME "XIAO ESP32-C3"
#define XIAO_CHIP_FAMILY "ESP32-C3"
#define XIAO_OTA_ARTIFACT "firmware-app-c3.bin"
#define XIAO_HAS_RF_SWITCH 0
#elif defined(ARDUINO_XIAO_ESP32S3)
#define XIAO_BOARD_ID "xiao-esp32s3"
#define XIAO_BOARD_NAME "XIAO ESP32-S3"
#define XIAO_CHIP_FAMILY "ESP32-S3"
#define XIAO_OTA_ARTIFACT "firmware-app-s3.bin"
#define XIAO_HAS_RF_SWITCH 0
#elif defined(ARDUINO_XIAO_ESP32C5)
#define XIAO_BOARD_ID "xiao-esp32c5"
#define XIAO_BOARD_NAME "XIAO ESP32-C5"
#define XIAO_CHIP_FAMILY "ESP32-C5"
#define XIAO_OTA_ARTIFACT "firmware-app-c5.bin"
#define XIAO_HAS_RF_SWITCH 0
#elif defined(ARDUINO_XIAO_ESP32C6)
#define XIAO_BOARD_ID "xiao-esp32c6"
#define XIAO_BOARD_NAME "XIAO ESP32-C6"
#define XIAO_CHIP_FAMILY "ESP32-C6"
#define XIAO_OTA_ARTIFACT "firmware-app-c6.bin"
#define XIAO_HAS_RF_SWITCH 1
#else
#define XIAO_BOARD_ID "esp32-generic"
#define XIAO_BOARD_NAME "ESP32 generic"
#define XIAO_CHIP_FAMILY "ESP32"
#define XIAO_OTA_ARTIFACT ""
#define XIAO_HAS_RF_SWITCH 0
#endif

const char* FW_BOARD_ID_STR = XIAO_BOARD_ID;
const char* FW_BOARD_NAME_STR = XIAO_BOARD_NAME;
const char* FW_CHIP_FAMILY_STR = XIAO_CHIP_FAMILY;
const char* FW_OTA_ARTIFACT_STR = XIAO_OTA_ARTIFACT;

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
String mdnsHostname = "esp32mic"; // results in <hostname>.local
bool mdnsEnabled = true;
bool mdnsRunning = false;

// OTA password (optional):
// - For production, set a strong password to protect OTA updates.
// - You can leave it undefined to disable password protection.
// - Example placeholder (edit as needed):
// #define OTA_PASSWORD "1234"  // Optional: change or leave undefined

// -- DEFAULT PARAMETERS (configurable via Web UI / API)
#define DEFAULT_SAMPLE_RATE 48000
#define DEFAULT_GAIN_FACTOR 1.5f
#define DEFAULT_BUFFER_SIZE 512    // Balanced default; safer for BirdNET-Pi UDP than 1024-sample packets
#if defined(M5STACK_ATOM_VOICES3R)
#define DEFAULT_I2S_SHIFT_BITS 0
#else
#define DEFAULT_I2S_SHIFT_BITS 12
#endif
#define DEFAULT_WIFI_TX_DBM 15.0f  // Stable default; higher power adds substantial heat on C6
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
static const uint8_t OVERHEAT_CONFIRM_SAMPLES = 3;
static const float OVERHEAT_EMERGENCY_MARGIN_C = 10.0f;
static const unsigned long TEMPERATURE_CHECK_INTERVAL_MS = 5000UL;

// -- Pins
#if defined(M5STACK_ATOM_VOICES3R)
static constexpr int I2S_MCLK_PIN = 11;
static constexpr int I2S_BCLK_PIN = 17;
static constexpr int I2S_LRCLK_PIN = 3;
static constexpr int I2S_DOUT_PIN = 4;
static constexpr int ES8311_I2C_SDA_PIN = 45;
static constexpr int ES8311_I2C_SCL_PIN = 0;
static constexpr uint8_t ES8311_I2C_ADDRESS = 0x18;
#elif defined(ARDUINO_XIAO_ESP32C3) || defined(ARDUINO_XIAO_ESP32S3) || \
    defined(ARDUINO_XIAO_ESP32C5) || defined(ARDUINO_XIAO_ESP32C6)
static constexpr int I2S_MCLK_PIN = D7;
static constexpr int I2S_BCLK_PIN = D3;
static constexpr int I2S_LRCLK_PIN = D1;
static constexpr int I2S_DOUT_PIN = D2;
#else
static constexpr int I2S_MCLK_PIN = 7;
static constexpr int I2S_BCLK_PIN = 21;
static constexpr int I2S_LRCLK_PIN = 1;
static constexpr int I2S_DOUT_PIN = 2;
#endif

// -- Servers
WiFiServer rtspServer(8554);
WiFiClient rtspClient;

static const uint8_t MAX_CLIENTS = 3;

enum TransportMode : uint8_t {
    TRANSPORT_TCP = 0,
    TRANSPORT_UDP = 1
};

enum StreamTarget : uint8_t {
    STREAM_TARGET_BIRDNET_GO = 0,
    STREAM_TARGET_BIRDNET_PI = 1
};

struct StreamProfileConfig {
    uint8_t target = STREAM_TARGET_BIRDNET_GO;
};

struct ClientSession {
    WiFiClient client;
    WiFiUDP udpSocket;
    WiFiUDP rtcpSocket;
    TransportMode transport = TRANSPORT_TCP;
    IPAddress clientRtpAddress = IPAddress();
    uint16_t clientRtpPort = 0;
    uint16_t serverRtpPort = 0;
    String sessionId = "";
    bool streaming = false;
    uint8_t profileIndex = 0;  // 0 => /audio1 (alias /audio), 1 => /audio2
    uint16_t rtpSequence = 0;
    uint32_t rtpTimestamp = 0;
    unsigned long lastActivity = 0;
    unsigned long connectedAtMs = 0;
    unsigned long playStartedAtMs = 0;
    unsigned long lastRtpPacketMs = 0;
    char userAgent[64] = {0};
    uint8_t parseBuffer[1024];
    int parseBufferPos = 0;
    unsigned long packetsSent = 0;

    void reset() {
        if (client.connected()) client.stop();
        if (transport == TRANSPORT_UDP) {
            udpSocket.stop();
            rtcpSocket.stop();
        }
        transport = TRANSPORT_TCP;
        clientRtpPort = 0;
        serverRtpPort = 0;
        sessionId = "";
        streaming = false;
        profileIndex = 0;
        rtpSequence = 0;
        rtpTimestamp = 0;
        lastActivity = 0;
        connectedAtMs = 0;
        playStartedAtMs = 0;
        lastRtpPacketMs = 0;
        userAgent[0] = '\0';
        parseBufferPos = 0;
        packetsSent = 0;
    }
};

ClientSession clients[MAX_CLIENTS];
StreamProfileConfig streamProfiles[2] = {
    {STREAM_TARGET_BIRDNET_GO},
    {STREAM_TARGET_BIRDNET_PI},
};
bool streamEnabled[2] = {true, false};
uint8_t maxActiveClients = 2;

struct StreamStats {
    uint8_t clientCount = 0;
    bool streaming = false;
    uint32_t packetsSent = 0;
    unsigned long statsResetMs = 0;
    unsigned long lastConnectMs = 0;
    unsigned long lastPlayMs = 0;
};
StreamStats streamStats[2];

void stopAllRtspClients(const char* reason);
void stopRtspClientsForStream(uint8_t profileIndex, const char* reason);
void getStreamClientCounts(uint8_t &s1, uint8_t &s2);
uint8_t getRtspClientCount();
String getRtspClientSummary();

// -- RTSP Streaming
String rtspSessionId = "";
volatile bool isStreaming = false;
uint16_t rtpSequence = 0;
uint32_t rtpTimestamp = 0;
uint32_t rtpSSRC = 0x43215678;
unsigned long lastRTSPActivity = 0;

// -- Buffers
int32_t* i2s_32bit_buffer = nullptr;
int16_t* i2s_16bit_buffer = nullptr;
int16_t* i2s_16bit_network_buffer = nullptr;
uint8_t* rtpPacketScratch = nullptr;
i2s_chan_handle_t i2s_rx_handle = nullptr;
RingbufHandle_t audioRingBuffer = nullptr;
TaskHandle_t audioProducerTaskHandle = nullptr;
volatile bool audioProducerStopRequested = false;
volatile bool audioProducerRunning = false;
size_t audioRingBufferCapacityBytes = 0;

// -- Global state
unsigned long audioPacketsSent = 0;
unsigned long lastStatsReset = 0;
bool rtspServerEnabled = true;

// -- Audio parameters (runtime configurable)
uint32_t currentSampleRate = DEFAULT_SAMPLE_RATE;
float currentGainFactor = DEFAULT_GAIN_FACTOR;
uint16_t currentBufferSize = DEFAULT_BUFFER_SIZE;
uint8_t i2sShiftBits = DEFAULT_I2S_SHIFT_BITS;

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
static const unsigned long WEBUI_HANDLE_INTERVAL_MS = 20;
unsigned long lastWebuiHandleMs = 0;
uint32_t minFreeHeap = 0xFFFFFFFF;
uint32_t maxPacketRate = 0;
uint32_t minPacketRate = 0xFFFFFFFF;
bool autoRecoveryEnabled = true;
bool autoThresholdEnabled = true; // auto compute minAcceptableRate from sample rate and buffer size
// Deferred reboot scheduling (to restart safely outside HTTP context)
volatile bool scheduledFactoryReset = false;
volatile unsigned long scheduledRebootAt = 0;

// Deferred WiFi reconnect
volatile unsigned long wifiReconnectAt = 0;
bool wifiReconnectHasBssid = false;
uint8_t wifiReconnectBssid[6] = {0};
unsigned long bootTime = 0;
unsigned long lastI2SReset = 0;
float maxTemperature = 0.0f;
float lastTemperatureC = 0.0f;
bool lastTemperatureValid = false;
bool overheatProtectionEnabled = DEFAULT_OVERHEAT_PROTECTION;
float overheatShutdownC = (float)DEFAULT_OVERHEAT_LIMIT_C;
bool overheatLockoutActive = false;
uint8_t overheatConsecutiveSamples = 0;
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
void scheduleWifiReconnect(const uint8_t *bssid, uint32_t delayMs);
void mqttRequestReconnect(bool forceDiscovery);
void mqttPublishDiscoverySoon();
void saveAudioSettings();

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
uint32_t audioI2SErrorCount = 0;
uint32_t audioRingBufferDropCount = 0;
uint32_t audioRingBufferChunkCount = 0;
uint32_t audioRingBufferFlushCount = 0;
bool audioRingBufferFlushPending = false;
uint32_t rtspWriteStallCount = 0;
uint32_t rtspWriteTimeoutCount = 0;

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
IPAddress mqttResolvedIp;
String mqttResolvedHost = "";
bool mqttResolvedIpValid = false;
unsigned long lastMqttReconnectAttempt = 0;
unsigned long lastMqttPublishMs = 0;
unsigned long lastMqttLogMs = 0;
bool mqttDiscoveryPublished = false;
bool mqttForceDiscovery = false;
static const unsigned long MQTT_RECONNECT_INTERVAL_MS = 10000UL;
static const unsigned long MQTT_STREAMING_RECONNECT_INTERVAL_MS = 300000UL;
static const int32_t MQTT_STREAMING_CONNECT_TIMEOUT_MS = 500;
static const uint16_t MQTT_SOCKET_TIMEOUT_SEC = 1;
static const uint16_t MQTT_PUBLISH_INTERVAL_MIN_SEC = 10;
static const uint16_t MQTT_PUBLISH_INTERVAL_MAX_SEC = 3600;

// WiFi reconnect timing
static const unsigned long WIFI_RECONNECT_SETTLE_MS = 100UL;
static const unsigned long WIFI_RECONNECT_TIMEOUT_MS = 8000UL;
static const unsigned long WIFI_RECONNECT_POLL_MS = 100UL;

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

static String bssidBytesToStr(const uint8_t b[6]) {
    char s[18];
    snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X",
             b[0], b[1], b[2], b[3], b[4], b[5]);
    return String(s);
}

static void clearStoredBssidPin() {
    wifi_config_t cur;
    if (esp_wifi_get_config(WIFI_IF_STA, &cur) != ESP_OK) return;
    if (!cur.sta.bssid_set) return;
    cur.sta.bssid_set = false;
    memset(cur.sta.bssid, 0, 6);
    if (esp_wifi_set_config(WIFI_IF_STA, &cur) == ESP_OK) {
        simplePrintln("Cleared stored WiFi BSSID pin");
    }
}

static void logConnectedAp(const char *tag) {
    if (WiFi.status() != WL_CONNECTED) return;
    simplePrintln(String(tag) + " AP: ssid=" + WiFi.SSID() +
                  " bssid=" + WiFi.BSSIDstr() +
                  " ch=" + String(WiFi.channel()) +
                  " rssi=" + String(WiFi.RSSI()) + "dBm");
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

static bool isZeroMacBytes(const uint8_t mac[6]) {
    for (uint8_t i = 0; i < 6; ++i) {
        if (mac[i] != 0) return false;
    }
    return true;
}

static String macBytesToHex(const uint8_t mac[6]) {
    char buf[13];
    snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

static String buildMqttMacSuffix() {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK && !isZeroMacBytes(mac)) {
        return macBytesToHex(mac);
    }

    // Fallback for unusual cores: WiFi.macAddress() is valid after Wi-Fi init.
    String macStr = WiFi.macAddress();
    macStr.replace(":", "");
    macStr.toLowerCase();
    if (macStr != "000000000000") return macStr;

    // Last-resort fallback, formatted in standard MAC byte order.
    uint64_t efuse = ESP.getEfuseMac();
    mac[0] = (uint8_t)(efuse >> 40);
    mac[1] = (uint8_t)(efuse >> 32);
    mac[2] = (uint8_t)(efuse >> 24);
    mac[3] = (uint8_t)(efuse >> 16);
    mac[4] = (uint8_t)(efuse >> 8);
    mac[5] = (uint8_t)efuse;
    if (!isZeroMacBytes(mac)) return macBytesToHex(mac);

    return "000000000000";
}

static String defaultMdnsHostname() {
    String suffix = buildMqttMacSuffix();
    if (suffix.length() > 6) suffix = suffix.substring(suffix.length() - 6);
    suffix.toLowerCase();
    return String("esp32mic-") + suffix;
}

String sanitizeMdnsHostname(const String &input, const String &fallback) {
    String out;
    out.reserve(input.length() + 4);
    bool prevDash = false;
    for (size_t i = 0; i < input.length() && out.length() < 32; ++i) {
        char c = input[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!ok) c = '-';
        if (c == '-') {
            if (out.length() == 0 || prevDash) continue;
            prevDash = true;
        } else {
            prevDash = false;
        }
        out += c;
    }
    while (out.length() && out[out.length() - 1] == '-') out.remove(out.length() - 1);
    if (out.length() == 0) return fallback;
    return out;
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

static bool isHexMacSuffix(const String &s) {
    if (s.length() != 12) return false;
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        bool ok = (c >= '0' && c <= '9') ||
                  (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

static String mqttDeviceMacSuffix() {
    const String prefix = "esp32mic_";
    if (mqttDeviceId.startsWith(prefix)) {
        String suffix = mqttDeviceId.substring(prefix.length());
        suffix.toLowerCase();
        if (isHexMacSuffix(suffix)) return suffix;
    }
    return buildMqttMacSuffix();
}

static String mqttDefaultTopicPrefix() {
    return String("esp32mic/") + mqttDeviceMacSuffix();
}

static String mqttDefaultClientId() {
    return String("esp32mic-") + mqttDeviceMacSuffix();
}

static bool mqttIsLegacyDefaultTopicPrefix(const String &value) {
    String v = value;
    v.toLowerCase();
    return v == String("esp32mic/") + mqttDeviceId ||
           v == "esp32mic/esp32mic_000000000000" ||
           v == "esp32mic_000000000000" ||
           v == "esp32mic/000000000000";
}

static bool mqttIsLegacyDefaultClientId(const String &value) {
    String v = value;
    v.toLowerCase();
    return v == String("esp32mic-") + mqttDeviceId ||
           v == "esp32mic-esp32mic_000000000000" ||
           v == "esp32mic_000000000000" ||
           v == "esp32mic-000000000000";
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

static String mqttCmdStreamEnabledTopic(uint8_t profileIndex) {
    return mqttTopicPrefix + "/cmd/stream" + String(profileIndex + 1) + "_enabled";
}

static String mqttCmdStreamTargetTopic(uint8_t profileIndex) {
    return mqttTopicPrefix + "/cmd/stream" + String(profileIndex + 1) + "_target";
}

static String mqttCmdRebootTopic() {
    return mqttTopicPrefix + "/cmd/reboot";
}

static String streamTargetName(uint8_t target) {
    return (target == STREAM_TARGET_BIRDNET_PI) ? String("BirdNET-Pi") : String("BirdNET-Go");
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
    if (mqttIsLegacyDefaultTopicPrefix(sanitizeMqttTopicPath(mqttTopicPrefix, ""))) {
        mqttTopicPrefix = "";
    }
    if (mqttIsLegacyDefaultClientId(sanitizeMqttClientId(mqttClientId, ""))) {
        mqttClientId = "";
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
    json += "\"mdl\":\"" + mqttJsonEscape(String(FW_BOARD_NAME_STR)) + "\",";
    json += "\"mf\":\"Sukecz\",";
    json += "\"sw\":\"" + mqttJsonEscape(String(FW_VERSION_STR)) + "\",";
    json += "\"cu\":\"http://" + mqttJsonEscape(WiFi.localIP().toString()) + "/\"";
    json += "}";
    return json;
}

static uint32_t calculatePacketRate(uint32_t packets, uint32_t runtimeMs) {
    if (runtimeMs == 0) return 0;
    return (uint32_t)(((uint64_t)packets * 1000ULL) / runtimeMs);
}

static String mqttBuildStateJson() {
    unsigned long nowMs = millis();
    unsigned long uptimeSeconds = (nowMs - bootTime) / 1000;
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < minFreeHeap) minFreeHeap = freeHeap;
    unsigned long runtime = nowMs - lastStatsReset;
    uint32_t currentRate = (isStreaming && runtime > 1000) ? calculatePacketRate(audioPacketsSent, runtime) : 0;
    uint32_t streamUptimeSeconds = (isStreaming && streamStartedAtMs > 0 && nowMs >= streamStartedAtMs)
                                       ? (uint32_t)((nowMs - streamStartedAtMs) / 1000UL)
                                       : 0;
    uint8_t clientCount = getRtspClientCount();
    uint8_t s1clients = 0, s2clients = 0;
    getStreamClientCounts(s1clients, s2clients);
    uint32_t streamRate[2] = {0, 0};
    uint32_t streamLastPlayAge[2] = {0, 0};
    for (uint8_t i = 0; i < 2; i++) {
        unsigned long streamRuntime = nowMs - streamStats[i].statsResetMs;
        if (streamStats[i].streaming && streamRuntime > 1000) {
            streamRate[i] = calculatePacketRate(streamStats[i].packetsSent, streamRuntime);
        }
        if (streamStats[i].lastPlayMs > 0 && nowMs >= streamStats[i].lastPlayMs) {
            streamLastPlayAge[i] = (uint32_t)((nowMs - streamStats[i].lastPlayMs) / 1000UL);
        }
    }

    String json = "{";
    json += "\"fw_version\":\"" + mqttJsonEscape(String(FW_VERSION_STR)) + "\",";
    json += "\"board_id\":\"" + mqttJsonEscape(String(FW_BOARD_ID_STR)) + "\",";
    json += "\"board_name\":\"" + mqttJsonEscape(String(FW_BOARD_NAME_STR)) + "\",";
    json += "\"chip_family\":\"" + mqttJsonEscape(String(FW_CHIP_FAMILY_STR)) + "\",";
    json += "\"fw_build\":\"" + mqttJsonEscape(String(FW_BUILD_DATE_STR)) + "\",";
    json += "\"reboot_reason\":\"" + mqttJsonEscape(rebootReason) + "\",";
    json += "\"restart_counter\":" + String(restartCounter) + ",";
    json += "\"ip\":\"" + mqttJsonEscape(WiFi.localIP().toString()) + "\",";
    json += "\"wifi_ssid\":\"" + mqttJsonEscape(WiFi.SSID()) + "\",";
    json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"wifi_reconnect_count\":" + String(wifiReconnectCount) + ",";
    json += "\"wifi_tx_dbm\":" + String(wifiPowerLevelToDbm(currentWifiPowerLevel), 1) + ",";
    json += "\"free_heap_kb\":" + String(freeHeap / 1024) + ",";
    json += "\"min_free_heap_kb\":" + String(minFreeHeap / 1024) + ",";
    json += "\"uptime_s\":" + String(uptimeSeconds) + ",";
    json += "\"rtsp_server_enabled\":" + String(rtspServerEnabled ? "true" : "false") + ",";
    json += "\"streaming\":" + String(isStreaming ? "true" : "false") + ",";
    json += "\"stream_uptime_s\":" + String(streamUptimeSeconds) + ",";
    json += "\"client_count\":" + String((uint32_t)clientCount) + ",";
    json += "\"current_rate_pkt_s\":" + String(currentRate) + ",";
    json += "\"stream1_url_ip\":\"rtsp://" + mqttJsonEscape(WiFi.localIP().toString()) + ":8554/audio1\",";
    json += "\"stream2_url_ip\":\"rtsp://" + mqttJsonEscape(WiFi.localIP().toString()) + ":8554/audio2\",";
    json += "\"stream1_url_mdns\":\"rtsp://" + mqttJsonEscape(mdnsHostname) + ".local:8554/audio1\",";
    json += "\"stream2_url_mdns\":\"rtsp://" + mqttJsonEscape(mdnsHostname) + ".local:8554/audio2\",";
    json += "\"stream1_enabled\":" + String(streamEnabled[0] ? "true" : "false") + ",";
    json += "\"stream2_enabled\":" + String(streamEnabled[1] ? "true" : "false") + ",";
    json += "\"stream1_streaming\":" + String(streamStats[0].streaming ? "true" : "false") + ",";
    json += "\"stream2_streaming\":" + String(streamStats[1].streaming ? "true" : "false") + ",";
    json += "\"stream1_clients\":" + String((uint32_t)s1clients) + ",";
    json += "\"stream2_clients\":" + String((uint32_t)s2clients) + ",";
    json += "\"stream1_packet_rate\":" + String(streamRate[0]) + ",";
    json += "\"stream2_packet_rate\":" + String(streamRate[1]) + ",";
    json += "\"stream1_target\":\"" + streamTargetName(streamProfiles[0].target) + "\",";
    json += "\"stream2_target\":\"" + streamTargetName(streamProfiles[1].target) + "\",";
    json += "\"stream1_last_play_age_s\":" + String(streamLastPlayAge[0]) + ",";
    json += "\"stream2_last_play_age_s\":" + String(streamLastPlayAge[1]) + ",";
    json += "\"sample_rate\":" + String(currentSampleRate) + ",";
    json += "\"audio_format\":\"L16/mono\",";
    json += "\"buffer_size\":" + String(currentBufferSize) + ",";
    json += "\"gain\":" + String(currentGainFactor, 2) + ",";
    json += "\"client\":\"" + mqttJsonEscape(getRtspClientSummary()) + "\",";
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

struct MqttDiscoveryEntity {
    const char *component;
    const char *objectId;
};

static const MqttDiscoveryEntity MQTT_DISCOVERY_ENTITIES[] = {
    {"sensor", "wifi_rssi"},
    {"sensor", "heap_kb"},
    {"sensor", "packet_rate"},
    {"sensor", "temperature_c"},
    {"sensor", "max_temperature_c"},
    {"sensor", "uptime_s"},
    {"binary_sensor", "streaming"},
    {"switch", "rtsp_server"},
    {"sensor", "rtsp_client"},
    {"sensor", "fw_version"},
    {"sensor", "fw_build"},
    {"sensor", "reboot_reason"},
    {"sensor", "restart_counter"},
    {"sensor", "wifi_ssid"},
    {"sensor", "wifi_reconnect_count"},
    {"sensor", "stream_uptime_s"},
    {"sensor", "client_count"},
    {"switch", "stream1_enabled"},
    {"switch", "stream2_enabled"},
    {"binary_sensor", "stream1_streaming"},
    {"binary_sensor", "stream2_streaming"},
    {"sensor", "stream1_clients"},
    {"sensor", "stream2_clients"},
    {"sensor", "stream1_packet_rate"},
    {"sensor", "stream2_packet_rate"},
    {"sensor", "stream1_url"},
    {"sensor", "stream2_url"},
    {"select", "stream1_target"},
    {"select", "stream2_target"},
    {"sensor", "sample_rate_hz"},
    {"sensor", "audio_format"},
    {"button", "reboot"},
};

static void mqttClearRetainedTopic(const String &topic) {
    if (topic.length() == 0) return;
    mqttClient.publish(topic.c_str(), "", true);
}

static void mqttClearDiscoveryForDeviceId(const String &deviceId) {
    if (deviceId.length() == 0 || deviceId == mqttDeviceId) return;
    for (const MqttDiscoveryEntity &entity : MQTT_DISCOVERY_ENTITIES) {
        String topic = mqttDiscoveryPrefix + "/" + entity.component + "/" + deviceId + "/" + entity.objectId + "/config";
        mqttClearRetainedTopic(topic);
    }
}

static void mqttClearLegacyRetainedTopics() {
    mqttClearDiscoveryForDeviceId("esp32mic_000000000000");

    String currentAvailability = mqttAvailabilityTopic();
    String oldZeroAvailability = "esp32mic/esp32mic_000000000000/availability";
    if (oldZeroAvailability != currentAvailability) mqttClearRetainedTopic(oldZeroAvailability);

    String oldRepeatedAvailability = String("esp32mic/") + mqttDeviceId + "/availability";
    if (oldRepeatedAvailability != currentAvailability) mqttClearRetainedTopic(oldRepeatedAvailability);
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

    mqttClearLegacyRetainedTopics();

    String dev = mqttBuildDeviceJson();
    String st = mqttStateTopic();
    String av = mqttAvailabilityTopic();
    String cmdRtsp = mqttCmdRtspTopic();
    String cmdReboot = mqttCmdRebootTopic();
    String cmdS1Enabled = mqttCmdStreamEnabledTopic(0);
    String cmdS2Enabled = mqttCmdStreamEnabledTopic(1);
    String cmdS1Target = mqttCmdStreamTargetTopic(0);
    String cmdS2Target = mqttCmdStreamTargetTopic(1);
    bool ok = true;

    String p;

    p = "{\"name\":\"WiFi RSSI\",\"uniq_id\":\"" + mqttDeviceId + "_wifi_rssi\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.wifi_rssi }}\",\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\",\"stat_cla\":\"measurement\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "wifi_rssi", p);

    p = "{\"name\":\"Free Heap\",\"uniq_id\":\"" + mqttDeviceId + "_heap_kb\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.free_heap_kb }}\",\"unit_of_meas\":\"KB\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "heap_kb", p);

    p = "{\"name\":\"Packet Rate\",\"uniq_id\":\"" + mqttDeviceId + "_pkt_rate\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.current_rate_pkt_s }}\",\"unit_of_meas\":\"pkt/s\",\"stat_cla\":\"measurement\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "packet_rate", p);

    p = "{\"name\":\"Temperature\",\"uniq_id\":\"" + mqttDeviceId + "_temp_c\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.temperature_c }}\",\"unit_of_meas\":\"\u00B0C\",\"dev_cla\":\"temperature\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "temperature_c", p);

    p = "{\"name\":\"Peak Temperature\",\"uniq_id\":\"" + mqttDeviceId + "_max_temp_c\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.max_temperature_c }}\",\"unit_of_meas\":\"\u00B0C\",\"dev_cla\":\"temperature\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\",\"ic\":\"mdi:thermometer-high\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "max_temperature_c", p);

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

    p = "{\"name\":\"Stream 1 Enabled\",\"uniq_id\":\"" + mqttDeviceId + "_stream1_enabled\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ 'ON' if value_json.stream1_enabled else 'OFF' }}\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"cmd_t\":\"" + cmdS1Enabled + "\",\"ic\":\"mdi:microphone\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("switch", "stream1_enabled", p);

    p = "{\"name\":\"Stream 2 Enabled\",\"uniq_id\":\"" + mqttDeviceId + "_stream2_enabled\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ 'ON' if value_json.stream2_enabled else 'OFF' }}\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"cmd_t\":\"" + cmdS2Enabled + "\",\"ic\":\"mdi:microphone\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("switch", "stream2_enabled", p);

    p = "{\"name\":\"Stream 1 Streaming\",\"uniq_id\":\"" + mqttDeviceId + "_stream1_streaming\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ 'ON' if value_json.stream1_streaming else 'OFF' }}\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"dev_cla\":\"running\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("binary_sensor", "stream1_streaming", p);

    p = "{\"name\":\"Stream 2 Streaming\",\"uniq_id\":\"" + mqttDeviceId + "_stream2_streaming\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ 'ON' if value_json.stream2_streaming else 'OFF' }}\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"dev_cla\":\"running\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("binary_sensor", "stream2_streaming", p);

    p = "{\"name\":\"Stream 1 Clients\",\"uniq_id\":\"" + mqttDeviceId + "_stream1_clients\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.stream1_clients }}\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\",\"ic\":\"mdi:account-multiple\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "stream1_clients", p);

    p = "{\"name\":\"Stream 2 Clients\",\"uniq_id\":\"" + mqttDeviceId + "_stream2_clients\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.stream2_clients }}\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\",\"ic\":\"mdi:account-multiple\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "stream2_clients", p);

    p = "{\"name\":\"Stream 1 Packet Rate\",\"uniq_id\":\"" + mqttDeviceId + "_stream1_packet_rate\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.stream1_packet_rate }}\",\"unit_of_meas\":\"pkt/s\",\"stat_cla\":\"measurement\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "stream1_packet_rate", p);

    p = "{\"name\":\"Stream 2 Packet Rate\",\"uniq_id\":\"" + mqttDeviceId + "_stream2_packet_rate\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.stream2_packet_rate }}\",\"unit_of_meas\":\"pkt/s\",\"stat_cla\":\"measurement\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "stream2_packet_rate", p);

    p = "{\"name\":\"Stream 1 URL\",\"uniq_id\":\"" + mqttDeviceId + "_stream1_url\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.stream1_url_ip }}\",\"ic\":\"mdi:link-variant\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "stream1_url", p);

    p = "{\"name\":\"Stream 2 URL\",\"uniq_id\":\"" + mqttDeviceId + "_stream2_url\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.stream2_url_ip }}\",\"ic\":\"mdi:link-variant\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("sensor", "stream2_url", p);

    p = "{\"name\":\"Stream 1 Target\",\"uniq_id\":\"" + mqttDeviceId + "_stream1_target\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.stream1_target }}\",\"cmd_t\":\"" + cmdS1Target + "\",\"options\":[\"BirdNET-Go\",\"BirdNET-Pi\"],\"ic\":\"mdi:target\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("select", "stream1_target", p);

    p = "{\"name\":\"Stream 2 Target\",\"uniq_id\":\"" + mqttDeviceId + "_stream2_target\",\"stat_t\":\"" + st + "\",\"val_tpl\":\"{{ value_json.stream2_target }}\",\"cmd_t\":\"" + cmdS2Target + "\",\"options\":[\"BirdNET-Go\",\"BirdNET-Pi\"],\"ic\":\"mdi:target\",\"avty_t\":\"" + av + "\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"dev\":" + dev + "}";
    ok &= mqttPublishDiscoveryConfig("select", "stream2_target", p);

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
            stopAllRtspClients("MQTT RTSP server disabled");
            rtspServer.stop();
            simplePrintln("MQTT command: RTSP server disabled.");
        }
        mqttPublishState(true);
        return;
    }

    for (uint8_t i = 0; i < 2; i++) {
        if (t == mqttCmdStreamEnabledTopic(i)) {
            if (up == "ON" || up == "OFF") {
                bool enabled = (up == "ON");
                if (streamEnabled[i] != enabled) {
                    streamEnabled[i] = enabled;
                    if (!enabled) stopRtspClientsForStream(i, "MQTT stream disabled");
                    saveAudioSettings();
                    simplePrintln("MQTT command: stream" + String(i + 1) + (enabled ? " enabled." : " disabled."));
                }
                mqttPublishState(true);
            }
            return;
        }

        if (t == mqttCmdStreamTargetTopic(i)) {
            uint8_t target = streamProfiles[i].target;
            if (up == "BIRDNET-GO" || up == "GO" || up == "0") {
                target = STREAM_TARGET_BIRDNET_GO;
            } else if (up == "BIRDNET-PI" || up == "PI" || up == "1") {
                target = STREAM_TARGET_BIRDNET_PI;
            } else {
                return;
            }
            if (streamProfiles[i].target != target) {
                streamProfiles[i].target = target;
                stopRtspClientsForStream(i, "MQTT stream target changed");
                saveAudioSettings();
                simplePrintln("MQTT command: stream" + String(i + 1) + " target set to " + streamTargetName(target) + ".");
            }
            mqttPublishState(true);
            return;
        }
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
    mqttClient.setSocketTimeout(MQTT_SOCKET_TIMEOUT_SEC);
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

static bool mqttConnectNow(bool streamingReconnect = false) {
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

    if (streamingReconnect) {
        IPAddress brokerIp;
        bool haveBrokerIp = brokerIp.fromString(mqttHost);
        if (!haveBrokerIp && mqttResolvedIpValid && mqttResolvedHost == mqttHost) {
            brokerIp = mqttResolvedIp;
            haveBrokerIp = true;
        }
        if (!haveBrokerIp) {
            mqttConnected = false;
            mqttLastError = "reconnect_waiting_for_dns";
            return false;
        }

        // Keep a failed broker probe short enough that RTSP delivery can resume
        // promptly. PubSubClient will reuse this established TCP connection.
        mqttNetClient.stop();
        if (!mqttNetClient.connect(brokerIp, mqttPort, MQTT_STREAMING_CONNECT_TIMEOUT_MS)) {
            mqttConnected = false;
            mqttLastError = "connect_failed_tcp";
            return false;
        }
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
    mqttResolvedIp = mqttNetClient.remoteIP();
    mqttResolvedHost = mqttHost;
    mqttResolvedIpValid = true;
    mqttClient.publish(availTopic.c_str(), "online", true);
    mqttClient.subscribe(mqttCmdRtspTopic().c_str());
    for (uint8_t i = 0; i < 2; i++) {
        mqttClient.subscribe(mqttCmdStreamEnabledTopic(i).c_str());
        mqttClient.subscribe(mqttCmdStreamTargetTopic(i).c_str());
    }
    mqttClient.subscribe(mqttCmdRebootTopic().c_str());
    mqttDiscoveryPublished = false;
    if (!streamingReconnect && !mqttPublishDiscovery()) {
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
        // Streaming reconnects use a cached/numeric broker IP, a short TCP
        // timeout, and a deliberately relaxed retry cadence to protect audio.
        unsigned long interval = isStreaming
            ? MQTT_STREAMING_RECONNECT_INTERVAL_MS
            : MQTT_RECONNECT_INTERVAL_MS;
        if (lastMqttReconnectAttempt != 0 && (now - lastMqttReconnectAttempt) < interval) return;
        lastMqttReconnectAttempt = now;
        bool ok = mqttConnectNow(isStreaming);
        if (!ok && (now - lastMqttLogMs) > 60000UL) {
            simplePrintln("MQTT connect failed: " + mqttLastError);
            lastMqttLogMs = now;
        }
        return;
    }

    mqttConnected = true;
    mqttClient.loop();
    if (!isStreaming && (mqttForceDiscovery || !mqttDiscoveryPublished)) {
        if (!mqttPublishDiscovery()) mqttLastError = "discovery_publish_failed";
    }
    if (!mqttPublishState(false)) {
        mqttLastError = "state_publish_failed";
    }
}

uint16_t maxHighpassCutoffForRate(uint32_t sampleRate) {
    uint32_t maxCutoff = (uint32_t)((float)sampleRate * 0.45f);
    if (maxCutoff < 10U) maxCutoff = 10U;
    if (maxCutoff > 10000U) maxCutoff = 10000U;
    return (uint16_t)maxCutoff;
}

static uint16_t normalizeHighpassCutoff(uint32_t sampleRate, uint32_t requestedCutoff) {
    if (requestedCutoff < 10U) return 10U;
    uint16_t maxCutoff = maxHighpassCutoffForRate(sampleRate);
    return requestedCutoff > maxCutoff ? maxCutoff : (uint16_t)requestedCutoff;
}

// Recompute HPF coefficients (2nd-order Butterworth high-pass)
void updateHighpassCoeffs() {
    highpassCutoffHz = normalizeHighpassCutoff(currentSampleRate, highpassCutoffHz);
    if (!highpassEnabled) {
        hpf.reset();
        hpfConfigSampleRate = currentSampleRate;
        hpfConfigCutoff = highpassCutoffHz;
        return;
    }
    float fs = (float)currentSampleRate;
    float fc = (float)highpassCutoffHz;

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
    hpfConfigCutoff = highpassCutoffHz;
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
        stopAllRtspClients("Stream schedule window closed");
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
    if (getRtspClientCount() > 0) {
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
    stopAllRtspClients("Deep sleep outside stream schedule");
    if (rtspServerEnabled) {
        rtspServerEnabled = false;
        rtspServer.stop();
    }
    if (mqttClient.connected()) {
        mqttPublishState(true);
        mqttClient.publish(mqttAvailabilityTopic().c_str(), "offline", true);
        mqttClient.loop();
        mqttClient.disconnect();
        delay(200);
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);

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
        overheatConsecutiveSamples = 0;
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
        if (!overheatLockoutActive) {
            bool confirmedTrip = false;
            if (temp >= (overheatShutdownC + OVERHEAT_EMERGENCY_MARGIN_C)) {
                // A clearly dangerous excursion still shuts down immediately.
                overheatConsecutiveSamples = 0;
                confirmedTrip = true;
            } else if (temp >= overheatShutdownC) {
                if (overheatConsecutiveSamples < OVERHEAT_CONFIRM_SAMPLES) {
                    overheatConsecutiveSamples++;
                }
                confirmedTrip = (overheatConsecutiveSamples >= OVERHEAT_CONFIRM_SAMPLES);
            } else {
                overheatConsecutiveSamples = 0;
            }

            if (confirmedTrip) {
                overheatLockoutActive = true;
                recordOverheatTrip(temp);
                // Disable streaming until user restarts manually
                stopAllRtspClients("Thermal protection");
                rtspServerEnabled = false;
                rtspServer.stop();
                mqttPublishState(true);
            }
        } else if (overheatLockoutActive && temp <= (overheatShutdownC - OVERHEAT_LIMIT_STEP_C)) {
            // Allow re-arming after we cool down by at least one step
            overheatLockoutActive = false;
            overheatConsecutiveSamples = 0;
        }
    } else {
        overheatLockoutActive = false;
        overheatConsecutiveSamples = 0;
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
        uint32_t currentRate = calculatePacketRate(audioPacketsSent, runtime);

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
        if (timeOffsetMinutes < -720 || timeOffsetMinutes > 840) timeOffsetMinutes = 0;
        timeSyncEnabled = bootPrefs.getBool("timeSyncEn", true);
        bootPrefs.end();
    }
    configureTimeService(timeSyncEnabled);
}

// Load settings from flash
void loadAudioSettings() {
    bool settingsRepaired = false;
    audioPrefs.begin("audio", false);
    currentSampleRate = audioPrefs.getUInt("sampleRate", DEFAULT_SAMPLE_RATE);
    currentGainFactor = audioPrefs.getFloat("gainFactor", DEFAULT_GAIN_FACTOR);
    currentBufferSize = audioPrefs.getUShort("bufferSize", DEFAULT_BUFFER_SIZE);
    // Respect the board-specific compile-time default on first boot.
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
    mdnsHostname = sanitizeMdnsHostname(audioPrefs.getString("mdnsHost", defaultMdnsHostname()), defaultMdnsHostname());
    streamScheduleEnabled = audioPrefs.getBool("strSchedEn", false);
    streamScheduleStartMin = (uint16_t)audioPrefs.getUInt("strSchStart", 0);
    streamScheduleStopMin = (uint16_t)audioPrefs.getUInt("strSchStop", 0);
    deepSleepScheduleEnabled = audioPrefs.getBool("deepSchSlp", false);
    mqttEnabled = audioPrefs.getBool("mqttEn", false);
    mqttHost = audioPrefs.getString("mqttHost", "");
    uint32_t storedMqttPort = audioPrefs.getUInt("mqttPort", DEFAULT_MQTT_PORT);
    mqttPort = (storedMqttPort >= 1U && storedMqttPort <= 65535U)
                   ? (uint16_t)storedMqttPort
                   : DEFAULT_MQTT_PORT;
    if (storedMqttPort != mqttPort) settingsRepaired = true;
    mqttUser = audioPrefs.getString("mqttUser", "");
    mqttPassword = audioPrefs.getString("mqttPass", "");
    mqttTopicPrefix = audioPrefs.getString("mqttTop", "");
    mqttDiscoveryPrefix = audioPrefs.getString("mqttDisc", "homeassistant");
    mqttClientId = audioPrefs.getString("mqttCid", "");
    uint32_t storedMqttInterval = audioPrefs.getUInt("mqttIntSec", DEFAULT_MQTT_PUBLISH_INTERVAL_SEC);
    mqttPublishIntervalSec = (storedMqttInterval >= MQTT_PUBLISH_INTERVAL_MIN_SEC &&
                              storedMqttInterval <= MQTT_PUBLISH_INTERVAL_MAX_SEC)
                                 ? (uint16_t)storedMqttInterval
                                 : DEFAULT_MQTT_PUBLISH_INTERVAL_SEC;
    if (storedMqttInterval != mqttPublishIntervalSec) settingsRepaired = true;
    streamProfiles[0].target = (uint8_t)audioPrefs.getUChar("s1Target", STREAM_TARGET_BIRDNET_GO);
    streamProfiles[1].target = (uint8_t)audioPrefs.getUChar("s2Target", STREAM_TARGET_BIRDNET_PI);
    streamEnabled[0] = audioPrefs.getBool("s1Enabled", true);
    streamEnabled[1] = audioPrefs.getBool("s2Enabled", false);
    maxActiveClients = (uint8_t)audioPrefs.getUChar("maxClients", 2);
    if (streamScheduleStartMin > 1439) { streamScheduleStartMin = 0; settingsRepaired = true; }
    if (streamScheduleStopMin > 1439) { streamScheduleStopMin = 0; settingsRepaired = true; }
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

    if (currentSampleRate < 8000U || currentSampleRate > 192000U) {
        currentSampleRate = DEFAULT_SAMPLE_RATE;
        settingsRepaired = true;
    }
    if (!isfinite(currentGainFactor) || currentGainFactor < 0.1f || currentGainFactor > 100.0f) {
        currentGainFactor = DEFAULT_GAIN_FACTOR;
        settingsRepaired = true;
    }
    if (currentBufferSize < 256U || currentBufferSize > 8192U) {
        currentBufferSize = DEFAULT_BUFFER_SIZE;
        settingsRepaired = true;
    }
    if (i2sShiftBits > 24U) {
        i2sShiftBits = DEFAULT_I2S_SHIFT_BITS;
        settingsRepaired = true;
    }
    if (resetIntervalHours < 1U || resetIntervalHours > 168U) {
        resetIntervalHours = 24;
        settingsRepaired = true;
    }
    if (minAcceptableRate < 5U || minAcceptableRate > 200U) {
        minAcceptableRate = 50;
        settingsRepaired = true;
    }
    if (performanceCheckInterval < 1U || performanceCheckInterval > 60U) {
        performanceCheckInterval = 15;
        settingsRepaired = true;
    }
    if (cpuFrequencyMhz != 40U && cpuFrequencyMhz != 80U && cpuFrequencyMhz != 160U) {
        cpuFrequencyMhz = 160;
        settingsRepaired = true;
    }
    if (!isfinite(wifiTxPowerDbm) || wifiTxPowerDbm < -1.0f || wifiTxPowerDbm > 19.5f) {
        wifiTxPowerDbm = DEFAULT_WIFI_TX_DBM;
        settingsRepaired = true;
    } else {
        float snappedTx = wifiPowerLevelToDbm(pickWifiPowerLevel(wifiTxPowerDbm));
        if (fabsf(snappedTx - wifiTxPowerDbm) > 0.01f) settingsRepaired = true;
        wifiTxPowerDbm = snappedTx;
    }
    uint16_t normalizedCutoff = normalizeHighpassCutoff(currentSampleRate, highpassCutoffHz);
    if (normalizedCutoff != highpassCutoffHz) {
        highpassCutoffHz = normalizedCutoff;
        settingsRepaired = true;
    }
    if (timeOffsetMinutes < -720 || timeOffsetMinutes > 840) {
        timeOffsetMinutes = 0;
        settingsRepaired = true;
    }
    if (streamProfiles[0].target > STREAM_TARGET_BIRDNET_PI) { streamProfiles[0].target = STREAM_TARGET_BIRDNET_GO; settingsRepaired = true; }
    if (streamProfiles[1].target > STREAM_TARGET_BIRDNET_PI) { streamProfiles[1].target = STREAM_TARGET_BIRDNET_PI; settingsRepaired = true; }
    if (maxActiveClients < 1 || maxActiveClients > MAX_CLIENTS) { maxActiveClients = 2; settingsRepaired = true; }
    mqttApplyClientSettings(false);

    // Apply timezone/offset immediately after loading persisted settings so that
    // *all* early boot logs (including "Loaded settings") use the correct local time.
    // This does not block; NTP sync is attempted later when Wi-Fi is available.
    configureTimeService(timeSyncEnabled);

    if (autoThresholdEnabled) {
        minAcceptableRate = computeRecommendedMinRate();
    }
    if (settingsRepaired) {
        simplePrintln("Invalid stored settings repaired and replaced with safe values.");
        saveAudioSettings();
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
    audioPrefs.putString("mdnsHost", mdnsHostname);
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
    audioPrefs.putUChar("s1Target", streamProfiles[0].target);
    audioPrefs.putUChar("s2Target", streamProfiles[1].target);
    audioPrefs.putBool("s1Enabled", streamEnabled[0]);
    audioPrefs.putBool("s2Enabled", streamEnabled[1]);
    audioPrefs.putUChar("maxClients", maxActiveClients);
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
    if (!MDNS.begin(mdnsHostname.c_str())) {
        simplePrintln("mDNS start failed");
        mdnsRunning = false;
        return;
    }
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("rtsp", "tcp", 8554);
    mdnsRunning = true;
    simplePrintln("mDNS ready: http://" + mdnsHostname + ".local/ ; rtsp://" + mdnsHostname + ".local:8554/audio1");
}

// Schedule a safe reboot (optionally with factory reset) after delayMs
void scheduleReboot(bool factoryReset, uint32_t delayMs) {
    scheduledFactoryReset = factoryReset;
    scheduledRebootAt = millis() + delayMs;
}

// Schedule a WiFi reconnect after delayMs, optionally pinning to a specific BSSID
void scheduleWifiReconnect(const uint8_t *bssid, uint32_t delayMs) {
    if (bssid) {
        wifiReconnectHasBssid = true;
        memcpy(wifiReconnectBssid, bssid, 6);
    } else {
        wifiReconnectHasBssid = false;
    }
    wifiReconnectAt = millis() + delayMs;
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
    i2sShiftBits = DEFAULT_I2S_SHIFT_BITS;

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
    overheatConsecutiveSamples = 0;
    overheatTripTemp = 0.0f;
    overheatTriggeredAt = 0;
    overheatLastReason = "";
    overheatLastTimestamp = "";
    overheatSensorFault = false;
    overheatLatched = false;
    lastTemperatureC = 0.0f;
    lastTemperatureValid = false;
    timeOffsetMinutes = 0;
    timeSyncEnabled = true;
    mdnsEnabled = true;
    mdnsHostname = defaultMdnsHostname();
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
    streamEnabled[0] = true;
    streamEnabled[1] = false;
    maxActiveClients = 2;
    streamProfiles[0].target = STREAM_TARGET_BIRDNET_GO;
    streamProfiles[1].target = STREAM_TARGET_BIRDNET_PI;

    saveAudioSettings();

    simplePrintln("Defaults applied. Device will reboot.");
}

// Apply all producer-visible audio settings only after the producer has stopped.
// This prevents a larger requested buffer size from being used with the old,
// smaller allocation during a Web UI reconfiguration.
bool applyAudioConfig(uint32_t newRate, float newGain, uint16_t newBuffer, uint8_t newShift) {
    uint32_t oldRate = currentSampleRate;
    float oldGain = currentGainFactor;
    uint16_t oldBuffer = currentBufferSize;
    uint8_t oldShift = i2sShiftBits;
    uint16_t oldCutoff = highpassCutoffHz;
    uint32_t oldMinRate = minAcceptableRate;

    stopAudioProducer();
    currentSampleRate = newRate;
    currentGainFactor = newGain;
    currentBufferSize = newBuffer;
    i2sShiftBits = newShift;
    highpassCutoffHz = normalizeHighpassCutoff(currentSampleRate, highpassCutoffHz);
    if (autoThresholdEnabled) minAcceptableRate = computeRecommendedMinRate();

    if (restartI2S()) return true;

    simplePrintln("Audio setting rejected: I2S restart failed, rolling back.");
    currentSampleRate = oldRate;
    currentGainFactor = oldGain;
    currentBufferSize = oldBuffer;
    i2sShiftBits = oldShift;
    highpassCutoffHz = oldCutoff;
    minAcceptableRate = oldMinRate;
    if (!restartI2S()) {
        simplePrintln("Audio rollback failed; reboot recommended.");
    }
    return false;
}

// Restart I2S with new parameters
bool restartI2S() {
    simplePrintln("Restarting I2S with new parameters...");
    stopAllRtspClients("I2S restart");
    stopAudioProducer();
    if (i2s_rx_handle) {
        i2s_channel_disable(i2s_rx_handle);
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = nullptr;
    }

    if (i2s_32bit_buffer) { free(i2s_32bit_buffer); i2s_32bit_buffer = nullptr; }
    if (i2s_16bit_buffer) { free(i2s_16bit_buffer); i2s_16bit_buffer = nullptr; }
    if (i2s_16bit_network_buffer) { free(i2s_16bit_network_buffer); i2s_16bit_network_buffer = nullptr; }
    if (rtpPacketScratch) { free(rtpPacketScratch); rtpPacketScratch = nullptr; }

    i2s_32bit_buffer = (int32_t*)malloc(currentBufferSize * sizeof(int32_t));
    i2s_16bit_buffer = (int16_t*)malloc(currentBufferSize * sizeof(int16_t));
    i2s_16bit_network_buffer = (int16_t*)malloc(currentBufferSize * sizeof(int16_t));
    rtpPacketScratch = (uint8_t*)malloc(16 + (size_t)currentBufferSize * sizeof(int16_t));
    if (!i2s_32bit_buffer || !i2s_16bit_buffer || !i2s_16bit_network_buffer || !rtpPacketScratch) {
        simplePrintln("FATAL: Memory allocation failed after parameter change!");
        if (i2s_32bit_buffer) { free(i2s_32bit_buffer); i2s_32bit_buffer = nullptr; }
        if (i2s_16bit_buffer) { free(i2s_16bit_buffer); i2s_16bit_buffer = nullptr; }
        if (i2s_16bit_network_buffer) { free(i2s_16bit_network_buffer); i2s_16bit_network_buffer = nullptr; }
        if (rtpPacketScratch) { free(rtpPacketScratch); rtpPacketScratch = nullptr; }
        return false;
    }

    if (!setup_i2s_driver()) {
        simplePrintln("FATAL: I2S restart failed!");
        return false;
    }
    // Refresh HPF with current parameters
    updateHighpassCoeffs();
    maxPacketRate = 0;
    minPacketRate = 0xFFFFFFFF;
    simplePrintln("I2S restarted successfully");
    return true;
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
    ArduinoOTA.setHostname(mdnsHostname.c_str());
#ifdef OTA_PASSWORD
    ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
    ArduinoOTA.begin();
}

static bool hasActiveRtspStream() {
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].streaming) return true;
    }
    return false;
}

static size_t computeAudioRingBufferCapacity() {
    size_t chunkBytes = (size_t)currentBufferSize * sizeof(int16_t);
    size_t chunkCount = 8;
    if (currentBufferSize > 4096) chunkCount = 3;
    else if (currentBufferSize > 2048) chunkCount = 4;
    size_t capacity = chunkBytes * chunkCount;
    // Keep RTP packets small for UDP compatibility while providing roughly
    // 32 chunks (~341 ms at 48 kHz/512 samples) of internal TCP jitter reserve.
    if (currentBufferSize <= 2048 && capacity < 32768) capacity = 32768;
    if (capacity < 4096) capacity = 4096;
    return capacity;
}

void flushAudioRingBuffer() {
    if (!audioRingBuffer) return;
    size_t itemSize = 0;
    void* item = nullptr;
    uint32_t flushed = 0;
    while ((item = xRingbufferReceive(audioRingBuffer, &itemSize, 0)) != nullptr) {
        vRingbufferReturnItem(audioRingBuffer, item);
        flushed++;
    }
    if (flushed > 0) {
        audioRingBufferFlushCount += flushed;
    }
}

void audioProducerTask(void* /*arg*/) {
    audioProducerRunning = true;

    while (!audioProducerStopRequested) {
        size_t bytesRead = 0;
#if defined(M5STACK_ATOM_VOICES3R)
        esp_err_t result = i2s_channel_read(i2s_rx_handle, i2s_16bit_buffer,
                                            currentBufferSize * sizeof(int16_t),
                                            &bytesRead, 100);
#else
        esp_err_t result = i2s_channel_read(i2s_rx_handle, i2s_32bit_buffer,
                                            currentBufferSize * sizeof(int32_t),
                                            &bytesRead, 100);
#endif
        if (audioProducerStopRequested) break;

        if (result != ESP_OK || bytesRead == 0) {
            audioI2SErrorCount++;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

#if defined(M5STACK_ATOM_VOICES3R)
        int samplesRead = bytesRead / sizeof(int16_t);
#else
        int samplesRead = bytesRead / sizeof(int32_t);
#endif
        if (samplesRead <= 0) continue;

        // If HPF params changed dynamically, recompute in the producer context.
        if (highpassEnabled && (hpfConfigSampleRate != currentSampleRate || hpfConfigCutoff != highpassCutoffHz)) {
            updateHighpassCoeffs();
        }

        bool clipped = false;
        float peakAbs = 0.0f;
        for (int i = 0; i < samplesRead; i++) {
#if defined(M5STACK_ATOM_VOICES3R)
            float sample = (float)i2s_16bit_buffer[i];
#else
            float sample = (float)(i2s_32bit_buffer[i] >> i2sShiftBits);
#endif
            if (highpassEnabled) sample = hpf.process(sample);
            float amplified = sample * currentGainFactor;
            float aabs = fabsf(amplified);
            if (aabs > peakAbs) peakAbs = aabs;
            if (aabs > 32767.0f) clipped = true;
            if (amplified > 32767.0f) amplified = 32767.0f;
            if (amplified < -32768.0f) amplified = -32768.0f;
            i2s_16bit_buffer[i] = (int16_t)amplified;
        }

        if (peakAbs > 32767.0f) peakAbs = 32767.0f;
        lastPeakAbs16 = (uint16_t)peakAbs;
        audioClippedLastBlock = clipped;
        if (clipped) audioClipCount++;

        if (lastPeakAbs16 > peakHoldAbs16) {
            peakHoldAbs16 = lastPeakAbs16;
            peakHoldUntilMs = millis() + 3000UL;
        } else if (peakHoldAbs16 > 0 && millis() > peakHoldUntilMs) {
            peakHoldAbs16 = 0;
        }

        if (!audioRingBuffer || !hasActiveRtspStream()) {
            continue;
        }

        size_t payloadBytes = (size_t)samplesRead * sizeof(int16_t);
        if (xRingbufferSend(audioRingBuffer, i2s_16bit_buffer, payloadBytes, 0) == pdTRUE) {
            audioRingBufferChunkCount++;
        } else {
            audioRingBufferDropCount++;
        }
    }

    audioProducerRunning = false;
    audioProducerTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

void stopAudioProducer() {
    audioProducerStopRequested = true;
    unsigned long startMs = millis();
    while (audioProducerTaskHandle != nullptr && (millis() - startMs) < 600UL) {
        delay(5);
    }
    if (audioProducerTaskHandle != nullptr) {
        vTaskDelete(audioProducerTaskHandle);
        audioProducerTaskHandle = nullptr;
        audioProducerRunning = false;
    }
    if (audioRingBuffer) {
        vRingbufferDelete(audioRingBuffer);
        audioRingBuffer = nullptr;
    }
    audioRingBufferCapacityBytes = 0;
    audioRingBufferFlushPending = false;
    audioProducerStopRequested = false;
}

bool startAudioProducer() {
    stopAudioProducer();

    audioRingBufferCapacityBytes = computeAudioRingBufferCapacity();
    audioRingBuffer = xRingbufferCreate(audioRingBufferCapacityBytes, RINGBUF_TYPE_BYTEBUF);
    if (!audioRingBuffer) {
        simplePrintln("Audio RB alloc failed");
        audioRingBufferCapacityBytes = 0;
        return false;
    }

    audioProducerStopRequested = false;
    BaseType_t rc = xTaskCreate(
        audioProducerTask,
        "audio_prod",
        6144,
        nullptr,
        5,
        &audioProducerTaskHandle
    );
    if (rc != pdPASS) {
        simplePrintln("Audio producer failed");
        vRingbufferDelete(audioRingBuffer);
        audioRingBuffer = nullptr;
        audioRingBufferCapacityBytes = 0;
        return false;
    }

    simplePrintln("Audio producer started");
    return true;
}

// Atom VoiceS3R/C126 uses an ES8311 analog codec in front of I2S. The
// register sequence is the microphone-enable sequence from M5Stack's
// M5Unified implementation for this exact board.
#if defined(M5STACK_ATOM_VOICES3R)
static bool voiceS3RCodecInitialized = false;

static bool writeEs8311Register(uint8_t reg, uint8_t value) {
    Wire1.beginTransmission(ES8311_I2C_ADDRESS);
    Wire1.write(reg);
    Wire1.write(value);
    return Wire1.endTransmission() == 0;
}

static bool setupVoiceS3RCodec() {
    if (voiceS3RCodecInitialized) return true;

    Wire1.begin(ES8311_I2C_SDA_PIN, ES8311_I2C_SCL_PIN, 100000);
    static constexpr uint8_t init[][2] = {
        {0x00, 0x80}, // reset / CSM power on
        {0x01, 0xBA}, // clock manager: MCLK/BCLK relationship
        {0x02, 0x18}, // clock manager: MULT_PRE=3
        {0x0D, 0x01}, // power up analog circuitry
        {0x0E, 0x02}, // enable analog PGA and ADC modulator
        {0x14, 0x15}, // differential microphone input, 15 dB PGA gain
        {0x17, 0xFF}, // ADC digital volume
        {0x1C, 0x6A}, // bypass EQ and cancel DC offset
    };
    for (const auto &entry : init) {
        if (!writeEs8311Register(entry[0], entry[1])) {
            simplePrintln("ES8311 write failed at register 0x" + String(entry[0], HEX));
            return false;
        }
    }
    voiceS3RCodecInitialized = true;
    simplePrintln("ES8311 microphone codec ready on I2C address 0x18");
    return true;
}
#endif

// I2S setup
bool setup_i2s_driver() {
    stopAudioProducer();
    if (i2s_rx_handle) {
        i2s_channel_disable(i2s_rx_handle);
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = nullptr;
    }

    uint16_t dma_buf_len = (currentBufferSize > 512) ? 512 : currentBufferSize;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = dma_buf_len;

    esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &i2s_rx_handle);
    if (err != ESP_OK) {
        simplePrintln("I2S channel create failed: " + String(esp_err_to_name(err)));
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(currentSampleRate),
#if defined(M5STACK_ATOM_VOICES3R)
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
#else
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
#endif
        .gpio_cfg = {
            .mclk = (gpio_num_t)I2S_MCLK_PIN,
            .bclk = (gpio_num_t)I2S_BCLK_PIN,
            .ws = (gpio_num_t)I2S_LRCLK_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = (gpio_num_t)I2S_DOUT_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    // PCM1808 slave mode accepts 256/384/512 fs. Mics without an MCLK input
    // leave D7 unconnected and continue to use the same BCLK/WS/SD wiring.
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

#if defined(M5STACK_ATOM_VOICES3R)
    if (!setupVoiceS3RCodec()) {
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = nullptr;
        return false;
    }
#endif

    err = i2s_channel_init_std_mode(i2s_rx_handle, &std_cfg);
    if (err != ESP_OK) {
        simplePrintln("I2S pin/mode setup failed: " + String(esp_err_to_name(err)));
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = nullptr;
        return false;
    }

    err = i2s_channel_enable(i2s_rx_handle);
    if (err != ESP_OK) {
        simplePrintln("I2S channel enable failed: " + String(esp_err_to_name(err)));
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = nullptr;
        return false;
    }

    simplePrintln("I2S ready: " + String(currentSampleRate) + "Hz, gain " +
                  String(currentGainFactor, 1) + ", buffer " + String(currentBufferSize) +
                  ", shiftBits " + String(i2sShiftBits) + ", pins MCLK/BCLK/WS/SD " +
                  String(I2S_MCLK_PIN) + "/" + String(I2S_BCLK_PIN) + "/" +
                  String(I2S_LRCLK_PIN) + "/" + String(I2S_DOUT_PIN));
    if (!startAudioProducer()) {
        i2s_channel_disable(i2s_rx_handle);
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = nullptr;
        return false;
    }
    return true;
}

// The Wi-Fi/TCP transport pause occurs first; a blocked socket write is its
// consequence, not its origin. Allow brief pauses to recover without forcing
// BirdNET-Go through a much longer FFmpeg reconnect. The old blocking
// NetworkClient::write() could then hold the main loop for about 10 seconds;
// keep the same ten-second tolerance, but implement it explicitly with
// non-blocking sends so its behavior and failure cause remain controlled.
static const uint32_t RTSP_WRITE_TIMEOUT_MS = 10000UL;
// The ring buffer holds roughly 341 ms at the default 48 kHz / 512-sample
// configuration. After a longer recovered stall, discard queued old audio so
// the stream resumes live instead of retaining permanent latency.
static const uint32_t RTSP_BACKLOG_FLUSH_AFTER_MS = 250UL;

// Non-blocking write helper with a bounded retry window for TCP backpressure.
// NetworkClient::write() can wait internally for about 10 seconds in Arduino-ESP32
// 3.3.8, which would stall the main loop and fill the audio ring buffer.
static bool writeAll(WiFiClient &client, const uint8_t* data, size_t len) {
    size_t off = 0;
    bool stalled = false;
    unsigned long startMs = millis();
    while (off < len) {
        int socketFd = client.fd();
        if (!client.connected() || socketFd < 0) {
            return false;
        }

        if ((millis() - startMs) >= RTSP_WRITE_TIMEOUT_MS) {
            rtspWriteTimeoutCount++;
            return false;
        }

        errno = 0;
        int w = send(socketFd, data + off, len - off, MSG_DONTWAIT);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }

        if (w == 0) {
            return false;
        }

        int socketError = errno;
        if (socketError != EAGAIN && socketError != EWOULDBLOCK &&
            socketError != EINTR) {
            return false;
        }

        if (!stalled) {
            rtspWriteStallCount++;
            stalled = true;
        }
        delay(1);
    }

    unsigned long elapsedMs = millis() - startMs;
    if (stalled && elapsedMs >= RTSP_BACKLOG_FLUSH_AFTER_MS) {
        audioRingBufferFlushPending = true;
    }
    return true;
}

static void stopStreamOnWriteFailure(ClientSession &session, const char* reason) {
    uint8_t pi = session.profileIndex;
    session.streaming = false;
    rtspWriteFailCount++;
    lastStreamStopReason = reason;
    lastStreamStopMs = millis();
    session.reset();
    // Flush only after streamAudio() returns its currently borrowed item.
    audioRingBufferFlushPending = true;
    bool anyStillStreaming = false;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].streaming && clients[i].profileIndex == pi) { anyStillStreaming = true; break; }
    }
    if (!anyStillStreaming) streamStats[pi].streaming = false;

    simplePrintln("STREAMING STOPPED: " + lastStreamStopReason + " | client dropped");
    mqttPublishState(true);
}

void sendRTPPacket(ClientSession &session, const int16_t* networkAudioData, int numSamples) {
    const uint16_t payloadSize = (uint16_t)(numSamples * (int)sizeof(int16_t));
    const uint16_t packetSize = (uint16_t)(12 + payloadSize);

    uint8_t header[12];
    header[0] = 0x80;
    header[1] = 96;
    header[2] = (uint8_t)((session.rtpSequence >> 8) & 0xFF);
    header[3] = (uint8_t)(session.rtpSequence & 0xFF);
    header[4] = (uint8_t)((session.rtpTimestamp >> 24) & 0xFF);
    header[5] = (uint8_t)((session.rtpTimestamp >> 16) & 0xFF);
    header[6] = (uint8_t)((session.rtpTimestamp >> 8) & 0xFF);
    header[7] = (uint8_t)(session.rtpTimestamp & 0xFF);
    header[8]  = (uint8_t)((rtpSSRC >> 24) & 0xFF);
    header[9]  = (uint8_t)((rtpSSRC >> 16) & 0xFF);
    header[10] = (uint8_t)((rtpSSRC >> 8) & 0xFF);
    header[11] = (uint8_t)(rtpSSRC & 0xFF);

    bool success = false;
    if (session.transport == TRANSPORT_TCP) {
        if (!session.client.connected()) {
            session.streaming = false;
            return;
        }
        if (!rtpPacketScratch) {
            stopStreamOnWriteFailure(session, "RTP write failed (no scratch buffer)");
            return;
        }
        rtpPacketScratch[0] = 0x24;
        rtpPacketScratch[1] = 0x00;
        rtpPacketScratch[2] = (uint8_t)((packetSize >> 8) & 0xFF);
        rtpPacketScratch[3] = (uint8_t)(packetSize & 0xFF);
        memcpy(rtpPacketScratch + 4, header, sizeof(header));
        memcpy(rtpPacketScratch + 4 + sizeof(header), networkAudioData, payloadSize);
        size_t totalLen = 4 + sizeof(header) + payloadSize;
        if (writeAll(session.client, rtpPacketScratch, totalLen)) {
            success = true;
        } else {
            stopStreamOnWriteFailure(session, "RTP write failed");
            return;
        }
    } else {
        if (session.clientRtpPort == 0) {
            session.streaming = false;
            return;
        }
        session.udpSocket.beginPacket(session.clientRtpAddress, session.clientRtpPort);
        session.udpSocket.write(header, sizeof(header));
        session.udpSocket.write((const uint8_t*)networkAudioData, payloadSize);
        if (session.udpSocket.endPacket()) {
            success = true;
        }
    }
    if (success) {
        session.rtpSequence++;
        session.rtpTimestamp += (uint32_t)numSamples;
        session.packetsSent++;
        session.lastRtpPacketMs = millis();
        lastRtpPacketMs = session.lastRtpPacketMs;
    }
}

static void drainRtcpPackets(ClientSession &session) {
    if (session.transport != TRANSPORT_UDP) return;
    uint8_t buffer[64];
    int packetSize = session.rtcpSocket.parsePacket();
    while (packetSize > 0) {
        while (packetSize > 0) {
            int toRead = packetSize;
            if (toRead > (int)sizeof(buffer)) toRead = sizeof(buffer);
            int readNow = session.rtcpSocket.read(buffer, toRead);
            if (readNow <= 0) break;
            packetSize -= readNow;
        }
        packetSize = session.rtcpSocket.parsePacket();
    }
}

// Audio streaming
void streamAudio() {
    bool anyStreaming = false;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].streaming) { anyStreaming = true; break; }
    }
    if (!anyStreaming) return;
    if (!audioRingBuffer) return;

    uint8_t chunksProcessed = 0;
    while (chunksProcessed < 4) {
        size_t itemSize = 0;
        int16_t* audioChunk = (int16_t*)xRingbufferReceive(audioRingBuffer, &itemSize, 0);
        if (!audioChunk) break;

        int samplesRead = itemSize / sizeof(int16_t);
        if (samplesRead > currentBufferSize) samplesRead = currentBufferSize;

        // Prepare one network-order copy and reuse for all active client sessions.
        for (int i = 0; i < samplesRead; ++i) {
            uint16_t s = (uint16_t)audioChunk[i];
            s = (uint16_t)((s << 8) | (s >> 8));
            i2s_16bit_network_buffer[i] = (int16_t)s;
        }
        bool deliveredProfile[2] = {false, false};
        for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].streaming) continue;
            uint8_t pi = clients[i].profileIndex;
            sendRTPPacket(clients[i], i2s_16bit_network_buffer, samplesRead);
            if (clients[i].streaming) {
                deliveredProfile[pi] = true;
            }
        }
        bool deliveredAny = false;
        for (uint8_t pi = 0; pi < 2; pi++) {
            if (!deliveredProfile[pi]) continue;
            streamStats[pi].packetsSent++;
            deliveredAny = true;
        }
        if (deliveredAny) {
            audioPacketsSent++;
        }
        vRingbufferReturnItem(audioRingBuffer, (void*)audioChunk);
        if (audioRingBufferFlushPending) {
            audioRingBufferFlushPending = false;
            flushAudioRingBuffer();
            break;
        }
        chunksProcessed++;
    }
}

static uint8_t detectProfileFromRequest(const String &request) {
    int lineEnd = request.indexOf("\r\n");
    String firstLine = (lineEnd > 0) ? request.substring(0, lineEnd) : request;
    if (firstLine.indexOf("/audio2") >= 0) return 1;
    return 0;
}

static String rtspHeaderValue(const String &request, const char *headerName) {
    String marker = String(headerName) + ":";
    int start = request.indexOf(marker);
    if (start < 0) return String();
    start += marker.length();
    while (start < (int)request.length() && (request[start] == ' ' || request[start] == '\t')) start++;
    int end = request.indexOf("\r\n", start);
    if (end < 0) end = request.length();
    String value = request.substring(start, end);
    value.trim();
    return value;
}

static bool anyRtspSessionStreaming(uint8_t profileIndex = 255) {
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].streaming) continue;
        if (profileIndex == 255 || clients[i].profileIndex == profileIndex) return true;
    }
    return false;
}

static void parseTransportHeader(ClientSession &session, const String &request, String &transportResponse, uint8_t clientIdx) {
    // Auto-select transport based on target: BirdNET-Go=TCP, BirdNET-Pi=UDP
    bool forceTcp = (streamProfiles[session.profileIndex].target == STREAM_TARGET_BIRDNET_GO);
    bool forceUdp = !forceTcp;

    if (forceUdp) {
        session.transport = TRANSPORT_UDP;
        int transportPos = request.indexOf("Transport:");
        if (transportPos >= 0) {
            String transportLine = request.substring(transportPos);
            int endPos = transportLine.indexOf("\r\n");
            if (endPos > 0) transportLine = transportLine.substring(0, endPos);
            transportLine.toLowerCase();
            int clientPortPos = transportLine.indexOf("client_port=");
            if (clientPortPos >= 0) {
                String portStr = transportLine.substring(clientPortPos + 12);
                int dashPos = portStr.indexOf("-");
                if (dashPos > 0) portStr = portStr.substring(0, dashPos);
                int semiPos = portStr.indexOf(";");
                if (semiPos > 0) portStr = portStr.substring(0, semiPos);
                portStr.trim();
                session.clientRtpPort = (uint16_t)portStr.toInt();
            }
        }
        if (session.client.connected()) session.clientRtpAddress = session.client.remoteIP();
        session.serverRtpPort = (uint16_t)(5004 + (clientIdx * 2));
        session.udpSocket.stop();
        session.rtcpSocket.stop();
        session.udpSocket.begin(session.serverRtpPort);
        session.rtcpSocket.begin(session.serverRtpPort + 1);
        transportResponse = "RTP/AVP/UDP;unicast;client_port=" + String(session.clientRtpPort) + "-" + String(session.clientRtpPort + 1);
        transportResponse += ";server_port=" + String(session.serverRtpPort) + "-" + String(session.serverRtpPort + 1);
        transportResponse += ";source=" + WiFi.localIP().toString();
        transportResponse += ";ssrc=" + String(rtpSSRC, HEX);
    } else {
        session.transport = TRANSPORT_TCP;
        transportResponse = "RTP/AVP/TCP;unicast;interleaved=0-1";
    }
}

// RTSP handling
void handleRTSPCommand(ClientSession &session, String request, uint8_t clientIdx) {
    String cseq = "1";
    int cseqPos = request.indexOf("CSeq: ");
    if (cseqPos >= 0) {
        cseq = request.substring(cseqPos + 6, request.indexOf("\r", cseqPos));
        cseq.trim();
    }
    session.profileIndex = detectProfileFromRequest(request);
    String requestUserAgent = rtspHeaderValue(request, "User-Agent");
    if (requestUserAgent.length()) {
        requestUserAgent.replace("\r", "");
        requestUserAgent.replace("\n", "");
        requestUserAgent.toCharArray(session.userAgent, sizeof(session.userAgent));
    }

    // Reject disabled streams
    if (!streamEnabled[session.profileIndex]) {
        session.client.print("RTSP/1.0 454 Session Not Found\r\n");
        session.client.print("CSeq: " + cseq + "\r\n");
        session.client.print("\r\n");
        simplePrintln("Rejected disabled stream" + String(session.profileIndex + 1) + " from " + session.client.remoteIP().toString());
        return;
    }

    int methodEnd = request.indexOf(' ');
    if (methodEnd <= 0) methodEnd = request.length();
    lastRtspCommand = request.substring(0, methodEnd);
    lastRtspCommandMs = millis();
    lastRTSPActivity = lastRtspCommandMs;

    if (request.startsWith("OPTIONS")) {
        session.client.print("RTSP/1.0 200 OK\r\n");
        session.client.print("CSeq: " + cseq + "\r\n");
        session.client.print("Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n\r\n");

    } else if (request.startsWith("DESCRIBE")) {
        String ip = WiFi.localIP().toString();
        String streamPath = (session.profileIndex == 1) ? "/audio2" : "/audio1";
        String sdp = "v=0\r\n";
        sdp += "o=- 0 0 IN IP4 " + ip + "\r\n";
        sdp += "s=ESP32 RTSP Mic " + streamPath + " (" + String(currentSampleRate) + "Hz, 16-bit PCM)\r\n";
        sdp += "c=IN IP4 " + ip + "\r\n";
        sdp += "t=0 0\r\n";
        sdp += "m=audio 0 RTP/AVP 96\r\n";
        sdp += "a=rtpmap:96 L16/" + String(currentSampleRate) + "/1\r\n";
        sdp += "a=control:track1\r\n";

        session.client.print("RTSP/1.0 200 OK\r\n");
        session.client.print("CSeq: " + cseq + "\r\n");
        session.client.print("Content-Type: application/sdp\r\n");
        session.client.print("Content-Base: rtsp://" + ip + ":8554" + streamPath + "/\r\n");
        session.client.print("Content-Length: " + String(sdp.length()) + "\r\n\r\n");
        session.client.print(sdp);

    } else if (request.startsWith("SETUP")) {
        session.sessionId = String(random(100000000, 999999999));
        rtspSessionId = session.sessionId;
        String transportResponse;
        parseTransportHeader(session, request, transportResponse, clientIdx);
        session.client.print("RTSP/1.0 200 OK\r\n");
        session.client.print("CSeq: " + cseq + "\r\n");
        session.client.print("Session: " + session.sessionId + "\r\n");
        session.client.print("Transport: " + transportResponse + "\r\n\r\n");

    } else if (request.startsWith("PLAY")) {
        String ip = WiFi.localIP().toString();
        String streamPath = (session.profileIndex == 1) ? "/audio2" : "/audio1";
        session.client.print("RTSP/1.0 200 OK\r\n");
        session.client.print("CSeq: " + cseq + "\r\n");
        session.client.print("Session: " + session.sessionId + "\r\n");
        session.client.print("Range: npt=0.000-\r\n");
        session.client.print("RTP-Info: url=rtsp://" + ip + ":8554" + streamPath + "/track1;seq=0;rtptime=0\r\n\r\n");

        bool wasAnyStreaming = anyRtspSessionStreaming();
        bool wasProfileStreaming = anyRtspSessionStreaming(session.profileIndex);
        if (!wasAnyStreaming) {
            flushAudioRingBuffer();
        }
        session.streaming = true;
        session.rtpSequence = 0;
        session.rtpTimestamp = 0;
        session.packetsSent = 0;
        session.playStartedAtMs = millis();
        session.lastRtpPacketMs = session.playStartedAtMs;
        if (!wasAnyStreaming) {
            audioPacketsSent = 0;
            lastStatsReset = millis();
            streamStartedAtMs = millis();
        }
        lastRtspPlayMs = millis();
        streamStats[session.profileIndex].streaming = true;
        streamStats[session.profileIndex].lastPlayMs = millis();
        if (!wasProfileStreaming) {
            streamStats[session.profileIndex].packetsSent = 0;
            streamStats[session.profileIndex].statsResetMs = millis();
        }
        rtspPlayCount++;
        lastRtpPacketMs = millis();
        lastStreamStopReason = "none";
        lastStreamStopMs = 0;
        simplePrintln("STREAMING STARTED stream" + String(session.profileIndex + 1));
        mqttPublishState(true);

    } else if (request.startsWith("TEARDOWN")) {
        unsigned long now = millis();
        IPAddress remoteIp = session.client.remoteIP();
        unsigned long sessionAgeMs = session.connectedAtMs ? now - session.connectedAtMs : 0;
        unsigned long streamAgeMs = session.playStartedAtMs ? now - session.playStartedAtMs : 0;
        unsigned long lastRtpAgeMs = session.lastRtpPacketMs ? now - session.lastRtpPacketMs : 0;
        String teardownLog;
        teardownLog.reserve(260);
        teardownLog += F("RTSP TEARDOWN: client=");
        teardownLog += remoteIp.toString();
        teardownLog += F(" slot=");
        teardownLog += String(clientIdx);
        teardownLog += F(" stream=");
        teardownLog += String(session.profileIndex + 1);
        teardownLog += F(" transport=");
        teardownLog += session.transport == TRANSPORT_UDP ? F("UDP") : F("TCP");
        teardownLog += F(" cseq=");
        teardownLog += cseq;
        teardownLog += F(" session_age_ms=");
        teardownLog += String(sessionAgeMs);
        teardownLog += F(" stream_age_ms=");
        teardownLog += String(streamAgeMs);
        teardownLog += F(" packets=");
        teardownLog += String(session.packetsSent);
        teardownLog += F(" last_rtp_ms=");
        teardownLog += String(lastRtpAgeMs);
        teardownLog += F(" rssi=");
        teardownLog += String(WiFi.RSSI());
        teardownLog += F("dBm user_agent=");
        teardownLog += session.userAgent[0] ? session.userAgent : "unknown";
        session.client.print("RTSP/1.0 200 OK\r\n");
        session.client.print("CSeq: " + cseq + "\r\n");
        session.client.print("Session: " + session.sessionId + "\r\n\r\n");
        session.streaming = false;
        bool anyStillStreaming = false;
        for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
            if (i != clientIdx && clients[i].streaming && clients[i].profileIndex == session.profileIndex) {
                anyStillStreaming = true; break;
            }
        }
        if (!anyStillStreaming) streamStats[session.profileIndex].streaming = false;
        lastStreamStopReason = "RTSP TEARDOWN";
        lastStreamStopMs = millis();
        simplePrintln(teardownLog);
        simplePrintln("STREAMING STOPPED (" + lastStreamStopReason + ")");
        mqttPublishState(true);
    } else if (request.startsWith("GET_PARAMETER")) {
        session.client.print("RTSP/1.0 200 OK\r\n");
        session.client.print("CSeq: " + cseq + "\r\n\r\n");
    } else {
        session.client.print("RTSP/1.0 501 Not Implemented\r\n");
        session.client.print("CSeq: " + cseq + "\r\n\r\n");
        simplePrintln("RTSP unsupported command: " + lastRtspCommand);
    }
}

// RTSP processing
void processRTSP(ClientSession &session, uint8_t clientIdx) {
    if (!session.client.connected()) return;
    while (session.client.available()) {
        int available = session.client.available();
        if (session.parseBufferPos + available >= (int)sizeof(session.parseBuffer)) {
            available = sizeof(session.parseBuffer) - session.parseBufferPos - 1;
            if (available <= 0) {
                simplePrintln("RTSP buffer overflow - resetting");
                session.parseBufferPos = 0;
                return;
            }
        }
        int bytesRead = session.client.read(session.parseBuffer + session.parseBufferPos, available);
        if (bytesRead <= 0) return;
        session.parseBufferPos += bytesRead;
        session.parseBuffer[session.parseBufferPos] = '\0';

        while (true) {
            char* endOfHeader = strstr((char*)session.parseBuffer, "\r\n\r\n");
            if (endOfHeader == nullptr) break;
            *endOfHeader = '\0';
            String request = String((char*)session.parseBuffer);

            handleRTSPCommand(session, request, clientIdx);

            int headerLen = (endOfHeader - (char*)session.parseBuffer) + 4;
            if (headerLen > session.parseBufferPos) {
                session.parseBufferPos = 0;
                break;
            }
            int remaining = session.parseBufferPos - headerLen;
            if (remaining > 0) {
                memmove(session.parseBuffer, session.parseBuffer + headerLen, remaining);
            }
            session.parseBufferPos = remaining;
            session.parseBuffer[session.parseBufferPos] = '\0';
        }
    }
}

static int findFreeClientSlot() {
    for (uint8_t i = 0; i < maxActiveClients; i++) {
        if (!clients[i].client.connected()) return (int)i;
    }
    return -1;
}

void getStreamClientCounts(uint8_t &s1, uint8_t &s2) {
    s1 = 0; s2 = 0;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].client.connected()) {
            if (clients[i].profileIndex == 1) s2++; else s1++;
        }
    }
}

uint8_t getRtspClientCount() {
    uint8_t count = 0;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].client.connected()) count++;
    }
    return count;
}

String getRtspClientSummary() {
    String summary;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].client.connected()) continue;
        if (summary.length()) summary += ",";
        summary += clients[i].client.remoteIP().toString();
        summary += "/audio";
        summary += String(clients[i].profileIndex + 1);
    }
    return summary;
}

void stopRtspClientsForStream(uint8_t profileIndex, const char* reason) {
    if (profileIndex >= 2) return;
    bool hadStreaming = false;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].profileIndex != profileIndex) continue;
        if (clients[i].streaming) hadStreaming = true;
        clients[i].reset();
    }
    streamStats[profileIndex].streaming = false;
    streamStats[profileIndex].clientCount = 0;
    isStreaming = anyRtspSessionStreaming();
    if (!isStreaming) {
        rtspClient = WiFiClient();
        rtspSessionId = "";
    }
    if (reason && reason[0]) {
        lastStreamStopReason = reason;
        if (hadStreaming) lastStreamStopMs = millis();
    }
}

void stopAllRtspClients(const char* reason) {
    bool hadStreaming = false;
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].streaming) hadStreaming = true;
        clients[i].reset();
    }
    for (uint8_t i = 0; i < 2; i++) {
        streamStats[i].streaming = false;
        streamStats[i].clientCount = 0;
    }
    rtspClient = WiFiClient();
    rtspSessionId = "";
    isStreaming = false;
    if (reason && reason[0]) {
        lastStreamStopReason = reason;
        if (hadStreaming) lastStreamStopMs = millis();
    }
}

static void refreshLegacyRtspState() {
    isStreaming = false;
    rtspClient = WiFiClient();
    rtspSessionId = "";
    for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].client.connected() && !rtspClient.connected()) {
            rtspClient = clients[i].client;
            lastRtspClientIp = rtspClient.remoteIP().toString();
        }
        if (clients[i].streaming) {
            isStreaming = true;
            if (rtspSessionId.length() == 0) rtspSessionId = clients[i].sessionId;
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

#if XIAO_HAS_RF_SWITCH
    // Enable external antenna path on XIAO ESP32-C6.
    pinMode(3, OUTPUT);
    digitalWrite(3, LOW);
    Serial.println("RF switch control enabled (GPIO3 LOW)");
    pinMode(14, OUTPUT);
    digitalWrite(14, HIGH);
    Serial.println("External antenna selected (GPIO14 HIGH)");
#else
    Serial.print("Board profile: ");
    Serial.print(FW_BOARD_NAME_STR);
    Serial.println(" (no firmware-controlled RF switch)");
#endif

    // Load settings from flash
    loadAudioSettings();

    // Allocate buffers with current size
    i2s_32bit_buffer = (int32_t*)malloc(currentBufferSize * sizeof(int32_t));
    i2s_16bit_buffer = (int16_t*)malloc(currentBufferSize * sizeof(int16_t));
    i2s_16bit_network_buffer = (int16_t*)malloc(currentBufferSize * sizeof(int16_t));
    rtpPacketScratch = (uint8_t*)malloc(16 + (size_t)currentBufferSize * sizeof(int16_t));
    if (!i2s_32bit_buffer || !i2s_16bit_buffer || !i2s_16bit_network_buffer || !rtpPacketScratch) {
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
    logConnectedAp("initial");
    clearStoredBssidPin();

    // Apply configured WiFi TX power after connect (logs once on change)
    applyWifiTxPower(true);

    // Attempt initial NTP sync (non-blocking). Timezone/offset is configured in loadAudioSettings().
    if (timeSyncEnabled) {
        attemptTimeSync(false);
    }
    applyMdnsSetting();
    logDeepSleepWakeSnapshotIfAny();

    setupOTA();
    if (!setup_i2s_driver()) {
        simplePrintln("FATAL: I2S setup failed!");
        ESP.restart();
    }
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
        simplePrintln("RTSP URL1 (IP): rtsp://" + WiFi.localIP().toString() + ":8554/audio1");
        simplePrintln("RTSP URL2 (IP): rtsp://" + WiFi.localIP().toString() + ":8554/audio2");
        if (mdnsEnabled) {
            simplePrintln("RTSP URL1 (mDNS): rtsp://" + mdnsHostname + ".local:8554/audio1");
            simplePrintln("RTSP URL2 (mDNS): rtsp://" + mdnsHostname + ".local:8554/audio2");
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

    if (millis() - lastWebuiHandleMs >= WEBUI_HANDLE_INTERVAL_MS) {
        webui_handleClient();
        lastWebuiHandleMs = millis();
    }

    if (millis() - lastTempCheck > TEMPERATURE_CHECK_INTERVAL_MS) {
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

    // RTSP client management (configurable 1-3 concurrent sessions)
    if (rtspServerEnabled) {
        for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].client && !clients[i].client.connected()) {
                bool wasStreaming = clients[i].streaming;
                clients[i].reset();
                if (wasStreaming) {
                    lastStreamStopReason = "TCP client disconnected";
                    lastStreamStopMs = millis();
                    mqttPublishState(true);
                }
            }
            if (clients[i].client.connected() && !clients[i].streaming) {
                if (millis() - clients[i].lastActivity > 30000UL) {
                    clients[i].reset();
                }
            }
        }

        WiFiClient newClient = rtspServer.accept();
        if (newClient) {
            int slot = findFreeClientSlot();
            if (slot >= 0) {
                clients[slot].client = newClient;
                clients[slot].client.setNoDelay(true);
                clients[slot].lastActivity = millis();
                clients[slot].connectedAtMs = clients[slot].lastActivity;
                clients[slot].playStartedAtMs = 0;
                clients[slot].lastRtpPacketMs = 0;
                clients[slot].userAgent[0] = '\0';
                clients[slot].parseBufferPos = 0;
                lastRtspClientConnectMs = millis();
                rtspConnectCount++;
                simplePrintln("New RTSP client connected in slot " + String(slot) + " from: " + newClient.remoteIP().toString());
                mqttPublishState(true);
            } else {
                newClient.stop();
                simplePrintln("RTSP client rejected: max sessions reached");
            }
        }

        for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].client.connected()) {
                drainRtcpPackets(clients[i]);
                if (clients[i].client.available()) {
                    clients[i].lastActivity = millis();
                    lastRTSPActivity = clients[i].lastActivity;
                }
                processRTSP(clients[i], i);
            }
        }
        streamAudio();
    } else {
        stopAllRtspClients("RTSP server disabled");
    }
    refreshLegacyRtspState();
    // Handle deferred WiFi reconnect
    if (wifiReconnectAt != 0 && millis() >= wifiReconnectAt) {
        wifiReconnectAt = 0;

        bool wasStreaming = isStreaming;
        stopAllRtspClients("WiFi reconnect requested");
        refreshLegacyRtspState();
        if (wasStreaming) mqttPublishState(true);

        String ssid = WiFi.SSID();
        String pass = WiFi.psk();
        if (ssid.length() == 0) {
            simplePrintln("WiFi reconnect aborted: no stored SSID");
        } else {
            String fromBssid = (WiFi.status() == WL_CONNECTED) ? WiFi.BSSIDstr() : String("none");
            const char *passArg = pass.length() ? pass.c_str() : nullptr;

            if (wifiReconnectHasBssid) {
                simplePrintln("WiFi reconnect: from " + fromBssid +
                              " -> pinning " + bssidBytesToStr(wifiReconnectBssid));
                WiFi.persistent(false);
                WiFi.disconnect(false);
                delay(WIFI_RECONNECT_SETTLE_MS);
                WiFi.begin(ssid.c_str(), passArg, 0, wifiReconnectBssid, true);
                WiFi.persistent(true);
            } else {
                simplePrintln("WiFi reconnect: from " + fromBssid + " -> supplicant choice");
                clearStoredBssidPin();
                WiFi.disconnect(false);
                delay(WIFI_RECONNECT_SETTLE_MS);
                WiFi.begin(ssid.c_str(), passArg);
            }

            unsigned long waitStart = millis();
            while (WiFi.status() != WL_CONNECTED &&
                   (millis() - waitStart) < WIFI_RECONNECT_TIMEOUT_MS) {
                delay(WIFI_RECONNECT_POLL_MS);
            }

            if (WiFi.status() == WL_CONNECTED) {
                wifiReconnectCount++;
                clearStoredBssidPin();
                logConnectedAp("after-reconnect");
                applyMdnsSetting();
                if (timeSyncEnabled) {
                    attemptTimeSync(false);
                }
                mqttRequestReconnect(true);
                mqttPublishState(true);
            } else {
                simplePrintln("WiFi reconnect: still not associated after " +
                              String(WIFI_RECONNECT_TIMEOUT_MS / 1000UL) + "s");
            }
        }

        wifiReconnectHasBssid = false;
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
