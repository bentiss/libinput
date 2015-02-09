/*
 * Copyright © 2014 Red Hat, Inc.
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

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <limits.h>

#include "evdev-mt-touchpad.h"

#define DEFAULT_ACCEL_NUMERATOR 1200.0
#define DEFAULT_HYSTERESIS_MARGIN_DENOMINATOR 700.0
#define DEFAULT_TRACKPOINT_ACTIVITY_TIMEOUT 500 /* ms */

static inline int
tp_hysteresis(int in, int center, int margin)
{
	int diff = in - center;
	if (abs(diff) <= margin)
		return center;

	if (diff > margin)
		return center + diff - margin;
	else
		return center + diff + margin;
}

static inline struct tp_motion *
tp_motion_history_offset(struct tp_touch *t, int offset)
{
	int offset_index =
		(t->history.index - offset + TOUCHPAD_HISTORY_LENGTH) %
		TOUCHPAD_HISTORY_LENGTH;

	return &t->history.samples[offset_index];
}

void
tp_filter_motion(struct tp_dispatch *tp,
	         double *dx, double *dy,
	         double *dx_unaccel, double *dy_unaccel,
		 uint64_t time)
{
	struct motion_params motion;

	motion.dx = *dx * tp->accel.x_scale_coeff;
	motion.dy = *dy * tp->accel.y_scale_coeff;

	if (dx_unaccel)
		*dx_unaccel = motion.dx;
	if (dy_unaccel)
		*dy_unaccel = motion.dy;

	if (motion.dx != 0.0 || motion.dy != 0.0)
		filter_dispatch(tp->device->pointer.filter, &motion, tp, time);

	*dx = motion.dx;
	*dy = motion.dy;
}

static inline void
tp_motion_history_push(struct tp_touch *t)
{
	int motion_index = (t->history.index + 1) % TOUCHPAD_HISTORY_LENGTH;

	if (t->history.count < TOUCHPAD_HISTORY_LENGTH)
		t->history.count++;

	t->history.samples[motion_index].x = t->x;
	t->history.samples[motion_index].y = t->y;
	t->history.index = motion_index;
}

static inline void
tp_motion_hysteresis(struct tp_dispatch *tp,
		     struct tp_touch *t)
{
	int x = t->x,
	    y = t->y;

	if (t->history.count == 0) {
		t->hysteresis.center_x = t->x;
		t->hysteresis.center_y = t->y;
	} else {
		x = tp_hysteresis(x,
				  t->hysteresis.center_x,
				  tp->hysteresis.margin_x);
		y = tp_hysteresis(y,
				  t->hysteresis.center_y,
				  tp->hysteresis.margin_y);
		t->hysteresis.center_x = x;
		t->hysteresis.center_y = y;
		t->x = x;
		t->y = y;
	}
}

static inline void
tp_motion_history_reset(struct tp_touch *t)
{
	t->history.count = 0;
}

static inline struct tp_touch *
tp_current_touch(struct tp_dispatch *tp)
{
	return &tp->touches[min(tp->slot, tp->ntouches - 1)];
}

static inline struct tp_touch *
tp_get_touch(struct tp_dispatch *tp, unsigned int slot)
{
	assert(slot < tp->ntouches);
	return &tp->touches[slot];
}

static inline unsigned int
tp_fake_finger_count(struct tp_dispatch *tp)
{
	/* don't count BTN_TOUCH */
	return ffs(tp->fake_touches >> 1);
}

static inline bool
tp_fake_finger_is_touching(struct tp_dispatch *tp)
{
	return tp->fake_touches & 0x1;
}

static inline void
tp_fake_finger_set(struct tp_dispatch *tp,
		   unsigned int code,
		   bool is_press)
{
	unsigned int shift;

	switch (code) {
	case BTN_TOUCH:
		shift = 0;
		break;
	case BTN_TOOL_FINGER:
		shift = 1;
		break;
	case BTN_TOOL_DOUBLETAP:
	case BTN_TOOL_TRIPLETAP:
	case BTN_TOOL_QUADTAP:
		shift = code - BTN_TOOL_DOUBLETAP + 2;
		break;
	default:
		return;
	}

	if (is_press)
		tp->fake_touches |= 1 << shift;
	else
		tp->fake_touches &= ~(0x1 << shift);
}

static inline void
tp_new_touch(struct tp_dispatch *tp, struct tp_touch *t, uint64_t time)
{
	if (t->state == TOUCH_BEGIN ||
	    t->state == TOUCH_UPDATE ||
	    t->state == TOUCH_HOVERING)
		return;

	/* we begin the touch as hovering because until BTN_TOUCH happens we
	 * don't know if it's a touch down or not. And BTN_TOUCH may happen
	 * after ABS_MT_TRACKING_ID */
	tp_motion_history_reset(t);
	t->dirty = true;
	t->has_ended = false;
	t->state = TOUCH_HOVERING;
	t->pinned.is_pinned = false;
	t->millis = time;
	tp->queued |= TOUCHPAD_EVENT_MOTION;
}

static inline void
tp_begin_touch(struct tp_dispatch *tp, struct tp_touch *t, uint64_t time)
{
	t->dirty = true;
	t->state = TOUCH_BEGIN;
	t->millis = time;
	tp->nfingers_down++;
	assert(tp->nfingers_down >= 1);
}

/**
 * End a touch, even if the touch sequence is still active.
 */
static inline void
tp_end_touch(struct tp_dispatch *tp, struct tp_touch *t, uint64_t time)
{
	switch (t->state) {
	case TOUCH_HOVERING:
		t->state = TOUCH_NONE;
		/* fallthough */
	case TOUCH_NONE:
	case TOUCH_END:
		return;
	case TOUCH_BEGIN:
	case TOUCH_UPDATE:
		break;

	}

	t->dirty = true;
	t->is_pointer = false;
	t->palm.is_palm = false;
	t->state = TOUCH_END;
	t->pinned.is_pinned = false;
	t->millis = time;
	assert(tp->nfingers_down >= 1);
	tp->nfingers_down--;
	tp->queued |= TOUCHPAD_EVENT_MOTION;
}

