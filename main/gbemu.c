#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include <esp_attr.h>

#include "gbemu.h"
#include "gbemu_internal.h"
#include "audio.h"
#include "profiler.h"

/* Walnut-CGB feature flags – must be set before including walnut_cgb.h */
#define WALNUT_FULL_GBC_SUPPORT 0   /* DMG-only, no CGB colour mode */
#define WALNUT_GB_12_COLOUR     0   /* must match WALNUT_FULL_GBC_SUPPORT */
#define WALNUT_GB_32BIT_DMA     1   /* ESP32-S3 handles unaligned 32-bit fine */
#define ENABLE_SOUND            1   /* Enable minigb_apu audio */
#define ENABLE_LCD              1

/* Forward declarations for audio callbacks used by walnut_cgb.h */
uint8_t audio_read(uint16_t addr);
void audio_write(uint16_t addr, uint8_t val);

#include "walnut_cgb.h"

static const char *TAG = "gbemu";

static struct gb_s s_gb;
static const uint8_t *s_rom;
static size_t s_rom_size;
static uint8_t *s_cart_ram;
static size_t s_cart_ram_size;
static uint8_t *s_framebuffer;
static bool s_ready;
static char s_last_error[128];
static uint32_t s_frame_count;

struct persist_header {
    uint8_t magic[4];
    uint8_t version;
    uint8_t has_cart_ram;
    uint8_t has_rtc;
    uint8_t reserved;
    uint32_t cart_ram_size;
    uint32_t timestamp;
};

#define PERSIST_VERSION 1u
static const uint8_t PERSIST_MAGIC[4] = {'P', 'B', 'S', 'V'};

static void set_last_error(const char *msg)
{
    strlcpy(s_last_error, msg, sizeof(s_last_error));
}

void paperboy_gb_blit_line(uint8_t *fb, const uint8_t *pixels, uint_fast8_t line)
{
    int target_x = (143 - line) * 3;
    uint8_t *wrptr = &(fb[target_x / 8]);
    int offset_x = target_x % 8;
    bool crossing = offset_x >= 6;
    uint32_t stride = crossing ? 53 : 54;
    uint8_t clrmask;
    uint8_t setmask[4];
    uint8_t clrmask_crossing;
    uint8_t setmask_crossing[4];
    const uint16_t dithermap[4] = {0x0000, 0x2000, 0x6000, 0xe000};
    uint16_t tmp;

    tmp = 0xe000 >> offset_x;
    clrmask = ~(tmp >> 8);
    clrmask_crossing = ~(tmp & 0xff);
    for (int i = 0; i < 4; i++) {
        tmp = dithermap[i] >> offset_x;
        setmask[i] = tmp >> 8;
        setmask_crossing[i] = tmp & 0xff;
    }

    if (!crossing) {
        for (int i = 0; i < 160; i++) {
            uint8_t b = *wrptr;
            b &= clrmask;
            b |= setmask[pixels[i] & 0x03u];
            *wrptr = b;
            wrptr += stride;
        }
    } else {
        for (int i = 0; i < 160; i++) {
            uint8_t b = *wrptr;
            b &= clrmask;
            b |= setmask[pixels[i] & 0x03u];
            *wrptr++ = b;
            b = *wrptr;
            b &= clrmask_crossing;
            b |= setmask_crossing[pixels[i] & 0x03u];
            *wrptr = b;
            wrptr += stride;
        }
    }
}

static bool gb_has_persist_data(void)
{
    // return s_cart_ram_size > 0 || s_gb.cart_battery;
    return false;
}

static size_t gb_persist_rtc_size(void)
{
    // return s_gb.cart_battery ? sizeof(s_gb.cart_rtc) : 0u;
    return 0;
}

/* Audio I/O callbacks – called by Walnut-CGB when accessing APU registers.
 * The APU state lives in audio.c; route through its public API. */
uint8_t audio_read(uint16_t addr)
{
    return audio_apu_read(addr);
}

void audio_write(uint16_t addr, uint8_t val)
{
    audio_apu_write(addr, val);
}

