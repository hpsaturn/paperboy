//
// board.h — compile-time board / platform selection for Paperboy.
//
// The display transport, pin map and peripheral wiring differ completely
// between the two supported boards:
//
//   M5PaperS3   (ESP32-S3)      : bare ED047TC1 EPD driven directly by an
//                                 8-bit parallel bus ("MSG" engine).
//   M5Paper     (ESP32-D0WDQ6)  : ED047TC1 panel behind an IT8951 controller
//                                 over SPI.
//
// Select with the IDF target (idf.py set-target).  Both boards share the rest
// of the firmware (emulator, UI, audio, SD, battery).
//
// Only plain integer pin numbers are defined here so this header stays free of
// driver dependencies.
//
#pragma once

#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
#  define PLAT_M5PAPERS3 1
#elif defined(CONFIG_IDF_TARGET_ESP32)
#  define PLAT_M5PAPER   1
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
#  define PLAT_M5PAPERS3 1   /* closest match; ESP32-S2 not validated */
#else
#  error "paperboy: unsupported IDF target (expected esp32 or esp32s3)"
#endif

#if defined(PLAT_M5PAPER)
#  include "board_m5paper.h"
#  define BOARD_HAS_IT8951 1
#else
#  include "board_m5papers3.h"
#  define BOARD_HAS_IT8951 0
#endif
