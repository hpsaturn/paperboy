#pragma once

#include "gbemu.h"

typedef struct paperboy_gb_backend_ops {
    const char *name;
    bool (*is_rom_supported)(const uint8_t *rom, size_t rom_size);
    bool (*init)(const uint8_t *rom, size_t rom_size);
    void (*set_buttons)(uint8_t pressed_mask);
    bool (*run_frame)(uint8_t *fb, bool skip_render);
    bool (*has_persist)(void);
    size_t (*persist_size)(void);
    bool (*persist_is_dirty)(void);
    bool (*persist_export)(uint8_t *dst, size_t dst_size, uint32_t timestamp);
    bool (*persist_import)(const uint8_t *src, size_t src_size, uint32_t current_timestamp);
    void (*persist_mark_clean)(void);
    size_t (*state_size)(void);
    bool (*state_export)(uint8_t *dst, size_t dst_size);
    bool (*state_import)(const uint8_t *src, size_t src_size);
    bool (*is_ready)(void);
    const char *(*last_error)(void);
    bool (*get_stats)(paperboy_gb_stats_t *stats);
} paperboy_gb_backend_ops_t;

const paperboy_gb_backend_ops_t *paperboy_gb_walnut_backend(void);
const paperboy_gb_backend_ops_t *paperboy_gb_gbjit_backend(void);

void paperboy_gb_blit_line(uint8_t *fb, const uint8_t *pixels, uint_fast8_t line);
