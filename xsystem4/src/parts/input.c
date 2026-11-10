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

#include "system4.h"
#include "system4/ain.h"
#include <limits.h>
#include <math.h>
#include "system4/string.h"

#include "asset_manager.h"
#include "audio.h"
#include "input.h"
#include "kiwi.h"
#include "parts.h"
#include "vm.h"
#include "xsystem4.h"
#include "parts_internal.h"

// true between calls to BeginClick and EndClick
bool parts_began_click = false;
static int input_depth;

// the mouse position at last update
static Point parts_prev_pos = {0};
// true when the left mouse button was down last update
static bool prev_clicking = false;
static bool prev_rclicking = false;
static bool prev_mclicking = false;
static bool whole_lclick_down = false;
static bool whole_rclick_down = false;
static bool whole_mclick_down = false;
// the last (fully) clicked parts number
static int clicked_parts = 0;
// the current (partially) clicked parts number
static int click_down_parts = 0;

// drag state
static struct parts *drag_parts = NULL;
static bool is_dragging = false;
static SDL_FPoint drag_initial_pos;
static Point drag_start_cursor;
static struct parts *drop_target = NULL;
static struct string *click_miss_sound_name;

static void parts_set_focus(struct parts *parts, bool focus)
{
	if (parts->focused == focus) return;
	parts->focused = focus;
	parts_msg_push(parts, focus ? PARTS_MSG_FOCUS_IN : PARTS_MSG_FOCUS_OUT, "");
}

static void parts_clear_focus(int controller)
{
	struct parts *parts;
	PARTS_LIST_FOREACH(parts) {
		if (controller < 0 || parts->controller_no == controller)
			parts_set_focus(parts, false);
	}
}

void PE_SetFocus(int no)
{
	struct parts *parts = no > 0 ? parts_try_get(no) : NULL;
	if (!parts) return;
	parts_clear_focus(parts->controller_no);
	parts_set_focus(parts, true);
}

bool PE_IsFocus(int no)
{
	struct parts *parts = parts_try_get(no);
	return parts && parts->focused;
}

static int parts_focused_descendant(struct parts *parts)
{
	if (parts->focused) return parts->no;
	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		int focused = parts_focused_descendant(child);
		if (focused) return focused;
	}
	return 0;
}

static void parts_update_focus(int root_no)
{
	struct parts *root = parts_try_get(root_no);
	if (!root) return;
	int focused = parts_focused_descendant(root);
	if (focused == root->active_part) return;
	struct parts *before = parts_try_get(root->active_part), *after = parts_try_get(focused);
	if (before) parts_msg_push(before, PARTS_MSG_FOCUS_OUT, "");
	if (after) parts_msg_push(after, PARTS_MSG_FOCUS_IN, "");
	root->active_part = focused;
}

int PE_GetActiveParts(void)
{
	struct parts *root = parts_try_get(PE_get_active_controller());
	return root && parts_try_get(root->active_part) ? root->active_part : 0;
}

static void parts_play_sound(struct parts *parts, int state)
{
	if (parts->sound_names[state]->size)
		kiwi_play_system_sound(parts->sound_names[state]);
	else if (state == PARTS_STATE_HOVERED && parts->on_cursor_sound >= 0)
		audio_play_sound(parts->on_cursor_sound);
	else if (state == PARTS_STATE_CLICKED && parts->on_click_sound >= 0)
		audio_play_sound(parts->on_click_sound);
}

