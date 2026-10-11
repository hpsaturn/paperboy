#pragma once

/*
 * Local build-time tuning knobs.
 *
 * Keep these in a normal header so iteration does not require regenerating
 * sdkconfig or touching the full Kconfig dependency graph.
 */

// Buzzer/speaker GPIO is defined per board in board.h (M5PaperS3 = GPIO21,
// original M5Paper = GPIO19 on the expansion header; the M5Paper has no
// on-board speaker).
#include "board.h"
#define PAPERBOY_BUZZER_GPIO                    BOARD_BUZZER_GPIO

/*
 * PCM audio settings (used by audio.c / audio_pwm.c only).
 * Not referenced by audio_poly_pwm.c.
 */
#define PAPERBOY_AUDIO_RING_FRAMES              6u
#define PAPERBOY_AUDIO_TARGET_SAMPLES_PER_FRAME 704u
#define PAPERBOY_AUDIO_BUDGET_MS                2

/*
 * Long-window profiler averaging for emulator core comparisons.
 * Set to 0 to disable the extra summary log.
 */
#define PAPERBOY_PROF_SPEED_WINDOW_MS           120000u