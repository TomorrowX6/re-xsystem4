/* Sys43 vertical/horizontal scrollbars, GPL-2.0-or-later. */
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "system4.h"
#include "system4/string.h"
#include "system4/utfsjis.h"
#include "parts.h"
#include "parts_scrollbar.h"
#include "parts_internal.h"

int parts_scrollbar_range(struct parts_scrollbar *s)
{
	return min((int64_t)INT_MAX, max((int64_t)0, (int64_t)s->total - s->view));
}

static void set_pos(struct parts *parts, int pos)
{
	struct parts_scrollbar *s = parts->scrollbar;
	int range = parts_scrollbar_range(s);
	int new_pos = min(range, max(0, pos));
	float new_rate = range ? (float)new_pos / range : 1.f;
	if (s->pos == new_pos && s->rate == new_rate) return;
	s->pos = new_pos;
	s->rate = new_rate;
	parts_dirty(parts);
}

void parts_scrollbar_free(struct parts *parts)
{
	struct parts_scrollbar *s = parts->scrollbar;
	if (!s) return;
	for (int resource = 0; resource < 2; resource++)
		for (int piece = 0; piece < 5; piece++) free_string(s->names[resource][piece]);
	for (int piece = 0; piece < 4; piece++)
		for (int state = 0; state < 4; state++)
			parts_state_reset(&s->skins[piece][state], PARTS_UNINITIALIZED);
	free(s);
	parts->scrollbar = NULL;
}

static bool load_skin(struct parts *parts, struct parts_state *state, struct string *base,
		bool flat, const char *piece, const char *suffix)
{
	struct string *name = string_dup(base);
	const char *names[] = {piece, suffix};
	for (int i = 0; i < 2; i++) {
		char *encoded = utf2sjis(names[i], strlen(names[i]));
		string_append_cstr(&name, encoded, strlen(encoded));
		free(encoded);
	}
	parts_state_reset(state, flat ? PARTS_FLAT : PARTS_CG);
	bool ok = flat ? parts_flat_load(parts, &state->flat, name)
		: parts_cg_set(parts, &state->cg, name);
	free_string(name);
	if (!ok) parts_state_reset(state, PARTS_UNINITIALIZED);
	return ok;
}

void parts_scrollbar_rebuild(struct parts *parts)
{
	struct parts_scrollbar *s = parts->scrollbar;
	static const char *pieces[2][4] = {
		{"／上按钮", "／下按钮", "／栏", "／背景"},
		{"／左按钮", "／右按钮", "／栏", "／背景"}
	};
	static const char *states[] = {"／一般", "／オン", "／DOWN", "／无效"};
	for (int piece = 0; piece < 4; piece++) {
		bool flat = s->names[1][piece + 1]->size
			|| (!s->names[0][piece + 1]->size && s->names[1][0]->size);
		struct string *override = s->names[flat][piece + 1];
		struct string *base = override->size ? override : s->names[flat][0];
		for (int state = 0; state < 4; state++) {
			struct parts_state *target = &s->skins[piece][state];
			bool loaded = base->size && load_skin(parts, target, base, flat,
					override->size ? "" : pieces[s->horizontal][piece], piece == 3 ? "" : states[state]);
			if (!loaded && state && base->size)
				loaded = load_skin(parts, target, base, flat, override->size ? "" : pieces[s->horizontal][piece],
						piece == 3 ? "" : states[0]);
			s->loaded[piece][state] = loaded;
			if (!loaded) {
				parts_state_reset(target, PARTS_CG);
				int shade = piece == 3 ? 96 : state == 2 ? 144 : state == 1 ? 224 : 200;
				parts_set_dims(parts, &target->common, 1, 1);
				gfx_init_texture_rgba(&target->common.texture, 1, 1, (SDL_Color){shade, shade, shade, 255});
			}
		}
	}
	for (int state = 0; state < PARTS_NR_STATES; state++) {
		parts_state_reset(&parts->states[state], PARTS_RECT_DETECTION);
		parts_set_dims(parts, &parts->states[state].common, s->width, s->height);
	}
	parts_component_dirty(parts);
	parts_recalculate_hitbox(parts);
	parts_dirty(parts);
}

