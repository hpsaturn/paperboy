//
// MSG
// Copyright 2026 Wenting Zhang
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Board / platform selection (defines PLAT_M5PAPER or PLAT_M5PAPERS3 and the
// pin map).  The same API below is implemented by two backends:
//   msg/msg.c        — M5PaperS3 bare EPD parallel bus ("MSG" engine)
//   msg/msg_it8951.c — M5Paper IT8951 SPI controller
#include "board.h"

/* Panel geometry (common to both boards). */
#define EPD_WIDTH           960
#define EPD_HEIGHT          540

/* Emulated-screen ("video") window, in panel pixels and in the packed 1bpp
 * buffer layout produced by the emulator core. */
#define EPD_VIDEO_WIDTH     (144*3)
#define EPD_VIDEO_HEIGHT    (160)

#define EPD_FB_SIZE         (EPD_WIDTH * EPD_HEIGHT / 8)
#define EPD_VIDEO_FB_SIZE   (EPD_VIDEO_WIDTH * EPD_VIDEO_HEIGHT / 8)

#define MSG_TASK_PRIORITY   1

void msg_init(void);
void msg_start(void);
void msg_display_image(uint8_t *img, bool to_white);
void msg_enable_video(bool en);
// Block until the next VSYNC, swap buffers, and return the new back-buffer.
uint8_t *msg_flip(void);
// Return the current back-buffer immediately without waiting for VSYNC or
// swapping buffers.  The EPD keeps displaying the last submitted front-buffer.
// Use when a rendered frame will be skipped to catch up after a missed VSYNC.
uint8_t *msg_flip_nowait(void);
// Monotonic counter incremented at every EPD frame-start (VSYNC).
// Read before and after rendering to detect a missed VSYNC.
uint32_t msg_get_vsync_count(void);

// Turn the board off.  Stops the display path, powers down the panel and (on
// M5Paper) opens the main-power latch (GPIO2 low).  Does not return on hardware
// where the latch actually cuts power.
void msg_power_off(void);