static bool parts_hittest(struct parts *parts, int state, Point pos)
{
	if (parts->states[state].type == PARTS_LAYOUT_BOX || parts->component_type == 15)
		return false;
	Rectangle hitbox = parts->states[state].common.hitbox;
	if (hitbox.w <= 0 || hitbox.h <= 0) {
		if (parts->states[state].type != PARTS_FLAT &&
				!(parts->states[state].type == PARTS_RECT_DETECTION && parts->states[state].rect.custom))
			return false;
	}
	if (ain->version >= 12) {
		int clipper_no = parts_get_effective_alpha_clipper(parts);
		if (clipper_no > 0) {
			static uint64_t cached_clip_gen = 0;
			static int cached_clip_no = 0;
			static Point cached_clip_pos = { INT_MIN, INT_MIN };
			static bool cached_clip_inside = true;
			if (cached_clip_gen != parts_render_gen || cached_clip_no != clipper_no ||
					cached_clip_pos.x != pos.x || cached_clip_pos.y != pos.y) {
				cached_clip_gen = parts_render_gen;
				cached_clip_no = clipper_no;
				cached_clip_pos = pos;
				cached_clip_inside = true;
				struct parts *clipper = parts_try_get(clipper_no);
				if (clipper) {
					struct parts_common *c_common = &clipper->states[clipper->state].common;
					mat4 clip_m;
					parts_world_matrix(clipper, clip_m);
					float cdet = clip_m[0][0] * clip_m[1][1] - clip_m[0][1] * clip_m[1][0];
					if (!isfinite(cdet) || fabsf(cdet) < 1e-12f) {
						cached_clip_inside = false;
					} else {
						float cdx = pos.x - clip_m[3][0], cdy = pos.y - clip_m[3][1];
						float cx = (clip_m[1][1] * cdx - clip_m[1][0] * cdy) / cdet - c_common->origin_offset.x;
						float cy = (clip_m[0][0] * cdy - clip_m[0][1] * cdx) / cdet - c_common->origin_offset.y;
						cached_clip_inside = (cx >= 0 && cy >= 0 && cx < c_common->w && cy < c_common->h);
					}
				}
			}
			if (!cached_clip_inside)
				return false;
		}
		mat4 matrix;
		parts_world_matrix(parts, matrix);
		float det = matrix[0][0] * matrix[1][1] - matrix[0][1] * matrix[1][0];
		if (!isfinite(det) || fabsf(det) < 1e-12f) return false;
		float dx = pos.x - matrix[3][0], dy = pos.y - matrix[3][1];
		float x = (matrix[1][1] * dx - matrix[1][0] * dy) / det;
		float y = (matrix[0][0] * dy - matrix[0][1] * dx) / det;
		if (parts->states[state].type == PARTS_RECT_DETECTION && parts->states[state].rect.custom) {
			struct parts_rect *rect = &parts->states[state].rect;
			x -= rect->common.origin_offset.x;
			y -= rect->common.origin_offset.y;
			const int order[4] = {0, 1, 3, 2};
			bool inside = false;
			for (int i = 0, j = 3; i < 4; j = i++) {
				Point a = rect->points[order[i]], b = rect->points[order[j]];
				if ((a.y > y) != (b.y > y)
						&& x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)
					inside = !inside;
			}
			return inside;
		}
		if (parts->states[state].type == PARTS_FLAT
				&& !(parts->component_type == 1 && parts->widget && !parts->widget->button_mode && parts->widget->text->size)) {
			return parts_flat_hittest(&parts->states[state].flat, x, y);
		}
		return x >= hitbox.x && y >= hitbox.y
			&& x < hitbox.x + hitbox.w && y < hitbox.y + hitbox.h;
	}
	if (parts->parent) {
		hitbox.x += parts->parent->global.pos.x;
		hitbox.y += parts->parent->global.pos.y;
	}
	return SDL_PointInRect(&pos, &hitbox);
}

static void drag_state_reset(void)
{
	drag_parts = NULL;
	is_dragging = false;
	drop_target = NULL;
}

void parts_input_reset_drag(struct parts *parts)
{
	if (parts == drag_parts)
		drag_state_reset();
	else if (parts == drop_target)
		drop_target = NULL;
}

void parts_input_reset(void)
{
	PE_EndInput();
	input_depth = 0;
	parts_began_click = false;
	prev_clicking = false;
	prev_rclicking = false;
	prev_mclicking = false;
	whole_lclick_down = false;
	whole_rclick_down = false;
	whole_mclick_down = false;
	click_down_parts = 0;
	parts_prev_pos = (Point) {0};
	if (click_miss_sound_name) free_string(click_miss_sound_name);
	click_miss_sound_name = NULL;
}

static bool parts_needs_mouse_hittest(struct parts *parts)
{
	if (!parts->pass_cursor || parts->clickable || parts->draggable || parts->wheelable)
		return true;
	if (parts->delegate_index >= 0 || parts->widget || parts->scrollbar)
		return true;
	if (parts->linked_from >= 0 || parts->linked_to >= 0 || parts->is_hovered)
		return true;
	if (parts->states[PARTS_STATE_HOVERED].type != PARTS_UNINITIALIZED ||
			parts->states[PARTS_STATE_CLICKED].type != PARTS_UNINITIALIZED)
		return true;
	if (parts->on_cursor_sound >= 0 || parts->sound_names[PARTS_STATE_HOVERED]->size > 0)
		return true;
	return false;
}

