/*
 * Copyright © 2015 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */


#ifndef EVDEV_BUTTONSET_H
#define EVDEV_BUTTONSET_H

#include "evdev.h"

#define LIBINPUT_BUTTONSET_AXIS_NONE 0

enum buttonset_status {
	BUTTONSET_NONE = 0,
	BUTTONSET_AXES_UPDATED = 1 << 0,
	BUTTONSET_BUTTONS_PRESSED = 1 << 1,
	BUTTONSET_BUTTONS_RELEASED = 1 << 2,
	BUTTONSET_STYLUS_IN_CONTACT = 1 << 3,
	BUTTONSET_TOOL_LEAVING_PROXIMITY = 1 << 4,
	BUTTONSET_TOOL_OUT_OF_PROXIMITY = 1 << 5,
	BUTTONSET_TOOL_ENTERING_PROXIMITY = 1 << 6
};

struct button_state {
	uint32_t buttons; /* bitmask of evcode - BTN_TOUCH */
};

struct buttonset_dispatch {
	struct evdev_dispatch base;
	struct evdev_device *device;
	unsigned char status;
	unsigned char changed_axes[NCHARS(LIBINPUT_BUTTONSET_AXIS_MAX + 1)];
	double axes[LIBINPUT_BUTTONSET_AXIS_MAX + 1];
	unsigned char axis_caps[NCHARS(LIBINPUT_BUTTONSET_AXIS_MAX + 1)];

	struct button_state button_state;
	struct button_state prev_button_state;
};

static inline enum libinput_buttonset_axis
evcode_to_axis(const uint32_t evcode)
{
	enum libinput_buttonset_axis axis;

	switch (evcode) {
	case ABS_WHEEL:
		axis = LIBINPUT_BUTTONSET_AXIS_RING;
		break;
	case ABS_THROTTLE:
		axis = LIBINPUT_BUTTONSET_AXIS_RING2;
		break;
	case ABS_RX:
		axis = LIBINPUT_BUTTONSET_AXIS_STRIP;
		break;
	case ABS_RY:
		axis = LIBINPUT_BUTTONSET_AXIS_STRIP2;
		break;
	default:
		axis = LIBINPUT_BUTTONSET_AXIS_NONE;
		break;
	}

	return axis;
}

static inline uint32_t
axis_to_evcode(const enum libinput_buttonset_axis axis)
{
	uint32_t evcode;

	switch (axis) {
	case LIBINPUT_BUTTONSET_AXIS_RING:
		evcode = ABS_WHEEL;
		break;
	case LIBINPUT_BUTTONSET_AXIS_RING2:
		evcode = ABS_THROTTLE;
		break;
	case LIBINPUT_BUTTONSET_AXIS_STRIP:
		evcode = ABS_RX;
		break;
	case LIBINPUT_BUTTONSET_AXIS_STRIP2:
		evcode = ABS_RY;
		break;
	default:
		abort();
	}

	return evcode;
}

#endif