void parts_scrollbar_create(struct parts *parts, bool horizontal)
{
	if (parts->scrollbar && parts->scrollbar->horizontal == horizontal) return;
	parts_scrollbar_free(parts);
	parts_widget_free(parts);
	parts_panel_free(parts);
	parts_message_window_free(parts);
	struct parts_scrollbar *s = parts->scrollbar = xcalloc(1, sizeof(*s));
	s->horizontal = horizontal;
	s->width = horizontal ? 256 : 24;
	s->height = horizontal ? 24 : 256;
	s->before = s->after = 24;
	s->total = 100;
	s->view = s->step = 1;
	s->reported_range = 99;
	s->hovered = s->pressed = -1;
	for (int resource = 0; resource < 2; resource++)
		for (int piece = 0; piece < 5; piece++) s->names[resource][piece] = string_ref(&EMPTY_STRING);
	parts->component_type = horizontal ? 3 : 2;
	parts_scrollbar_rebuild(parts);
}

static struct parts *scrollbar(int no, bool horizontal)
{
	struct parts *parts = parts_get(no);
	parts_scrollbar_create(parts, horizontal);
	return parts;
}

int parts_scrollbar_skin_state(struct parts_scrollbar *s, int piece)
{
	if (!parts_scrollbar_range(s) || (piece == 0 && !s->pos)
			|| (piece == 1 && s->pos == parts_scrollbar_range(s))) return 3;
	return s->pressed == piece ? 2 : s->hovered == piece ? 1 : 0;
}

void parts_scrollbar_rects(struct parts *parts, Rectangle rects[4])
{
	struct parts_scrollbar *s = parts->scrollbar;
	int length = max(0, s->horizontal ? s->width : s->height);
	int breadth = max(0, s->horizontal ? s->height : s->width);
	int before = min(length, max(0, s->before));
	int after = min(length - before, max(0, s->after));
	int track = length - before - after;
	int state = parts_scrollbar_skin_state(s, 2);
	struct parts_common *thumb = &s->skins[2][state].common;
	int size = s->loaded[2][state] ? (s->horizontal ? thumb->w : thumb->h)
		: max(8, s->total > 0 ? (int)((int64_t)track * max(0, s->view) / s->total) : track);
	size = min(track, max(0, size));
	int thumb_pos = before + (int)((track - size) * s->rate);
	int positions[] = {0, length - after, thumb_pos, before};
	int sizes[] = {before, after, size, track};
	for (int piece = 0; piece < 4; piece++) {
		rects[piece] = s->horizontal ? (Rectangle){positions[piece], 0, sizes[piece], breadth}
			: (Rectangle){0, positions[piece], breadth, sizes[piece]};
	}
}

static float cursor_axis(struct parts *parts, Point cursor)
{
	mat4 world;
	parts_world_matrix(parts, world);
	float det = world[0][0] * world[1][1] - world[1][0] * world[0][1];
	if (!isfinite(det) || fabsf(det) < 1e-12f) return 0;
	float x = cursor.x - world[3][0], y = cursor.y - world[3][1];
	Point origin = parts->states[0].common.origin_offset;
	return parts->scrollbar->horizontal ? (world[1][1] * x - world[1][0] * y) / det - origin.x
		: (world[0][0] * y - world[0][1] * x) / det - origin.y;
}

void parts_scrollbar_input(struct parts *parts, Point cursor, bool hovered,
		bool down, bool pressed, int elapsed, int wheel)
{
	struct parts_scrollbar *s = parts->scrollbar;
	Rectangle rects[4];
	parts_scrollbar_rects(parts, rects);
	float axis = cursor_axis(parts, cursor);
	int begin = s->horizontal ? rects[3].x : rects[3].y;
	int length = s->horizontal ? rects[3].w : rects[3].h;
	int thumb = s->horizontal ? rects[2].x : rects[2].y;
	int size = s->horizontal ? rects[2].w : rects[2].h;
	int old_pos = s->pos, old_hover = s->hovered, old_pressed = s->pressed;
	s->hovered = !hovered ? -1 : axis < begin ? 0 : axis >= begin + length ? 1
		: axis >= thumb && axis < thumb + size ? 2 : 3;
	if (pressed) {
		s->pressed = s->hovered;
		s->drag_offset = axis - thumb;
		s->repeat_ms = -400;
	}
	if (hovered && wheel) set_pos(parts, min((int64_t)INT_MAX, max((int64_t)INT_MIN,
			(int64_t)s->pos + (int64_t)wheel * s->step)));
	if (down && s->pressed >= 0 && parts_scrollbar_range(s)) {
		s->repeat_ms += max(0, elapsed);
		if (s->pressed == 2 && length > size) {
			float rate = (axis - s->drag_offset - begin) / (length - size);
			s->rate = min(1.f, max(0.f, rate));
			s->pos = (int)(s->rate * parts_scrollbar_range(s));
		} else if (pressed || s->repeat_ms >= 60) {
			int direction = s->pressed == 0 ? -1 : s->pressed == 1 ? 1 : axis < thumb ? -1 : 1;
			int step = s->pressed < 2 ? s->step : s->view;
			set_pos(parts, min((int64_t)INT_MAX, max((int64_t)INT_MIN, (int64_t)s->pos + (int64_t)direction * step)));
			if (!pressed) s->repeat_ms = 0;
		}
	}
	if (!down) s->pressed = -1;
	if (s->pos != old_pos || s->hovered != old_hover || s->pressed != old_pressed) parts_dirty(parts);
}

