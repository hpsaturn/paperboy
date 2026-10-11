//
// msg_it8951.c — MSG API backend for the original M5Paper (ESP32-D0WDQ6-V3).
//
// This board does not expose the bare EPD; instead an IT8951E controller sits
// between the ESP32 and the ED047TC1 panel, spoken to over SPI:
//
//     ESP32 (SPI2) ──SCK/MOSI/MISO+CS── IT8951E ── TCON ──> ED047TC1
//
// The IT8951 owns the panel timing and the waveform ("update mode") state, so
// this backend is intentionally much simpler than the M5PaperS3 "MSG" engine:
//
//   * msg_display_image()  → load a full-panel 1bpp image into IT8951 memory
//                            and refresh it (direct update).
//   * msg_flip()           → expand the emulator's 432x160 packed 1bpp frame
//                            into the 432x480 on-panel window and refresh it.
//
// Low-level SPI framing (0x6000 command / 0x0000 data / 0x1000 read preambles,
// LD_IMG_AREA 4bpp "B" endian loads, DPY_BUF_AREA refresh) follows M5Stack's
// M5EPD driver, which is the shipping driver for this exact hardware.
//
// Note: the original M5Paper cannot refresh the panel at 57 fps.  msg_flip()
// therefore blocks until each IT8951 update completes; the emulator naturally
// runs as fast as the panel allows.
//
// The 4bpp wire encoding uses 0x0 = black, 0xF = white.
//
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "board.h"
#include "msg.h"

#if defined(PLAT_M5PAPER)

static const char *TAG = "msg_it8951";

/* ── IT8951 command / register map (from M5EPD IT8951_Defines.h) ────────── */
#define IT8951_TCON_SYS_RUN         0x0001
#define IT8951_TCON_STANDBY         0x0002
#define IT8951_TCON_SLEEP           0x0003
#define IT8951_TCON_REG_RD          0x0010
#define IT8951_TCON_REG_WR          0x0011
#define IT8951_TCON_LD_IMG_AREA     0x0021
#define IT8951_TCON_LD_IMG_END      0x0022
#define IT8951_I80_CMD_DPY_BUF_AREA 0x0037
#define IT8951_I80_CMD_VCOM         0x0039

#define IT8951_I80CPCR              0x0004
#define IT8951_LISAR                0x0208
#define IT8951_LUTAFSR              0x1224

/* 4bpp, "B" endian, rotation 0 → the value written into LD_IMG_AREA arg0. */
#define IT8951_LDIMG_AREA_PARAM     ((uint16_t)((1u << 8) | (2u << 4) | 0u))

/* Update modes */
#define UPDATE_MODE_INIT            0
#define UPDATE_MODE_DU              1
#define UPDATE_MODE_GC16            2
#define UPDATE_MODE_GL16            3
#define UPDATE_MODE_DU4             6

/* IT8951 memory address used for image loads (M5EPD default). */
#define IT8951_MEM_ADDR             0x001236E0u

/* ── State ─────────────────────────────────────────────────────────────── */
static spi_device_handle_t s_spi;
static volatile uint32_t   s_vsync_count;

/* Triple-buffer-ish pair, matching the msg.c contract: the emulator renders
 * into one and the other is the frame currently on the panel. */
static uint8_t *fb[2];
static int      cur;   /* buffer the caller is (or is about to be) rendering into */
static bool     enable_video;
static bool     s_ready;
static bool     s_img_to_white;

/* ── Low-level SPI framing ─────────────────────────────────────────────── */

/* All transfers are 16-bit words sent most-significant byte first. */
static void it_wait_busy(void)
{
    int64_t deadline = esp_timer_get_time() + 3000000LL; /* 3 s */
    while (gpio_get_level((gpio_num_t)EPD_BUSY_PIN) == 0) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(TAG, "BUSY timeout");
            return;
        }
        esp_rom_delay_us(20);
    }
}