static void parts_update_mouse(struct parts *parts, Point cur_pos, bool cur_clicking,
		int passed_time, int wheel_forward, int wheel_back, bool *hover_consumed, bool *click_consumed)
{
	if (!parts->enable_input)
		return;
	if (ain->version >= 12 && parts->controller_no != ctrl_stack.active) {
		parts->is_hovered = false;
		parts->hover_time = 0;
		parts_set_state(parts, PARTS_STATE_DEFAULT);
		parts_input_reset_drag(parts);
		if (click_down_parts == parts->no)
			click_down_parts = 0;
		return;
	}
	// Visibility affects automatic routing, not Parts_IsCursorIn's geometry query.
	if (ain->version >= 12 && !parts_is_visible(parts)) {
		parts->is_hovered = false;
		parts->hover_time = 0;
		parts_set_state(parts, PARTS_STATE_DEFAULT);
		parts_input_reset_drag(parts);
		if (click_down_parts == parts->no)
			click_down_parts = 0;
		return;
	}

	// Always use DEFAULT state hitbox regardless of the current display state.
	bool is_hovered = !*hover_consumed
		&& parts_needs_mouse_hittest(parts)
		&& parts_hittest(parts, PARTS_STATE_DEFAULT, cur_pos);

	bool was_hovered = parts->is_hovered;
	parts->is_hovered = is_hovered;

	// !pass_cursor parts consume the cursor for parts behind them
	if (is_hovered && !parts->pass_cursor)
		*hover_consumed = true;

	if (is_hovered != was_hovered) {
		if (parts->linked_from >= 0) {
			struct parts *lf = parts_try_get(parts->linked_from);
			if (lf) parts_dirty(lf);
		}
		parts_engine_dirty();
	}

	if (!parts_began_click)
		return;

	if (is_hovered && !was_hovered) {
		parts_msg_push(parts, PARTS_MSG_MOUSE_ENTER, "ii", cur_pos.x, cur_pos.y);
		parts->hover_time = 0;
	}
	if (!is_hovered && was_hovered) {
		parts_msg_push(parts, PARTS_MSG_MOUSE_LEAVE, "ii", cur_pos.x, cur_pos.y);
	}

	if (is_hovered) {
		if (parts->wheelable && (wheel_forward || wheel_back))
			parts_msg_push(parts, PARTS_MSG_MOUSE_WHEEL, "ii", wheel_forward, wheel_back);
		// MOUSE_ON message fires every frame while hovering
		parts_msg_push(parts, PARTS_MSG_MOUSE_ON,
				"iii", cur_pos.x, cur_pos.y, parts->hover_time);
		parts->hover_time += passed_time;

		// MOUSE_MOVE message fires when cursor moves while hovering
		if (cur_pos.x != parts_prev_pos.x || cur_pos.y != parts_prev_pos.y) {
			parts_msg_push(parts, PARTS_MSG_MOUSE_MOVE, "ii", cur_pos.x, cur_pos.y);
		}
	}

	if (parts->scrollbar) {
		parts_scrollbar_input(parts, cur_pos, is_hovered, cur_clicking,
			is_hovered && !*click_consumed && cur_clicking && !prev_clicking,
			passed_time, wheel_back - wheel_forward);
	}
	if (!is_hovered) {
		parts_set_state(parts, PARTS_STATE_DEFAULT);
		return;
	}

	bool enabled = !parts->widget || parts->widget->enabled;
	bool clickable = enabled && (parts->clickable || parts->widget || parts->scrollbar);
	bool click_eligible = enabled && (clickable || !parts->pass_cursor);
	if (!click_eligible || *click_consumed) {
		if (!was_hovered)
			parts_play_sound(parts, PARTS_STATE_HOVERED);
		parts_set_state(parts, PARTS_STATE_HOVERED);
		return;
	}

	// click down: first eligible part captures the click
	if (cur_clicking && !prev_clicking) {
		click_down_parts = parts->no;
		*click_consumed = true;
		if (ain->version >= 12 && clickable) PE_SetFocus(parts->no);

		drag_parts = parts;
		drag_initial_pos = parts->local.pos;
		drag_start_cursor = cur_pos;

		// KEY_TRIGGER message fires on press transition
		parts_msg_push(parts, PARTS_MSG_KEY_TRIGGER, "i", VK_LBUTTON);
	}

	// KEY_DOWN message fires every frame while held (not first frame)
	if (prev_clicking && cur_clicking && click_down_parts == parts->no) {
		parts_msg_push(parts, PARTS_MSG_KEY_DOWN, "i", VK_LBUTTON);
	}

	if (cur_clicking && click_down_parts == parts->no) {
		parts_set_state(parts, PARTS_STATE_CLICKED);
	} else {
		if (!was_hovered) {
				parts_play_sound(parts, PARTS_STATE_HOVERED);
		}
		parts_set_state(parts, PARTS_STATE_HOVERED);
	}

	// click event: only if the click down event had same parts number
	if (clickable && prev_clicking && !cur_clicking
			&& click_down_parts == parts->no) {
		parts_play_sound(parts, PARTS_STATE_CLICKED);
		clicked_parts = parts->no;
		if (parts->component_type == 1 && parts->widget)
			parts_widget_check(parts, !parts->widget->checked);

		parts_msg_push(parts, PARTS_MSG_MOUSE_CLICK,
				"iii", cur_pos.x, cur_pos.y, VK_LBUTTON);
		parts_msg_push(parts, PARTS_MSG_KEY_UP, "i", VK_LBUTTON);
	}
}

