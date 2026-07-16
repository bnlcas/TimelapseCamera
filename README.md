# Timelapse Camera

Software to run a battery-powered timelapse camera built on the Seeed Studio **XIAO ESP32S3
Sense** (ESP32-S3 with an OV2640 camera and a microSD slot on the expansion
board). The basic idea is to take a single photo every 24 hours to observe the passage of time. Flip the power switch and it takes one photo per day, forever, saving a stack of images
`image1.jpg`, `image2.jpg`, ... onto the SD card.

A hardware sibling of [photographatree.com](https://photographatree.com):
point it at something alive and let it watch.


## Configuration

All knobs are `#define`s at the top of [src/main.cpp](src/main.cpp):

| Define                 | Default | Meaning                                          |
| ---------------------- | ------- | ------------------------------------------------ |
| `CAPTURE_INTERVAL_SEC` | `86400ULL` | Seconds between photos (86400 = daily). Keep the `ULL` — see below. |
| `DEEP_SLEEP_MODE`      | `1`     | Set `0` for desk debugging (debug uses `delay()` instead of deep sleep. Deep sleep makes it hard to reflash the esp) |
| `WB_MODE`              | `2`     | Fixed white balance: 1 = sunny (~5500K), 2 = cloudy (warmer). Auto-WB is disabled so timelapse color shifts stay honest |
| `PULSE_LED_ON_START`   | `1`     | Blink the user LED once on wake - helpful to know that the device works                  |
| `WARMUP_FRAMES`        | `4`     | Discarded frames per wake for exposure settling   |
| `MAX_CAPTURE_ATTEMPTS` | `3`     | Retries when a frame fails the JPEG sanity check  |


## Build and flash

Requires [PlatformIO](https://platformio.org) (`brew install platformio`).

```sh
pio run                    # compile
pio run -t upload          # flash over USB
pio device monitor         # watch serial output at 115200 baud
```

Note: in deep-sleep mode the USB serial connection drops when the board
sleeps and re-enumerates on each wake, so set `DEEP_SLEEP_MODE 0` when you
want to watch it work at your desk.

## Hardware notes

- **GPIO21 is double-booked**: it is both the user LED and the SD card
  chip-select on the Sense expansion board. The LED blink must happen before
  `SD.begin()` and never after, or it will glitch the SD bus.
- The camera sensor is mounted upside-down relative to the board; the code
  vertically flips the image.
- Deep-sleep timer drift is real but small (the RTC oscillator is roughly
  ±1–2%); at daily intervals the shots will slowly walk around the clock.
- **Battery budget is dominated by sleep current, not capture.** A wake cycle
  is a few seconds at ~100 mA (~0.1 mAh/shot); what matters over 90 days is
  the floor while asleep. The firmware stops the camera clock and deselects
  the SD card before sleeping, but both stay on the 3V3 rail — the ESP32 has
  no control over their power. If measured sleep current is high (SD cards
  vary from ~100 µA to ~1 mA idle), the fix is hardware: a high-side switch
  on the Sense board's supply.
- Images are bare JFIF JPEGs straight from the sensor — no EXIF, no
  timestamps. Shot order is the filename index only.

## Bugs fixed relative to v1-as-shipped

- **Wrong interval (9 min instead of 24 h):** the wakeup time was computed as
  `CAPTURE_INTERVAL * 1000000` in 32-bit `int` arithmetic. 86,400,000,000 µs
  overflows to 500,654,080 µs ≈ 8.3 minutes. Fixed with 64-bit math — hence
  the `ULL` on `CAPTURE_INTERVAL_SEC`.
- **Image corruption / frame-dump kludge:** the old warm-up code fetched one
  frame and returned the *same* buffer to the driver four times, corrupting
  the frame queue. Now warm-up does matched get/return pairs, and captures
  are validated as structurally-sound JPEGs with retry.

## Someday / maybe

- Potentiometer to set the interval in the field (would require some hardware rework)
- Timestamped filenames via the RTC.
- csv for metadata on captures.
