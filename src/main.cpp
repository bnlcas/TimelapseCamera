// Timelapse camera for the Seeed Studio XIAO ESP32S3 Sense.
//
// Behavior: on every boot (cold power-on or deep-sleep timer wake) the board
// takes one photo, saves it to the SD card as /imageN.jpg, and goes back into
// deep sleep for CAPTURE_INTERVAL_SEC. Numbering continues from the highest
// imageN.jpg already on the card, so power cycles don't overwrite anything.

#include "esp_camera.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "esp_wifi.h"
#include "esp_bt.h"

#include "camera_pins.h"

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

// Seconds between photos. 60 = minute, 3600 = hour, 86400 = day.
// The ULL suffix matters: this gets multiplied by 1,000,000 to convert to
// microseconds, which overflows 32-bit arithmetic for anything over ~71
// minutes (the bug that turned "daily" into "every 9 minutes").
//#define CAPTURE_INTERVAL_SEC 1800ULL //half hourly
//#define CAPTURE_INTERVAL_SEC 5400ULL //90 minues hourly
#define CAPTURE_INTERVAL_SEC 86400ULL // daily

// After a deep-sleep wake the sensor's auto-exposure and white balance need a
// few frames to settle. These frames are captured and thrown away.
#define WARMUP_FRAMES 4
#define WARMUP_FRAME_DELAY_MS 150

// Retries if a captured frame fails the JPEG sanity check.
#define MAX_CAPTURE_ATTEMPTS 3

// Fixed white-balance preset (auto-WB is disabled for timelapse stability).
// 1 = sunny (~5500K), 2 = cloudy (warmer, more red -- nicer for plants).
#define WB_MODE 1

// Set to 0 for desk debugging: uses delay() instead of deep sleep, so the
// serial monitor stays connected between shots.
#define DEEP_SLEEP_MODE 1

// Blink the user LED once on wake as a sign of life.
// NOTE: on the XIAO ESP32S3 the user LED (GPIO21) is the same pin as the
// Sense expansion board's SD chip-select, so this must happen BEFORE
// SD.begin() and never after.
#define PULSE_LED_ON_START 1

#define SD_CS_PIN 21

// ---------------------------------------------------------------------------

bool camera_ok = false;
bool sd_ok = false;
int imageIndex = 1;

// WiFi and Bluetooth are never used; shut them down to save power.
void disableRadios() {
  esp_wifi_stop();
  esp_bt_controller_disable();
  esp_bt_mem_release(ESP_BT_MODE_BTDM);
}

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_UXGA;  // 1600x1200, max for the OV2640
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = 10;  // 0-63, lower = better
    config.fb_count = 2;
  } else {
    // The Sense board always has PSRAM; this fallback only matters if it
    // fails to initialize. A UXGA frame buffer doesn't fit in DRAM.
    Serial.println("WARNING: PSRAM not found, falling back to SVGA in DRAM");
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);  // sensor is mounted upside-down
  s->set_hmirror(s, 0);
  s->set_denoise(s, 1);

  // Fixed white balance for outdoor use. Auto-WB would fight the real color
  // shifts of sunrise/sunset across a timelapse, so lock it: disable the auto
  // algorithm, keep the WB gain stage active, and load a fixed preset.
  // Presets: 1 = sunny (~5500K neutral daylight), 2 = cloudy (warmer / more
  // red, flattering for foliage and flowers), 3 = office, 4 = home.
  s->set_whitebal(s, 0);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, WB_MODE);
  return true;
}

bool initSdCard() {
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("Card mount failed");
    return false;
  }
  if (SD.cardType() == CARD_NONE) {
    Serial.println("No SD card attached");
    return false;
  }
  return true;
}

// Scan the card root for existing image<N>.jpg files and continue after the
// highest N found.
int findNextImageIndex() {
  File root = SD.open("/");
  if (!root) {
    Serial.println("Failed to open root directory");
    return 1;
  }

  int maxIndex = 0;
  while (File file = root.openNextFile()) {
    if (!file.isDirectory()) {
      const char *n = file.name();
      if (n[0] == '/') n++;
      int idx = 0;
      if (sscanf(n, "image%d.jpg", &idx) == 1 && idx > maxIndex) {
        maxIndex = idx;
      }
    }
    file.close();
  }
  root.close();

  Serial.printf("Highest existing image index: %d\n", maxIndex);
  return maxIndex + 1;
}

