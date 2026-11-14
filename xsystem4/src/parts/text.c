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

#include <math.h>
#include <string.h>

#include "system4.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

#include "xsystem4.h"
#include "parts.h"
#include "parts_internal.h"

static int s_modifier_font_scale_pct = 100;

void parts_set_modifier_font_scale(int pct)
{
	if (pct < 70) pct = 70;
	if (pct > 180) pct = 180;
	s_modifier_font_scale_pct = pct;
}

int parts_get_modifier_font_scale(void)
{
	return s_modifier_font_scale_pct;
}

struct text_style parts_get_effective_text_style_pct(const struct text_style *src, int pct)
{
	struct text_style eff = *src;
	if (pct != 100 && eff.size > 0.0f) {
		float scaled = roundf(eff.size * ((float)pct / 100.0f));
		if (scaled < 8.0f)
			scaled = 8.0f;
		if (fabsf(scaled - eff.size) > 0.01f) {
			eff.size = scaled;
			eff.font_size = NULL;
		}
	}
	return eff;
}

struct text_style parts_get_effective_text_style(const struct text_style *src)
{
	return parts_get_effective_text_style_pct(src, s_modifier_font_scale_pct);
}

static int extract_sjis_char(const char *src, char *dst)
{
	if (SJIS_2BYTE(*src)) {
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = '\0';
		return 2;
	}
	dst[0] = src[0];
	dst[1] = '\0';
	return 1;
}

struct string *parts_text_line_get(struct parts_text_line *line)
{
	struct string *s = make_string("", 0);
	for (int i = 0; i < line->nr_chars; i++) {
		string_append_cstr(&s, line->chars[i].ch, strlen(line->chars[i].ch));
	}
	return s;
}

struct string *parts_text_get(struct parts_text *t)
{
	struct string *s = make_string("", 0);
	for (int i = 0; i < t->nr_lines; i++) {
		if (i > 0)
			string_push_back(&s, '\n');
		struct parts_text_line *line = &t->lines[i];
		for (int i = 0; i < line->nr_chars; i++) {
			string_append_cstr(&s, line->chars[i].ch, strlen(line->chars[i].ch));
		}
	}
	return s;
}

static Point text_style_offset(struct text_style *ts)
{
	int x = max(ts->bold_width, ts->edge_left) * ts->scale_x;
	int y = max(ts->bold_width, ts->edge_up);
	return (Point){x,y};
}

#define PARTS_GLYPH_CACHE_SIZE 2048
#define PARTS_GLYPH_HASH_BUCKETS 4096

struct parts_glyph_key {
	char ch[4];
	unsigned face;
	float size;
	float bold_width;
	int weight;
	float edge_left;
	float edge_up;
	float edge_right;
	float edge_down;
	SDL_Color color;
	SDL_Color edge_color;
	float scale_x;
	float space_scale_x;
	float font_spacing;
	bool advance_edges;
};

struct parts_glyph_cache_entry {
	bool valid;
	uint32_t hash;
	struct parts_glyph_key key;
	Texture t;
	int advance;
	int height;
	int refcount;
	int prev_lru;
	int next_lru;
	int next_hash;
};

static struct parts_glyph_cache_entry glyph_cache[PARTS_GLYPH_CACHE_SIZE];
static int glyph_hash_buckets[PARTS_GLYPH_HASH_BUCKETS];
static int glyph_lru_head = -1;
static int glyph_lru_tail = -1;
static int glyph_cache_used = 0;
static bool glyph_cache_initialized = false;

static void glyph_cache_init(void)
{
	if (glyph_cache_initialized)
		return;
	for (int i = 0; i < PARTS_GLYPH_HASH_BUCKETS; i++)
		glyph_hash_buckets[i] = -1;
	for (int i = 0; i < PARTS_GLYPH_CACHE_SIZE; i++) {
		glyph_cache[i].prev_lru = -1;
		glyph_cache[i].next_lru = -1;
		glyph_cache[i].next_hash = -1;
	}
	glyph_cache_initialized = true;
}

static void glyph_lru_remove(int idx)
{
	int prev = glyph_cache[idx].prev_lru;
	int next = glyph_cache[idx].next_lru;
	if (prev >= 0)
		glyph_cache[prev].next_lru = next;
	else if (glyph_lru_head == idx)
		glyph_lru_head = next;
	if (next >= 0)
		glyph_cache[next].prev_lru = prev;
	else if (glyph_lru_tail == idx)
		glyph_lru_tail = prev;
	glyph_cache[idx].prev_lru = -1;
	glyph_cache[idx].next_lru = -1;
}

