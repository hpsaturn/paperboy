//
// reset_button.h — physical "reset to main menu" button (M5Paper dial switch).
//
// On the M5Paper the centre press of the left-edge "dial switch" (GPIO38) is
// the only user button the firmware uses.  Real power on/off is a hardware
// function on that board: running on battery the physical reset/power path
// already cuts the main-power rail, so the firmware no longer maps a software
// shutdown onto this pin.
//
// Instead a long press asks for a full device reset: the running loop flushes
// its state and calls esp_restart(), which brings the firmware straight back
// to the ROM picker (the main menu).
//
// There is no equivalent button wired on the M5PaperS3, so on that target
// these calls are no-ops.
//
#pragma once

#include <stdbool.h>

// Configure the reset-button GPIO (idempotent, safe if the pin is absent).
void reset_button_init(void);

// Start the low-priority polling task that watches for the long press.
void reset_button_start(void);

// Non-consuming: true once the user has held the reset button long enough to
// ask for a device reset.  The owner of the game/UI state performs the actual
// restart on its next loop iteration.
bool reset_button_requested(void);
