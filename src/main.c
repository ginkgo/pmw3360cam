/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "bsp/board_api.h"
#include "tusb.h"
#include "usb_descriptors.h"

#include "pico/multicore.h"

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF PROTYPES
//--------------------------------------------------------------------+

/* Blink pattern
 * - 250 ms  : device not mounted
 * - 1000 ms : device mounted
 * - 2500 ms : device is suspended
 */
enum {
  BLINK_NOT_MOUNTED = 250,
  BLINK_MOUNTED = 1000,
  BLINK_SUSPENDED = 2500,
};

static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;

void led_blinking_task(void* param);
void usb_device_task(void *param);
void video_task(void* param);

void framebuffer_update_task(void);

//--------------------------------------------------------------------+
// Main
//--------------------------------------------------------------------+
int main(void) {
  board_init();

  // init device stack on configured roothub port
  tusb_rhport_init_t dev_init = {
    .role = TUSB_ROLE_DEVICE,
    .speed = TUSB_SPEED_AUTO
  };
  tusb_init(BOARD_TUD_RHPORT, &dev_init);

  board_init_after_tusb();

  multicore_launch_core1(framebuffer_update_task);

  while (1) {
    tud_task(); // tinyusb device task
    led_blinking_task(NULL);
    video_task(NULL);
  }
}

//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+

// Invoked when device is mounted
void tud_mount_cb(void) {
  blink_interval_ms = BLINK_MOUNTED;
}

// Invoked when device is unmounted
void tud_umount_cb(void) {
  blink_interval_ms = BLINK_NOT_MOUNTED;
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en) {
  (void) remote_wakeup_en;
  blink_interval_ms = BLINK_SUSPENDED;
}

// Invoked when usb bus is resumed
void tud_resume_cb(void) {
  blink_interval_ms = tud_mounted() ? BLINK_MOUNTED : BLINK_NOT_MOUNTED;
}

//--------------------------------------------------------------------+
// Framebuffer background updates (on core 1)
//--------------------------------------------------------------------+

// grayscale frame buffer
static uint8_t frame_buffer[FRAME_WIDTH * FRAME_HEIGHT];

void framebuffer_update_task(void)
{
	while (1)
	{
		unsigned t = board_millis();

		for (int y = 0; y < FRAME_HEIGHT; ++y)
		{
			for (int x = 0; x < FRAME_WIDTH; ++x)
			{
				frame_buffer[x + y * FRAME_WIDTH] = (uint8_t)(127.5 + 127.5 * sin(0.3*x + 0.25*y + t * 0.01));
			}
		}
	}
}

//--------------------------------------------------------------------+
// USB Video
//--------------------------------------------------------------------+
static unsigned frame_num = 0;
static unsigned tx_busy = 0;
static unsigned interval_ms = 1000 / FRAME_RATE;


// YUY2 frame buffer
static uint8_t yuy2_frame_buffer[FRAME_WIDTH * FRAME_HEIGHT * 16 / 8];

static void convert_frame_buffer(uint8_t* yuy2_buffer, unsigned frame_no) {
	uint8_t* ip = frame_buffer;
	uint8_t* op = yuy2_buffer;
	for (int i = 0; i < FRAME_WIDTH*FRAME_HEIGHT; ++i)
	{
		*(op++) = *(ip++); // Luma
		*(op++) = 128;     // neutral chroma
	}
}



static void video_send_frame(void) {
  static unsigned start_ms = 0;
  static unsigned already_sent = 0;

  if (!tud_video_n_streaming(0, 0)) {
    already_sent = 0;
    frame_num = 0;
    return;
  }

  if (!already_sent) {
    already_sent = 1;
    tx_busy = 1;
    start_ms = board_millis();
    convert_frame_buffer(yuy2_frame_buffer, frame_num);
    tud_video_n_frame_xfer(0, 0, (void*) yuy2_frame_buffer, FRAME_WIDTH * FRAME_HEIGHT * 16 / 8);
  }

  unsigned cur = board_millis();
  if (cur - start_ms < interval_ms) {
    return; // not enough time
  }
  if (tx_busy) {
    return;
  }
  start_ms += interval_ms;
  tx_busy = 1;

  convert_frame_buffer(yuy2_frame_buffer, frame_num);
  tud_video_n_frame_xfer(0, 0, (void*) yuy2_frame_buffer, FRAME_WIDTH * FRAME_HEIGHT * 16 / 8);
}


void video_task(void* param) {
  (void) param;

  while(1) {
    video_send_frame();

    return;
  }
}

void tud_video_frame_xfer_complete_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx) {
  (void) ctl_idx;
  (void) stm_idx;
  tx_busy = 0;
  /* flip buffer */
  ++frame_num;
}

int tud_video_commit_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx,
                        video_probe_and_commit_control_t const* parameters) {
  (void) ctl_idx;
  (void) stm_idx;
  /* convert unit to ms from 100 ns */
  interval_ms = parameters->dwFrameInterval / 10000;
  return VIDEO_ERROR_NONE;
}

//--------------------------------------------------------------------+
// Blinking Task
//--------------------------------------------------------------------+
void led_blinking_task(void* param) {
  (void) param;
  static uint32_t start_ms = 0;
  static bool led_state = false;

  while (1) {
    if (board_millis() - start_ms < blink_interval_ms) {
      return; // not enough time
    }

    start_ms += blink_interval_ms;
    board_led_write(led_state);
    led_state = 1 - led_state; // toggle
  }
}
