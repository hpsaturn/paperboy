//
// reset_button.c — M5Paper "reset to main menu" button.
//
// The M5Paper has a three-way "dial switch" on the left edge (M5Stack M5Paper
// pin map, section "Dial Switch"):
//
//      rotate left   -> GPIO37   (user button, unused)
//      press centre  -> GPIO38   firmware "reset" button
//      rotate right  -> GPIO39   (user button, unused)
//
// Power on/off is handled by the hardware on this board: on battery the
// physical reset/power path already opens the main-power rail, so the firmware
// no longer needs a software shutdown on the dial switch.  Instead a long
// press here asks the application for a device reset; the running loop flushes
// its state and calls esp_restart(), returning the firmware to the ROM picker
// (the main menu).
//
// GPIO34..39 are input-only on the classic ESP32 and have no internal
// pull-up/down, so the dial relies on the board's external pull-ups.  Enabling
// an internal pull here would only log a "gpio_pullup_en: GPIO number error".
//
#include "reset_button.h"
#include "board.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "rstbtn";

#if defined(PLAT_M5PAPER) && defined(BOARD_RESET_BTN_PIN)

#define RST_BTN_GPIO   BOARD_RESET_BTN_PIN
#define RST_ACTIVE_LVL 0                 /* pressed = low (external pull-up) */
#define RST_HOLD_MS    2000              /* hold time before a reset is asked */
#define RST_POLL_MS    20
#define RST_TASK_STACK 2560
#define RST_TASK_PRIO  2

static volatile bool s_reset_req;

static bool rst_pressed(void)
{
    return gpio_get_level((gpio_num_t)RST_BTN_GPIO) == RST_ACTIVE_LVL;
}

static void reset_button_task(void *arg)
{
    (void)arg;

    /* Do not act on a press until the button has been released at least once.
     * The press that powered the board on is often still being held when this
     * task starts; requiring a release avoids resetting straight after boot. */
    while (rst_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(RST_POLL_MS));
    }

    int64_t press_start = 0;

    for (;;) {
        if (rst_pressed()) {
            if (press_start == 0) {
                press_start = esp_timer_get_time();
            } else if ((esp_timer_get_time() - press_start) >=
                       (int64_t)RST_HOLD_MS * 1000) {
                ESP_LOGW(TAG, "Reset button held %d ms - returning to main menu",
                         RST_HOLD_MS);
                s_reset_req = true;
                press_start = 0;
                /* Wait for release so a single long press fires once. */
                while (rst_pressed()) {
                    vTaskDelay(pdMS_TO_TICKS(RST_POLL_MS));
                }
            }
        } else {
            press_start = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(RST_POLL_MS));
    }
}

void reset_button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << RST_BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,   /* input-only pad, no int PU */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gpio_config(GPIO%d) failed: %s",
                 RST_BTN_GPIO, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Reset button on GPIO%d (idle level %d, pressed = %d)",
             RST_BTN_GPIO, gpio_get_level((gpio_num_t)RST_BTN_GPIO),
             RST_ACTIVE_LVL);
}

void reset_button_start(void)
{
    xTaskCreatePinnedToCore(reset_button_task, "rstbtn", RST_TASK_STACK,
                            NULL, RST_TASK_PRIO, NULL, 0);
}

bool reset_button_requested(void)
{
    return s_reset_req;
}

#else  /* !PLAT_M5PAPER — no such button on this board */

void reset_button_init(void)    { }
void reset_button_start(void)   { }
bool reset_button_requested(void) { return false; }

#endif
