//
// msg_it8951.c — MSG API backend for the original M5Paper (ESP32-D0WDQ6-V3).
//
// This board does not expose the bare EPD; instead an IT8951E controller sits
// between the ESP32 and the ED047TC1 panel, spoken to over SPI:
//
//     ESP32 (SPI2) ──SCK/MOSI/MISO+CS── IT8951E ── TCON ──> ED047TC1
//
// Architecture — mirrors the M5PaperS3 "MSG" engine (msg/msg.c):
//
//   * msg_start() spawns a dedicated display task pinned to CPU1 which owns the
//     SPI bus and the panel.  The task blocks until a completed frame is
//     published, snapshots it, then pushes it to the IT8951 (4bpp area load +
//     DU refresh), and drops frames the panel cannot keep up with.
//
//   * The emulator (app_main, CPU0) renders into a scratch buffer and calls
//     msg_flip().  msg_flip() hands a copy of the finished frame to the display
//     task and returns the scratch buffer again, so emulation and panel I/O run
//     concurrently on the two cores -- exactly the split the S3 backend has
//     between app_main and msg_update_task.
//
//   * msg_flip() is paced by a 60 Hz emulated-frame clock (the Game Boy frame
//     rate), not by the panel.  On the S3 the panel VSYNC *is* the frame clock
//     (57 fps) and the two coincide; here one refresh takes ~300 ms (~3 fps),
//     so tying the emulator to the panel would run the game in 20x slow motion.
//     Instead the game logic (and audio) run at their correct speed and the
//     display task shows the freshest frame it has.
//
// Low-level SPI framing (0x6000 command / 0x0000 data / 0x1000 read preambles,
// LD_IMG_AREA 4bpp "B" endian loads, DPY_BUF_AREA refresh) follows M5Stack's
// M5EPD driver, the shipping driver for this exact hardware.
//
// The 4bpp wire encoding uses 0x0 = black, 0xF = white.
//
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "board.h"
#include "msg.h"
#include "audio.h"

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

/* IT8951 memory address used for image loads (M5EPD default). */
#define IT8951_MEM_ADDR             0x001236E0u

/* Emulated frame period: 60 Hz, the Game Boy frame rate.  This is the pacing
 * clock handed to the emulator (see msg_flip). */
#define IT_FRAME_US                 16667

/* ── State ─────────────────────────────────────────────────────────────── */
static spi_device_handle_t s_spi;
static volatile uint32_t   s_vsync_count;

/* Frame hand-off buffers.
 *
 *   s_render  — scratch the emulator draws into; msg_flip() always returns it.
 *               The emulator fully redraws EPD_VIDEO_FB_SIZE every frame, so a
 *               single buffer is sufficient.
 *   s_pending — copy of the most recently finished frame (guarded by
 *               s_pub_mutex).  Written by msg_flip(), read by the display task.
 *   s_push    — display task private snapshot, stable for the whole (slow)
 *               panel refresh.
 */
static uint8_t *s_render;
static uint8_t *s_pending;
static uint8_t *s_push;

static SemaphoreHandle_t s_pub_mutex;   /* guards s_pending                */
static SemaphoreHandle_t s_disp_sem;    /* frame ready -> display task     */
static SemaphoreHandle_t s_frame_sem;   /* 60 Hz frame clock -> msg_flip() */
static esp_timer_handle_t s_frame_timer;
static TaskHandle_t      s_disp_task;
static bool              s_ready;
static volatile bool     enable_video;

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
        /* The IT8951 holds HRDY low for the whole panel refresh (~300 ms).  A
         * pure busy loop would keep the CPU spinning the entire time; block for
         * one tick instead.  This runs in the core-1 display task, so IDLE1
         * gets scheduled and the task watchdog stays happy. */
        vTaskDelay(1);
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
        vTaskDelay(1);   /* real 10 ms block so IDLE1 is scheduled */
    } while (++guard < 500);
}

/* ── Streaming 4bpp image writer ───────────────────────────────────────── */

/* Send a run of 4bpp words as one CS-asserted burst (preamble once, chunks of
 * a DMA-capable internal buffer held together with SPI_TRANS_CS_KEEP_ACTIVE). */
#define ITW_CHUNK 8192
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

/* ── Panel writers (callers must be the only user of the SPI bus) ───────── */

/* Push one 432x160 packed frame into the 432x480 on-panel window. */
static void it_push_video_frame(const uint8_t *src)
{
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

/* Display task: owns the panel.  Waits for a published frame, snapshots it and
 * pushes it.  Because it only ever picks up the *latest* frame, frames produced
 * by the (much faster) emulator while a refresh is in flight are dropped. */
static void it_display_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_disp_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!s_ready || !enable_video) {
            continue;
        }

        /* Snapshot quickly, then release so the emulator can publish again. */
        xSemaphoreTake(s_pub_mutex, portMAX_DELAY);
        memcpy(s_push, s_pending, EPD_VIDEO_FB_SIZE);
        xSemaphoreGive(s_pub_mutex);

        it_push_video_frame(s_push);
    }
}

