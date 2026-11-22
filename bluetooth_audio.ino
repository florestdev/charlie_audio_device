#include "AudioTools.h"
#include "BluetoothA2DPSink.h"

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

// ==========================================
// CONFIGURATION
// ==========================================

#define AMP_SD   33
#define I2S_BCLK 27
#define I2S_LRC  26
#define I2S_DIN  25

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1

#define MY_SDA 19
#define MY_SCL 18

#define OLED_ADDRESS 0x3C

#define WIFI_TIMEOUT_MS 15000
#define REPORT_INTERVAL 1000

// ==========================================
// NETWORK
// ==========================================

static const char WIFI_SSID[] = "florestdev";
static const char WIFI_PASSWORD[] = "florestdev-victoria123";

static const char NTP_SERVER[] = "pool.ntp.org";

static constexpr long GMT_OFFSET = 10800;
static constexpr int DST_OFFSET = 0;

// ==========================================
// AUDIO
// ==========================================

I2SStream i2s;
BluetoothA2DPSink a2dp_sink(i2s);

// ==========================================
// DISPLAY
// ==========================================

Adafruit_SSD1306 display(
    SCREEN_WIDTH,
    SCREEN_HEIGHT,
    &Wire,
    OLED_RESET
);

// Fixed-size screen buffers
static char currentTitle[32]  = "florestdev";
static char currentArtist[32] = "";
static char currentBottom[96] = "Initializing...";

// Shared screen state
static portMUX_TYPE screenMux = portMUX_INITIALIZER_UNLOCKED;

static bool screenDirty = true;

// ==========================================
// PLAYBACK POSITION
// ==========================================

static uint32_t playbackPositionMs = 0;
static uint32_t playbackDurationMs = 0;
static uint32_t playbackLastTick = 0;
static bool playbackPlaying = false;

static portMUX_TYPE playbackMux = portMUX_INITIALIZER_UNLOCKED;

// ==========================================
// AUDIO DIAGNOSTICS / FLOW DETECTION
// ==========================================

static uint32_t audioPackets = 0;
static uint32_t audioBytes = 0;

static uint32_t lastPackets = 0;
static uint32_t lastBytes = 0;

static uint32_t lastReport = 0;

// Track whether audio is actually flowing
static bool audioFlowing = false;
static uint32_t lastAudioFlowCheck = 0;

// ==========================================
// SYSTEM STATE
// ==========================================

static bool wifiStarted = false;
static bool wifiFinished = false;
static bool ntpStarted = false;

static uint32_t wifiStartTime = 0;
static uint32_t lastScreenUpdate = 0;

static time_t lastDisplayedSecond = 0;

// ==========================================
// SCREEN STATE
// ==========================================

void setText(const char* title, const char* artist, const char* bottom)
{
    portENTER_CRITICAL(&screenMux);

    snprintf(currentTitle,  sizeof(currentTitle),  "%s", title  ? title  : "");
    snprintf(currentArtist, sizeof(currentArtist), "%s", artist ? artist : "");
    snprintf(currentBottom, sizeof(currentBottom), "%s", bottom ? bottom : "");

    screenDirty = true;

    portEXIT_CRITICAL(&screenMux);
}

// ==========================================
// PLAYBACK POSITION HELPERS
// ==========================================

static void resetPlayback()
{
    portENTER_CRITICAL(&playbackMux);
    playbackPositionMs = 0;
    playbackDurationMs = 0;
    playbackLastTick = millis();
    playbackPlaying = false;
    portEXIT_CRITICAL(&playbackMux);
}

// durationMs == 0 -> unknown duration
// Resets position and restarts the timer from zero.
static void setPlaybackDuration(uint32_t durationMs)
{
    portENTER_CRITICAL(&playbackMux);
    playbackDurationMs = durationMs;
    playbackPositionMs = 0;
    playbackLastTick = millis();
    playbackPlaying = true;
    portEXIT_CRITICAL(&playbackMux);
}

