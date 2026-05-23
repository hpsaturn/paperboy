#include <string.h>

#include "esp_log.h"

#include "gbemu_internal.h"
#include "paperboy_config.h"

static const char *TAG = "gbemu";

static const paperboy_gb_backend_ops_t *s_backend;
static char s_last_error[128] = "not initialized";

static void set_last_error(const char *msg)
{
    strlcpy(s_last_error, msg, sizeof(s_last_error));
}

static const paperboy_gb_backend_ops_t *select_backend(const uint8_t *rom, size_t rom_size)
{
    const paperboy_gb_backend_ops_t *walnut = paperboy_gb_walnut_backend();
    const paperboy_gb_backend_ops_t *gbjit = paperboy_gb_gbjit_backend();
    return gbjit;
    // switch (PAPERBOY_GB_DEFAULT_BACKEND) {
    // case PAPERBOY_GB_BACKEND_WALNUT:
    //     return walnut;
    // case PAPERBOY_GB_BACKEND_GBJIT:
    //     if (gbjit->is_rom_supported(rom, rom_size)) {
    //         return gbjit;
    //     }
    //     ESP_LOGW(TAG, "gbjit backend does not support this ROM, falling back to Walnut");
    //     return walnut;
    // case PAPERBOY_GB_BACKEND_AUTO:
    // default:
    //     if (gbjit->is_rom_supported(rom, rom_size)) {
    //         return gbjit;
    //     }
    //     return walnut;
    // }
}

bool paperboy_gb_init(const uint8_t *rom, size_t rom_size)
{
    s_backend = select_backend(rom, rom_size);
    if (!s_backend->init(rom, rom_size)) {
        set_last_error(s_backend->last_error());
        return false;
    }

    set_last_error("ok");
    ESP_LOGI(TAG, "Using %s backend", s_backend->name);
    return true;
}

void paperboy_gb_set_buttons(uint8_t pressed_mask)
{
    if (s_backend == NULL) {
        return;
    }
    s_backend->set_buttons(pressed_mask);
}

bool paperboy_gb_run_frame(uint8_t *fb, bool skip_render)
{
    if (s_backend == NULL) {
        set_last_error("backend not initialized");
        return false;
    }

    if (!s_backend->run_frame(fb, skip_render)) {
        set_last_error(s_backend->last_error());
        return false;
    }

    set_last_error("ok");
    return true;
}

bool paperboy_gb_has_persist(void)
{
    return s_backend != NULL && s_backend->has_persist();
}

size_t paperboy_gb_persist_size(void)
{
    return s_backend != NULL ? s_backend->persist_size() : 0;
}

bool paperboy_gb_persist_is_dirty(void)
{
    return s_backend != NULL && s_backend->persist_is_dirty();
}

bool paperboy_gb_persist_export(uint8_t *dst, size_t dst_size, uint32_t timestamp)
{
    if (s_backend == NULL) {
        set_last_error("backend not initialized");
        return false;
    }
    if (!s_backend->persist_export(dst, dst_size, timestamp)) {
        set_last_error(s_backend->last_error());
        return false;
    }
    set_last_error("ok");
    return true;
}

bool paperboy_gb_persist_import(const uint8_t *src, size_t src_size, uint32_t current_timestamp)
{
    if (s_backend == NULL) {
        set_last_error("backend not initialized");
        return false;
    }
    if (!s_backend->persist_import(src, src_size, current_timestamp)) {
        set_last_error(s_backend->last_error());
        return false;
    }
    set_last_error("ok");
    return true;
}

void paperboy_gb_persist_mark_clean(void)
{
    if (s_backend != NULL) {
        s_backend->persist_mark_clean();
    }
}

size_t paperboy_gb_state_size(void)
{
    return s_backend != NULL ? s_backend->state_size() : 0;
}

bool paperboy_gb_state_export(uint8_t *dst, size_t dst_size)
{
    if (s_backend == NULL) {
        set_last_error("backend not initialized");
        return false;
    }
    if (!s_backend->state_export(dst, dst_size)) {
        set_last_error(s_backend->last_error());
        return false;
    }
    set_last_error("ok");
    return true;
}

bool paperboy_gb_state_import(const uint8_t *src, size_t src_size)
{
    if (s_backend == NULL) {
        set_last_error("backend not initialized");
        return false;
    }
    if (!s_backend->state_import(src, src_size)) {
        set_last_error(s_backend->last_error());
        return false;
    }
    set_last_error("ok");
    return true;
}

bool paperboy_gb_is_ready(void)
{
    return s_backend != NULL && s_backend->is_ready();
}

const char *paperboy_gb_last_error(void)
{
    if (s_backend == NULL) {
        return s_last_error;
    }
    return s_backend->last_error();
}

const char *paperboy_gb_backend_name(void)
{
    return s_backend != NULL ? s_backend->name : "none";
}

bool paperboy_gb_get_stats(paperboy_gb_stats_t *stats)
{
    if (stats == NULL || s_backend == NULL || s_backend->get_stats == NULL) {
        return false;
    }
    return s_backend->get_stats(stats);
}