static IRAM_ATTR uint8_t gb_rom_read_cb(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;

    if (addr < s_rom_size) {
        return s_rom[addr];
    }

    return 0xFF;
}

static IRAM_ATTR uint16_t gb_rom_read16_cb(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;

    const uint8_t *src = &s_rom[addr];
    // Alignment check, not required for all platforms. ESP32 series mcu flash memory and psram sources *require* this
    if ((uintptr_t)src & 1) {
        // fallback to safe 8-bit reads when not aligned
        return ((uint16_t)src[0]) | ((uint16_t)src[1] << 8);          
    } 
    return *(uint16_t *)src;
}

static IRAM_ATTR uint32_t gb_rom_read32_cb(struct gb_s *gb, const uint_fast32_t addr)
{
    const uint8_t *src = &s_rom[addr];

    // Alignment check: ESP32 flash / PSRAM require 32-bit alignment
    if ((uintptr_t)src & 3) {
        // fallback to safe 8-bit reads when not aligned
        return ((uint32_t)src[0]) |
               ((uint32_t)src[1] << 8) |
               ((uint32_t)src[2] << 16) |
               ((uint32_t)src[3] << 24);
    }

    return *(uint32_t *)src;
}

static IRAM_ATTR uint8_t gb_cart_ram_read_cb(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;

    if (s_cart_ram == NULL || s_cart_ram_size == 0 || addr >= s_cart_ram_size) {
        return 0xFF;
    }

    return s_cart_ram[addr];
}

static IRAM_ATTR void gb_cart_ram_write_cb(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val)
{
    (void)gb;

    if (s_cart_ram == NULL || s_cart_ram_size == 0 || addr >= s_cart_ram_size) {
        return;
    }

    s_cart_ram[addr] = val;
}

static void gb_error_cb(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr)
{
    (void)gb;

    ESP_LOGE(TAG, "Emulator error=%d at addr=0x%04x", (int)err, addr);
    set_last_error("runtime core error");
}

static IRAM_ATTR void lcd_draw_line_cb(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line)
{
    (void)gb;

    PROF_BEGIN(PROF_LCD);

    if (line >= GB_LCD_HEIGHT) {
        PROF_END(PROF_LCD);
        return;
    }

    paperboy_gb_blit_line(s_framebuffer, pixels, line);

    PROF_END(PROF_LCD);
}

static bool walnut_init(const uint8_t *rom, size_t rom_size)
{
    enum gb_init_error_e init_err;
    size_t save_size = 0;
    char rom_name[17];

    s_ready = false;
    set_last_error("not initialized");

    if (rom == NULL || rom_size < 0x150) {
        set_last_error("rom buffer missing or too small");
        return false;
    }

    memset(&s_gb, 0, sizeof(s_gb));
    s_frame_count = 0;

    s_rom = rom;
    s_rom_size = rom_size;

    free(s_cart_ram);
    s_cart_ram = NULL;
    s_cart_ram_size = 0;

    init_err = gb_init(&s_gb,
                       gb_rom_read_cb,
                       gb_rom_read16_cb,
                       gb_rom_read32_cb,
                       gb_cart_ram_read_cb,
                       gb_cart_ram_write_cb,
                       gb_error_cb,
                       NULL);

    if (init_err != GB_INIT_NO_ERROR) {
        ESP_LOGE(TAG, "gb_init failed: %d", (int)init_err);
        set_last_error("gb_init failed");
        return false;
    }

    if (gb_get_save_size_s(&s_gb, &save_size) == 0 && save_size > 0) {
        s_cart_ram = calloc(1, save_size);
        if (s_cart_ram == NULL) {
            set_last_error("cart ram alloc failed");
            return false;
        }
        s_cart_ram_size = save_size;
    }

    #if ENABLE_LCD
    gb_init_lcd(&s_gb, lcd_draw_line_cb);
    #endif
    s_gb.direct.joypad = 0xFF;

    /* Initialize audio system (also inits the APU) */
    audio_init();

    ESP_LOGI(TAG, "Walnut-CGB initialized. ROM title: %s", gb_get_rom_name(&s_gb, rom_name));

    s_ready = true;
    set_last_error("ok");
    return true;
}