static void updatePlaybackPosition()
{
    portENTER_CRITICAL(&playbackMux);

    if (playbackPlaying)
    {
        const uint32_t now = millis();
        const uint32_t elapsed = now - playbackLastTick;

        playbackPositionMs += elapsed;
        playbackLastTick = now;

        if (playbackDurationMs > 0 && playbackPositionMs >= playbackDurationMs)
        {
            playbackPositionMs = playbackDurationMs;
            playbackPlaying = false;
        }
    }
    else
    {
        playbackLastTick = millis();
    }

    portEXIT_CRITICAL(&playbackMux);
}

static void pausePlayback()
{
    portENTER_CRITICAL(&playbackMux);
    playbackPlaying = false;
    portEXIT_CRITICAL(&playbackMux);
}

static void resumePlayback()
{
    portENTER_CRITICAL(&playbackMux);
    if (playbackDurationMs > 0 && playbackPositionMs < playbackDurationMs)
    {
        playbackLastTick = millis();
        playbackPlaying = true;
    }
    portEXIT_CRITICAL(&playbackMux);
}

static void getPlaybackPosition(uint32_t* positionMs, uint32_t* durationMs)
{
    portENTER_CRITICAL(&playbackMux);
    *positionMs = playbackPositionMs;
    *durationMs = playbackDurationMs;
    portEXIT_CRITICAL(&playbackMux);
}

static void formatPlaybackTime(char* buffer, size_t size, uint32_t ms)
{
    const uint32_t totalSeconds = ms / 1000;
    const uint32_t minutes = totalSeconds / 60;
    const uint32_t seconds = totalSeconds % 60;

    snprintf(buffer, size, "%02lu:%02lu",
             (unsigned long)minutes, (unsigned long)seconds);
}

// ==========================================
// DATE AND TIME
// ==========================================

static void getDateTime(
    char* dateBuffer, size_t dateSize,
    char* timeBuffer, size_t timeSize)
{
    const time_t now = time(nullptr);
    struct tm timeInfo;

    if (now > 1700000000 && localtime_r(&now, &timeInfo))
    {
        strftime(dateBuffer, dateSize, "%d.%m.%Y", &timeInfo);
        strftime(timeBuffer, timeSize, "%H:%M:%S", &timeInfo);
    }
    else
    {
        snprintf(dateBuffer, dateSize, "--.--.----");
        snprintf(timeBuffer, timeSize, "--:--:--");
    }
}

// ==========================================
// OLED UPDATE
// ==========================================

static void updateDisplay()
{
    char title[sizeof(currentTitle)];
    char artist[sizeof(currentArtist)];
    char bottom[sizeof(currentBottom)];

    portENTER_CRITICAL(&screenMux);
    memcpy(title,  currentTitle,  sizeof(title));
    memcpy(artist, currentArtist, sizeof(artist));
    memcpy(bottom, currentBottom, sizeof(bottom));
    screenDirty = false;
    portEXIT_CRITICAL(&screenMux);

    char dateBuffer[16];
    char timeBuffer[16];

    getDateTime(dateBuffer, sizeof(dateBuffer), timeBuffer, sizeof(timeBuffer));

    uint32_t positionMs = 0;
    uint32_t durationMs = 0;

    getPlaybackPosition(&positionMs, &durationMs);

    char positionText[16];
    char durationText[16];

    formatPlaybackTime(positionText, sizeof(positionText), positionMs);

    if (durationMs > 0)
        formatPlaybackTime(durationText, sizeof(durationText), durationMs);
    else
        snprintf(durationText, sizeof(durationText), "--:--");

    char playbackLine[40];
    snprintf(playbackLine, sizeof(playbackLine), "%s/%s",
             positionText, durationText);

    display.clearDisplay();

    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);

    // Track title
    display.setCursor(0, 0);
    display.println(title);

    // Artist / composer
    display.setCursor(0, 10);
    display.println(artist);

    // Status (connected / ready / etc.)
    display.setCursor(0, 20);
    display.println(bottom);

    display.drawLine(0, 30, 127, 30, SSD1306_WHITE);

    // Playback position / duration
    display.setCursor(0, 34);
    display.println(playbackLine);

    // Date
    display.setCursor(0, 44);
    display.print("DATE: ");
    display.println(dateBuffer);

    // Time
    display.setCursor(0, 54);
    display.print("TIME: ");
    display.println(timeBuffer);

    display.display();
}