void PE_UpdateInputState(int passed_time)
{
	Point cur_pos;
	input_parts_poll_begin(parts_began_click);
	mouse_get_pos(&cur_pos.x, &cur_pos.y);
	bool cur_clicking = key_is_down(VK_LBUTTON);
	bool cur_rclicking = key_is_down(VK_RBUTTON);
	bool cur_mclicking = key_is_down(VK_MBUTTON);
	int wheel_forward, wheel_back;
	mouse_get_wheel(&wheel_forward, &wheel_back);

	bool hover_consumed = false;
	bool click_consumed = false;
	if (ain->version >= 12 && parts_began_click && cur_clicking && !prev_clicking)
		parts_clear_focus(ctrl_stack.active);
	struct parts *parts;
	// Iterate front-to-back (highest z first) for proper cursor consumption
	PARTS_LIST_FOREACH_REVERSE(parts) {
		parts_update_mouse(parts, cur_pos, cur_clicking, passed_time,
				wheel_forward, wheel_back, &hover_consumed, &click_consumed);
	}

	if (ain->version >= 12 && parts_began_click) {
		if (cur_pos.x != parts_prev_pos.x || cur_pos.y != parts_prev_pos.y)
			parts_msg_push(NULL, PARTS_MSG_MOUSE_MOVE, "ii", cur_pos.x, cur_pos.y);
		if (wheel_forward || wheel_back)
			parts_msg_push(NULL, PARTS_MSG_MOUSE_WHEEL, "ii", wheel_forward, wheel_back);
		const int btns[3] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
		const bool cur_btns[3] = { cur_clicking, cur_rclicking, cur_mclicking };
		const bool prev_btns[3] = { prev_clicking, prev_rclicking, prev_mclicking };
		bool *whole_downs[3] = { &whole_lclick_down, &whole_rclick_down, &whole_mclick_down };
		for (int i = 0; i < 3; i++) {
			if (cur_btns[i] && !prev_btns[i]) {
				*whole_downs[i] = true;
				parts_msg_push(NULL, PARTS_MSG_KEY_TRIGGER, "i", btns[i]);
			} else if (prev_btns[i] && cur_btns[i] && *whole_downs[i]) {
				parts_msg_push(NULL, PARTS_MSG_KEY_DOWN, "i", btns[i]);
			} else if (prev_btns[i] && !cur_btns[i] && *whole_downs[i]) {
				*whole_downs[i] = false;
				parts_msg_push(NULL, PARTS_MSG_MOUSE_CLICK, "iii", cur_pos.x, cur_pos.y, btns[i]);
				parts_msg_push(NULL, PARTS_MSG_KEY_UP, "i", btns[i]);
			}
		}
	}

	// Drag movement processing
	if (drag_parts && cur_clicking) {
		bool cursor_moved = (cur_pos.x != parts_prev_pos.x ||
				cur_pos.y != parts_prev_pos.y);
		if (cursor_moved && drag_parts->draggable) {
			SDL_FPoint new_pos = {
				drag_initial_pos.x + (cur_pos.x - drag_start_cursor.x),
				drag_initial_pos.y + (cur_pos.y - drag_start_cursor.y)
			};
			parts_set_posf(drag_parts, new_pos);
			if (!is_dragging) {
				parts_msg_push(drag_parts, PARTS_MSG_DRAG_BEGIN, "");
			}
			parts_msg_push(drag_parts, PARTS_MSG_DRAGGING, "iiii",
					drag_start_cursor.x, drag_start_cursor.y, cur_pos.x, cur_pos.y);
			is_dragging = true;
		}

		// Drop target tracking
		if (cursor_moved && is_dragging) {
			struct parts *new_drop = NULL;
			PARTS_LIST_FOREACH_REVERSE(parts) {
				if (parts == drag_parts || !parts->enable_input
						|| (ain->version >= 12 && !parts_is_visible(parts)))
					continue;
				if (parts_hittest(parts, PARTS_STATE_DEFAULT, cur_pos)) {
					new_drop = parts;
					if (!parts->pass_cursor)
						break;
				}
			}
			if (new_drop != drop_target) {
				if (drop_target) {
					parts_msg_push(drop_target, PARTS_MSG_DROP_LEAVE,
							"i", drag_parts->no);
				}
				if (new_drop) {
					parts_msg_push(new_drop, PARTS_MSG_DROP_ENTER,
							"i", drag_parts->no);
				}
				drop_target = new_drop;
			} else if (drop_target) {
				parts_msg_push(drop_target, PARTS_MSG_DROP_ON,
						"iii", drag_parts->no, cur_pos.x, cur_pos.y);
			}
		}
	}

	// Release handling
	if (prev_clicking && !cur_clicking) {
		if (is_dragging && drag_parts && drag_parts->draggable) {
			parts_msg_push(drag_parts, PARTS_MSG_DRAG_END, "");
		}
		if (drop_target && drag_parts) {
			parts_msg_push(drop_target, PARTS_MSG_DROPPED,
					"iii", drag_parts->no, cur_pos.x, cur_pos.y);
			parts_msg_push(drop_target, PARTS_MSG_DROP_LEAVE, "i", drag_parts->no);
		}
		drag_state_reset();

		if (!click_down_parts) {
			if (parts_began_click && click_miss_sound_name && click_miss_sound_name->size)
				kiwi_play_system_sound(click_miss_sound_name);
		}
		click_down_parts = 0;
	}

	if (parts_began_click) {
		prev_clicking = cur_clicking;
		prev_rclicking = cur_rclicking;
		prev_mclicking = cur_mclicking;
	} else {
		prev_clicking = false;
		prev_rclicking = false;
		prev_mclicking = false;
		click_down_parts = 0;
	}
	parts_prev_pos = cur_pos;
	input_notify_parts_poll(parts_began_click, cur_clicking || cur_rclicking);
	if (ain->version >= 12) {
		parts_update_focus(ctrl_stack.overlay_id);
		for (int i = 0; i < ctrl_stack.nr_controllers; i++)
			parts_update_focus(ctrl_stack.ids[i]);
	}
}