/**
 * End the touch sequence on ABS_MT_TRACKING_ID -1 or when the BTN_TOOL_* 0 is received.
 */
static inline void
tp_end_sequence(struct tp_dispatch *tp, struct tp_touch *t, uint64_t time)
{
	t->has_ended = true;
	tp_end_touch(tp, t, time);
}

static double
tp_estimate_delta(int x0, int x1, int x2, int x3)
{
	return (x0 + x1 - x2 - x3) / 4.0;
}

void
tp_get_delta(struct tp_touch *t, double *dx, double *dy)
{
	if (t->history.count < TOUCHPAD_MIN_SAMPLES) {
		*dx = 0;
		*dy = 0;
		return;
	}

	*dx = tp_estimate_delta(tp_motion_history_offset(t, 0)->x,
				tp_motion_history_offset(t, 1)->x,
				tp_motion_history_offset(t, 2)->x,
				tp_motion_history_offset(t, 3)->x);
	*dy = tp_estimate_delta(tp_motion_history_offset(t, 0)->y,
				tp_motion_history_offset(t, 1)->y,
				tp_motion_history_offset(t, 2)->y,
				tp_motion_history_offset(t, 3)->y);
}

static void
tp_process_absolute(struct tp_dispatch *tp,
		    const struct input_event *e,
		    uint64_t time)
{
	struct tp_touch *t = tp_current_touch(tp);

	switch(e->code) {
	case ABS_MT_POSITION_X:
		t->x = e->value;
		t->millis = time;
		t->dirty = true;
		tp->queued |= TOUCHPAD_EVENT_MOTION;
		break;
	case ABS_MT_POSITION_Y:
		t->y = e->value;
		t->millis = time;
		t->dirty = true;
		tp->queued |= TOUCHPAD_EVENT_MOTION;
		break;
	case ABS_MT_SLOT:
		tp->slot = e->value;
		break;
	case ABS_MT_TRACKING_ID:
		if (e->value != -1)
			tp_new_touch(tp, t, time);
		else
			tp_end_sequence(tp, t, time);
	}
}

static void
tp_process_absolute_st(struct tp_dispatch *tp,
		       const struct input_event *e,
		       uint64_t time)
{
	struct tp_touch *t = tp_current_touch(tp);

	switch(e->code) {
	case ABS_X:
		t->x = e->value;
		t->millis = time;
		t->dirty = true;
		tp->queued |= TOUCHPAD_EVENT_MOTION;
		break;
	case ABS_Y:
		t->y = e->value;
		t->millis = time;
		t->dirty = true;
		tp->queued |= TOUCHPAD_EVENT_MOTION;
		break;
	}
}

static void
tp_process_fake_touch(struct tp_dispatch *tp,
		      const struct input_event *e,
		      uint64_t time)
{
	struct tp_touch *t;
	unsigned int nfake_touches;
	unsigned int i, start;

	tp_fake_finger_set(tp, e->code, e->value != 0);

	nfake_touches = tp_fake_finger_count(tp);

	start = tp->has_mt ? tp->real_touches : 0;
	for (i = start; i < tp->ntouches; i++) {
		t = tp_get_touch(tp, i);
		if (i < nfake_touches)
			tp_new_touch(tp, t, time);
		else
			tp_end_sequence(tp, t, time);
	}
}

static void
tp_process_trackpoint_button(struct tp_dispatch *tp,
			     const struct input_event *e,
			     uint64_t time)
{
	struct evdev_dispatch *dispatch;
	struct input_event event;

	if (!tp->buttons.trackpoint ||
	    (tp->device->tags & EVDEV_TAG_TOUCHPAD_TRACKPOINT) == 0)
		return;

	dispatch = tp->buttons.trackpoint->dispatch;

	event = *e;

	switch (event.code) {
	case BTN_0:
		event.code = BTN_LEFT;
		break;
	case BTN_1:
		event.code = BTN_RIGHT;
		break;
	case BTN_2:
		event.code = BTN_MIDDLE;
		break;
	default:
		return;
	}

	dispatch->interface->process(dispatch,
				     tp->buttons.trackpoint,
				     &event, time);
}

static void
tp_process_key(struct tp_dispatch *tp,
	       const struct input_event *e,
	       uint64_t time)
{
	switch (e->code) {
		case BTN_LEFT:
		case BTN_MIDDLE:
		case BTN_RIGHT:
			tp_process_button(tp, e, time);
			break;
		case BTN_TOUCH:
		case BTN_TOOL_FINGER:
		case BTN_TOOL_DOUBLETAP:
		case BTN_TOOL_TRIPLETAP:
		case BTN_TOOL_QUADTAP:
			tp_process_fake_touch(tp, e, time);
			break;
		case BTN_0:
		case BTN_1:
		case BTN_2:
			tp_process_trackpoint_button(tp, e, time);
			break;
	}
}

static void
tp_unpin_finger(struct tp_dispatch *tp, struct tp_touch *t)
{
	unsigned int xdist, ydist;

	if (!t->pinned.is_pinned)
		return;

	xdist = abs(t->x - t->pinned.center_x);
	ydist = abs(t->y - t->pinned.center_y);

	if (xdist * xdist + ydist * ydist >=
			tp->buttons.motion_dist * tp->buttons.motion_dist) {
		t->pinned.is_pinned = false;
		tp_set_pointer(tp, t);
		return;
	}

	/* The finger may slowly drift, adjust the center */
	t->pinned.center_x = t->x + t->pinned.center_x / 2;
	t->pinned.center_y = t->y + t->pinned.center_y / 2;
}

