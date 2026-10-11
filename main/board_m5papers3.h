//
// board_m5papers3.h — M5PaperS3 (ESP32-S3) pin map.
//
// Display: bare ED047TC1 EPD driven directly over an 8-bit parallel bus by the
//          "MSG" (Modos Smooth Graphics) engine in msg/msg.c.
//
#pragma once

/* ── EPD parallel bus (MSG engine) ─────────────────────────────────────── */
#define EPD_PWR_EN_PIN      45
#define EPD_BST_EN_PIN      46
#define EPD_SDCE_PIN        13
#define EPD_SDLE_PIN        15
#define EPD_SDCK_PIN        16
#define EPD_GDSP_PIN        17
#define EPD_GDCK_PIN        18

#define EPD_D0_PIN          6
#define EPD_D1_PIN          14
#define EPD_D2_PIN          7
#define EPD_D3_PIN          12
#define EPD_D4_PIN          9
#define EPD_D5_PIN          11
#define EPD_D6_PIN          8
#define EPD_D7_PIN          10

#define EPD_BUSW            8
#define EPD_XCK             24000000
#define EPD_IMAGE_FIELDS    8

/* ── microSD (SPI) ─────────────────────────────────────────────────────── */
#define SD_PIN_CLK          39
#define SD_PIN_MOSI         38
#define SD_PIN_MISO         40
#define SD_PIN_CS           47

/* ── Touch: GT911 (I2C) ────────────────────────────────────────────────── */
#define TOUCH_I2C_PORT_NUM  1
#define TOUCH_PIN_SDA       41
#define TOUCH_PIN_SCL       42
#define TOUCH_PIN_INT       48

/* ── Battery ADC: GPIO3 = ADC1_CH2, 1/2 resistor divider ───────────────── */
#define BAT_ADC_UNIT_NUM    1
#define BAT_ADC_CHANNEL_NUM 2
#define BAT_DIVIDER         2

/* ── Buzzer / speaker (I2S PDM or LEDC) ────────────────────────────────── */
#define BOARD_BUZZER_GPIO   21

/* ── MSG engine bus parameters (S3) ────────────────────────────────────── */
#define EPD_LINE_PAD        16
#define EPD_XCK             24000000
#define EPD_IMAGE_FIELDS    8