// ==========================================
// AUDIO CALLBACK
// ==========================================

void audio_callback(const uint8_t* data, uint32_t length)
{
    (void)data;

    __atomic_fetch_add(&audioPackets, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&audioBytes, length, __ATOMIC_RELAXED);
}

// ==========================================
// AVRCP POSITION CALLBACK
// ==========================================

void avrc_rn_play_pos_callback(uint32_t play_pos)
{
    // play_pos приходит в миллисекундах
    portENTER_CRITICAL(&playbackMux);
    playbackPositionMs = play_pos;
    playbackLastTick = millis();
    portEXIT_CRITICAL(&playbackMux);
}

// ==========================================
// BLUETOOTH METADATA
// ==========================================

void avrc_metadata_callback(uint8_t id, const uint8_t* text)
{
    switch (id)
    {
        case ESP_AVRC_MD_ATTR_TITLE:
            if (text == nullptr)
                return;
            setText(reinterpret_cast<const char*>(text),
                    currentArtist,
                    currentBottom);
            break;

        case ESP_AVRC_MD_ATTR_ARTIST:
            if (text == nullptr)
                return;
            setText(currentTitle,
                    reinterpret_cast<const char*>(text),
                    currentBottom);
            break;

        case ESP_AVRC_MD_ATTR_PLAYING_TIME:
        {
            if (text == nullptr)
                return;

            uint32_t raw = (uint32_t)strtoul(
                reinterpret_cast<const char*>(text),
                nullptr, 10);

            // AVRCP spec says milliseconds, but some phones send seconds.
            // Heuristic: values < 100000 are almost certainly seconds.
            if (raw > 0 && raw < 100000)
                raw *= 1000;

            setPlaybackDuration(raw);
            break;
        }

        default:
            break;
    }
}

// ==========================================
// BLUETOOTH CONNECTION
// ==========================================

void connection_state_changed(
    esp_a2d_connection_state_t state,
    void* ptr)
{
    (void)ptr;

    switch (state)
    {
        case ESP_A2D_CONNECTION_STATE_CONNECTED:
            setText("florestdev", "", "connected");
            break;

        case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
            setText("florestdev", "", "disconnected");
            resetPlayback();
            break;

        default:
            break;
    }
}

// ==========================================
// WIFI MANAGEMENT
// ==========================================

static void updateWiFi()
{
    if (wifiFinished)
        return;

    if (!wifiStarted)
    {
        wifiStarted = true;
        wifiStartTime = millis();

        WiFi.mode(WIFI_STA);
        WiFi.setHostname("florestdev-audio");
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

        Serial.println("[WIFI] Connecting...");
        return;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        if (!ntpStarted)
        {
            ntpStarted = true;

            configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER);

            Serial.print("[WIFI] IP: ");
            Serial.println(WiFi.localIP());
            Serial.println("[NTP] Synchronization started");
        }

        const time_t now = time(nullptr);

        if (now > 1700000000)
        {
            Serial.println("[NTP] Time synchronized");

            setText("florestdev", "", "Internet ready!");

            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);

            wifiFinished = true;
            return;
        }
    }

    if (millis() - wifiStartTime >= WIFI_TIMEOUT_MS)
    {
        Serial.println("[WIFI] Timeout. Disabling Wi-Fi.");

        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);

        wifiFinished = true;
    }
}

// ==========================================
// AUDIO FLOW DETECTION + REPORT
// ==========================================