static void
tp_pin_fingers(struct tp_dispatch *tp)
{
	struct tp_touch *t;

	tp_for_each_touch(tp, t) {
		t->is_pointer = false;
		t->pinned.is_pinned = true;
		t->pinned.center_x = t->x;
		t->pinned.center_y = t->y;
	}
}

static int
tp_touch_active(struct tp_dispatch *tp, struct tp_touch *t)
{
	return (t->state == TOUCH_BEGIN || t->state == TOUCH_UPDATE) &&
		!t->palm.is_palm &&
		!t->pinned.is_pinned &&
		tp_button_touch_active(tp, t) &&
		tp_edge_scroll_touch_active(tp, t);
}

void
tp_set_pointer(struct tp_dispatch *tp, struct tp_touch *t)
{
	struct tp_touch *tmp = NULL;

	/* Only set the touch as pointer if we don't have one yet */
	tp_for_each_touch(tp, tmp) {
		if (tmp->is_pointer)
			return;
	}

	if (tp_touch_active(tp, t))
		t->is_pointer = true;
}

static void
tp_palm_detect(struct tp_dispatch *tp, struct tp_touch *t, uint64_t time)
{
	const int PALM_TIMEOUT = 200; /* ms */
	const int DIRECTIONS = NE|E|SE|SW|W|NW;

	/* If labelled a touch as palm, we unlabel as palm when
	   we move out of the palm edge zone within the timeout, provided
	   the direction is within 45 degrees of the horizontal.
	 */
	if (t->palm.is_palm) {
		if (time < t->palm.time + PALM_TIMEOUT &&
		    (t->x > tp->palm.left_edge && t->x < tp->palm.right_edge)) {
			int dirs = vector_get_direction(t->x - t->palm.x, t->y - t->palm.y);
			if ((dirs & DIRECTIONS) && !(dirs & ~DIRECTIONS)) {
				t->palm.is_palm = false;
				tp_set_pointer(tp, t);
			}
		}
		return;
	}

	/* palm must start in exclusion zone, it's ok to move into
	   the zone without being a palm */
	if (t->state != TOUCH_BEGIN ||
	    (t->x > tp->palm.left_edge && t->x < tp->palm.right_edge))
		return;

	/* don't detect palm in software button areas, it's
	   likely that legitimate touches start in the area
	   covered by the exclusion zone */
	if (tp->buttons.is_clickpad &&
	    tp_button_is_inside_softbutton_area(tp, t))
		return;

	t->palm.is_palm = true;
	t->palm.time = time;
	t->palm.x = t->x;
	t->palm.y = t->y;
}

static void
tp_post_twofinger_scroll(struct tp_dispatch *tp, uint64_t time)
{
	struct tp_touch *t;
	int nchanged = 0;
	double dx = 0, dy =0;
	double tmpx, tmpy;

	tp_for_each_touch(tp, t) {
		if (tp_touch_active(tp, t) && t->dirty) {
			nchanged++;
			tp_get_delta(t, &tmpx, &tmpy);

			dx += tmpx;
			dy += tmpy;
		}
		/* Stop spurious MOTION events at the end of scrolling */
		t->is_pointer = false;
	}

	if (nchanged == 0)
		return;

	dx /= nchanged;
	dy /= nchanged;

	tp_filter_motion(tp, &dx, &dy, NULL, NULL, time);

	evdev_post_scroll(tp->device,
			  time,
			  LIBINPUT_POINTER_AXIS_SOURCE_FINGER,
			  dx, dy);
	tp->scroll.twofinger_state = TWOFINGER_SCROLL_STATE_ACTIVE;
}

static void
tp_twofinger_stop_scroll(struct tp_dispatch *tp, uint64_t time)
{
	struct tp_touch *t, *ptr = NULL;
	int nfingers_down = 0;

	evdev_stop_scroll(tp->device,
			  time,
			  LIBINPUT_POINTER_AXIS_SOURCE_FINGER);

	/* If we were scrolling and now there's exactly 1 active finger,
	   switch back to pointer movement */
	if (tp->scroll.twofinger_state == TWOFINGER_SCROLL_STATE_ACTIVE) {
		tp_for_each_touch(tp, t) {
			if (tp_touch_active(tp, t)) {
				nfingers_down++;
				if (ptr == NULL)
					ptr = t;
			}
		}

		if (nfingers_down == 1)
			tp_set_pointer(tp, ptr);
	}

	tp->scroll.twofinger_state = TWOFINGER_SCROLL_STATE_NONE;
}

static int
tp_twofinger_scroll_post_events(struct tp_dispatch *tp, uint64_t time)
{
	struct tp_touch *t;
	int nfingers_down = 0;

	/* No 2fg scrolling during tap-n-drag */
	if (tp_tap_dragging(tp))
		return 0;

	/* No 2fg scrolling while a clickpad is clicked */
	if (tp->buttons.is_clickpad && tp->buttons.state)
		return 0;

	/* Only count active touches for 2 finger scrolling */
	tp_for_each_touch(tp, t) {
		if (tp_touch_active(tp, t))
			nfingers_down++;
	}

	if (nfingers_down == 2) {
		tp_post_twofinger_scroll(tp, time);
		return 1;
	}

	tp_twofinger_stop_scroll(tp, time);

	return 0;
}

static void
tp_scroll_handle_state(struct tp_dispatch *tp, uint64_t time)
{
	/* Note this must be always called, so that it knows the state of
	 * touches when the scroll-mode changes.
	 */
	tp_edge_scroll_handle_state(tp, time);
}

static int
tp_post_scroll_events(struct tp_dispatch *tp, uint64_t time)
{
	struct libinput *libinput = tp->device->base.seat->libinput;

	switch (tp->scroll.method) {
	case LIBINPUT_CONFIG_SCROLL_NO_SCROLL:
		break;
	case LIBINPUT_CONFIG_SCROLL_2FG:
		return tp_twofinger_scroll_post_events(tp, time);
	case LIBINPUT_CONFIG_SCROLL_EDGE:
		return tp_edge_scroll_post_events(tp, time);
	case LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN:
		log_bug_libinput(libinput, "Unexpected scroll mode\n");
		break;
	}
	return 0;
}

