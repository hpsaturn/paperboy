//
// power_button.h — physical power button (M5Paper dial-switch centre).
//
// On the M5Paper the centre press of the left-edge "dial switch" (GPIO38) is
// the POWER button: it powers the board on in hardware, and a long press while
// running is turned into a clean software shutdown.  There is no equivalent
// button wired on the M5PaperS3, so on that target these calls are no-ops.
//
#pragma once

#include <stdbool.h>

// Configure the power-button GPIO (idempotent, safe if the pin is absent).
void power_button_init(void);

// Start the low-priority polling task that watches for the long press.
void power_button_start(void);

// Non-consuming: true once the user has held the power button long enough to
// ask for a shutdown.  The owner of the game/UI state performs the actual
// power-off (save + latch release) on its next loop iteration.
bool power_button_off_requested(void);
