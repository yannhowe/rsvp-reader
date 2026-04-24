# RSVP Reader for ESP32

Speed-reading e-reader using RSVP (Rapid Serial Visual Presentation) on Waveshare ESP32-S3 LCD boards. Displays one word at a time with ORP (Optimal Recognition Point) highlighting for faster reading.

## Live Demo

**[Try the interactive demo](https://yannhowe.github.io/rsvp-reader/)** — board comparison with working RSVP readers at actual pixel resolution.

[Original 320×820 prototype](https://yannhowe.github.io/rsvp-reader/prototype.html)

## What is RSVP?

RSVP displays text one word at a time at a configurable speed (WPM), eliminating saccadic eye movement. The red "pivot letter" (ORP) is positioned at a fixed point so your eyes never move — just absorb words as they flash.

## Supported Boards

| Board | Display | Resolution | Best For |
|-------|---------|-----------|----------|
| **ESP32-S3-LCD-3.16** | 3.16" LCD | 320×820 | Primary target, tall portrait |
| **ESP32-S3-Touch-LCD-3.49** | 3.49" LCD | 172×640 | Narrower alternative, has touch + audio |
| **ESP32-S3-LCD-1.54** | 1.54" LCD | 240×240 | Pocket-sized, 3 buttons, cheap ($15) |
| ESP32-S3-AMOLED-1.91 | 1.91" AMOLED | 240×536 | Tall AMOLED, gorgeous display |
| ESP32-S3-Touch-AMOLED-1.64 | 1.64" AMOLED | 280×456 | Compact tall AMOLED |
| ESP32-S3-Touch-AMOLED-1.8 | 1.8" AMOLED | 368×448 | Fully loaded (audio, mic, RTC) |
| ESP32-S3-Touch-AMOLED-2.06 | 2.06" AMOLED | 410×502 | Large tall AMOLED |
| ESP32-S3-Touch-AMOLED-1.75 | 1.75" AMOLED | 466×466 | Square AMOLED |
| ESP32-S3-Touch-AMOLED-2.16 | 2.16" AMOLED | 480×480 | Large square AMOLED |
| ESP32-S3-Touch-AMOLED-2.41 | 2.41" AMOLED | 600×450 | Largest, landscape-oriented |

All boards share ESP32-S3R8 (8MB PSRAM, 16MB Flash), QMI8658 IMU (tilt gestures), and TF/SD card (for EPUB storage).

## Features

- **EPUB reader** — reads .epub files directly from SD/TF card
- **ORP highlighting** — red pivot character at optimal recognition point
- **Adjustable WPM** — 100–1000 words per minute, tilt to adjust
- **Tilt gestures** — tilt right/left for WPM, forward/back for chapters
- **Auto-rotation** — portrait/landscape via accelerometer
- **Bookmarks** — auto-saves position in NVS, resumes on power-up
- **Chapter navigation** — double-press or tilt to switch chapters
- **Timing intelligence** — pauses longer after punctuation, adjusts for word length

## Controls

| Action | BOOT Button | IMU Tilt | Keyboard (demo) |
|--------|------------|----------|-----------------|
| Play/Pause | Short press | — | Space |
| Menu | Long press (1s) | — | — |
| WPM +25 | — | Tilt right | → |
| WPM -25 | — | Tilt left | ← |
| Next chapter | Double press | Tilt forward | ↓ |
| Prev chapter | — | Tilt back | ↑ |

## Architecture

```
[SD Card: book.epub]
       │
  [miniz: ZIP extract]
       │
  [XML state machine: strip tags → plaintext]
       │
  [Text cache: /tmp/ch_XXX.txt on SD]
       │
  [RSVP Engine: word buffer + timer]
       │
  [LVGL UI: ORP word display + progress]
       │
  [Display: ST7701S / ST7789 / AMOLED]
```

## Project Structure

```
firmware/
├── rsvp-reader.ino         # Main sketch: display init, LVGL, callback wiring
├── user_config.h           # Board-specific pin definitions
├── epub_parser.h/.cpp      # EPUB ZIP extraction + XHTML text stripping
├── rsvp_engine.h/.cpp      # Word timing, ORP calc, chapter management
├── ui_manager.h/.cpp       # LVGL screens (file picker, reader, menu)
├── sd_manager.h/.cpp       # SD/SDMMC init, recursive .epub scanning
├── bookmark_manager.h/.cpp # NVS save/restore reading position
├── imu_controls.h/.cpp     # QMI8658 tilt gestures + BOOT button state machine
├── lib/miniz/              # Single-file ZIP library for EPUB extraction
└── fonts/                  # LVGL compiled fonts

docs/
├── index.html              # Board comparison demo (GitHub Pages)
└── prototype.html          # Original 320×820 prototype
```

## Building

### Requirements
- Arduino IDE 2.x
- ESP32 board package by Espressif (latest)
- LVGL v8 library
- Target board with PSRAM enabled

### Arduino IDE Settings
- Board: `ESP32S3 Dev Module`
- PSRAM: `OPI PSRAM`
- Flash: `16MB (128Mb)`
- Partition: `16M Flash (3MB APP/9.9MB FATFS)`
- CPU: `240 MHz`

### Steps
1. Clone this repo
2. Open `firmware/rsvp-reader.ino` in Arduino IDE
3. Install LVGL v8 library
4. Copy Waveshare demo display driver files (see wiki links below)
5. Select board settings above
6. Upload

## Implementation Status

### Complete
- [x] HTML prototypes (320×820 + board comparison)
- [x] EPUB parser (ZIP extraction + XHTML stripping)
- [x] RSVP engine (word timing, ORP, chapter management)
- [x] UI manager (file picker, reader, menu, resume prompt)
- [x] SD card manager (SDMMC 4-wire, recursive scanning)
- [x] Bookmark manager (NVS persistence)
- [x] IMU controls (button state machine + tilt gestures)
- [x] Auto-rotation detection (accelerometer gravity vector)

### In Progress
- [ ] Auto-rotation UI (reposition LVGL elements for landscape)
- [ ] Wire orientation callback in main sketch
- [ ] Multi-board support (abstract display driver + pin configs)
- [ ] Adjustable font size

### Future
- [ ] WiFi web remote control
- [ ] Battery indicator
- [ ] Sleep mode (auto-sleep after 5 min pause)
- [ ] Reading stats (WPM history, total words read)
- [ ] Text-to-speech using audio boards (1.54", 3.49", 1.8")

## References

- [Waveshare ESP32-S3-LCD-3.16 Wiki](https://www.waveshare.com/wiki/ESP32-S3-LCD-3.16)
- [Waveshare ESP32-S3-Touch-LCD-3.49 Wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-3.49)
- [Waveshare ESP32-S3-LCD-1.54 Wiki](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.54)
- [miniz library](https://github.com/richgel999/miniz)

## License

MIT