static void
tp_stop_scroll_events(struct tp_dispatch *tp, uint64_t time)
{
	struct libinput *libinput = tp->device->base.seat->libinput;

	switch (tp->scroll.method) {
	case LIBINPUT_CONFIG_SCROLL_NO_SCROLL:
		break;
	case LIBINPUT_CONFIG_SCROLL_2FG:
		tp_twofinger_stop_scroll(tp, time);
		break;
	case LIBINPUT_CONFIG_SCROLL_EDGE:
		tp_edge_scroll_stop_events(tp, time);
		break;
	case LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN:
		log_bug_libinput(libinput, "Unexpected scroll mode\n");
		break;
	}
}

static void
tp_remove_scroll(struct tp_dispatch *tp)
{
	tp_remove_edge_scroll(tp);
}

static void
tp_unhover_touches(struct tp_dispatch *tp, uint64_t time)
{
	struct tp_touch *t;
	unsigned int nfake_touches;
	int i;

	if (!tp->fake_touches && !tp->nfingers_down)
		return;

	nfake_touches = tp_fake_finger_count(tp);
	if (tp->nfingers_down == nfake_touches &&
	    ((tp->nfingers_down == 0 && !tp_fake_finger_is_touching(tp)) ||
	     (tp->nfingers_down > 0 && tp_fake_finger_is_touching(tp))))
		return;

	/* if BTN_TOUCH is set and we have less fingers down than fake
	 * touches, switch each hovering touch to BEGIN
	 * until nfingers_down matches nfake_touches
	 */
	if (tp_fake_finger_is_touching(tp) &&
	    tp->nfingers_down < nfake_touches) {
		for (i = 0; i < (int)tp->ntouches; i++) {
			t = tp_get_touch(tp, i);

			if (t->state == TOUCH_HOVERING) {
				tp_begin_touch(tp, t, time);

				if (tp->nfingers_down >= nfake_touches)
					break;
			}
		}
	}

	/* if BTN_TOUCH is unset end all touches, we're hovering now. If we
	 * have too many touches also end some of them. This is done in
	 * reverse order.
	 */
	if (tp->nfingers_down > nfake_touches ||
	    !tp_fake_finger_is_touching(tp)) {
		for (i = tp->ntouches - 1; i >= 0; i--) {
			t = tp_get_touch(tp, i);

			if (t->state == TOUCH_HOVERING)
				continue;

			tp_end_touch(tp, t, time);

			if (tp_fake_finger_is_touching(tp) &&
			    tp->nfingers_down == nfake_touches)
				break;
		}
	}
}

static void
tp_process_state(struct tp_dispatch *tp, uint64_t time)
{
	struct tp_touch *t;
	struct tp_touch *first = tp_get_touch(tp, 0);
	unsigned int i;

	tp_unhover_touches(tp, time);

	for (i = 0; i < tp->ntouches; i++) {
		t = tp_get_touch(tp, i);

		/* semi-mt finger postions may "jump" when nfingers changes */
		if (tp->semi_mt && tp->nfingers_down != tp->old_nfingers_down)
			tp_motion_history_reset(t);

		if (i >= tp->real_touches && t->state != TOUCH_NONE) {
			t->x = first->x;
			t->y = first->y;
			if (!t->dirty)
				t->dirty = first->dirty;
		}

		if (!t->dirty)
			continue;

		tp_palm_detect(tp, t, time);

		tp_motion_hysteresis(tp, t);
		tp_motion_history_push(t);

		tp_unpin_finger(tp, t);
	}

	tp_button_handle_state(tp, time);
	tp_scroll_handle_state(tp, time);

	/*
	 * We have a physical button down event on a clickpad. To avoid
	 * spurious pointer moves by the clicking finger we pin all fingers.
	 * We unpin fingers when they move more then a certain threshold to
	 * to allow drag and drop.
	 */
	if ((tp->queued & TOUCHPAD_EVENT_BUTTON_PRESS) &&
	    tp->buttons.is_clickpad)
		tp_pin_fingers(tp);
}

static void
tp_post_process_state(struct tp_dispatch *tp, uint64_t time)
{
	struct tp_touch *t;

	tp_for_each_touch(tp, t) {

		if (!t->dirty)
			continue;

		if (t->state == TOUCH_END) {
			if (t->has_ended)
				t->state = TOUCH_NONE;
			else
				t->state = TOUCH_HOVERING;
		} else if (t->state == TOUCH_BEGIN) {
			t->state = TOUCH_UPDATE;
		}

		t->dirty = false;
	}

	tp->old_nfingers_down = tp->nfingers_down;
	tp->buttons.old_state = tp->buttons.state;

	tp->queued = TOUCHPAD_EVENT_NONE;
}

static void
tp_get_pointer_delta(struct tp_dispatch *tp, double *dx, double *dy)
{
	struct tp_touch *t = tp_current_touch(tp);

	if (!t->is_pointer) {
		tp_for_each_touch(tp, t) {
			if (t->is_pointer)
				break;
		}
	}

	if (!t->is_pointer || !t->dirty)
		return;

	tp_get_delta(t, dx, dy);
}

static void
tp_get_active_touches_delta(struct tp_dispatch *tp, double *dx, double *dy)
{
	struct tp_touch *t;
	double tdx, tdy;
	unsigned int i;

	for (i = 0; i < tp->real_touches; i++) {
		t = tp_get_touch(tp, i);

		if (!tp_touch_active(tp, t) || !t->dirty)
			continue;

		tp_get_delta(t, &tdx, &tdy);
		*dx += tdx;
		*dy += tdy;
	}
}