static void walnut_set_buttons(uint8_t pressed_mask)
{
    if (!s_ready) {
        return;
    }

    s_gb.direct.joypad = (uint8_t)(~pressed_mask);
}

static bool walnut_run_frame(uint8_t *fb, bool skip_render)
{
    if (!s_ready) {
        return false;
    }

    if (skip_render) {
        /* Tell the emulator core to skip all pixel rendering this frame.
         * __gb_draw_line() returns immediately when frame_skip is true and
         * frame_skip_count is 0, skipping all sprite/background work. */
        s_gb.direct.frame_skip = true;
        s_gb.display.frame_skip_count = 0;
    } else {
        s_framebuffer = fb;
    }

    gb_run_frame_dualfetch(&s_gb);

    /* Restore frame_skip to off so normal rendering resumes next frame. */
    s_gb.direct.frame_skip = false;
    s_gb.display.frame_skip_count = 0;

    /* Audio synthesis is now driven by the audio task at the I2S hardware
     * clock rate, decoupled from emulation speed.  Nothing to do here. */

    s_frame_count++;
    return true;
}

static bool walnut_has_persist(void)
{
    if (!s_ready) {
        return false;
    }

    return gb_has_persist_data();
}

static size_t walnut_persist_size(void)
{
    if (!s_ready || !gb_has_persist_data()) {
        return 0;
    }

    return sizeof(struct persist_header) + s_cart_ram_size + gb_persist_rtc_size();
}

static bool walnut_persist_is_dirty(void)
{
    if (!s_ready || !gb_has_persist_data()) {
        return false;
    }

    return true; // no counting
    // return s_gb.direct.sram_updated != 0;
}

static bool walnut_persist_export(uint8_t *dst, size_t dst_size, uint32_t timestamp)
{
    struct persist_header header;
    uint8_t *cursor;

    if (!s_ready || !gb_has_persist_data() || dst == NULL) {
        set_last_error("persist export unavailable");
        return false;
    }

    if (dst_size < walnut_persist_size()) {
        set_last_error("persist export buffer too small");
        return false;
    }

    memcpy(header.magic, PERSIST_MAGIC, sizeof(header.magic));
    header.version = PERSIST_VERSION;
    header.has_cart_ram = (uint8_t)(s_cart_ram_size > 0);
    // header.has_rtc = (uint8_t)(s_gb.cart_battery != 0);
    header.reserved = 0;
    header.cart_ram_size = (uint32_t)s_cart_ram_size;
    header.timestamp = timestamp;

    memcpy(dst, &header, sizeof(header));
    cursor = dst + sizeof(header);

    if (s_cart_ram_size > 0) {
        memcpy(cursor, s_cart_ram, s_cart_ram_size);
        cursor += s_cart_ram_size;
    }

    // if (s_gb.cart_battery) {
    //     memcpy(cursor, s_gb.cart_rtc, sizeof(s_gb.cart_rtc));
    // }

    set_last_error("ok");
    return true;
}