static void updateAudioDiagnostics()
{
    const uint32_t now = millis();

    // Detect real audio flow every ~200 ms
    if (now - lastAudioFlowCheck >= 200)
    {
        lastAudioFlowCheck = now;

        const uint32_t packets = __atomic_load_n(
            &audioPackets, __ATOMIC_RELAXED);

        const bool flowing = (packets != lastPackets);

        if (flowing != audioFlowing)
        {
            audioFlowing = flowing;

            if (flowing)
                resumePlayback();
            else
                pausePlayback();
        }
    }

    if (now - lastReport < REPORT_INTERVAL)
        return;

    lastReport = now;

    const uint32_t packets = __atomic_load_n(
        &audioPackets, __ATOMIC_RELAXED);
    const uint32_t bytes = __atomic_load_n(
        &audioBytes, __ATOMIC_RELAXED);

    const uint32_t newPackets = packets - lastPackets;
    const uint32_t newBytes = bytes - lastBytes;

    lastPackets = packets;
    lastBytes = bytes;

    Serial.printf(
        "[AUDIO] Packets: %lu | Bytes: %lu | %s\n",
        (unsigned long)packets,
        (unsigned long)bytes,
        newPackets ? "ACTIVE" : "NO DATA");
}

// ==========================================
// DISPLAY SCHEDULER
// ==========================================

static void updateScreenTask()
{
    const uint32_t now = millis();

    if (now - lastScreenUpdate < 100)
        return;

    bool dirty;

    portENTER_CRITICAL(&screenMux);
    dirty = screenDirty;
    portEXIT_CRITICAL(&screenMux);

    const time_t currentSecond = time(nullptr);

    const bool clockChanged =
        currentSecond > 0 &&
        currentSecond != lastDisplayedSecond;

    uint32_t positionMs = 0;
    uint32_t durationMs = 0;

    getPlaybackPosition(&positionMs, &durationMs);

    const bool playbackChanged =
        durationMs > 0 || positionMs > 0;

    if (!dirty && !clockChanged && !playbackChanged)
        return;

    lastScreenUpdate = now;
    lastDisplayedSecond = currentSecond;

    updateDisplay();
}

// ==========================================
// SETUP
// ==========================================

void setup()
{
    Serial.begin(115200);

    Serial.println();
    Serial.println("================================");
    Serial.println("     FlorestAudio");
    Serial.println("================================");

    Wire.begin(MY_SDA, MY_SCL);

    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS))
    {
        Serial.println("[ERROR] OLED initialization failed");
    }
    else
    {
        display.clearDisplay();
        display.display();
    }

    setText("florestdev", "", "Initializing...");

    resetPlayback();

    updateWiFi();

    pinMode(AMP_SD, OUTPUT);
    digitalWrite(AMP_SD, HIGH);

    delay(100);

    auto cfg = i2s.defaultConfig(TX_MODE);

    cfg.pin_bck = I2S_BCLK;
    cfg.pin_ws = I2S_LRC;
    cfg.pin_data = I2S_DIN;

    cfg.sample_rate = 44100;
    cfg.bits_per_sample = 16;
    cfg.channels = 2;
    cfg.i2s_format = I2S_STD_FORMAT;

    if (!i2s.begin(cfg))
    {
        Serial.println("[ERROR] I2S initialization failed");
        return;
    }

    Serial.println("[OK] I2S initialized");

    a2dp_sink.set_volume(100);

    a2dp_sink.set_stream_reader(audio_callback, true);

    // Включаем запрос нужных метаданных
    a2dp_sink.set_avrc_metadata_attribute_mask(
        ESP_AVRC_MD_ATTR_TITLE |
        ESP_AVRC_MD_ATTR_ARTIST |
        ESP_AVRC_MD_ATTR_PLAYING_TIME
    );

    a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);

    // Позиция воспроизведения из уведомлений AVRCP
    a2dp_sink.set_avrc_rn_play_pos_callback(avrc_rn_play_pos_callback);

    a2dp_sink.set_on_connection_state_changed(connection_state_changed);

    a2dp_sink.start("Charlie (florestdev)");

    Serial.println("[OK] Bluetooth started");
    Serial.println("[INFO] Waiting for phone");

    setText("florestdev", "", "Ready!");
}

// ==========================================
// MAIN LOOP
// ==========================================

void loop()
{
    updateWiFi();

    updatePlaybackPosition();

    updateScreenTask();

    updateAudioDiagnostics();

    delay(10);
}