static void
tp_post_pointer_motion(struct tp_dispatch *tp, uint64_t time)
{
	double dx = 0.0, dy = 0.0;
	double dx_unaccel, dy_unaccel;

	/* When a clickpad is clicked, combine motion of all active touches */
	if (tp->buttons.is_clickpad && tp->buttons.state)
		tp_get_active_touches_delta(tp, &dx, &dy);
	else
		tp_get_pointer_delta(tp, &dx, &dy);

	tp_filter_motion(tp, &dx, &dy, &dx_unaccel, &dy_unaccel, time);

	if (dx != 0.0 || dy != 0.0 || dx_unaccel != 0.0 || dy_unaccel != 0.0) {
		pointer_notify_motion(&tp->device->base, time,
				      dx, dy, dx_unaccel, dy_unaccel);
	}
}

static void
tp_post_events(struct tp_dispatch *tp, uint64_t time)
{
	int filter_motion = 0;

	/* Only post (top) button events while suspended */
	if (tp->device->suspended) {
		tp_post_button_events(tp, time);
		return;
	}

	filter_motion |= tp_tap_handle_state(tp, time);
	filter_motion |= tp_post_button_events(tp, time);

	if (filter_motion || tp->sendevents.trackpoint_active) {
		tp_stop_scroll_events(tp, time);
		return;
	}

	if (tp_post_scroll_events(tp, time) != 0)
		return;

	tp_post_pointer_motion(tp, time);
}

static void
tp_handle_state(struct tp_dispatch *tp,
		uint64_t time)
{
	tp_process_state(tp, time);
	tp_post_events(tp, time);
	tp_post_process_state(tp, time);
}

static void
tp_process(struct evdev_dispatch *dispatch,
	   struct evdev_device *device,
	   struct input_event *e,
	   uint64_t time)
{
	struct tp_dispatch *tp =
		(struct tp_dispatch *)dispatch;

	switch (e->type) {
	case EV_ABS:
		if (tp->has_mt)
			tp_process_absolute(tp, e, time);
		else
			tp_process_absolute_st(tp, e, time);
		break;
	case EV_KEY:
		tp_process_key(tp, e, time);
		break;
	case EV_SYN:
		tp_handle_state(tp, time);
		break;
	}
}

static void
tp_remove_sendevents(struct tp_dispatch *tp)
{
	libinput_timer_cancel(&tp->sendevents.trackpoint_timer);

	if (tp->buttons.trackpoint)
		libinput_device_remove_event_listener(
					&tp->sendevents.trackpoint_listener);
}

static void
tp_remove(struct evdev_dispatch *dispatch)
{
	struct tp_dispatch *tp =
		(struct tp_dispatch*)dispatch;

	tp_remove_tap(tp);
	tp_remove_buttons(tp);
	tp_remove_sendevents(tp);
	tp_remove_scroll(tp);
}

static void
tp_destroy(struct evdev_dispatch *dispatch)
{
	struct tp_dispatch *tp =
		(struct tp_dispatch*)dispatch;


	free(tp->touches);
	free(tp);
}

static void
tp_clear_state(struct tp_dispatch *tp)
{
	uint64_t now = libinput_now(tp->device->base.seat->libinput);
	struct tp_touch *t;

	/* Unroll the touchpad state.
	 * Release buttons first. If tp is a clickpad, the button event
	 * must come before the touch up. If it isn't, the order doesn't
	 * matter anyway
	 *
	 * Then cancel all timeouts on the taps, triggering the last set
	 * of events.
	 *
	 * Then lift all touches so the touchpad is in a neutral state.
	 *
	 */
	tp_release_all_buttons(tp, now);
	tp_release_all_taps(tp, now);

	tp_for_each_touch(tp, t) {
		tp_end_sequence(tp, t, now);
	}

	tp_handle_state(tp, now);
}

static void
tp_suspend(struct tp_dispatch *tp, struct evdev_device *device)
{
	tp_clear_state(tp);

	/* On devices with top softwarebuttons we don't actually suspend the
	 * device, to keep the "trackpoint" buttons working. tp_post_events()
	 * will only send events for the trackpoint while suspended.
	 */
	if (tp->buttons.has_topbuttons) {
		evdev_notify_suspended_device(device);
		/* Enlarge topbutton area while suspended */
		tp_init_top_softbuttons(tp, device, 1.5);
	} else {
		evdev_device_suspend(device);
	}
}

static void
tp_resume(struct tp_dispatch *tp, struct evdev_device *device)
{
	if (tp->buttons.has_topbuttons) {
		/* tap state-machine is offline while suspended, reset state */
		tp_clear_state(tp);
		/* restore original topbutton area size */
		tp_init_top_softbuttons(tp, device, 1.0);
		evdev_notify_resumed_device(device);
	} else {
		evdev_device_resume(device);
	}
}

static void
tp_trackpoint_timeout(uint64_t now, void *data)
{
	struct tp_dispatch *tp = data;

	tp_tap_resume(tp, now);
	tp->sendevents.trackpoint_active = false;
}

static void
tp_trackpoint_event(uint64_t time, struct libinput_event *event, void *data)
{
	struct tp_dispatch *tp = data;

	/* Buttons do not count as trackpad activity, as people may use
	   the trackpoint buttons in combination with the touchpad. */
	if (event->type == LIBINPUT_EVENT_POINTER_BUTTON)
		return;

	if (!tp->sendevents.trackpoint_active) {
		evdev_stop_scroll(tp->device,
				  time,
				  LIBINPUT_POINTER_AXIS_SOURCE_FINGER);
		tp_tap_suspend(tp, time);
		tp->sendevents.trackpoint_active = true;
	}

	libinput_timer_set(&tp->sendevents.trackpoint_timer,
			   time + DEFAULT_TRACKPOINT_ACTIVITY_TIMEOUT);
}

static void
tp_device_added(struct evdev_device *device,
		struct evdev_device *added_device)
{
	struct tp_dispatch *tp = (struct tp_dispatch*)device->dispatch;

