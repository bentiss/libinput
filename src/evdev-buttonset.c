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
#include "config.h"
#include "evdev-buttonset.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#define buttonset_set_status(buttonset_,s_) (buttonset_)->status |= (s_)
#define buttonset_unset_status(buttonset_,s_) (buttonset_)->status &= ~(s_)
#define buttonset_has_status(buttonset_,s_) (!!((buttonset_)->status & (s_)))

static unsigned long *
buttonset_get_pressed_buttons(struct buttonset_dispatch *buttonset)
{
	struct button_state *state = &buttonset->button_state;
	struct button_state *prev_state = &buttonset->prev_button_state;
	unsigned int i;

	for (i = 0; i < NLONGS(KEY_CNT); i++)
		state->buttons_pressed[i] = state->buttons[i]
						& ~(prev_state->buttons[i]);

	return state->buttons_pressed;
}

static unsigned long *
buttonset_get_released_buttons(struct buttonset_dispatch *buttonset)
{
	struct button_state *state = &buttonset->button_state;
	struct button_state *prev_state = &buttonset->prev_button_state;
	unsigned int i;

	for (i = 0; i < NLONGS(KEY_CNT); i++)
		state->buttons_pressed[i] = prev_state->buttons[i]
						& ~(state->buttons[i]);

	return state->buttons_pressed;
}

static void
buttonset_process_absolute(struct buttonset_dispatch *buttonset,
			   struct evdev_device *device,
			   struct input_event *e,
			   uint32_t time)
{
	enum libinput_buttonset_axis axis;

	switch (e->code) {
	case ABS_WHEEL:
	case ABS_THROTTLE:
	case ABS_RX:
	case ABS_RY:
		axis = evcode_to_axis(e->code);
		if (axis == LIBINPUT_BUTTONSET_AXIS_NONE) {
			log_bug_libinput(device->base.seat->libinput,
					 "Invalid ABS event code %#x\n",
					 e->code);
			break;
		}

		set_bit(buttonset->changed_axes, axis);
		buttonset_set_status(buttonset, BUTTONSET_AXES_UPDATED);
		break;
	case ABS_MISC:
		/* ignore: this used to forward the current 'tool', and is
		 * always set to 15 */
		break;
	default:
		log_info(device->base.seat->libinput,
			 "Unhandled ABS event code %#x\n", e->code);
		break;
	}
}

static void
buttonset_mark_all_axes_changed(struct buttonset_dispatch *buttonset,
				struct evdev_device *device)
{
	enum libinput_buttonset_axis a;

	for (a = LIBINPUT_BUTTONSET_AXIS_RING; a <= LIBINPUT_BUTTONSET_AXIS_MAX; a++) {
		if (libevdev_has_event_code(device->evdev,
					    EV_ABS,
					    axis_to_evcode(a)))
			set_bit(buttonset->changed_axes, a);
	}

	buttonset_set_status(buttonset, BUTTONSET_AXES_UPDATED);
}

static inline double
normalize_ring(const struct input_absinfo * absinfo) {
	/*
	 * 0 is the ring's northernmost point in the device's current logical
	 * rotation, increasing clockwise to 1.
	 */
	double range = absinfo->maximum - absinfo->minimum;
	double value = (absinfo->value - absinfo->minimum) / range - 0.25;
	if (value <= 0.0)
		value += 1.0;

	return value;
}

static inline double
normalize_strip(const struct input_absinfo * absinfo) {
	double range = absinfo->maximum - absinfo->minimum;
	double value = (absinfo->value - absinfo->minimum) / range;

	return value;
}

static void
buttonset_check_notify_axes(struct buttonset_dispatch *buttonset,
			    struct evdev_device *device,
			    uint32_t time)
{
	struct libinput_device *base = &device->base;
	bool axis_update_needed = false;
	int a;

	for (a = LIBINPUT_BUTTONSET_AXIS_RING; a <= LIBINPUT_BUTTONSET_AXIS_MAX; a++) {
		const struct input_absinfo *absinfo;

		if (!bit_is_set(buttonset->changed_axes, a))
			continue;

		absinfo = libevdev_get_abs_info(device->evdev,
						axis_to_evcode(a));

		switch (a) {
		case LIBINPUT_BUTTONSET_AXIS_RING:
		case LIBINPUT_BUTTONSET_AXIS_RING2:
			buttonset->axes[a] = normalize_ring(absinfo);
			break;
		case LIBINPUT_BUTTONSET_AXIS_STRIP:
		case LIBINPUT_BUTTONSET_AXIS_STRIP2:
			buttonset->axes[a] = normalize_strip(absinfo);
			break;
		default:
			log_bug_libinput(device->base.seat->libinput,
					 "Invalid axis update: %d\n", a);
			break;
		}

		axis_update_needed = true;
	}

	if (axis_update_needed)
		buttonset_notify_axis(base,
				      time,
				      buttonset->changed_axes,
				      buttonset->axes);

	memset(buttonset->changed_axes, 0, sizeof(buttonset->changed_axes));
}

static void
buttonset_process_key(struct buttonset_dispatch *buttonset,
		      struct evdev_device *device,
		      struct input_event *e,
		      uint32_t time)
{
	uint32_t button = e->code;
	uint32_t enable = e->value;


	if (enable) {
		long_set_bit(buttonset->button_state.buttons, button);
		buttonset_set_status(buttonset, BUTTONSET_BUTTONS_PRESSED);
	} else {
		long_clear_bit(buttonset->button_state.buttons, button);
		buttonset_set_status(buttonset, BUTTONSET_BUTTONS_RELEASED);
	}
}