static bool walnut_persist_import(const uint8_t *src, size_t src_size, uint32_t current_timestamp)
{
    struct persist_header header;
    const uint8_t *cursor;

    if (!s_ready || !gb_has_persist_data() || src == NULL) {
        set_last_error("persist import unavailable");
        return false;
    }

    if (src_size < sizeof(header)) {
        set_last_error("persist blob too small");
        return false;
    }

    memcpy(&header, src, sizeof(header));
    if (memcmp(header.magic, PERSIST_MAGIC, sizeof(header.magic)) != 0 ||
        header.version != PERSIST_VERSION) {
        set_last_error("persist blob format mismatch");
        return false;
    }

    if ((header.has_cart_ram != 0) != (s_cart_ram_size > 0) || header.cart_ram_size != s_cart_ram_size) {
        set_last_error("persist cart ram mismatch");
        return false;
    }

    // if ((header.has_rtc != 0) != (s_gb.cart_battery != 0)) {
    //     set_last_error("persist rtc mismatch");
    //     return false;
    // }

    // if (src_size != sizeof(header) + header.cart_ram_size + (header.has_rtc ? sizeof(s_gb.cart_rtc) : 0u)) {
    if (src_size != sizeof(header) + header.cart_ram_size) {
        set_last_error("persist blob size mismatch");
        return false;
    }

    cursor = src + sizeof(header);
    if (header.cart_ram_size > 0) {
        memcpy(s_cart_ram, cursor, header.cart_ram_size);
        cursor += header.cart_ram_size;
    }

    // if (header.has_rtc) {
    //     memcpy(s_gb.cart_rtc, cursor, sizeof(s_gb.cart_rtc));
    //     if (header.timestamp > 0 && current_timestamp >= header.timestamp) {
    //         gb_catch_up_rtc_direct(&s_gb, current_timestamp - header.timestamp);
    //     }
    //     memcpy(s_gb.latched_rtc, s_gb.cart_rtc, sizeof(s_gb.latched_rtc));
    // }

    // s_gb.direct.sram_updated = 0;
    // s_gb.direct.sram_dirty = 0;
    set_last_error("ok");
    return true;
}

static void walnut_persist_mark_clean(void)
{
    if (!s_ready) {
        return;
    }

    // s_gb.direct.sram_updated = 0;
    // s_gb.direct.sram_dirty = 0;
}

static size_t walnut_state_size(void)
{
    if (!s_ready) {
        return 0;
    }

    // return gb_get_state_size(&s_gb);
    return 0;
}

static bool walnut_state_export(uint8_t *dst, size_t dst_size)
{
    const size_t state_size = walnut_state_size();

    if (!s_ready || dst == NULL) {
        set_last_error("state export unavailable");
        return false;
    }

    if (dst_size < state_size) {
        set_last_error("state export buffer too small");
        return false;
    }

    // gb_state_save(&s_gb, (char *)dst);
    set_last_error("ok");
    return true;
}

static bool walnut_state_import(const uint8_t *src, size_t src_size)
{
    const char *result;

    if (!s_ready || src == NULL) {
        set_last_error("state import unavailable");
        return false;
    }

    // result = gb_state_load(&s_gb, (const char *)src, src_size);
    // if (result != NULL) {
    //     set_last_error(result);
    //     return false;
    // }

    set_last_error("ok");
    return true;
}

static bool walnut_is_ready(void)
{
    return s_ready;
}

static const char *walnut_last_error(void)
{
    return s_last_error;
}

static bool walnut_get_stats(paperboy_gb_stats_t *stats)
{
    if (stats == NULL) {
        return false;
    }

    memset(stats, 0, sizeof(*stats));
    stats->frame_count = s_frame_count;
    return true;
}

static bool walnut_is_rom_supported(const uint8_t *rom, size_t rom_size)
{
    (void)rom;
    return rom_size >= 0x150;
}

const paperboy_gb_backend_ops_t *paperboy_gb_walnut_backend(void)
{
    static const paperboy_gb_backend_ops_t ops = {
        .name = "walnut",
        .is_rom_supported = walnut_is_rom_supported,
        .init = walnut_init,
        .set_buttons = walnut_set_buttons,
        .run_frame = walnut_run_frame,
        .has_persist = walnut_has_persist,
        .persist_size = walnut_persist_size,
        .persist_is_dirty = walnut_persist_is_dirty,
        .persist_export = walnut_persist_export,
        .persist_import = walnut_persist_import,
        .persist_mark_clean = walnut_persist_mark_clean,
        .state_size = walnut_state_size,
        .state_export = walnut_state_export,
        .state_import = walnut_state_import,
        .is_ready = walnut_is_ready,
        .last_error = walnut_last_error,
        .get_stats = walnut_get_stats,
    };

    return &ops;
}