	if (tp->buttons.trackpoint == NULL &&
	    (added_device->tags & EVDEV_TAG_TRACKPOINT)) {
		/* Don't send any pending releases to the new trackpoint */
		tp->buttons.active_is_topbutton = false;
		tp->buttons.trackpoint = added_device;
		libinput_device_add_event_listener(&added_device->base,
					&tp->sendevents.trackpoint_listener,
					tp_trackpoint_event, tp);
	}

	if (tp->sendevents.current_mode !=
	    LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE)
		return;

	if (added_device->tags & EVDEV_TAG_EXTERNAL_MOUSE)
		tp_suspend(tp, device);
}

static void
tp_device_removed(struct evdev_device *device,
		  struct evdev_device *removed_device)
{
	struct tp_dispatch *tp = (struct tp_dispatch*)device->dispatch;
	struct libinput_device *dev;

	if (removed_device == tp->buttons.trackpoint) {
		/* Clear any pending releases for the trackpoint */
		if (tp->buttons.active && tp->buttons.active_is_topbutton) {
			tp->buttons.active = 0;
			tp->buttons.active_is_topbutton = false;
		}
		libinput_device_remove_event_listener(
					&tp->sendevents.trackpoint_listener);
		tp->buttons.trackpoint = NULL;
	}

	if (tp->sendevents.current_mode !=
	    LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE)
		return;

	list_for_each(dev, &device->base.seat->devices_list, link) {
		struct evdev_device *d = (struct evdev_device*)dev;
		if (d != removed_device &&
		    (d->tags & EVDEV_TAG_EXTERNAL_MOUSE)) {
			return;
		}
	}

	tp_resume(tp, device);
}

static void
tp_tag_device(struct evdev_device *device,
	      struct udev_device *udev_device)
{
	int bustype;

	/* simple approach: touchpads on USB or Bluetooth are considered
	 * external, anything else is internal. Exception is Apple -
	 * internal touchpads are connected over USB and it doesn't have
	 * external USB touchpads anyway.
	 */
	bustype = libevdev_get_id_bustype(device->evdev);
	if (bustype == BUS_USB) {
		 if (libevdev_get_id_vendor(device->evdev) == VENDOR_ID_APPLE)
			 device->tags |= EVDEV_TAG_INTERNAL_TOUCHPAD;
	} else if (bustype != BUS_BLUETOOTH)
		device->tags |= EVDEV_TAG_INTERNAL_TOUCHPAD;

	if (udev_device_get_property_value(udev_device,
					   "TOUCHPAD_HAS_TRACKPOINT_BUTTONS"))
		device->tags |= EVDEV_TAG_TOUCHPAD_TRACKPOINT;
}

static struct evdev_dispatch_interface tp_interface = {
	tp_process,
	tp_remove,
	tp_destroy,
	tp_device_added,
	tp_device_removed,
	tp_device_removed, /* device_suspended, treat as remove */
	tp_device_added,   /* device_resumed, treat as add */
	tp_tag_device,
};

static void
tp_init_touch(struct tp_dispatch *tp,
	      struct tp_touch *t)
{
	t->tp = tp;
	t->has_ended = true;
}

static int
tp_init_slots(struct tp_dispatch *tp,
	      struct evdev_device *device)
{
	const struct input_absinfo *absinfo;
	struct map {
		unsigned int code;
		int ntouches;
	} max_touches[] = {
		{ BTN_TOOL_QUINTTAP, 5 },
		{ BTN_TOOL_QUADTAP, 4 },
		{ BTN_TOOL_TRIPLETAP, 3 },
		{ BTN_TOOL_DOUBLETAP, 2 },
	};
	struct map *m;
	unsigned int i, n_btn_tool_touches = 1;

	absinfo = libevdev_get_abs_info(device->evdev, ABS_MT_SLOT);
	if (absinfo) {
		tp->real_touches = absinfo->maximum + 1;
		tp->slot = absinfo->value;
		tp->has_mt = true;
	} else {
		tp->real_touches = 1;
		tp->slot = 0;
		tp->has_mt = false;
	}

	tp->semi_mt = libevdev_has_property(device->evdev, INPUT_PROP_SEMI_MT);

	ARRAY_FOR_EACH(max_touches, m) {
		if (libevdev_has_event_code(device->evdev,
					    EV_KEY,
					    m->code)) {
			n_btn_tool_touches = m->ntouches;
			break;
		}
	}

	tp->ntouches = max(tp->real_touches, n_btn_tool_touches);
	tp->touches = calloc(tp->ntouches, sizeof(struct tp_touch));
	if (!tp->touches)
		return -1;

	for (i = 0; i < tp->ntouches; i++)
		tp_init_touch(tp, &tp->touches[i]);

	return 0;
}

static int
tp_init_accel(struct tp_dispatch *tp, double diagonal)
{
	int res_x, res_y;

	if (tp->has_mt) {
		res_x = libevdev_get_abs_resolution(tp->device->evdev,
						    ABS_MT_POSITION_X);
		res_y = libevdev_get_abs_resolution(tp->device->evdev,
						    ABS_MT_POSITION_Y);
	} else {
		res_x = libevdev_get_abs_resolution(tp->device->evdev,
						    ABS_X);
		res_y = libevdev_get_abs_resolution(tp->device->evdev,
						    ABS_Y);
	}

	/*
	 * Not all touchpads report the same amount of units/mm (resolution).
	 * Normalize motion events to the default mouse DPI as base
	 * (unaccelerated) speed. This also evens out any differences in x
	 * and y resolution, so that a circle on the
	 * touchpad does not turn into an elipse on the screen.
	 */
	if (res_x > 1 && res_y > 1) {
		tp->accel.x_scale_coeff = (DEFAULT_MOUSE_DPI/25.4) / res_x;
		tp->accel.y_scale_coeff = (DEFAULT_MOUSE_DPI/25.4) / res_y;

		/* FIXME: once normalized, touchpads see the same
		   acceleration as mice. that is technically correct but
		   subjectively wrong, we expect a touchpad to be a lot
		   slower than a mouse.
		   For now, apply a magic factor here until this is
		   fixed in the actual filter code.
		 */
		{
			const double MAGIC = 0.4;
			tp->accel.x_scale_coeff *= MAGIC;
			tp->accel.y_scale_coeff *= MAGIC;
		}
	} else {
	/*
	 * For touchpads where the driver does not provide resolution, fall
	 * back to scaling motion events based on the diagonal size in units.
	 */
		tp->accel.x_scale_coeff = DEFAULT_ACCEL_NUMERATOR / diagonal;
		tp->accel.y_scale_coeff = DEFAULT_ACCEL_NUMERATOR / diagonal;
	}

	if (evdev_device_init_pointer_acceleration(tp->device) == -1)
		return -1;

	return 0;
}