static void it_xmit(const uint8_t *tx, size_t nbytes)
{
    if (nbytes == 0) {
        return;
    }
    spi_transaction_t t = {
        .length    = nbytes * 8,
        .tx_buffer = tx,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

/* command: preamble 0x6000 then the command word */
static void it_cmd(uint16_t cmd)
{
    uint8_t b[4] = { 0x60, 0x00, (uint8_t)(cmd >> 8), (uint8_t)cmd };
    it_wait_busy();
    it_xmit(b, sizeof(b));
}

/* one data word: preamble 0x0000 then the payload word */
static void it_word(uint16_t w)
{
    uint8_t b[4] = { 0x00, 0x00, (uint8_t)(w >> 8), (uint8_t)w };
    it_wait_busy();
    it_xmit(b, sizeof(b));
}

static void it_write_reg(uint16_t addr, uint16_t data)
{
    it_cmd(IT8951_TCON_REG_WR);
    it_word(addr);
    it_word(data);
}

static void it_read_reg(uint16_t addr, uint16_t *out)
{
    /* preamble 0x1000, one dummy word, then the payload word */
    uint8_t tx[6] = { 0x10, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t rx[6] = { 0 };

    it_cmd(IT8951_TCON_REG_RD);
    it_word(addr);

    it_wait_busy();
    spi_transaction_t t = {
        .length    = sizeof(tx) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
    *out = (uint16_t)((rx[4] << 8) | rx[5]);
}

static void it_set_target_mem_addr(uint32_t addr)
{
    it_write_reg(IT8951_LISAR + 2, (uint16_t)(addr >> 16));
    it_write_reg(IT8951_LISAR,     (uint16_t)(addr & 0xFFFF));
}

/* Wait until all pending LUT updates have finished. */
static void it_wait_lut(void)
{
    uint16_t afsr;
    int      guard = 0;
    do {
        it_read_reg(IT8951_LUTAFSR, &afsr);
        if (afsr == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    } while (++guard < 2000);
}

/* ── Streaming 4bpp image writer ───────────────────────────────────────── */

/* Send a run of 4bpp words as one CS-asserted burst (preamble once, chunks of
 * a DMA-capable internal buffer held together with SPI_TRANS_CS_KEEP_ACTIVE). */
#define ITW_CHUNK 2048
static uint8_t s_wbuf[ITW_CHUNK] __attribute__((aligned(4)));
static size_t  s_wpos;

static void itw_flush(bool last)
{
    if (s_wpos == 0) {
        return;
    }
    spi_transaction_t t = {
        .length    = s_wpos * 8,
        .tx_buffer = s_wbuf,
        .flags     = last ? 0 : SPI_TRANS_CS_KEEP_ACTIVE,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
    s_wpos = 0;
}

static inline void itw_raw(uint16_t w)
{
    if (s_wpos + 2 > ITW_CHUNK) {
        itw_flush(false);
    }
    s_wbuf[s_wpos++] = (uint8_t)(w >> 8);
    s_wbuf[s_wpos++] = (uint8_t)w;
}

/* Begin a data burst (asserts CS holding it across flushes). */
static void itw_begin(void)
{
    it_wait_busy();
    ESP_ERROR_CHECK(spi_device_acquire_bus(s_spi, portMAX_DELAY));
    s_wpos = 0;
    itw_raw(0x0000);           /* data preamble */
}

static void itw_end(void)
{
    itw_flush(true);
    spi_device_release_bus(s_spi);
}

/* ── Image load / refresh primitives ───────────────────────────────────── */

static void it_ld_img_area_begin(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    it_set_target_mem_addr(IT8951_MEM_ADDR);
    it_cmd(IT8951_TCON_LD_IMG_AREA);
    it_word(IT8951_LDIMG_AREA_PARAM);
    it_word(x);
    it_word(y);
    it_word(w);
    it_word(h);
    itw_begin();
}

static void it_ld_img_area_end(void)
{
    itw_end();
    it_cmd(IT8951_TCON_LD_IMG_END);
}

static void it_refresh(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t mode)
{
    it_cmd(IT8951_I80_CMD_DPY_BUF_AREA);
    it_word(x);
    it_word(y);
    it_word(w);
    it_word(h);
    it_word(mode);
    it_word((uint16_t)(IT8951_MEM_ADDR & 0xFFFF));
    it_word((uint16_t)(IT8951_MEM_ADDR >> 16));
}

static void it_update_area(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t mode)
{
    it_wait_lut();
    it_refresh(x, y, w, h, mode);
    it_wait_lut();
}

/* Pack four 1bpp source pixels (bit7 = leftmost) into a 4bpp word.
 * A set bit is rendered black unless invert is set. */
static inline uint16_t pack4(const uint8_t *src, int x, bool invert)
{
    uint8_t byte = src[x >> 3];
    uint16_t w = 0;
    for (int i = 0; i < 4; i++) {
        bool bit = (byte >> (7 - ((x + i) & 7))) & 1;
        bool black = bit ^ invert;
        w = (uint16_t)((w << 4) | (black ? 0x0 : 0xF));
    }
    return w;
}

/* ── Board power / controller init ─────────────────────────────────────── */

static void board_epd_power(bool on)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << EPD_MAIN_PWR_PIN) |
                        (1ULL << EPD_EXT_PWR_PIN)  |
                        (1ULL << EPD_PWR_PIN),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);

    if (on) {
        gpio_set_level((gpio_num_t)EPD_MAIN_PWR_PIN, 1);
        gpio_set_level((gpio_num_t)EPD_EXT_PWR_PIN, 1);
        gpio_set_level((gpio_num_t)EPD_PWR_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    } else {
        gpio_set_level((gpio_num_t)EPD_PWR_PIN, 0);
        gpio_set_level((gpio_num_t)EPD_EXT_PWR_PIN, 0);
        gpio_set_level((gpio_num_t)EPD_MAIN_PWR_PIN, 0);
    }
}

static void it_hw_init(void)
{
    gpio_config_t busy = {
        .pin_bit_mask = (1ULL << EPD_BUSY_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&busy);

    spi_bus_config_t buscfg = {
        .mosi_io_num   = EPD_MOSI_PIN,
        .miso_io_num   = EPD_MISO_PIN,
        .sclk_io_num   = EPD_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        /* Large enough for the shared microSD card transfers, too. */
        .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize((spi_host_device_t)EPD_SPI_HOST_NUM,
                                       &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = EPD_SPI_HZ,
        .mode           = 0,
        .spics_io_num   = EPD_CS_PIN,
        .queue_size     = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device((spi_host_device_t)EPD_SPI_HOST_NUM,
                                       &devcfg, &s_spi));

    it_cmd(IT8951_TCON_SYS_RUN);
    it_write_reg(IT8951_I80CPCR, 0x0001);   /* enable packed write */

    /* VCOM = -2.30 V (M5Paper factory value). */
    it_cmd(IT8951_I80_CMD_VCOM);
    it_word(0x0001);
    it_word(2300);

    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "IT8951 ready (SPI2 %d Hz, CS=%d BUSY=%d)",
             EPD_SPI_HZ, EPD_CS_PIN, EPD_BUSY_PIN);
}

/* ── Public MSG API ────────────────────────────────────────────────────── */

void msg_init(void)
{
    cur          = 0;
    enable_video = false;
    s_ready      = false;
    s_img_to_white = false;

    board_epd_power(true);
    it_hw_init();

    for (int i = 0; i < 2; i++) {
        /* MALLOC_CAP_8BIT is mandatory here.  MALLOC_CAP_INTERNAL alone may hand
         * out IRAM (0x4008_0000-0x400A_0000) on the original ESP32, which the CPU
         * can only read/write with 4-byte aligned word accesses.  Both the
         * emulator core and pack4() touch this buffer byte-wise, which aborts
         * with a LoadStoreError if the buffer lands in IRAM.  Fall back to PSRAM
         * (byte accessible) if internal DRAM cannot satisfy the request. */
        fb[i] = (uint8_t *)heap_caps_aligned_calloc(16, 1, EPD_VIDEO_FB_SIZE,
                                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (fb[i] == NULL) {
            fb[i] = (uint8_t *)heap_caps_aligned_calloc(16, 1, EPD_VIDEO_FB_SIZE,
                                                        MALLOC_CAP_SPIRAM);
        }
        if (fb[i] == NULL) {
            ESP_LOGE(TAG, "failed to allocate video buffer %d", i);
        } else {
            memset(fb[i], 0xFF, EPD_VIDEO_FB_SIZE);   /* start white */
        }
    }
}

void msg_start(void)
{
    /* Flash the panel to a clean white state (INIT waveform). */
    it_ld_img_area_begin(0, 0, EPD_WIDTH, EPD_HEIGHT);
    for (int i = 0; i < (EPD_WIDTH * EPD_HEIGHT / 4); i++) {
        itw_raw(0xFFFF);
    }
    it_ld_img_area_end();
    it_update_area(0, 0, EPD_WIDTH, EPD_HEIGHT, UPDATE_MODE_INIT);

    s_ready = true;
    ESP_LOGI(TAG, "Panel initialised (white)");
}

void msg_display_image(uint8_t *img, bool to_white)
{
    if (img == NULL || !s_ready) {
        return;
    }

    /* Load the full 960x540 1bpp image, MSB-first, bit set = black unless
     * to_white requests the inverse polarity. */
    it_ld_img_area_begin(0, 0, EPD_WIDTH, EPD_HEIGHT);
    for (int y = 0; y < EPD_HEIGHT; y++) {
        const uint8_t *row = img + (size_t)y * (EPD_WIDTH / 8);
        for (int x = 0; x < EPD_WIDTH; x += 4) {
            itw_raw(pack4(row, x, to_white));
        }
    }
    it_ld_img_area_end();
    it_update_area(0, 0, EPD_WIDTH, EPD_HEIGHT, UPDATE_MODE_DU);
}

void msg_enable_video(bool en)
{
    enable_video = en;
}

uint8_t *msg_flip(void)
{
    /* The caller has just finished rendering into fb[cur]; push it to the
     * panel, then hand out the other buffer for the next frame. */
    if (enable_video && s_ready && fb[cur] != NULL) {
        const uint8_t *src = fb[cur];
        it_ld_img_area_begin(EPD_VIDEO_WIN_X, EPD_VIDEO_WIN_Y,
                             EPD_VIDEO_WIN_W, EPD_VIDEO_WIN_H);
        for (int ry = 0; ry < EPD_VIDEO_WIN_H; ry++) {
            /* Each packed source row (logical GB x) is shown 3x vertically. */
            const uint8_t *row = src + (size_t)(ry / 3) * (EPD_VIDEO_WIDTH / 8);
            for (int x = 0; x < EPD_VIDEO_WIDTH; x += 4) {
                itw_raw(pack4(row, x, false));
            }
        }
        it_ld_img_area_end();
        it_update_area(EPD_VIDEO_WIN_X, EPD_VIDEO_WIN_Y,
                       EPD_VIDEO_WIN_W, EPD_VIDEO_WIN_H, UPDATE_MODE_DU);
    }

    cur ^= 1;
    s_vsync_count++;
    return fb[cur];
}

uint8_t *msg_flip_nowait(void)
{
    /* Return the current render buffer without touching the panel. */
    return fb[cur];
}

uint32_t msg_get_vsync_count(void)
{
    return s_vsync_count;
}

#endif /* PLAT_M5PAPER */