void PE_SetPassCursor(int parts_no, bool pass)
{
	parts_get(parts_no)->pass_cursor = !!pass;
}

bool PE_GetPartsPassCursor(int parts_no)
{
	return parts_get(parts_no)->pass_cursor;
}

void PE_SetClickable(int parts_no, bool clickable)
{
	parts_get(parts_no)->clickable = !!clickable;
}

void PE_SetWheelable(int parts_no, bool enable)
{
	parts_get(parts_no)->wheelable = enable;
}

bool PE_GetPartsClickable(int parts_no)
{
	return parts_get(parts_no)->clickable;
}

void PE_Parts_SetSoundName(int parts_no, struct string *name, int state)
{
	if (!parts_state_valid(--state)) return;
	struct parts *parts = parts_get(parts_no);
	free_string(parts->sound_names[state]);
	parts->sound_names[state] = string_dup(name);
}

struct string *PE_Parts_GetSoundName(int parts_no, int state)
{
	if (!parts_state_valid(--state)) return string_ref(&EMPTY_STRING);
	return string_ref(parts_get(parts_no)->sound_names[state]);
}

void PE_Parts_PlaySound(int parts_no, int state)
{
	if (parts_state_valid(--state))
		parts_play_sound(parts_get(parts_no), state);
}

void PE_SetEnableInputProcess(int parts_no, bool enable)
{
	struct parts *parts = parts_get(parts_no);
	parts->enable_input = enable;
	if (!enable) {
		parts_set_focus(parts, false);
		parts_input_reset_drag(parts);
		parts->is_hovered = false;
		parts->hover_time = 0;
		if (click_down_parts == parts_no)
			click_down_parts = 0;
	}
}