static uint32_t
tp_scroll_config_scroll_method_get_methods(struct libinput_device *device)
{
	struct evdev_device *evdev = (struct evdev_device*)device;
	struct tp_dispatch *tp = (struct tp_dispatch*)evdev->dispatch;
	uint32_t methods = LIBINPUT_CONFIG_SCROLL_NO_SCROLL;

	if (tp->ntouches >= 2)
		methods |= LIBINPUT_CONFIG_SCROLL_2FG;

	if (!tp->buttons.is_clickpad)
		methods |= LIBINPUT_CONFIG_SCROLL_EDGE;

	return methods;
}

static enum libinput_config_status
tp_scroll_config_scroll_method_set_method(struct libinput_device *device,
		        enum libinput_config_scroll_method method)
{
	struct evdev_device *evdev = (struct evdev_device*)device;
	struct tp_dispatch *tp = (struct tp_dispatch*)evdev->dispatch;

	if (method == tp->scroll.method)
		return LIBINPUT_CONFIG_STATUS_SUCCESS;

	tp_stop_scroll_events(tp, libinput_now(device->seat->libinput));
	tp->scroll.method = method;

	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static enum libinput_config_scroll_method
tp_scroll_config_scroll_method_get_method(struct libinput_device *device)
{
	struct evdev_device *evdev = (struct evdev_device*)device;
	struct tp_dispatch *tp = (struct tp_dispatch*)evdev->dispatch;

	return tp->scroll.method;
}

static enum libinput_config_scroll_method
tp_scroll_get_default_method(struct tp_dispatch *tp)
{
	if (tp->ntouches >= 2)
		return LIBINPUT_CONFIG_SCROLL_2FG;
	else
		return LIBINPUT_CONFIG_SCROLL_EDGE;
}

static enum libinput_config_scroll_method
tp_scroll_config_scroll_method_get_default_method(struct libinput_device *device)
{
	struct evdev_device *evdev = (struct evdev_device*)device;
	struct tp_dispatch *tp = (struct tp_dispatch*)evdev->dispatch;

	return tp_scroll_get_default_method(tp);
}

static int
tp_init_scroll(struct tp_dispatch *tp, struct evdev_device *device)
{
	if (tp_edge_scroll_init(tp, device) != 0)
		return -1;

	evdev_init_natural_scroll(device);

	tp->scroll.config_method.get_methods = tp_scroll_config_scroll_method_get_methods;
	tp->scroll.config_method.set_method = tp_scroll_config_scroll_method_set_method;
	tp->scroll.config_method.get_method = tp_scroll_config_scroll_method_get_method;
	tp->scroll.config_method.get_default_method = tp_scroll_config_scroll_method_get_default_method;
	tp->scroll.method = tp_scroll_get_default_method(tp);
	tp->device->base.config.scroll_method = &tp->scroll.config_method;

	/* In mm for touchpads with valid resolution, see tp_init_accel() */
	tp->device->scroll.threshold = 5.0;

	return 0;
}

static int
tp_init_palmdetect(struct tp_dispatch *tp,
		   struct evdev_device *device)
{
	int width;

	tp->palm.right_edge = INT_MAX;
	tp->palm.left_edge = INT_MIN;

	width = abs(device->abs.absinfo_x->maximum -
		    device->abs.absinfo_x->minimum);

	/* Apple touchpads are always big enough to warrant palm detection */
	if (evdev_device_get_id_vendor(device) != VENDOR_ID_APPLE) {
		/* We don't know how big the touchpad is */
		if (device->abs.absinfo_x->resolution == 1)
			return 0;

		/* Enable palm detection on touchpads >= 80 mm. Anything smaller
		   probably won't need it, until we find out it does */
		if (width/device->abs.absinfo_x->resolution < 80)
			return 0;
	}

	/* palm edges are 5% of the width on each side */
	tp->palm.right_edge = device->abs.absinfo_x->maximum - width * 0.05;
	tp->palm.left_edge = device->abs.absinfo_x->minimum + width * 0.05;

	return 0;
}

static int
tp_init_sendevents(struct tp_dispatch *tp,
		   struct evdev_device *device)
{
	libinput_timer_init(&tp->sendevents.trackpoint_timer,
			    tp->device->base.seat->libinput,
			    tp_trackpoint_timeout, tp);
	return 0;
}

static int
tp_init(struct tp_dispatch *tp,
	struct evdev_device *device)
{
	int width, height;
	double diagonal;

	tp->base.interface = &tp_interface;
	tp->device = device;

	if (tp_init_slots(tp, device) != 0)
		return -1;

	width = abs(device->abs.absinfo_x->maximum -
		    device->abs.absinfo_x->minimum);
	height = abs(device->abs.absinfo_y->maximum -
		     device->abs.absinfo_y->minimum);
	diagonal = sqrt(width*width + height*height);

