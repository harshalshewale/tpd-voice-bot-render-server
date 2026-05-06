/*
 * ESP32-S3 Voice Bot Client (Cloud Backend, ESP32 Core 3.1+ compatible)
 * =====================================================================
 * Uses the NEW I2S driver (driver/i2s_std.h) so it coexists with the latest
 * ESP32-audioI2S library on ESP32 Arduino core 3.1+.
 *
 * BEHAVIOR:
 *   - Press BOOT button (GPIO 0)  -> agent ON, LED on, listens
 *   - Speak naturally; VAD captures speech, sends to cloud server
 *   - Server replies with MP3 streamed back through speaker
 *   - Press BOOT again -> agent OFF (also barge-in during playback)
 *   - 5 minutes silence -> agent auto-OFF
 *
 * HARDWARE:
 *   INMP441 (mic):    VDD->3V3, GND->GND, L/R->GND
 *                     WS->GPIO39, SCK->GPIO40, SD->GPIO41
 *   MAX98357A (amp):  VIN->5V, GND->GND, SD->3V3 (must be HIGH!)
 *                     LRC->GPIO21, BCLK->GPIO48, DIN->GPIO47
 *                     Speaker on + and - terminals
 *   BOOT BUTTON:      Already on board (GPIO 0). No external wiring needed.
 *
 * LIBRARIES (Arduino IDE Library Manager):
 *   ESP32-audioI2S  (latest)  by schreibfaul1
 *
 * BOARD SETTINGS:
 *   Board: ESP32S3 Dev Module
 *   PSRAM: OPI PSRAM   (REQUIRED)
 *   Partition Scheme: Huge APP (3MB No OTA / 1MB SPIFFS)
 *   USB CDC On Boot: Enabled
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include "driver/i2s_std.h"   // NEW I2S driver (ESP32 core 3.1+)
#include "Audio.h"

// ============================================================
// CONFIGURATION — EDIT THESE
// ============================================================
const char* WIFI_SSID     = "EW20240310_1";
const char* WIFI_PASSWORD = "Aj123456";

// Your Render service URL (no protocol, no trailing slash)
const char* SERVER_HOST   = "tpd-voice-bot-render-server.onrender.com";
const bool  USE_HTTPS     = true;

// ============================================================
// PINS
// ============================================================
#define BUTTON_PIN    0       // BOOT button on ESP32-S3
#define LED_PIN       2

// INMP441 microphone
#define I2S_MIC_WS    39
#define I2S_MIC_SCK   40
#define I2S_MIC_SD    41

// MAX98357A amp — used by Audio library
#define I2S_SPK_LRC   21
#define I2S_SPK_BCLK  48
#define I2S_SPK_DIN   47

// ============================================================
// AUDIO + VAD
// ============================================================
#define SAMPLE_RATE         16000
#define MAX_RECORD_SECS     20
#define MAX_SAMPLES         (SAMPLE_RATE * MAX_RECORD_SECS)
#define MIC_CHUNK           512
#define VAD_THRESHOLD       1500
#define SPEECH_START_MS     150
#define SPEECH_END_MS       1000
#define MIN_UTTERANCE_MS    300
#define IDLE_TIMEOUT_MS     (5UL * 60UL * 1000UL)

// ============================================================
// GLOBALS
// ============================================================
int16_t* audio_buffer = nullptr;
size_t   audio_samples = 0;

// Speaker on I2S_NUM_1 to avoid conflict with mic on I2S_NUM_0
Audio audio(I2S_NUM_1);

// New I2S driver handle for the mic (RX channel on I2S_NUM_0)
i2s_chan_handle_t mic_handle = nullptr;

enum AgentState { OFF, LISTENING, THINKING, SPEAKING };
volatile AgentState state = OFF;

unsigned long last_activity_ms = 0;
unsigned long last_button_ms   = 0;

// ============================================================
// HELPERS
// ============================================================
String base_url() {
  String proto = USE_HTTPS ? "https://" : "http://";
  return proto + SERVER_HOST;
}

// ============================================================
// I2S MIC INIT — NEW DRIVER
// ============================================================
void init_i2s_mic() {
  // Allocate an RX channel on I2S_NUM_0
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num  = 4;
  chan_cfg.dma_frame_num = 256;
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &mic_handle));

  // Standard I2S config: Philips, 32-bit (INMP441 outputs 24 bits in a 32-bit slot), mono left
  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)I2S_MIC_SCK,
      .ws   = (gpio_num_t)I2S_MIC_WS,
      .dout = I2S_GPIO_UNUSED,
      .din  = (gpio_num_t)I2S_MIC_SD,
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv   = false,
      },
    },
  };
  // INMP441 with L/R tied to GND outputs on the LEFT slot
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(mic_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(mic_handle));
}

// ============================================================
// BOOT BUTTON edge detection
// ============================================================
bool button_just_pressed() {
  static int last_state = HIGH;
  int now = digitalRead(BUTTON_PIN);
  bool pressed = false;
  if (last_state == HIGH && now == LOW) {
    if (millis() - last_button_ms > 250) {
      pressed = true;
      last_button_ms = millis();
    }
  }
  last_state = now;
  return pressed;
}

// ============================================================
// READ ONE CHUNK FROM MIC
// ============================================================
size_t read_mic_chunk(int16_t* out, size_t max_samples, uint32_t* rms_out) {
  static int32_t in32[MIC_CHUNK];
  size_t bytes_read = 0;
  esp_err_t err = i2s_channel_read(mic_handle, in32, sizeof(in32),
                                   &bytes_read, 50 / portTICK_PERIOD_MS);
  if (err != ESP_OK) {
    *rms_out = 0;
    return 0;
  }
  size_t n = bytes_read / 4;
  if (n > max_samples) n = max_samples;
  uint64_t sum_sq = 0;
  for (size_t i = 0; i < n; i++) {
    int16_t s = (int16_t)(in32[i] >> 14);
    out[i] = s;
    sum_sq += (int32_t)s * s;
  }
  *rms_out = (n > 0) ? (uint32_t)sqrt((double)sum_sq / n) : 0;
  return n;
}

// ============================================================
// LISTEN-AND-RECORD
// ============================================================
bool listen_and_record() {
  Serial.println("[LISTEN] Waiting for you to speak…");
  audio_samples = 0;

  // Drain any stale data in the I2S buffer
  uint8_t drain[256];
  size_t  drained;
  while (i2s_channel_read(mic_handle, drain, sizeof(drain), &drained, 0) == ESP_OK && drained > 0) {}

  bool in_speech = false;
  unsigned long speech_start_ms = 0;
  unsigned long last_speech_ms  = 0;
  int16_t chunk[MIC_CHUNK];

  while (true) {
    if (button_just_pressed()) {
      Serial.println("[LISTEN] Button -> OFF");
      state = OFF;
      return false;
    }
    if (millis() - last_activity_ms > IDLE_TIMEOUT_MS) {
      Serial.println("[LISTEN] 5-min idle timeout -> OFF");
      state = OFF;
      return false;
    }

    uint32_t rms;
    size_t n = read_mic_chunk(chunk, MIC_CHUNK, &rms);
    if (n == 0) continue;

    bool is_speech = (rms > VAD_THRESHOLD);
    unsigned long now = millis();

    if (!in_speech) {
      if (is_speech) {
        if (speech_start_ms == 0) speech_start_ms = now;
        if (now - speech_start_ms > SPEECH_START_MS) {
          in_speech = true;
          last_speech_ms = now;
          last_activity_ms = now;
          Serial.println("[REC] Speech started");
          for (size_t i = 0; i < n && audio_samples < MAX_SAMPLES; i++) {
            audio_buffer[audio_samples++] = chunk[i];
          }
        }
      } else {
        speech_start_ms = 0;
      }
    } else {
      for (size_t i = 0; i < n && audio_samples < MAX_SAMPLES; i++) {
        audio_buffer[audio_samples++] = chunk[i];
      }
      if (is_speech) last_speech_ms = now;
      if (now - last_speech_ms > SPEECH_END_MS) {
        unsigned long dur = now - speech_start_ms;
        Serial.printf("[REC] Done. dur=%lums, samples=%u\n", dur, (unsigned)audio_samples);
        if (dur < MIN_UTTERANCE_MS) {
          Serial.println("[REC] Too short, ignoring");
          audio_samples = 0;
          in_speech = false;
          speech_start_ms = 0;
          continue;
        }
        return true;
      }
      if (audio_samples >= MAX_SAMPLES) {
        Serial.println("[REC] Max length reached");
        return true;
      }
    }
  }
}

// ============================================================
// POST audio to /chat
// ============================================================
bool post_audio_to_server() {
  String url = base_url() + "/chat";
  Serial.printf("[NET] POST %s (%u bytes)\n", url.c_str(), (unsigned)(audio_samples * 2));

  HTTPClient http;
  WiFiClientSecure secure_client;

  bool ok;
  if (USE_HTTPS) {
    secure_client.setInsecure();
    ok = http.begin(secure_client, url);
  } else {
    ok = http.begin(url);
  }
  if (!ok) {
    Serial.println("[NET] http.begin failed");
    return false;
  }
  http.addHeader("Content-Type", "application/octet-stream");
  http.setTimeout(90000);

  const char* keep[] = { "X-Transcript", "X-Reply" };
  http.collectHeaders(keep, 2);

  int code = http.POST((uint8_t*)audio_buffer, audio_samples * 2);
  Serial.printf("[NET] HTTP %d\n", code);

  if (code == 200) {
    String t = http.header("X-Transcript");
    String r = http.header("X-Reply");
    if (t.length()) Serial.printf("You:  %s\n", t.c_str());
    if (r.length()) Serial.printf("Bot:  %s\n", r.c_str());
    http.end();
    return true;
  }
  if (code != 204) {
    Serial.println(http.getString().substring(0, 200));
  } else {
    Serial.println("[NET] No speech detected");
  }
  http.end();
  return false;
}

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32-S3 Voice Bot Client (Cloud, new I2S driver) ===");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  audio_buffer = (int16_t*)ps_malloc((size_t)MAX_SAMPLES * sizeof(int16_t));
  if (!audio_buffer) {
    Serial.println("FATAL: PSRAM alloc failed. Tools->PSRAM->OPI PSRAM");
    while (1) delay(1000);
  }
  Serial.printf("Audio buffer: %u KB in PSRAM\n",
                (unsigned)(MAX_SAMPLES * sizeof(int16_t) / 1024));

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());

  init_i2s_mic();

  audio.setPinout(I2S_SPK_BCLK, I2S_SPK_LRC, I2S_SPK_DIN);
  audio.setVolume(15);

  Serial.printf("Server: %s\n", base_url().c_str());
  Serial.println("\nReady. Press the BOOT button to start the agent.");
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  switch (state) {
    case OFF: {
      digitalWrite(LED_PIN, LOW);
      if (button_just_pressed()) {
        Serial.println("\n>>> AGENT ON <<<");
        digitalWrite(LED_PIN, HIGH);
        last_activity_ms = millis();
        state = LISTENING;
      } else {
        delay(20);
      }
      break;
    }

    case LISTENING: {
      bool got = listen_and_record();
      if (!got) {
        digitalWrite(LED_PIN, LOW);
        Serial.println(">>> AGENT OFF <<<");
        state = OFF;
        break;
      }
      state = THINKING;
      break;
    }

    case THINKING: {
      Serial.println("[THINK] Sending to server… (first call after sleep may take ~30s)");
      // Pulse LED while waiting so the user knows it's not frozen
      digitalWrite(LED_PIN, LOW);
      delay(150);
      digitalWrite(LED_PIN, HIGH);
      if (post_audio_to_server()) {
        String url = base_url() + "/reply.mp3";
        audio.connecttohost(url.c_str());
        state = SPEAKING;
      } else {
        Serial.println("[THINK] No response. Listening again.");
        state = LISTENING;
      }
      break;
    }

    case SPEAKING: {
      static bool ever_running = false;
      static unsigned long speak_start_ms = 0;
      if (speak_start_ms == 0) speak_start_ms = millis();

      audio.loop();

      if (button_just_pressed()) {
        audio.stopSong();
        Serial.println("[SPEAK] Button pressed -> AGENT OFF");
        state = OFF;
        digitalWrite(LED_PIN, LOW);
        speak_start_ms = 0;
        ever_running = false;
        break;
      }

      if (audio.isRunning()) ever_running = true;

      // Only treat playback as "done" once the stream has actually started,
      // OR after a 5s safety timeout (covers stream-never-starts errors).
      bool grace_elapsed = (millis() - speak_start_ms > 5000);
      if ((ever_running || grace_elapsed) && !audio.isRunning()) {
        Serial.println("[SPEAK] Done.");
        state = LISTENING;
        speak_start_ms = 0;
        ever_running = false;
      }
      break;
    }
  }
}
