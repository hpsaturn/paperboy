# Paperboy

Paperboy turns an M5Stack e-paper tablet into a handheld Game Boy (DMG).

## Introduction

Paperboy is an ESP-IDF firmware that runs original Game Boy games on M5Stack
e-ink devices. It emulates the DMG, renders every frame straight onto the
e-paper panel and provides sound, touch controls, an on-device ROM picker and
persistent save states. The emulation core is CrankBoy (peanut_gb), so game
logic runs at full DMG speed while the panel is refreshed as fast as the
e-paper physics allow.

## Supported boards

Both hardware revisions are supported. The board is selected at build time with
the IDF target; there is nothing else to configure:

| Target    | Board                | SoC              | Display                 | Audio      |
|-----------|----------------------|------------------|-------------------------|------------|
| `esp32s3` | M5PaperS3            | ESP32-S3         | ED047TC1 parallel (MSG) | I2S PDM    |
| `esp32`   | M5Paper (original)   | ESP32-D0WDQ6-V3  | ED047TC1 via IT8951 SPI | LEDC       |

## Features

- Game Boy / DMG emulation at full speed.
- ROM picker at startup, browsing the microSD card.
- Save states (`.sst`) and persistent configuration (`paperboy.cfg`).
- Battery gauge and low-battery overlay.
- Touch zones for load / save / power.
- Hardware power button (long press) with true power-off on the M5Paper.

## Requirements

- ESP-IDF v5.5.x installed and exported (`idf.py` available on your PATH).
- An M5PaperS3 or M5Paper, a USB cable and a FAT32 microSD card with `.gb` ROMs.

## Building and flashing

Select the board, then build and flash it:

    # M5PaperS3 (ESP32-S3)
    idf.py set-target esp32s3

    # ...or the original M5Paper (ESP32)
    idf.py set-target esp32

    idf.py build
    idf.py -p /dev/ttyUSB0 flash monitor

`set-target` selects the matching pin map, display transport and defaults from
`sdkconfig.defaults*`, so no further configuration is needed. When you switch
between boards, run `set-target` again: it regenerates `sdkconfig` for the new
target.

## Using it

1. Copy your Game Boy ROMs (`.gb`) to the root of the microSD card.
2. Insert the card and power the board on.
3. Select a ROM with the D-pad (UP / DOWN) and confirm with A or START.
4. In game: the touch zones handle load / save / power; hold the power button
   to shut the device down.

## Credits

- Emulation core: CrankBoy / peanut_gb.
- Upstream Paperboy project by zephray.
- M5Paper (ESP32) support, IT8951 display backend and hardware fixes in this fork.