static void glyph_lru_insert_head(int idx)
{
	glyph_cache[idx].prev_lru = -1;
	glyph_cache[idx].next_lru = glyph_lru_head;
	if (glyph_lru_head >= 0)
		glyph_cache[glyph_lru_head].prev_lru = idx;
	else
		glyph_lru_tail = idx;
	glyph_lru_head = idx;
}

static void glyph_hash_remove(int idx)
{
	uint32_t bucket = glyph_cache[idx].hash & (PARTS_GLYPH_HASH_BUCKETS - 1);
	int *at = &glyph_hash_buckets[bucket];
	while (*at >= 0) {
		if (*at == idx) {
			*at = glyph_cache[idx].next_hash;
			glyph_cache[idx].next_hash = -1;
			return;
		}
		at = &glyph_cache[*at].next_hash;
	}
}

static uint32_t glyph_key_hash(const struct parts_glyph_key *k)
{
	const uint8_t *bytes = (const uint8_t *)k;
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < sizeof(*k); i++) {
		h ^= bytes[i];
		h *= 16777619u;
	}
	return h;
}

const char *parts_text_append_char(struct parts_text *t, const char *str)
{
	struct text_style eff_ts = parts_get_effective_text_style(&t->ts);
	if (!t->nr_lines) {
		t->lines = xcalloc(1, sizeof(struct parts_text_line));
		t->lines[0].line_space = t->line_space;
		t->nr_lines = 1;
	}
	if (*str == '\n') {
		t->lines = xrealloc_array(t->lines, t->nr_lines, t->nr_lines + 1,
				sizeof(struct parts_text_line));
		t->lines[t->nr_lines].height = text_style_height(&eff_ts);
		t->lines[t->nr_lines].line_space = t->line_space;
		t->nr_lines++;
		return str + 1;
	}

	struct parts_text_line *line = &t->lines[t->nr_lines - 1];
	line->chars = xrealloc_array(line->chars, line->nr_chars, line->nr_chars + 1,
			sizeof(struct parts_text_char));
	struct parts_text_char *ch = &line->chars[line->nr_chars++];

	ch->off = text_style_offset(&eff_ts);
	memset(ch->ch, 0, sizeof(ch->ch));
	int len = extract_sjis_char(str, ch->ch);

	glyph_cache_init();
	struct parts_glyph_key key;
	memset(&key, 0, sizeof(key));
	memcpy(key.ch, ch->ch, sizeof(key.ch));
	key.face = eff_ts.face;
	key.size = eff_ts.size;
	key.bold_width = eff_ts.bold_width;
	key.weight = eff_ts.weight;
	key.edge_left = eff_ts.edge_left;
	key.edge_up = eff_ts.edge_up;
	key.edge_right = eff_ts.edge_right;
	key.edge_down = eff_ts.edge_down;
	key.color = eff_ts.color;
	key.edge_color = eff_ts.edge_color;
	key.scale_x = eff_ts.scale_x;
	key.space_scale_x = eff_ts.space_scale_x;
	key.font_spacing = eff_ts.font_spacing;
	key.advance_edges = gfx_text_advance_edges;

	uint32_t hash = glyph_key_hash(&key);
	uint32_t bucket = hash & (PARTS_GLYPH_HASH_BUCKETS - 1);
	for (int idx = glyph_hash_buckets[bucket]; idx >= 0; idx = glyph_cache[idx].next_hash) {
		struct parts_glyph_cache_entry *e = &glyph_cache[idx];
		if (e->hash == hash && !memcmp(&e->key, &key, sizeof(key))) {
			glyph_lru_remove(idx);
			glyph_lru_insert_head(idx);
			e->refcount++;
			ch->t = e->t;
			ch->advance = e->advance;
			ch->cache_idx = idx;
			line->width += ch->advance;
			line->height = max(line->height, e->height);
			return str + len;
		}
	}

	int slot = -1;
	if (glyph_cache_used < PARTS_GLYPH_CACHE_SIZE) {
		slot = glyph_cache_used++;
	} else {
		for (int idx = glyph_lru_tail; idx >= 0; idx = glyph_cache[idx].prev_lru) {
			if (glyph_cache[idx].refcount <= 0) {
				slot = idx;
				break;
			}
		}
	}

	int width = gfx_size_char(&eff_ts, ch->ch);
	int height = text_style_height(&eff_ts);
	if (slot >= 0) {
		struct parts_glyph_cache_entry *e = &glyph_cache[slot];
		if (e->valid) {
			glyph_hash_remove(slot);
			glyph_lru_remove(slot);
			gfx_delete_texture(&e->t);
		}
		gfx_init_texture_rgba(&e->t, width, height, (SDL_Color){0,0,0,0});
		e->advance = ceilf(gfx_render_textf(&e->t, 0, 0, ch->ch, &eff_ts, false));
		e->height = height;
		e->key = key;
		e->hash = hash;
		e->valid = true;
		e->refcount = 1;
		e->next_hash = glyph_hash_buckets[bucket];
		glyph_hash_buckets[bucket] = slot;
		glyph_lru_insert_head(slot);
		ch->t = e->t;
		ch->advance = e->advance;
		ch->cache_idx = slot;
	} else {
		gfx_init_texture_rgba(&ch->t, width, height, (SDL_Color){0,0,0,0});
		ch->advance = ceilf(gfx_render_textf(&ch->t, 0, 0, ch->ch, &eff_ts, false));
		ch->cache_idx = -1;
	}
	line->width += ch->advance;
	line->height = max(line->height, height);
	return str + len;
}