bool PE_IsEnableInputProcess(int parts_no)
{
	return parts_get(parts_no)->enable_input;
}

void PE_SetPartsGroupDecideOnCursor(possibly_unused int group_no, possibly_unused bool decide_on_cursor)
{
	UNIMPLEMENTED("(%d, %s)", group_no, decide_on_cursor ? "true" : "false");
}

void PE_SetPartsGroupDecideClick(possibly_unused int group_no, possibly_unused bool decide_click)
{
	UNIMPLEMENTED("(%d, %s)", group_no, decide_click ? "true" : "false");
}

void PE_SetOnCursorShowLinkPartsNumber(int parts_no, int link_parts_no)
{
	struct parts *parts = parts_get(parts_no);
	if (parts->linked_to > 0) {
		struct parts *old_link = parts_try_get(parts->linked_to);
		if (old_link && old_link->linked_from == parts_no)
			old_link->linked_from = -1;
	}
	if (link_parts_no <= 0) {
		parts->linked_to = -1;
		parts_dirty(parts);
		return;
	}
	struct parts *link_parts = parts_get(link_parts_no);
	parts->linked_to = link_parts_no;
	link_parts->linked_from = parts_no;
	parts_dirty(parts);
}

int PE_GetOnCursorShowLinkPartsNumber(int parts_no)
{
	int linked_to = parts_get(parts_no)->linked_to;
	return ain->version >= 12 && linked_to < 0 ? 0 : linked_to;
}

bool PE_SetPartsOnCursorSoundNumber(int parts_no, int sound_no)
{
	if (!asset_exists(ASSET_SOUND, sound_no)) {
		WARNING("Invalid sound number: %d", sound_no);
		return false;
	}

	struct parts *parts = parts_get(parts_no);
	parts->on_cursor_sound = sound_no;
	return true;
}

bool PE_SetPartsClickSoundNumber(int parts_no, int sound_no)
{
	if (!asset_exists(ASSET_SOUND, sound_no)) {
		WARNING("Invalid sound number: %d", sound_no);
		return false;
	}

	struct parts *parts = parts_get(parts_no);
	parts->on_click_sound = sound_no;
	return true;
}

bool PE_SetClickMissSoundNumber(possibly_unused int sound_no)
{
	UNIMPLEMENTED("(%d)", sound_no);
	return true;
}

void PE_SetClickMissSoundName(struct string *name)
{
	struct string *copy = string_dup(name);
	if (click_miss_sound_name) free_string(click_miss_sound_name);
	click_miss_sound_name = copy;
}

struct string *PE_GetClickMissSoundName(void)
{
	return string_ref(click_miss_sound_name ? click_miss_sound_name : &EMPTY_STRING);
}

void PE_BeginInput(void)
{
	if (ain->version >= 12) {
		input_depth++;
		parts_clear_focus(-1);
	}
	parts_began_click = true;
}

void PE_EndInput(void)
{
	if (ain->version >= 12) {
		if (input_depth > 0) input_depth--;
		if (input_depth > 0) parts_clear_focus(-1);
		parts_began_click = input_depth > 0;
	} else parts_began_click = false;
	if (!parts_began_click) {
		prev_clicking = false;
		prev_rclicking = false;
		prev_mclicking = false;
		whole_lclick_down = false;
		whole_rclick_down = false;
		whole_mclick_down = false;
		click_down_parts = 0;
	}
	clicked_parts = 0;
	drag_state_reset();
}

void PE_SetDrag(int parts_no, bool enable)
{
	parts_get(parts_no)->draggable = !!enable;
}

int PE_GetClickPartsNumber(void)
{
	return clicked_parts;
}

bool PE_IsCursorIn(int parts_no, int mouse_x, int mouse_y, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return false;

	Point mouse_pos = { mouse_x, mouse_y };
	return parts_hittest(parts, state, mouse_pos);
}