void parts_scrollbar_notify_changes(void)
{
	struct parts *parts;
	PARTS_LIST_FOREACH(parts) {
		struct parts_scrollbar *s = parts->scrollbar;
		if (!s) continue;
		int range = parts_scrollbar_range(s);
		if (s->pos == s->reported_pos && range == s->reported_range && s->rate == s->reported_rate)
			continue;
		// Sys43 coalesces API, linked-size and input changes at the frame boundary.
		// Rate changes notify even when rounding leaves the integer position unchanged.
		parts_msg_push(parts, PARTS_MSG_SCROLL, "ii", s->pos, range);
		s->reported_pos = s->pos;
		s->reported_range = range;
		s->reported_rate = s->rate;
	}
}

bool parts_scrollbar_update(struct parts *parts, int elapsed)
{
	struct parts_scrollbar *s = parts->scrollbar;
	bool dirty = false;
	for (int piece = 0; piece < 4; piece++)
		dirty |= parts_state_update(&s->skins[piece][parts_scrollbar_skin_state(s, piece)], elapsed);
	return dirty;
}

void parts_scrollbar_update_links(void)
{
	struct parts *parts;
	PARTS_LIST_FOREACH(parts) {
		struct parts_scrollbar *s = parts->scrollbar;
		if (!s) continue;
		// Zero disables the link, even if the scene has a component numbered zero.
		struct parts *total = s->total_link ? parts_try_get(s->total_link) : NULL;
		struct parts *view = s->view_link ? parts_try_get(s->view_link) : NULL;
		int total_size = total ? (s->horizontal ? PE_GetPartsWidth(total->no, 1) : PE_GetPartsHeight(total->no, 1)) : s->total;
		int view_size = view ? (s->horizontal ? PE_GetPartsWidth(view->no, 1) : PE_GetPartsHeight(view->no, 1)) : s->view;
		if (s->total == total_size && s->view == view_size) continue;
		s->total = total_size;
		s->view = view_size;
		set_pos(parts, s->pos);
	}
}

#define SCROLLBAR_API(axis, horizontal) \
void PE_Set##axis##ScrollbarSize(int no, int width, int height) { \
	struct parts *p = scrollbar(no, horizontal); \
	if (p->scrollbar->width == width && p->scrollbar->height == height) return; \
	p->scrollbar->width = width; p->scrollbar->height = height; parts_scrollbar_rebuild(p); } \
void PE_Set##axis##ScrollbarTotalSize(int no, int size) { \
	struct parts *p = scrollbar(no, horizontal); \
	if (p->scrollbar->total == size) return; \
	p->scrollbar->total = size; set_pos(p, p->scrollbar->pos); } \
void PE_Set##axis##ScrollbarViewSize(int no, int size) { \
	struct parts *p = scrollbar(no, horizontal); \
	if (p->scrollbar->view == size) return; \
	p->scrollbar->view = size; set_pos(p, p->scrollbar->pos); } \