	tp->hysteresis.margin_x =
		diagonal / DEFAULT_HYSTERESIS_MARGIN_DENOMINATOR;
	tp->hysteresis.margin_y =
		diagonal / DEFAULT_HYSTERESIS_MARGIN_DENOMINATOR;

	if (tp_init_accel(tp, diagonal) != 0)
		return -1;

	if (tp_init_tap(tp) != 0)
		return -1;

	if (tp_init_buttons(tp, device) != 0)
		return -1;

	if (tp_init_palmdetect(tp, device) != 0)
		return -1;

	if (tp_init_sendevents(tp, device) != 0)
		return -1;

	if (tp_init_scroll(tp, device) != 0)
		return -1;

	device->seat_caps |= EVDEV_DEVICE_POINTER;

	return 0;
}

static uint32_t
tp_sendevents_get_modes(struct libinput_device *device)
{
	struct evdev_device *evdev = (struct evdev_device*)device;
	uint32_t modes = LIBINPUT_CONFIG_SEND_EVENTS_DISABLED;

	if (evdev->tags & EVDEV_TAG_INTERNAL_TOUCHPAD)
		modes |= LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE;

	return modes;
}

static void
tp_suspend_conditional(struct tp_dispatch *tp,
		       struct evdev_device *device)
{
	struct libinput_device *dev;

	list_for_each(dev, &device->base.seat->devices_list, link) {
		struct evdev_device *d = (struct evdev_device*)dev;
		if (d->tags & EVDEV_TAG_EXTERNAL_MOUSE) {
			tp_suspend(tp, device);
			return;
		}
	}
}

static enum libinput_config_status
tp_sendevents_set_mode(struct libinput_device *device,
		       enum libinput_config_send_events_mode mode)
{
	struct evdev_device *evdev = (struct evdev_device*)device;
	struct tp_dispatch *tp = (struct tp_dispatch*)evdev->dispatch;

	/* DISABLED overrides any DISABLED_ON_ */
	if ((mode & LIBINPUT_CONFIG_SEND_EVENTS_DISABLED) &&
	    (mode & LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE))
	    mode &= ~LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE;

	if (mode == tp->sendevents.current_mode)
		return LIBINPUT_CONFIG_STATUS_SUCCESS;

	switch(mode) {
	case LIBINPUT_CONFIG_SEND_EVENTS_ENABLED:
		tp_resume(tp, evdev);
		break;
	case LIBINPUT_CONFIG_SEND_EVENTS_DISABLED:
		tp_suspend(tp, evdev);
		break;
	case LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE:
		tp_suspend_conditional(tp, evdev);
		break;
	default:
		return LIBINPUT_CONFIG_STATUS_UNSUPPORTED;
	}

	tp->sendevents.current_mode = mode;

	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static enum libinput_config_send_events_mode
tp_sendevents_get_mode(struct libinput_device *device)
{
	struct evdev_device *evdev = (struct evdev_device*)device;
	struct tp_dispatch *dispatch = (struct tp_dispatch*)evdev->dispatch;

	return dispatch->sendevents.current_mode;
}

static enum libinput_config_send_events_mode
tp_sendevents_get_default_mode(struct libinput_device *device)
{
	return LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;
}

static void
tp_change_to_left_handed(struct evdev_device *device)
{
	struct tp_dispatch *tp = (struct tp_dispatch *)device->dispatch;

	if (device->left_handed.want_enabled == device->left_handed.enabled)
		return;

	if (tp->buttons.state & 0x3) /* BTN_LEFT|BTN_RIGHT */
		return;

	/* tapping and clickfinger aren't affected by left-handed config,
	 * so checking physical buttons is enough */

	device->left_handed.enabled = device->left_handed.want_enabled;
}

struct model_lookup_t {
	uint16_t vendor;
	uint16_t product_start;
	uint16_t product_end;
	enum touchpad_model model;
};

static struct model_lookup_t model_lookup_table[] = {
	{ 0x0002, 0x0007, 0x0007, MODEL_SYNAPTICS },
	{ 0x0002, 0x0008, 0x0008, MODEL_ALPS },
	{ 0x0002, 0x000e, 0x000e, MODEL_ELANTECH },
	{ 0x05ac,      0, 0x0222, MODEL_APPLETOUCH },
	{ 0x05ac, 0x0223, 0x0228, MODEL_UNIBODY_MACBOOK },
	{ 0x05ac, 0x0229, 0x022b, MODEL_APPLETOUCH },
	{ 0x05ac, 0x022c, 0xffff, MODEL_UNIBODY_MACBOOK },
	{ 0, 0, 0, 0 }
};

static enum touchpad_model
tp_get_model(struct evdev_device *device)
{
	struct model_lookup_t *lookup;
	uint16_t vendor  = libevdev_get_id_vendor(device->evdev);
	uint16_t product = libevdev_get_id_product(device->evdev);

	for (lookup = model_lookup_table; lookup->vendor; lookup++) {
		if (lookup->vendor == vendor &&
		    lookup->product_start <= product &&
		    product <= lookup->product_end)
			return lookup->model;
	}
	return MODEL_UNKNOWN;
}

struct evdev_dispatch *
evdev_mt_touchpad_create(struct evdev_device *device)
{
	struct tp_dispatch *tp;

	tp = zalloc(sizeof *tp);
	if (!tp)
		return NULL;

	tp->model = tp_get_model(device);

	if (tp_init(tp, device) != 0) {
		tp_destroy(&tp->base);
		return NULL;
	}

	device->base.config.sendevents = &tp->sendevents.config;

	tp->sendevents.current_mode = LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;
	tp->sendevents.config.get_modes = tp_sendevents_get_modes;
	tp->sendevents.config.set_mode = tp_sendevents_set_mode;
	tp->sendevents.config.get_mode = tp_sendevents_get_mode;
	tp->sendevents.config.get_default_mode = tp_sendevents_get_default_mode;

	evdev_init_left_handed(device, tp_change_to_left_handed);

	return  &tp->base;
}