// Grab and immediately return a few frames so auto-exposure/white-balance
// settle. Each get/return must be a matched pair -- returning the same
// buffer repeatedly corrupts the driver's frame queue.
void warmUpSensor() {
  for (int i = 0; i < WARMUP_FRAMES; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(WARMUP_FRAME_DELAY_MS);
  }
}

// Cheap structural check that a frame is a plausible JPEG: starts with the
// SOI marker and has an EOI marker near the end (the driver may pad a few
// bytes after EOI, so scan the tail rather than just the last two bytes).
bool jpegLooksValid(const camera_fb_t *fb) {
  if (fb->len < 1024) return false;
  if (fb->buf[0] != 0xFF || fb->buf[1] != 0xD8) return false;
  size_t start = fb->len >= 16 ? fb->len - 16 : 0;
  for (size_t i = start; i + 1 < fb->len; i++) {
    if (fb->buf[i] == 0xFF && fb->buf[i + 1] == 0xD9) return true;
  }
  return false;
}

bool writeFileToSd(const char *path, const uint8_t *data, size_t len) {
  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    Serial.printf("Failed to open %s for writing\n", path);
    return false;
  }
  size_t written = file.write(data, len);
  file.close();
  if (written != len) {
    Serial.printf("Short write to %s: %u of %u bytes\n", path,
                  (unsigned)written, (unsigned)len);
    return false;
  }
  return true;
}

bool captureAndSave(const char *path) {
  for (int attempt = 1; attempt <= MAX_CAPTURE_ATTEMPTS; attempt++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Failed to get camera frame buffer");
      delay(WARMUP_FRAME_DELAY_MS);
      continue;
    }
    if (!jpegLooksValid(fb)) {
      Serial.printf("Frame failed JPEG sanity check (attempt %d)\n", attempt);
      esp_camera_fb_return(fb);
      delay(WARMUP_FRAME_DELAY_MS);
      continue;
    }
    size_t len = fb->len;
    bool saved = writeFileToSd(path, fb->buf, len);
    esp_camera_fb_return(fb);
    if (saved) {
      Serial.printf("Saved %s (%u bytes)\n", path, (unsigned)len);
      return true;
    }
  }
  Serial.println("Giving up on this capture");
  return false;
}

// Sleep until the next shot. In deep sleep the chip fully powers down and
// reboots on wake, so setup() runs again each cycle and loop() never repeats.
void sleepUntilNextShot() {
  Serial.printf("Sleeping for %llu seconds\n", CAPTURE_INTERVAL_SEC);
  Serial.flush();
#if DEEP_SLEEP_MODE
  // Shut peripherals down cleanly before sleeping. The camera and SD stay on
  // the 3V3 rail through deep sleep regardless (no firmware control over
  // their power), but this stops the camera's XCLK and deselects the SD card
  // so both sit in their lowest register-level idle states.
  esp_camera_deinit();
  SD.end();
  esp_sleep_enable_timer_wakeup(CAPTURE_INTERVAL_SEC * 1000000ULL);
  esp_deep_sleep_start();
#else
  // Debug mode: no deinit, since loop() reuses the camera without re-init.
  delay(CAPTURE_INTERVAL_SEC * 1000ULL);
#endif
}

void setup() {
#if PULSE_LED_ON_START
  // Before SD.begin() -- see the GPIO21 note above.
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);  // LED is active-low
  delay(100);                      // just long enough to see; CPU is burning
  digitalWrite(LED_BUILTIN, HIGH);
#endif

  disableRadios();

  Serial.begin(115200);
#if !DEEP_SLEEP_MODE
  // Only wait for a serial monitor in debug mode. In release there is no USB
  // host, so this loop would burn its full 3 s at active current every wake.
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) {
  }
#endif

  camera_ok = initCamera();
  sd_ok = initSdCard();
  if (sd_ok) {
    imageIndex = findNextImageIndex();
  }
}

void loop() {
  if (camera_ok && sd_ok) {
    warmUpSensor();
    char filename[32];
    snprintf(filename, sizeof(filename), "/image%d.jpg", imageIndex);
    if (captureAndSave(filename)) {
      imageIndex++;
    }
  } else {
    // Init failed (SD not seated, camera flaky, ...). Don't spin awake
    // draining the battery -- sleep and retry on the next cycle, since a
    // deep-sleep wake is a full reboot and re-init.
    Serial.println("Init failed; will retry after sleep");
  }
  sleepUntilNextShot();
}