void parts_text_append(struct parts *parts, struct parts_text *t, struct string *text)
{
	const char *msgp = text->text;
	while (*msgp) {
		msgp = parts_text_append_char(t, msgp);
	}
	parts_text_update_dims(parts, t);
}

void parts_text_update_dims(struct parts *parts, struct parts_text *t)
{
	// calculate dimensions of the text
	float f_width = 0;
	int height = 0;
	for (int i = 0; i < t->nr_lines; i++) {
		f_width = max(f_width, t->lines[i].width);
		if (i > 0)
			height += t->lines[i - 1].line_space;
		height += t->lines[i].height;
	}
	int width = ceilf(f_width);
	if (!width || !height)
		return;

	parts_set_dims(parts, &t->common, width, height);
}

void parts_text_free(struct parts_text *t)
{
	gfx_delete_texture(&t->common.texture);
	for (int i = 0; i < t->nr_lines; i++) {
		struct parts_text_line *line = &t->lines[i];
		for (int j = 0; j < line->nr_chars; j++) {
			int idx = line->chars[j].cache_idx;
			if (idx >= 0 && idx < PARTS_GLYPH_CACHE_SIZE && glyph_cache[idx].valid) {
				if (glyph_cache[idx].refcount > 0)
					glyph_cache[idx].refcount--;
			} else {
				gfx_delete_texture(&line->chars[j].t);
			}
		}
		free(line->chars);
	}
	free(t->lines);
}

static void parts_text_rerender(struct parts *parts, struct parts_text *t)
{
	if (!t->nr_lines)
		return;
	struct string *text = parts_text_get(t);
	parts_text_free(t);
	t->ts.font_size = NULL; // clear cached font
	t->lines = NULL;
	t->nr_lines = 0;
	parts_text_append(parts, t, text);
	free_string(text);
	parts_dirty(parts);
}

void parts_text_rerender_all(struct parts *parts, struct parts_text *t)
{
	parts_text_rerender(parts, t);
}

static void parts_text_clear(struct parts *parts, int state)
{
	struct parts_text *text = parts_get_text(parts, state);
	parts_text_free(text);
	text->lines = NULL;
	text->nr_lines = 0;
	text->cursor.x = 0;
	text->cursor.y = 0;
}

bool PE_SetText(int parts_no, struct string *text, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	if (parts->states[state].type == PARTS_TEXT) {
		struct parts_text *t = &parts->states[state].text;
		struct string *cur = parts_text_get(t);
		bool same = !strcmp(cur->text, text->text) && (t->nr_lines > 0 || !text->size);
		free_string(cur);
		if (same)
			return true;
	}
	parts_text_clear(parts, state);
	parts_text_append(parts, parts_get_text(parts, state), text);
	parts_dirty(parts);
	return true;
}

bool PE_AddPartsText(int parts_no, struct string *text, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	parts_text_append(parts, parts_get_text(parts, state), text);
	return true;
}

bool PE_SetPartsTextSurfaceArea(int parts_no, int x, int y, int w, int h, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	parts_set_surface_area(parts, &text->common, x, y, w, h);
	return true;
}

//bool PE_DeletePartsTopTextLine(int PartsNumber, int State);