void PE_Set##axis##ScrollbarScrollPos(int no, int pos) { set_pos(scrollbar(no, horizontal), pos); } \
void PE_Set##axis##ScrollbarScrollRate(int no, float rate) { \
	struct parts *p = scrollbar(no, horizontal); struct parts_scrollbar *s = p->scrollbar; \
	float clamped = isfinite(rate) ? min(1.f, max(0.f, rate)) : 0.f; \
	int new_pos = (int)((double)clamped * parts_scrollbar_range(s)); \
	if (s->rate == clamped && s->pos == new_pos) return; \
	s->rate = clamped; s->pos = new_pos; parts_dirty(p); } \
void PE_Set##axis##ScrollbarMoveSizeByButton(int no, int size) { scrollbar(no, horizontal)->scrollbar->step = size; } \
int PE_Get##axis##ScrollbarTotalSize(int no) { return scrollbar(no, horizontal)->scrollbar->total; } \
int PE_Get##axis##ScrollbarViewSize(int no) { return scrollbar(no, horizontal)->scrollbar->view; } \
int PE_Get##axis##ScrollbarScrollPos(int no) { return scrollbar(no, horizontal)->scrollbar->pos; } \
float PE_Get##axis##ScrollbarScrollRate(int no) { return scrollbar(no, horizontal)->scrollbar->rate; } \
int PE_Get##axis##ScrollbarMoveSizeByButton(int no) { return scrollbar(no, horizontal)->scrollbar->step; } \
void PE_Set##axis##ScrollTotalSizeLinkNumber(int no, int target) { scrollbar(no, horizontal)->scrollbar->total_link = target; } \
void PE_Set##axis##ScrollViewSizeLinkNumber(int no, int target) { scrollbar(no, horizontal)->scrollbar->view_link = target; } \
int PE_Get##axis##ScrollTotalSizeLinkNumber(int no) { return scrollbar(no, horizontal)->scrollbar->total_link; } \
int PE_Get##axis##ScrollViewSizeLinkNumber(int no) { return scrollbar(no, horizontal)->scrollbar->view_link; }
SCROLLBAR_API(V, false)
SCROLLBAR_API(H, true)
#undef SCROLLBAR_API

#define END_API(axis, horizontal, end, field) \
void PE_Set##axis##Scrollbar##end(int no, int size) { struct parts *p = scrollbar(no, horizontal); if (p->scrollbar->field == size) return; p->scrollbar->field = size; parts_dirty(p); } \
int PE_Get##axis##Scrollbar##end(int no) { return scrollbar(no, horizontal)->scrollbar->field; }
END_API(V, false, UpHeight, before)
END_API(V, false, DownHeight, after)
END_API(H, true, LeftWidth, before)
END_API(H, true, RightWidth, after)
#undef END_API

#define NAME_API(axis, horizontal, part, index, kind, flat) \
void PE_Set##axis##Scrollbar##part##kind##Name(int no, struct string *name) { \
	struct parts *p = scrollbar(no, horizontal); struct string **target = &p->scrollbar->names[flat][index]; \
	if (!strcmp((*target)->text, name->text)) return; \
	struct string *copy = string_dup(name); free_string(*target); *target = copy; parts_scrollbar_rebuild(p); } \
struct string *PE_Get##axis##Scrollbar##part##kind##Name(int no) { return string_dup(scrollbar(no, horizontal)->scrollbar->names[flat][index]); }
#define NAMES(axis, horizontal, first, last, kind, flat) \
NAME_API(axis, horizontal, , 0, kind, flat) \
NAME_API(axis, horizontal, Override##first, 1, kind, flat) \
NAME_API(axis, horizontal, Override##last, 2, kind, flat) \
NAME_API(axis, horizontal, OverrideBar, 3, kind, flat) \
NAME_API(axis, horizontal, OverrideBG, 4, kind, flat)
NAMES(V, false, Up, Down, CG, 0)
NAMES(V, false, Up, Down, Flat, 1)
NAMES(H, true, Left, Right, CG, 0)
NAMES(H, true, Left, Right, Flat, 1)
#undef NAMES
#undef NAME_API

#define LINK_API(name, index) \
void PE_SetComponentScroll##name##LinkNumber(int no, int target) { \
	struct parts *p = parts_get(no); p->scroll_links[index] = target; parts_component_dirty(p); parts_dirty(p); } \
int PE_GetComponentScroll##name##LinkNumber(int no) { return parts_get(no)->scroll_links[index]; }
LINK_API(PosX, 0)
LINK_API(PosY, 1)
LINK_API(Alpha, 2)
#undef LINK_API
