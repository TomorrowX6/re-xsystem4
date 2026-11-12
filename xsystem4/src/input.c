/* Copyright (C) 2019 Nunuhara Cabbage <nunuhara@haniwa.technology>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <time.h>
#include <SDL.h>
#ifdef __ANDROID__
#include <SDL_system.h>
#endif
#include "system4.h"
#include "queue.h"
#include "gfx/gfx.h"
#include "gfx/private.h"
#include "input.h"
#include "parts.h"
#include "savedata.h"
#include "scene.h"
#include "mixer.h"
#include "vm.h"
#include "xsystem4.h"
#include "debugger.h"

#ifndef M_PI
#define M_PI (3.14159265358979323846)
#endif

bool key_state[VK_NR_KEYCODES];
static bool hook_close_app = false;
static unsigned pending_close_requests = 0;
static uint32_t first_unpopped_close_tick = 0;
static bool kbd_ctrl_down = false;
static volatile bool touch_ctrl_skip = false;

void input_reset_close_hook(void)
{
	hook_close_app = false;
	pending_close_requests = 0;
	first_unpopped_close_tick = 0;
}

void input_set_close_hook(bool enabled)
{
	hook_close_app = enabled;
	if (!enabled) {
		pending_close_requests = 0;
		first_unpopped_close_tick = 0;
	}
}

bool input_get_close_hook(void)
{
	return hook_close_app;
}

bool input_has_system_message(void)
{
	return pending_close_requests != 0;
}

bool input_pop_system_message(int *message)
{
	if (!message || !pending_close_requests)
		return false;
	pending_close_requests--;
	*message = 7; // AppClosing consumed by SystemService.PopSystemMessage
	return true;
}

static bool input_hook_close_request(void)
{
	if (!hook_close_app)
		return false;
	uint32_t now = SDL_GetTicks();
	if (pending_close_requests == 0) {
		pending_close_requests = 1;
		first_unpopped_close_tick = now;
		return true;
	}
	if ((now - first_unpopped_close_tick) >= 1500)
		return false;
	return true;
}

static void sync_ctrl_key_state(void)
{
	key_state[VK_CONTROL] = kbd_ctrl_down || touch_ctrl_skip;
}

void input_set_touch_ctrl_skip(bool enabled)
{
	touch_ctrl_skip = enabled;
	sync_ctrl_key_state();
}

bool input_get_touch_ctrl_skip(void)
{
	return touch_ctrl_skip;
}
static enum sact_keycode sdl_keytable[] = {
	[SDL_SCANCODE_0] = VK_0,
	[SDL_SCANCODE_1] = VK_1,
	[SDL_SCANCODE_2] = VK_2,
	[SDL_SCANCODE_3] = VK_3,
	[SDL_SCANCODE_4] = VK_4,
	[SDL_SCANCODE_5] = VK_5,
	[SDL_SCANCODE_6] = VK_6,
	[SDL_SCANCODE_7] = VK_7,
	[SDL_SCANCODE_8] = VK_8,
	[SDL_SCANCODE_9] = VK_9,
	[SDL_SCANCODE_A] = VK_A,
	[SDL_SCANCODE_B] = VK_B,
	[SDL_SCANCODE_C] = VK_C,
	[SDL_SCANCODE_D] = VK_D,
	[SDL_SCANCODE_E] = VK_E,
	[SDL_SCANCODE_F] = VK_F,
	[SDL_SCANCODE_G] = VK_G,
	[SDL_SCANCODE_H] = VK_H,
	[SDL_SCANCODE_I] = VK_I,
	[SDL_SCANCODE_J] = VK_J,
	[SDL_SCANCODE_K] = VK_K,
	[SDL_SCANCODE_L] = VK_L,
	[SDL_SCANCODE_M] = VK_M,
	[SDL_SCANCODE_N] = VK_N,
	[SDL_SCANCODE_O] = VK_O,
	[SDL_SCANCODE_P] = VK_P,
	[SDL_SCANCODE_Q] = VK_Q,
	[SDL_SCANCODE_R] = VK_R,
	[SDL_SCANCODE_S] = VK_S,
	[SDL_SCANCODE_T] = VK_T,
	[SDL_SCANCODE_U] = VK_U,
	[SDL_SCANCODE_V] = VK_V,
	[SDL_SCANCODE_W] = VK_W,
	[SDL_SCANCODE_X] = VK_X,
	[SDL_SCANCODE_Y] = VK_Y,
	[SDL_SCANCODE_Z] = VK_Z,
	[SDL_SCANCODE_BACKSPACE] = VK_BACK,
	[SDL_SCANCODE_TAB] = VK_TAB,
	[SDL_SCANCODE_CLEAR] = VK_CLEAR,
	[SDL_SCANCODE_RETURN] = VK_RETURN,
	[SDL_SCANCODE_LSHIFT] = VK_SHIFT,
	[SDL_SCANCODE_RSHIFT] = VK_SHIFT,
	[SDL_SCANCODE_LCTRL] = VK_CONTROL,
	[SDL_SCANCODE_RCTRL] = VK_CONTROL,
	[SDL_SCANCODE_LALT] = VK_MENU,
	[SDL_SCANCODE_RALT] = VK_MENU,
	[SDL_SCANCODE_PAUSE] = VK_PAUSE,
	[SDL_SCANCODE_CAPSLOCK] = VK_CAPITAL,
	[SDL_SCANCODE_ESCAPE] = VK_ESCAPE,
	[SDL_SCANCODE_SPACE] = VK_SPACE,
	[SDL_SCANCODE_PAGEUP] = VK_PRIOR,
	[SDL_SCANCODE_PAGEDOWN] = VK_NEXT,
	[SDL_SCANCODE_END] = VK_END,
	[SDL_SCANCODE_HOME] = VK_HOME,
	[SDL_SCANCODE_LEFT] = VK_LEFT,
	[SDL_SCANCODE_UP] = VK_UP,
	[SDL_SCANCODE_RIGHT] = VK_RIGHT,
	[SDL_SCANCODE_DOWN] = VK_DOWN,
	[SDL_SCANCODE_SELECT] = VK_SELECT,
	[SDL_SCANCODE_EXECUTE] = VK_EXECUTE,
	[SDL_SCANCODE_PRINTSCREEN] = VK_SNAPSHOT,
	[SDL_SCANCODE_INSERT] = VK_INSERT,
	[SDL_SCANCODE_DELETE] = VK_DELETE,
	[SDL_SCANCODE_HELP] = VK_HELP,
	[SDL_SCANCODE_KP_0] = VK_NUMPAD0,
	[SDL_SCANCODE_KP_1] = VK_NUMPAD1,
	[SDL_SCANCODE_KP_2] = VK_NUMPAD2,
	[SDL_SCANCODE_KP_3] = VK_NUMPAD3,
	[SDL_SCANCODE_KP_4] = VK_NUMPAD4,
	[SDL_SCANCODE_KP_5] = VK_NUMPAD5,
	[SDL_SCANCODE_KP_6] = VK_NUMPAD6,
	[SDL_SCANCODE_KP_7] = VK_NUMPAD7,
	[SDL_SCANCODE_KP_8] = VK_NUMPAD8,
	[SDL_SCANCODE_KP_9] = VK_NUMPAD9,
	[SDL_SCANCODE_KP_MULTIPLY] = VK_MULTIPLY,
	[SDL_SCANCODE_KP_PLUS] = VK_ADD,
	[SDL_SCANCODE_KP_VERTICALBAR] = VK_SEPARATOR, // ???
	[SDL_SCANCODE_KP_MINUS] = VK_SUBTRACT,
	[SDL_SCANCODE_KP_PERIOD] = VK_DECIMAL,
	[SDL_SCANCODE_KP_DIVIDE] = VK_DIVIDE,
	[SDL_SCANCODE_KP_ENTER] = VK_RETURN,
	[SDL_SCANCODE_F1] = VK_F1,
	[SDL_SCANCODE_F2] = VK_F2,
	[SDL_SCANCODE_F3] = VK_F3,
	[SDL_SCANCODE_F4] = VK_F4,
	[SDL_SCANCODE_F5] = VK_F5,
	[SDL_SCANCODE_F6] = VK_F6,
	[SDL_SCANCODE_F7] = VK_F7,
	[SDL_SCANCODE_F8] = VK_F8,
	[SDL_SCANCODE_F9] = VK_F9,
	[SDL_SCANCODE_F10] = VK_F10,
	[SDL_SCANCODE_F11] = VK_F11,
	[SDL_SCANCODE_F12] = VK_F12,
	[SDL_SCANCODE_F13] = VK_F13,
	[SDL_SCANCODE_F14] = VK_F14,
	[SDL_SCANCODE_F15] = VK_F15,
	[SDL_SCANCODE_F16] = VK_F16,
	[SDL_SCANCODE_F17] = VK_F17,
	[SDL_SCANCODE_F18] = VK_F18,
	[SDL_SCANCODE_F19] = VK_F19,
	[SDL_SCANCODE_F20] = VK_F20,
	[SDL_SCANCODE_F21] = VK_F21,
	[SDL_SCANCODE_F22] = VK_F22,
	[SDL_SCANCODE_F23] = VK_F23,
	[SDL_SCANCODE_F24] = VK_F24,
	[SDL_SCANCODE_NUMLOCKCLEAR] = VK_NUMLOCK,
	[SDL_SCANCODE_SCROLLLOCK] = VK_SCROLL,
};

static enum sact_keycode sdl_to_sact_key(SDL_Scancode scancode, SDL_Keymod modifiers)
{
	if (!(modifiers & KMOD_NUM)) {
		switch (scancode) {
		case SDL_SCANCODE_KP_0:      return VK_INSERT;
		case SDL_SCANCODE_KP_1:      return VK_END;
		case SDL_SCANCODE_KP_2:      return VK_DOWN;
		case SDL_SCANCODE_KP_3:      return VK_NEXT;
		case SDL_SCANCODE_KP_4:      return VK_LEFT;
		case SDL_SCANCODE_KP_5:      return VK_CLEAR;
		case SDL_SCANCODE_KP_6:      return VK_RIGHT;
		case SDL_SCANCODE_KP_7:      return VK_HOME;
		case SDL_SCANCODE_KP_8:      return VK_UP;
		case SDL_SCANCODE_KP_9:      return VK_PRIOR;
		case SDL_SCANCODE_KP_PERIOD: return VK_DELETE;
		default: break;
		}
	}
	if (scancode >= sizeof(sdl_keytable) / sizeof(*sdl_keytable))
		return 0;
	return sdl_keytable[scancode];
}

bool mouse_focus = true;
bool keyboard_focus = true;

#define MAX_CONTROLLERS 4
static SDL_GameController *controllers[MAX_CONTROLLERS];

#ifdef __ANDROID__
struct deferred_keyevent {
	STAILQ_ENTRY(deferred_keyevent) entry;
	SDL_Event e;
};
static STAILQ_HEAD(deferred_keyevent_queue, deferred_keyevent) deferred_keyevent_queue =
	STAILQ_HEAD_INITIALIZER(deferred_keyevent_queue);
static uint32_t last_keyevent_timestamp;
#define DEFERRED_KEY_DELAY 10
#endif

// Stores mouse button events synthesized from touch events. We defer such
// events so the game first observes the pointer at the new position (hover),
// then at least one pressed frame, and then the release frame.
#define MAX_DEFERRED_SYNTHETIC_MOUSE_EVENTS 32
static SDL_MouseButtonEvent deferred_synthetic_mouse_events[MAX_DEFERRED_SYNTHETIC_MOUSE_EVENTS];
static int nr_deferred_synthetic_mouse_events = 0;
static bool synthetic_mouse_hover_observed = false;
static bool synthetic_mouse_down_observed = false;
static bool synthetic_mouse_up_observed = true;
static bool has_synthetic_mouse_pos = false;
static int synthetic_mouse_x = 0;
static int synthetic_mouse_y = 0;
static bool has_last_touch_down_pos = false;
static SDL_Point last_touch_down_pos = {0, 0};
static uint32_t last_parts_poll_timestamp = 0;
static bool in_parts_poll = false;
static bool mouse_down_unobserved[VK_NR_KEYCODES];
static uint32_t mouse_down_timestamp[VK_NR_KEYCODES];
static bool deferred_mouse_release[VK_NR_KEYCODES];
#define DEFERRED_MOUSE_RELEASE_MAX_DELAY 100
#define SYNTHETIC_MOUSE_EVENT_DELAY 50
#define SYNTHETIC_MOUSE_RELEASE_FALLBACK_DELAY 250

static void fire_deferred_events(bool from_parts_poll, bool allow_press);

void input_parts_poll_begin(bool allow_press)
{
	in_parts_poll = true;
	if (allow_press)
		last_parts_poll_timestamp = SDL_GetTicks();
	handle_events();
	fire_deferred_events(true, allow_press);
}

void input_notify_parts_poll(bool allow_press, bool clicking)
{
	in_parts_poll = false;
	if (allow_press && clicking) {
		synthetic_mouse_down_observed = true;
		if (key_state[VK_LBUTTON])
			mouse_down_unobserved[VK_LBUTTON] = false;
		if (key_state[VK_RBUTTON])
			mouse_down_unobserved[VK_RBUTTON] = false;
		if (key_state[VK_MBUTTON])
			mouse_down_unobserved[VK_MBUTTON] = false;
	} else if (!clicking) {
		synthetic_mouse_up_observed = true;
		if (allow_press && nr_deferred_synthetic_mouse_events > 0 &&
		    deferred_synthetic_mouse_events[0].state == SDL_PRESSED) {
			synthetic_mouse_x = deferred_synthetic_mouse_events[0].x;
			synthetic_mouse_y = deferred_synthetic_mouse_events[0].y;
			has_synthetic_mouse_pos = true;
			synthetic_mouse_hover_observed = true;
		}
	}
}

void input_cancel_long_touch(void)
{
}

static bool multi_touch_active = false;
static bool two_finger_tap_candidate = false;
static bool two_finger_scrolled = false;
static uint32_t two_finger_tap_start_tick = 0;
#define TWO_FINGER_TAP_MAX_MS 300

static float scroll_gesture_y;
#define SCROLL_GESTURE_SENSITIVITY 0.02f  // 2% of screen height

static enum sact_keycode sdl_to_sact_button(int button)
{
	switch (button) {
	case SDL_BUTTON_LEFT:   return VK_LBUTTON;
	case SDL_BUTTON_RIGHT:  return VK_RBUTTON;
	case SDL_BUTTON_MIDDLE: return VK_MBUTTON;
	default:                return 0;
	}
}

bool key_is_down(enum sact_keycode code)
{
	if (code < 0 || code >= VK_NR_KEYCODES) {
		return false;
	}
	if (code == VK_CONTROL && touch_ctrl_skip) {
		key_state[VK_CONTROL] = true;
	}
	bool is_mouse_btn = (code == VK_LBUTTON || code == VK_RBUTTON || code == VK_MBUTTON);
	bool parts_polling = last_parts_poll_timestamp != 0 &&
		(SDL_GetTicks() - last_parts_poll_timestamp) < 250;
	if (!in_parts_poll && !(is_mouse_btn && parts_polling) && key_state[code]) {
		mouse_down_unobserved[code] = false;
	}
	return key_state[code];
}

void key_clear_flag(bool no_ctrl)
{
	for (int i = 0; i < VK_NR_KEYCODES; i++) {
		if (no_ctrl && i == VK_CONTROL)
			continue;
		key_state[i] = false;
		mouse_down_unobserved[i] = false;
		deferred_mouse_release[i] = false;
	}
	if (touch_ctrl_skip)
		key_state[VK_CONTROL] = true;
}

void mouse_get_pos(int *x, int *y)
{
	int wx, wy;
	fire_deferred_events(false, false);
	SDL_PumpEvents();
	SDL_GetMouseState(&wx, &wy);
	if (has_synthetic_mouse_pos) {
		if (nr_deferred_synthetic_mouse_events == 0 &&
		    synthetic_mouse_down_observed &&
		    (key_state[VK_LBUTTON] || key_state[VK_RBUTTON])) {
			synthetic_mouse_x = wx;
			synthetic_mouse_y = wy;
		}
		wx = synthetic_mouse_x;
		wy = synthetic_mouse_y;
	}
	*x = (wx - sdl.viewport.x) * sdl.w / sdl.viewport.w;
	*y = (wy - sdl.viewport.y) * sdl.h / sdl.viewport.h;
}

void mouse_set_pos(int x, int y)
{
	int wx = x * sdl.viewport.w / sdl.w + sdl.viewport.x;
	int wy = y * sdl.viewport.h / sdl.h + sdl.viewport.y;
	has_synthetic_mouse_pos = false;
	SDL_WarpMouseInWindow(sdl.window, wx, wy);
}

static int wheel_dir = 0;

void mouse_get_wheel(int *forward, int *back)
{
	*forward = wheel_dir > 0;
	*back = wheel_dir < 0;
}

void mouse_clear_wheel(void)
{
	wheel_dir = 0;
}

bool mouse_show_cursor(bool show)
{
	return SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE) >= 0;
}

static void(*input_handler)(const char*);
static void(*editing_handler)(const char*, int, int);
static void(*key_handler)(int);

static void key_event(SDL_KeyboardEvent *e, bool pressed)
{
	enum sact_keycode code = sdl_to_sact_key(e->keysym.scancode, e->keysym.mod);
	if (code) {
		if (code == VK_CONTROL) {
			kbd_ctrl_down = pressed;
			sync_ctrl_key_state();
		} else {
			key_state[code] = pressed;
		}
		if (pressed && key_handler)
			key_handler(code);
	}
}

static void mouse_event(SDL_MouseButtonEvent *e)
{
	enum sact_keycode code = sdl_to_sact_button(e->button);
	if (code) {
		if (e->state == SDL_PRESSED) {
			deferred_mouse_release[code] = false;
			key_state[code] = true;
			mouse_down_unobserved[code] = true;
			mouse_down_timestamp[code] = SDL_GetTicks();
		} else if (key_state[code] && mouse_down_unobserved[code]) {
			deferred_mouse_release[code] = true;
		} else {
			key_state[code] = false;
			deferred_mouse_release[code] = false;
			mouse_down_unobserved[code] = false;
		}
	}
#ifdef DEBUGGER_ENABLED
	if (e->button == SDL_BUTTON_MIDDLE && e->state == SDL_PRESSED
			&& (dbg_start_in_debugger || dbg_dap))
		dbg_repl(DBG_STOP_PAUSE, NULL);
#endif
}

static void synthetic_mouse_event(SDL_MouseButtonEvent *e)
{
	if (e->state == SDL_PRESSED) {
		if (e->button == SDL_BUTTON_RIGHT) {
			key_state[VK_RBUTTON] = true;
			return;
		}
		SDL_Point p = { .x = e->x, .y = e->y };
		if (SDL_PointInRect(&p, &sdl.viewport)) {
			key_state[VK_LBUTTON] = true;
		} else {
#ifdef __ANDROID__
			const int COMMAND_TOGGLE_CTRL_OVERLAY = 0x8002;
			SDL_AndroidSendMessage(COMMAND_TOGGLE_CTRL_OVERLAY, 0);
#else
			key_state[VK_RBUTTON] = true;
#endif
		}
	} else {
		key_state[VK_LBUTTON] = false;
		key_state[VK_RBUTTON] = false;
	}
}

#define JOYAXIS_DEADZONE 13500

enum joyaxis_axis {
	JOYAXIS_X,
	JOYAXIS_Y
};

enum joyaxis_direction {
	JOYAXIS_DIR_LEFT_UP    = -1,
	JOYAXIS_DIR_NONE       =  0,
	JOYAXIS_DIR_RIGHT_DOWN =  1
};

enum joyaxis_state {
	// A: Initial state
	// - when joy_clear_flag is called, goes on long cooldown
	//   and then enters JOYAXIS_STATE_B
	JOYAXIS_STATE_A,
	// B: Held state
	// - when joy_clear_flag is called, goes on short cooldown
	//   state remains JOYAXIS_STATE_B until direction changes
	JOYAXIS_STATE_B,
};

struct joyaxis {
	enum joyaxis_state state;
	unsigned long cooldown_expire_time;
	enum joyaxis_direction real_direction;
	enum joyaxis_direction logical_direction;
};

struct joyaxis joyaxis_state[2] = { 0 };

bool joybutton_state[SDL_CONTROLLER_BUTTON_MAX];

static unsigned long joy_time(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((unsigned long)ts.tv_sec * 1000) + ((unsigned long)ts.tv_nsec / 1000000);
}

static bool joy_check_axis(struct joyaxis *axis, enum joyaxis_direction direction)
{
	if (axis->logical_direction == direction)
		return true;
	if (!axis->cooldown_expire_time)
		return false;
	// on cooldown: check if cooldown expired
	if (joy_time() >= axis->cooldown_expire_time) {
		axis->state = JOYAXIS_STATE_B;
		axis->cooldown_expire_time = 0;
		axis->logical_direction = axis->real_direction;
	}
	return axis->logical_direction == direction;
}

bool joy_key_is_down(uint8_t code)
{
	// Default SACT key mapping in comments below
	switch (code) {
	case 1: // Left Arrow
		return joy_check_axis(&joyaxis_state[JOYAXIS_X], JOYAXIS_DIR_LEFT_UP);
	case 2: // Right Arrow
		return joy_check_axis(&joyaxis_state[JOYAXIS_X], JOYAXIS_DIR_RIGHT_DOWN);
	case 3: // Up Arrow
		return joy_check_axis(&joyaxis_state[JOYAXIS_Y], JOYAXIS_DIR_LEFT_UP);
	case 4: // Down Arrow
		return joy_check_axis(&joyaxis_state[JOYAXIS_Y], JOYAXIS_DIR_RIGHT_DOWN);
	case 5: // Esc
		return joybutton_state[SDL_CONTROLLER_BUTTON_A];
	case 6: // Enter
		return joybutton_state[SDL_CONTROLLER_BUTTON_B];
	case 7: // Space
		return joybutton_state[SDL_CONTROLLER_BUTTON_X];
	case 8: // Ctrl
		return joybutton_state[SDL_CONTROLLER_BUTTON_Y];
	case 9: // a
		return joybutton_state[SDL_CONTROLLER_BUTTON_LEFTSHOULDER];
	case 10: // z
		return joybutton_state[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER];
	case 11: // Page Up
		return joybutton_state[SDL_CONTROLLER_BUTTON_BACK];
	case 12: // Page Down
		return joybutton_state[SDL_CONTROLLER_BUTTON_START];
	case 13:
		return joybutton_state[SDL_CONTROLLER_BUTTON_GUIDE];
	case 14:
		return joybutton_state[SDL_CONTROLLER_BUTTON_LEFTSTICK];
	case 15:
		return joybutton_state[SDL_CONTROLLER_BUTTON_RIGHTSTICK];
	case 16:
		return joybutton_state[SDL_CONTROLLER_BUTTON_DPAD_LEFT];
	case 17:
		return joybutton_state[SDL_CONTROLLER_BUTTON_DPAD_RIGHT];
	case 18:
		return joybutton_state[SDL_CONTROLLER_BUTTON_DPAD_UP];
	case 19:
		return joybutton_state[SDL_CONTROLLER_BUTTON_DPAD_DOWN];
	default:
		return false;
	}
}

static float joy_calc_deg(int x, int y)
{
	// calculate angle (degrees)
	float a = atan2f(y, x) * (180.0f/M_PI);
	if (a < 0)
		a += 360.0f;
	// rotate so that up is 0/360
	a += 90.0f;
	if (a > 360.0f)
		a -= 360.0f;
	return a;
}

static float joy_calc_pow(int x, int y)
{
	// XXX: in theory this should be scaled according to the angle so that
	//      (32767,32767) returns exactly 1, but alicesoft themselves seem
	//      to simply clamp pythagoras as below
	return min(1.0f, sqrtf(x*x + y*y) / 32767.0f);
}

static float joy_calc_trigger(int left, int right)
{
	// for whatever reason alicesoft treats triggers as an analog stick
	// (at least on an xbox controller)
	float v = 315.0f;
	v += ((float)left/32767.0f) * 90.0f;
	v -= ((float)right/32767.0f) * 90.0f;
	if (v < 0)
		v += 360.0f;
	if (v > 360.0f)
		v -= 360.0f;
	return v;
}

bool joy_get_stick_status(int controller, int type, float *deg, float *pow)
{
	SDL_GameController *ctrl = controllers[controller];
	if (!ctrl)
		goto invalid;
	if (type == 0) {
		int x = SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_LEFTX);
		int y = SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_LEFTY);
		if (abs(x) < JOYAXIS_DEADZONE && abs(y) < JOYAXIS_DEADZONE) {
			*pow = 0.0f;
			*deg = 0.0f;
		} else {
			*pow = joy_calc_pow(x, y);
			*deg = joy_calc_deg(x, y);
		}
		return true;
	}
	if (type == 1) {
		int left = SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
		int right = SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
		*pow = 1.0f;
		*deg = joy_calc_trigger(left, right);
		return true;
	}
invalid:
	*deg = 0.0f;
	*pow = 0.0f;
	return false;
}

static void joyaxis_clear_flag(struct joyaxis *joy)
{
	// put axis on cooldown
	if (joy->logical_direction) {
		joy->logical_direction = 0;
		if (joy->state == JOYAXIS_STATE_A) {
			joy->cooldown_expire_time = joy_time() + 300;
		} else {
			joy->cooldown_expire_time = joy_time() + 50;
		}
	}
}

void joy_clear_flag(void)
{
	for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
		joybutton_state[i] = false;
	}
	joyaxis_clear_flag(&joyaxis_state[JOYAXIS_X]);
	joyaxis_clear_flag(&joyaxis_state[JOYAXIS_Y]);
}

static void joyaxis_update(enum joyaxis_axis axis, int value)
{
	// determine direction
	int direction = 0;
	if (value < -JOYAXIS_DEADZONE)
		direction = -1;
	else if (value > JOYAXIS_DEADZONE)
		direction = 1;

	// reset axis state if direction changed
	if (direction != joyaxis_state[axis].real_direction) {
		joyaxis_state[axis].state = JOYAXIS_STATE_A;
		joyaxis_state[axis].cooldown_expire_time = 0;
		joyaxis_state[axis].real_direction = direction;
		joyaxis_state[axis].logical_direction = direction;
	}
}

static void controller_axis_event(SDL_ControllerAxisEvent *e)
{
	switch (e->axis) {
	case SDL_CONTROLLER_AXIS_LEFTX:
		joyaxis_update(JOYAXIS_X, e->value);
		break;
	case SDL_CONTROLLER_AXIS_LEFTY:
		joyaxis_update(JOYAXIS_Y, e->value);
		break;
	default:
		break;
	}
}

static void controller_button_event(SDL_ControllerButtonEvent *e)
{
	joybutton_state[e->button] = e->state == SDL_PRESSED;
}

void register_input_handler(void(*handler)(const char*))
{
	input_handler = handler;
}

void clear_input_handler(void)
{
	input_handler = NULL;
}

void register_editing_handler(void(*handler)(const char*, int, int))
{
	editing_handler = handler;
}

void clear_editing_handler(void)
{
	editing_handler = NULL;
}

void register_key_handler(void(*handler)(int))
{
	key_handler = handler;
}

void clear_key_handler(void)
{
	key_handler = NULL;
}

static void pop_deferred_synthetic_mouse_event(uint32_t now)
{
	SDL_MouseButtonEvent *ev = &deferred_synthetic_mouse_events[0];
	synthetic_mouse_event(ev);
	synthetic_mouse_x = ev->x;
	synthetic_mouse_y = ev->y;
	has_synthetic_mouse_pos = true;
	if (ev->state == SDL_PRESSED) {
		synthetic_mouse_down_observed = false;
	} else {
		synthetic_mouse_up_observed = false;
		synthetic_mouse_hover_observed = false;
	}
	nr_deferred_synthetic_mouse_events--;
	for (int i = 0; i < nr_deferred_synthetic_mouse_events; i++) {
		deferred_synthetic_mouse_events[i] = deferred_synthetic_mouse_events[i + 1];
	}
	if (nr_deferred_synthetic_mouse_events > 0) {
		deferred_synthetic_mouse_events[0].timestamp = now;
	}
}

static void fire_deferred_events(bool from_parts_poll, bool allow_press)
{
	uint32_t now = SDL_GetTicks();
	bool parts_active = last_parts_poll_timestamp != 0 && (now - last_parts_poll_timestamp) < 500;

	// Flush the next deferred synthetic mouse button event once the game
	// has observed the preceding hover/press/release phase (or after the timeout).
	while (nr_deferred_synthetic_mouse_events > 0 &&
	       deferred_synthetic_mouse_events[0].state == SDL_RELEASED &&
	       !key_state[VK_LBUTTON] && !key_state[VK_RBUTTON]) {
		pop_deferred_synthetic_mouse_event(now);
		synthetic_mouse_up_observed = true;
	}

	if (nr_deferred_synthetic_mouse_events > 0) {
		SDL_MouseButtonEvent *ev = &deferred_synthetic_mouse_events[0];
		if (from_parts_poll) {
			if (ev->state == SDL_PRESSED) {
				if (synthetic_mouse_up_observed) {
					if (!synthetic_mouse_hover_observed || !allow_press) {
						synthetic_mouse_x = ev->x;
						synthetic_mouse_y = ev->y;
						has_synthetic_mouse_pos = true;
					} else {
						pop_deferred_synthetic_mouse_event(now);
					}
				}
			} else {
				if (synthetic_mouse_down_observed) {
					if (has_synthetic_mouse_pos &&
					    (synthetic_mouse_x != ev->x || synthetic_mouse_y != ev->y)) {
						synthetic_mouse_x = ev->x;
						synthetic_mouse_y = ev->y;
					} else {
						pop_deferred_synthetic_mouse_event(now);
					}
				}
			}
		} else if (!parts_active) {
			bool ready = false;
			if (ev->state == SDL_PRESSED) {
				synthetic_mouse_x = ev->x;
				synthetic_mouse_y = ev->y;
				has_synthetic_mouse_pos = true;
				ready = now >= ev->timestamp + SYNTHETIC_MOUSE_EVENT_DELAY;
			} else {
				ready = now >= ev->timestamp + SYNTHETIC_MOUSE_RELEASE_FALLBACK_DELAY;
			}
			if (ready) {
				bool was_pressed = ev->state == SDL_PRESSED;
				pop_deferred_synthetic_mouse_event(now);
				if (!was_pressed)
					synthetic_mouse_up_observed = true;
			}
		}
	}

#ifdef __ANDROID__
	// Fire the deferred keyboard and text input events.
	while (!STAILQ_EMPTY(&deferred_keyevent_queue) && now >= last_keyevent_timestamp + DEFERRED_KEY_DELAY) {
		struct deferred_keyevent *ev = STAILQ_FIRST(&deferred_keyevent_queue);
		switch (ev->e.type) {
		case SDL_TEXTINPUT:
			if (input_handler)
				input_handler(ev->e.text.text);
			break;
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			last_keyevent_timestamp = now;
			key_event(&ev->e.key, ev->e.type == SDL_KEYDOWN);
			break;
		}
		STAILQ_REMOVE_HEAD(&deferred_keyevent_queue, entry);
		free(ev);
	}
#endif
}

static void handle_window_event(SDL_Event *e)
{
	switch (e->window.event) {
	case SDL_WINDOWEVENT_EXPOSED:
		gfx_swap();
		break;
	case SDL_WINDOWEVENT_ENTER:
		mouse_focus = true;
		break;
	case SDL_WINDOWEVENT_LEAVE:
#ifndef __ANDROID__
		mouse_focus = false;
#endif
		break;
	case SDL_WINDOWEVENT_FOCUS_GAINED:
		mouse_focus = true;
		keyboard_focus = true;
		mixer_set_window_focus(true);
		break;
	case SDL_WINDOWEVENT_FOCUS_LOST:
#ifndef __ANDROID__
		keyboard_focus = false;
#endif
		kbd_ctrl_down = false;
		touch_ctrl_skip = false;
		key_state[VK_CONTROL] = false;
		mixer_set_window_focus(false);
		break;
	case SDL_WINDOWEVENT_SIZE_CHANGED:
		gfx_update_screen_scale();
		gfx_swap();
		break;
	}
}

static void enqueue_synthetic_mouse_button(const SDL_MouseButtonEvent *btn)
{
	if (nr_deferred_synthetic_mouse_events == MAX_DEFERRED_SYNTHETIC_MOUSE_EVENTS) {
		pop_deferred_synthetic_mouse_event(SDL_GetTicks());
	}
	if (nr_deferred_synthetic_mouse_events == 0 && btn->state == SDL_PRESSED) {
		synthetic_mouse_hover_observed = false;
		if (!key_state[VK_LBUTTON] && !key_state[VK_RBUTTON]) {
			synthetic_mouse_x = btn->x;
			synthetic_mouse_y = btn->y;
			has_synthetic_mouse_pos = true;
		}
	}
	deferred_synthetic_mouse_events[nr_deferred_synthetic_mouse_events] = *btn;
	deferred_synthetic_mouse_events[nr_deferred_synthetic_mouse_events].timestamp = SDL_GetTicks();
	nr_deferred_synthetic_mouse_events++;
}

void handle_window_events(void)
{
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_QUIT:
			if (input_hook_close_request())
				break;
			savedata_flush_on_background();
			vm_exit(0);
			break;
		case SDL_WINDOWEVENT:
			handle_window_event(&e);
			break;
		case SDL_APP_WILLENTERBACKGROUND:
			kbd_ctrl_down = false;
			touch_ctrl_skip = false;
			key_state[VK_CONTROL] = false;
			savedata_flush_on_background();
			break;
		}
	}
}

void handle_events(void)
{
	static const enum sact_keycode mouse_btns[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
	uint32_t poll_now = SDL_GetTicks();
	for (size_t i = 0; i < sizeof(mouse_btns) / sizeof(mouse_btns[0]); i++) {
		enum sact_keycode code = mouse_btns[i];
		if (deferred_mouse_release[code] &&
		    (!mouse_down_unobserved[code] || (poll_now - mouse_down_timestamp[code]) >= DEFERRED_MOUSE_RELEASE_MAX_DELAY)) {
			key_state[code] = false;
			deferred_mouse_release[code] = false;
			mouse_down_unobserved[code] = false;
		}
	}

	fire_deferred_events(false, false);

	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_QUIT:
			if (input_hook_close_request())
				break;
			savedata_flush_on_background();
			vm_exit(0);
			break;
		case SDL_WINDOWEVENT:
			handle_window_event(&e);
			break;
		case SDL_APP_LOWMEMORY:
			PE_FlushCGCache();
			break;
		case SDL_APP_WILLENTERBACKGROUND:
			kbd_ctrl_down = false;
			touch_ctrl_skip = false;
			key_state[VK_CONTROL] = false;
			savedata_flush_on_background();
			break;
		case SDL_APP_DIDENTERFOREGROUND:
			gfx_swap();
			break;
		case SDL_AUDIODEVICEREMOVED:
			if (!e.adevice.iscapture)
				mixer_handle_device_removed(e.adevice.which);
			break;
		case SDL_KEYDOWN:
			if (e.key.keysym.scancode == SDL_SCANCODE_F9) {
				vm_stack_trace();
			} else if (e.key.keysym.scancode == SDL_SCANCODE_F11) {
				gfx_toggle_fullscreen();
			} else if (e.key.keysym.scancode == SDL_SCANCODE_S) {
				if (e.key.keysym.mod & (KMOD_LALT | KMOD_RALT)) {
					screenshot_save();
				}
			}
			// fallthrough
		case SDL_KEYUP:
#ifdef __ANDROID__
			if (input_handler && e.key.timestamp < last_keyevent_timestamp + DEFERRED_KEY_DELAY) {
				// Input from virtual keyboard is sent as consecutive
				// SDL_KEYDOWN and SDL_KEYUP events. To give the game a chance
				// to see the previous key event, delay the event.
				struct deferred_keyevent *ev = xmalloc(sizeof(struct deferred_keyevent));
				ev->e = e;
				STAILQ_INSERT_TAIL(&deferred_keyevent_queue, ev, entry);
			} else {
				last_keyevent_timestamp = e.key.timestamp;
				key_event(&e.key, e.type == SDL_KEYDOWN);
			}
#else
			key_event(&e.key, e.type == SDL_KEYDOWN);
#endif
			break;
		case SDL_MOUSEBUTTONUP:
		case SDL_MOUSEBUTTONDOWN:
			if (e.button.which == SDL_TOUCH_MOUSEID) {
				if (multi_touch_active) {
					has_last_touch_down_pos = false;
					break;
				}
				if (e.button.state == SDL_PRESSED) {
					last_touch_down_pos.x = e.button.x;
					last_touch_down_pos.y = e.button.y;
					has_last_touch_down_pos = true;
				} else {
					if (has_last_touch_down_pos) {
						int dx = abs(e.button.x - last_touch_down_pos.x);
						int dy = abs(e.button.y - last_touch_down_pos.y);
						int snap_dist = max(16, min(sdl.viewport.w, sdl.viewport.h) / 40);
						if (dx <= snap_dist && dy <= snap_dist) {
							e.button.x = last_touch_down_pos.x;
							e.button.y = last_touch_down_pos.y;
						}
						has_last_touch_down_pos = false;
					}
					if (nr_deferred_synthetic_mouse_events == 0 &&
					    !key_state[VK_LBUTTON] && !key_state[VK_RBUTTON]) {
						break;
					}
				}
				enqueue_synthetic_mouse_button(&e.button);
			} else {
				has_synthetic_mouse_pos = false;
				mouse_event(&e.button);
			}
			break;
		case SDL_MOUSEWHEEL:
			wheel_dir = e.wheel.y;
			break;
		case SDL_FINGERDOWN: {
			int num_fingers = SDL_GetNumTouchFingers(e.tfinger.touchId);
			if (num_fingers >= 2) {
				// Multi-touch gesture: cancel single-finger mouse state and
				// track 2-finger tap (right-click) vs 2-finger scroll.
				multi_touch_active = true;
				key_state[VK_LBUTTON] = false;
				key_state[VK_RBUTTON] = false;
				nr_deferred_synthetic_mouse_events = 0;
				synthetic_mouse_hover_observed = false;
				synthetic_mouse_down_observed = false;
				synthetic_mouse_up_observed = true;
				has_last_touch_down_pos = false;
				scroll_gesture_y = -1.0f;
				if (num_fingers == 2) {
					two_finger_tap_candidate = true;
					two_finger_scrolled = false;
					two_finger_tap_start_tick = SDL_GetTicks();
				} else {
					two_finger_tap_candidate = false;
				}
			} else {
				multi_touch_active = false;
				two_finger_tap_candidate = false;
			}
			break;
		}
		case SDL_FINGERMOTION:
			break;
		case SDL_FINGERUP: {
			int rem_fingers = SDL_GetNumTouchFingers(e.tfinger.touchId);
			if (rem_fingers == 0) {
				if (two_finger_tap_candidate && !two_finger_scrolled &&
				    (SDL_GetTicks() - two_finger_tap_start_tick) <= TWO_FINGER_TAP_MAX_MS) {
					int rx = has_synthetic_mouse_pos ? synthetic_mouse_x
						: (sdl.viewport.x + sdl.viewport.w / 2);
					int ry = has_synthetic_mouse_pos ? synthetic_mouse_y
						: (sdl.viewport.y + sdl.viewport.h / 2);
					if (sdl.viewport.w > 0 && sdl.viewport.h > 0) {
						rx = max(sdl.viewport.x, min(sdl.viewport.x + sdl.viewport.w - 1, rx));
						ry = max(sdl.viewport.y, min(sdl.viewport.y + sdl.viewport.h - 1, ry));
					}
					SDL_MouseButtonEvent rpress = {
						.type = SDL_MOUSEBUTTONDOWN,
						.which = SDL_TOUCH_MOUSEID,
						.button = SDL_BUTTON_RIGHT,
						.state = SDL_PRESSED,
						.clicks = 1,
						.x = rx,
						.y = ry,
					};
					SDL_MouseButtonEvent rrelease = rpress;
					rrelease.type = SDL_MOUSEBUTTONUP;
					rrelease.state = SDL_RELEASED;
					enqueue_synthetic_mouse_button(&rpress);
					enqueue_synthetic_mouse_button(&rrelease);
				}
				two_finger_tap_candidate = false;
				two_finger_scrolled = false;
			}
			break;
		}
		case SDL_MULTIGESTURE:
			if (e.mgesture.numFingers == 2) {
				if (scroll_gesture_y < 0.0f)
					scroll_gesture_y = e.mgesture.y;
				float dy = scroll_gesture_y - e.mgesture.y;
				if (dy * dy > SCROLL_GESTURE_SENSITIVITY * SCROLL_GESTURE_SENSITIVITY) {
					wheel_dir = dy < 0 ? 1 : -1;  // Swipe up to scroll down.
					scroll_gesture_y = e.mgesture.y;
					two_finger_scrolled = true;
					two_finger_tap_candidate = false;
				}
			}
			break;
		case SDL_CONTROLLERDEVICEADDED:
			if (e.cdevice.which < MAX_CONTROLLERS)
				controllers[e.cdevice.which] = SDL_GameControllerOpen(e.cdevice.which);
			break;
		case SDL_CONTROLLERAXISMOTION:
			controller_axis_event(&e.caxis);
			break;
		case SDL_CONTROLLERBUTTONUP:
		case SDL_CONTROLLERBUTTONDOWN:
			controller_button_event(&e.cbutton);
			break;
		case SDL_TEXTINPUT:
			if (!input_handler) {
				break;
#ifdef __ANDROID__
			} else if (!STAILQ_EMPTY(&deferred_keyevent_queue)) {
				// The order of key events and text events must be preserved.
				struct deferred_keyevent *ev = xmalloc(sizeof(struct deferred_keyevent));
				ev->e = e;
				STAILQ_INSERT_TAIL(&deferred_keyevent_queue, ev, entry);
#endif
			} else {
				input_handler(e.text.text);
			}
			break;
		case SDL_TEXTEDITING:
			if (editing_handler)
				editing_handler(e.edit.text, e.edit.start, e.edit.length);
			break;
		default:
			break;
		}
	}
	if (dbg_dap)
		dbg_dap_handle_messages();
}