bool PE_SetFont(int parts_no, int type, int size, int r, int g, int b, float bold_weight, int edge_r, int edge_g, int edge_b, float edge_weight, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	SDL_Color color = { r, g, b, 255 };
	SDL_Color edge_color = { edge_r, edge_g, edge_b, 255 };
	float old_left = text->ts.edge_left, old_up = text->ts.edge_up;
	float old_right = text->ts.edge_right, old_down = text->ts.edge_down;
	text_style_set_edge_width(&text->ts, edge_weight);
	if (text->ts.face == type && text->ts.size == size
			&& !memcmp(&text->ts.color, &color, sizeof(color))
			&& text->ts.bold_width == bold_weight
			&& !memcmp(&text->ts.edge_color, &edge_color, sizeof(edge_color))
			&& text->ts.edge_left == old_left && text->ts.edge_up == old_up
			&& text->ts.edge_right == old_right && text->ts.edge_down == old_down)
		return true;
	text->ts.face = type;
	text->ts.size = size;
	text->ts.color = color;
	text->ts.weight = bold_weight * 1000;
	text->ts.edge_color = edge_color;
	text->ts.bold_width = bold_weight;
	parts_text_rerender(parts, text);
	return true;
}

bool PE_SetPartsFontType(int parts_no, int type, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	if (text->ts.face == type)
		return true;
	text->ts.face = type;
	parts_text_rerender(parts, text);
	return true;
}

bool PE_SetPartsFontSize(int parts_no, int size, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	if (text->ts.size == size)
		return true;
	text->ts.size = size;
	parts_text_rerender(parts, text);
	return true;
}

bool PE_SetPartsFontColor(int parts_no, int r, int g, int b, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	SDL_Color color = { r, g, b, 255 };
	if (!memcmp(&text->ts.color, &color, sizeof(SDL_Color)))
		return true;
	text->ts.color = color;
	parts_text_rerender(parts, text);
	return true;
}

bool PE_SetPartsFontBoldWeight(int parts_no, float bold_weight, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	if (text->ts.bold_width == bold_weight)
		return true;
	text->ts.bold_width = bold_weight;
	parts_text_rerender(parts, text);
	return true;
}

bool PE_SetPartsFontEdgeColor(int parts_no, int r, int g, int b, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	SDL_Color color = { r, g, b, 255 };
	if (!memcmp(&text->ts.edge_color, &color, sizeof(SDL_Color)))
		return true;
	text->ts.edge_color = color;
	parts_text_rerender(parts, text);
	return true;
}

bool PE_SetPartsFontEdgeWeight(int parts_no, float edge_weight, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	float old_left = text->ts.edge_left;
	float old_up = text->ts.edge_up;
	float old_right = text->ts.edge_right;
	float old_down = text->ts.edge_down;
	text_style_set_edge_width(&text->ts, edge_weight);
	if (text->ts.edge_left == old_left && text->ts.edge_up == old_up &&
	    text->ts.edge_right == old_right && text->ts.edge_down == old_down)
		return true;
	parts_text_rerender(parts, text);
	return true;
}

bool PE_SetTextCharSpace(int parts_no, int char_space, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	if (text->ts.font_spacing == char_space)
		return true;
	text->ts.font_spacing = char_space;
	parts_text_rerender(parts, text);
	return true;
}

bool PE_SetTextLineSpace(int parts_no, int line_space, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_text *text = parts_get_text(parts, state);
	if (text->line_space == line_space)
		return true;
	text->line_space = line_space;
	parts_text_rerender(parts, text);
	return true;
}

static struct parts_text *try_get_text(int no, int state)
{
	if (!parts_state_valid(--state)) return NULL;
	struct parts *p = parts_try_get(no);
	return p && p->states[state].type == PARTS_TEXT ? &p->states[state].text : NULL;
}

int PE_GetTextCharSpace(int no, int state)
{
	struct parts_text *t = try_get_text(no, state);
	return t ? t->ts.font_spacing : 0;
}

int PE_GetTextLineSpace(int no, int state)
{
	struct parts_text *t = try_get_text(no, state);
	return t ? t->line_space : 0;
}

struct string *PE_GetTextPartsText(int no, int state)
{
	struct parts_text *t = try_get_text(no, state);
	return t ? parts_text_get(t) : string_ref(&EMPTY_STRING);
}

void PE_GetPartsTextSurfaceArea(int no, int *x, int *y, int *w, int *h, int state)
{
	struct parts_text *t = try_get_text(no, state);
	Rectangle area = t ? t->common.surface_area : (Rectangle){0};
	*x = area.x; *y = area.y; *w = area.w; *h = area.h;
}

void PE_GetPartsTextFontProperty(int no, int *face, int *size, int *r, int *g, int *b,
		float *bold, int *er, int *eg, int *eb, float *edge, int state)
{
	if (!parts_state_valid(--state)) return;
	struct text_style *ts = &parts_get_text(parts_get(no), state)->ts;
	*face = ts->face; *size = ts->size;
	*r = ts->color.r; *g = ts->color.g; *b = ts->color.b;
	*bold = ts->bold_width;
	*er = ts->edge_color.r; *eg = ts->edge_color.g; *eb = ts->edge_color.b;
	*edge = ts->edge_left;
}


