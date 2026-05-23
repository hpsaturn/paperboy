#pragma once

/*
 * Local build-time tuning knobs.
 *
 * Keep these in a normal header so iteration does not require regenerating
 * sdkconfig or touching the full Kconfig dependency graph.
 */

/*
 * Game Boy backend selection:
 *   0 = auto (prefer gbjit for supported ROMs, else Walnut)
 *   1 = Walnut-CGB
 *   2 = gbjit-xtensa
 */
#define PAPERBOY_GB_DEFAULT_BACKEND             0

/*
 * Headless test boot:
 *   Set PAPERBOY_TEST_ROM_PATH to an SD-card ROM path (for example
 *   "/sdcard/roms/sml.gb") to skip the picker UI and boot straight into
 *   gameplay. Useful for automated flash-and-run checks and benchmarks.
 */
#define PAPERBOY_TEST_ROM_PATH                  "/sdcard/game/tetris.gb"
#define PAPERBOY_TEST_LOAD_SNAPSHOT             0
#define PAPERBOY_TEST_FORCE_MUTE_AUDIO          0
#define PAPERBOY_TEST_BOOT_DELAY_MS             3000u

/*
 * Emit a one-shot benchmark line after N emulated frames. Set to 0 to disable.
 */
#define PAPERBOY_BENCHMARK_FRAMES               0u

// Currently frame time is 17.7ms (~57fps)
// Slightly lower than 59.7 so we are more likely to hit it
#define PAPERBOY_BUZZER_GPIO                    21

/*
 * PCM audio settings (used by audio.c / audio_pwm.c only).
 * Not referenced by audio_poly_pwm.c.
 */
#define PAPERBOY_AUDIO_RING_FRAMES              6u
#define PAPERBOY_AUDIO_TARGET_SAMPLES_PER_FRAME 704u
#define PAPERBOY_AUDIO_BUDGET_MS                2