/* 60 Hz frame clock: bumps the vsync counter and releases the emulator. */
static void it_frame_tick(void *arg)
{
    (void)arg;
    s_vsync_count++;
    xSemaphoreGive(s_frame_sem);
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
        .max_transfer_sz = 8192,
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
    s_ready = false;
    enable_video = false;
    s_vsync_count = 0;

    board_epd_power(true);
    it_hw_init();

    s_pub_mutex = xSemaphoreCreateMutex();
    s_disp_sem  = xSemaphoreCreateBinary();
    s_frame_sem = xSemaphoreCreateBinary();
    if (s_pub_mutex == NULL || s_disp_sem == NULL || s_frame_sem == NULL) {
        ESP_LOGE(TAG, "semaphore allocation failed");
    }

    esp_timer_create_args_t targs = {
        .callback = it_frame_tick,
        .arg      = NULL,
        .name     = "epd_frame",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_frame_timer));

    /* MALLOC_CAP_8BIT is mandatory here.  MALLOC_CAP_INTERNAL alone may hand
     * out IRAM (0x4008_0000-0x400A_0000) on the original ESP32, which the CPU
     * can only read/write with 4-byte aligned word accesses.  Both the
     * emulator core and pack4() touch these buffers byte-wise, which aborts
     * with a LoadStoreError if a buffer lands in IRAM.  Fall back to PSRAM
     * (byte accessible) if internal DRAM cannot satisfy the request. */
    uint8_t **bufs[3] = { &s_render, &s_pending, &s_push };
    for (int i = 0; i < 3; i++) {
        *bufs[i] = (uint8_t *)heap_caps_aligned_calloc(16, 1, EPD_VIDEO_FB_SIZE,
                                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (*bufs[i] == NULL) {
            *bufs[i] = (uint8_t *)heap_caps_aligned_calloc(16, 1, EPD_VIDEO_FB_SIZE,
                                                           MALLOC_CAP_SPIRAM);
        }
        if (*bufs[i] == NULL) {
            ESP_LOGE(TAG, "failed to allocate frame buffer %d", i);
        } else {
            memset(*bufs[i], 0xFF, EPD_VIDEO_FB_SIZE);   /* start white */
        }
    }
}

void msg_start(void)
{
    /* Flash the panel to a clean white state (INIT waveform).  Runs before the
     * display task exists, so it is the only SPI user here. */
    it_ld_img_area_begin(0, 0, EPD_WIDTH, EPD_HEIGHT);
    for (int i = 0; i < (EPD_WIDTH * EPD_HEIGHT / 4); i++) {
        itw_raw(0xFFFF);
    }
    it_ld_img_area_end();
    it_update_area(0, 0, EPD_WIDTH, EPD_HEIGHT, UPDATE_MODE_INIT);

    s_ready = true;

    /* Start the emulated-frame clock (60 Hz). */
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_frame_timer, IT_FRAME_US));

    BaseType_t result = xTaskCreatePinnedToCore(it_display_task, "msg", 8192,
                                                NULL, MSG_TASK_PRIORITY,
                                                &s_disp_task, 1);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create display task");
    }

    ESP_LOGI(TAG, "Panel initialised (white), display task on CPU1");
}

void msg_display_image(uint8_t *img, bool to_white)
{
    if (img == NULL || !s_ready) {
        return;
    }

    /* Boot-time full-panel image.  Video is not enabled yet, so the display
     * task is idle; still serialise on the publish mutex for safety. */
    xSemaphoreTake(s_pub_mutex, portMAX_DELAY);

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

    xSemaphoreGive(s_pub_mutex);
}

void msg_enable_video(bool en)
{
    /* While video mode is off the display task drops any published frame
     * (mirrors the S3 backend, which streams blank lines until enabled). */
    enable_video = en;
}

uint8_t *msg_flip(void)
{
    /* Pace to the 60 Hz emulated frame clock (see file header).
     *
     * Two distinct cases must both yield to the scheduler:
     *
     *   1. The clock has not ticked yet (the normal case - the caller is
     *      faster than 60 fps).  Block until the next tick; the caller is
     *      therefore paced at 60 fps and spends most of the frame blocked,
     *      which is what lets IDLE0 run and keeps the task WDT fed.
     *
     *   2. The clock already ticked while the caller was busy (the caller is
     *      SLOWER than 60 fps - e.g. the ROM menu, which repaints the whole
     *      432x480 window pixel by pixel and can exceed the 16.7 ms budget).
     *      Here a plain take() would succeed immediately and never block, so
     *      this task would hold CPU0 at 100% with nothing ever yielding:
     *      IDLE0 never runs and the task watchdog fires after 5 s (seen as
     *      "IDLE0 (CPU 0) did not reset the watchdog", PC inside ui_put_pixel).
     *      Detect that case (the semaphore is already posted) and block for
     *      one tick explicitly so the idle task is always scheduled.
     */
    if (xSemaphoreTake(s_frame_sem, 0) != pdTRUE) {
        xSemaphoreTake(s_frame_sem, portMAX_DELAY);
    } else {
        vTaskDelay(1);   /* behind the frame clock: still yield to IDLE0 */
    }

    /* Hand the finished frame to the display task.  A copy is required because
     * the emulator immediately reuses s_render for the next frame. */
    if (s_ready && s_render != NULL && s_pending != NULL) {
        xSemaphoreTake(s_pub_mutex, portMAX_DELAY);
        memcpy(s_pending, s_render, EPD_VIDEO_FB_SIZE);
        xSemaphoreGive(s_pub_mutex);
        xSemaphoreGive(s_disp_sem);
    }

    /* The S3 display task services audio once per frame; here the frame clock is
     * the emulator's, so service it here (once per emulated frame, ~60 Hz). */
    audio_service_frame();

    return s_render;
}

uint8_t *msg_flip_nowait(void)
{
    /* Frame skipped: do not publish, do not wait for the frame clock. */
    audio_service_frame();
    return s_render;
}

uint32_t msg_get_vsync_count(void)
{
    return s_vsync_count;
}

#endif /* PLAT_M5PAPER */