static void
buttonset_notify_button_mask(struct buttonset_dispatch *buttonset,
			     struct evdev_device *device,
			     uint32_t time,
			     unsigned long *buttons,
			     enum libinput_button_state state)
{
	struct libinput_device *base = &device->base;
	int32_t num_button;
	unsigned int i;

	for (i = 0; i < NLONGS(KEY_CNT); i++) {
		unsigned long buttons_slice = buttons[i];

		num_button = i * LONG_BITS;
		while (buttons_slice) {
			int enabled;

			num_button++;
			enabled = (buttons_slice & 1);
			buttons_slice >>= 1;

			if (!enabled)
				continue;

			buttonset_notify_button(base,
						time,
						buttonset->axes,
						num_button - 1,
						state);
		}
	}
}

static void
buttonset_notify_buttons(struct buttonset_dispatch *buttonset,
			 struct evdev_device *device,
			 uint32_t time,
			 enum libinput_button_state state)
{
	unsigned long *buttons;

	if (state == LIBINPUT_BUTTON_STATE_PRESSED)
		buttons = buttonset_get_pressed_buttons(buttonset);
	else
		buttons = buttonset_get_released_buttons(buttonset);

	buttonset_notify_button_mask(buttonset,
				     device,
				     time,
				     buttons,
				     state);
}

static void
sanitize_buttonset_axes(struct buttonset_dispatch *buttonset)
{
}

static void
buttonset_flush(struct buttonset_dispatch *buttonset,
		struct evdev_device *device,
		uint32_t time)
{
	if (buttonset_has_status(buttonset, BUTTONSET_AXES_UPDATED)) {
		sanitize_buttonset_axes(buttonset);
		buttonset_check_notify_axes(buttonset, device, time);
		buttonset_unset_status(buttonset, BUTTONSET_AXES_UPDATED);
	}

	if (buttonset_has_status(buttonset, BUTTONSET_BUTTONS_RELEASED)) {
		buttonset_notify_buttons(buttonset,
					 device,
					 time,
					 LIBINPUT_BUTTON_STATE_RELEASED);
		buttonset_unset_status(buttonset, BUTTONSET_BUTTONS_RELEASED);
	}

	if (buttonset_has_status(buttonset, BUTTONSET_BUTTONS_PRESSED)) {
		buttonset_notify_buttons(buttonset,
					 device,
					 time,
					 LIBINPUT_BUTTON_STATE_PRESSED);
		buttonset_unset_status(buttonset, BUTTONSET_BUTTONS_PRESSED);
	}

	/* Update state */
	memcpy(&buttonset->prev_button_state,
	       &buttonset->button_state,
	       sizeof(buttonset->button_state));
}

static void
buttonset_process(struct evdev_dispatch *dispatch,
		  struct evdev_device *device,
		  struct input_event *e,
		  uint64_t time)
{
	struct buttonset_dispatch *buttonset =
		(struct buttonset_dispatch *)dispatch;

	switch (e->type) {
	case EV_ABS:
		buttonset_process_absolute(buttonset, device, e, time);
		break;
	case EV_KEY:
		buttonset_process_key(buttonset, device, e, time);
		break;
	case EV_SYN:
		buttonset_flush(buttonset, device, time);
		break;
	default:
		log_error(device->base.seat->libinput,
			  "Unexpected event type %s (%#x)\n",
			  libevdev_event_type_get_name(e->type),
			  e->type);
		break;
	}
}

static void
buttonset_destroy(struct evdev_dispatch *dispatch)
{
	struct buttonset_dispatch *buttonset =
		(struct buttonset_dispatch*)dispatch;

	free(buttonset);
}

static struct evdev_dispatch_interface buttonset_interface = {
	buttonset_process,
	NULL, /* remove */
	buttonset_destroy,
	NULL, /* device_added */
	NULL, /* device_removed */
	NULL, /* device_suspended */
	NULL, /* device_resumed */
	NULL, /* tag_device */
};

static int
buttonset_init(struct buttonset_dispatch *buttonset,
	       struct evdev_device *device)
{
	enum libinput_buttonset_axis axis;

	buttonset->base.interface = &buttonset_interface;
	buttonset->device = device;
	buttonset->status = BUTTONSET_NONE;

	for (axis = LIBINPUT_BUTTONSET_AXIS_RING;
	     axis <= LIBINPUT_BUTTONSET_AXIS_MAX;
	     axis++) {
		if (libevdev_has_event_code(device->evdev,
					    EV_ABS,
					    axis_to_evcode(axis)))
			set_bit(buttonset->axis_caps, axis);
	}

	buttonset_mark_all_axes_changed(buttonset, device);

	return 0;
}

struct evdev_dispatch *
evdev_buttonset_create(struct evdev_device *device)
{
	struct buttonset_dispatch *buttonset;

	buttonset = zalloc(sizeof *buttonset);
	if (!buttonset)
		return NULL;

	if (buttonset_init(buttonset, device) != 0) {
		buttonset_destroy(&buttonset->base);
		return NULL;
	}

	return &buttonset->base;
}
