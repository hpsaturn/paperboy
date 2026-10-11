//
// board_m5paper.h — M5Paper (original, ESP32-D0WDQ6-V3) pin map.
//
// Sources: M5Stack M5EPD library (M5EPD.h), M5Unified pin tables and the
// official M5Paper schematic.
//
// Display: ED047TC1 panel driven by an IT8951E controller over SPI.
//          The IT8951, the microSD card and (on the S3) nothing else share
//          the same SPI bus (SCK/MOSI/MISO); only the CS lines differ.
//
#pragma once

/* ── EPD: IT8951 over SPI ──────────────────────────────────────────────── */
#define EPD_SPI_HOST_NUM    1  /* SPI2_HOST */
#define EPD_SPI_HZ          (20 * 1000 * 1000)

#define EPD_SCK_PIN         14
#define EPD_MOSI_PIN        12
#define EPD_MISO_PIN        13
#define EPD_CS_PIN          15
#define EPD_BUSY_PIN        27      /* high = ready    */
#define EPD_PWR_PIN         23      /* EPD power enable (M5EPD_EPD_PWR_EN_PIN) */
#define EPD_MAIN_PWR_PIN    2       /* main power enable (M5EPD_MAIN_PWR_PIN)  */
#define EPD_EXT_PWR_PIN     5       /* external/5V enable (M5EPD_EXT_PWR_EN_PIN) */

/* IT8951 area image load requires the X origin / width to be a multiple of 4.
 * The GB video window is placed here on the 960x540 panel. */
#define EPD_VIDEO_WIN_X     432
#define EPD_VIDEO_WIN_Y     30
#define EPD_VIDEO_WIN_W     432
#define EPD_VIDEO_WIN_H     480

/* ── microSD (SPI, shares the EPD bus) ─────────────────────────────────── */
#define SD_PIN_CLK          14
#define SD_PIN_MOSI         12
#define SD_PIN_MISO         13
#define SD_PIN_CS           4

/* ── Touch: GT911 (I2C) ────────────────────────────────────────────────── */
#define TOUCH_I2C_PORT_NUM  1
#define TOUCH_PIN_SDA       21
#define TOUCH_PIN_SCL       22
#define TOUCH_PIN_INT       36

/* ── Battery ADC: GPIO35 = ADC1_CH7, 1/2 resistor divider ──────────────── */
#define BAT_ADC_UNIT_NUM    1
#define BAT_ADC_CHANNEL_NUM 7
#define BAT_DIVIDER         2

/* ── Audio: the original M5Paper has no on-board speaker/amp.
 *     GPIO19 is a free pin on the 20-pin header, used if a buzzer is wired. */
#define BOARD_BUZZER_GPIO   19
