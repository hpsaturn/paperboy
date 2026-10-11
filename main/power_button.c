//
// power_button.c — M5Paper hardware power button.
//
// The M5Paper has a three-way "dial switch" on the left edge (M5Stack M5Paper
// pin map, section "Dial Switch"):
//
//      rotate left   -> GPIO37   (user button)
//      press centre  -> GPIO38   POWER BUTTON
//      rotate right  -> GPIO39   (user button)
//
// Behaviour:
//   * Unit OFF  -> pressing the button re-applies the main-power rail in
//                  hardware, so the board boots.  Firmware does nothing.
//   * Unit ON   -> a long press (>= PWR_HOLD_MS) triggers a clean shutdown:
//                  the game/UI owner saves state, flushes config, unmounts the
//                  SD card and then opens the MAIN_PWR latch (GPIO2 low), which
//                  drops the board to its quiescent microamp level.
//
// A capacitive touch on the panel can NOT switch the unit back on: once the
// latch is open the GT911 and the whole panel are unpowered.  Only the
// physical button (or USB) can wake it.
//
// GPIO34..39 are input-only on the classic ESP32 and have no internal
// pull-up/down, so the dial relies on the board's external pull-ups.  Enabling
// an internal pull here would only log a "gpio_pullup_en: GPIO number error".
//
#include "power_button.h"
#include "board.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "pwrbtn";

#if defined(PLAT_M5PAPER) && defined(BOARD_PWR_BTN_PIN)

#define PWR_BTN_GPIO   BOARD_PWR_BTN_PIN
#define PWR_ACTIVE_LVL 0                 /* pressed = low (external pull-up) */
#define PWR_HOLD_MS    2000              /* same long-press as M5Stack's fw  */
#define PWR_POLL_MS    20
#define PWR_TASK_STACK 2560
#define PWR_TASK_PRIO  2

static volatile bool s_off_req;

static bool pwr_pressed(void)
{
    return gpio_get_level((gpio_num_t)PWR_BTN_GPIO) == PWR_ACTIVE_LVL;
}

static void power_button_task(void *arg)
{
    (void)arg;

    /* Do not act on a press until the button has been released at least once.
     * The very press that powered the board on is often still being held when
     * this task starts; requiring a release makes the button behave like a
     * clean push-to-off / push-to-on toggle instead of shutting straight down
     * after boot. */
    while (pwr_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(PWR_POLL_MS));
    }

    int64_t press_start = 0;

    for (;;) {
        if (pwr_pressed()) {
            if (press_start == 0) {
                press_start = esp_timer_get_time();
            } else if ((esp_timer_get_time() - press_start) >=
                       (int64_t)PWR_HOLD_MS * 1000) {
                ESP_LOGW(TAG, "Power button held %d ms - requesting shutdown",
                         PWR_HOLD_MS);
                s_off_req   = true;
                press_start = 0;
                /* Wait for release so a single long press fires once. */
                while (pwr_pressed()) {
                    vTaskDelay(pdMS_TO_TICKS(PWR_POLL_MS));
                }
            }
        } else {
            press_start = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(PWR_POLL_MS));
    }
}

void power_button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PWR_BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,   /* input-only pad, no int PU */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gpio_config(GPIO%d) failed: %s",
                 PWR_BTN_GPIO, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Power button on GPIO%d (idle level %d, pressed = %d)",
             PWR_BTN_GPIO, gpio_get_level((gpio_num_t)PWR_BTN_GPIO),
             PWR_ACTIVE_LVL);
}

void power_button_start(void)
{
    xTaskCreatePinnedToCore(power_button_task, "pwrbtn", PWR_TASK_STACK,
                            NULL, PWR_TASK_PRIO, NULL, 0);
}

bool power_button_off_requested(void)
{
    return s_off_req;
}

#else  /* !PLAT_M5PAPER — no software power button on this board */

void power_button_init(void)  { }
void power_button_start(void) { }
bool power_button_off_requested(void) { return false; }

#endif
