/* Copyright (C) 2023 Nunuhara Cabbage <nunuhara@haniwa.technology>
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

#include <stdlib.h>
#include <zlib.h>
#include <cglm/cglm.h>
#include "system4.h"
#include "system4/string.h"
#include "system4/archive.h"

#include "vm/page.h"
#include "asset_manager.h"
#include "parts.h"
#include "parts_activity.h"
#include "parts_internal.h"
#include "../hll/iarray.h"

#define CURRENT_SAVE_VERSION 29

static struct parts_context *back_scene;

static void save_parts_params(struct iarray_writer *w, struct parts_params *params)
{
	iarray_write(w, params->z);
	iarray_write_float(w, params->pos.x);
	iarray_write_float(w, params->pos.y);
	iarray_write(w, params->show);
	iarray_write(w, params->alpha);
	iarray_write_float(w, params->scale.x);
	iarray_write_float(w, params->scale.y);
	iarray_write_float(w, params->rotation.x);
	iarray_write_float(w, params->rotation.y);
	iarray_write_float(w, params->rotation.z);
	iarray_write_color(w, &params->add_color);
	iarray_write_color(w, &params->multiply_color);
}

static void load_parts_params(struct iarray_reader *r, struct parts_params *params, int version)
{
	params->z = iarray_read(r);
	params->pos.x = version >= 19 ? iarray_read_float(r) : iarray_read(r);
	params->pos.y = version >= 19 ? iarray_read_float(r) : iarray_read(r);
	params->show = iarray_read(r);
	params->alpha = iarray_read(r);
	params->scale.x = iarray_read_float(r);
	params->scale.y = iarray_read_float(r);
	params->rotation.x = iarray_read_float(r);
	params->rotation.y = iarray_read_float(r);
	params->rotation.z = iarray_read_float(r);
	iarray_read_color(r, &params->add_color);
	iarray_read_color(r, &params->multiply_color);
}

static void save_parts_cg(struct iarray_writer *w, struct parts_cg *cg)
{
	iarray_write(w, cg->no);
	iarray_write_string_or_null(w, cg->name);
}

static void load_parts_cg(struct iarray_reader *r, struct parts *parts, struct parts_cg *cg)
{
	int no = iarray_read(r);
	struct string *name = iarray_read_string_or_null(r);
	if (name) {
		parts_cg_set(parts, cg, name);
		free_string(name);
	} else if (no) {
		parts_cg_set_by_index(parts, cg, no);
	}
}

static void save_parts_text(struct iarray_writer *w, struct parts_text *text)
{
	iarray_write(w, text->line_space);
	iarray_write_text_style(w, &text->ts);
	iarray_write(w, text->nr_lines);
	for (unsigned i = 0; i < text->nr_lines; i++) {
		struct string *s = parts_text_line_get(&text->lines[i]);
		iarray_write_string(w, s);
		free_string(s);
	}
}

static void load_parts_text(struct iarray_reader *r, struct parts *parts,
		struct parts_text *text)
{
	// FIXME: this won't accurately restore the text state if the text style
	//        varies per-character
	text->line_space = iarray_read(r);
	iarray_read_text_style(r, &text->ts);

	text->nr_lines = 0;
	text->lines = NULL;
	text->cursor.x = 0;
	text->cursor.y = 0;

	int nr_lines = iarray_read(r);
	for (int i = 0; i < nr_lines; i++) {
		struct string *line = iarray_read_string(r);
		if (i > 0) {
			struct string *nl = make_string("\n", 1);
			parts_text_append(parts, text, nl);
			free_string(nl);
		}
		parts_text_append(parts, text, line);
		free_string(line);
	}
}

static void save_parts_animation(struct iarray_writer *w, struct parts_animation *anim)
{
	iarray_write_string_or_null(w, anim->cg_name);
	iarray_write(w, anim->start_no);
	iarray_write(w, anim->nr_frames);
	iarray_write(w, anim->frame_time);
	iarray_write(w, anim->elapsed);
	iarray_write(w, anim->current_frame);
}

static void load_parts_animation(struct iarray_reader *r, struct parts *parts,
		struct parts_animation *anim)
{
	struct string *cg_name = iarray_read_string_or_null(r);
	int start_no = iarray_read(r);
	int nr_frames = iarray_read(r);
	int frame_time = iarray_read(r);

	if (cg_name) {
		parts_animation_set_cg(parts, anim, cg_name, start_no, nr_frames, frame_time);
		free_string(cg_name);
	} else {
		parts_animation_set_cg_by_index(parts, anim, start_no, nr_frames, frame_time);
	}

	anim->elapsed = iarray_read(r);
	anim->current_frame = iarray_read(r);
}

static void save_parts_numeral(struct iarray_writer *w, struct parts_numeral *num)
{
	iarray_write(w, num->have_num);
	iarray_write(w, num->num);
	iarray_write(w, num->space);
	iarray_write(w, num->show_comma);
	iarray_write(w, num->length);
	iarray_write(w, num->font_no);
	iarray_write(w, num->show_type);
	iarray_write(w, num->full_pitch);
	iarray_write(w, num->show_padding);
	iarray_write_text_style(w, &num->ts);
	iarray_write_string(w, num->cg_name);
}

static void load_parts_numeral(struct iarray_reader *r, struct parts *parts,
		struct parts_numeral *num, int version)
{
	num->have_num = iarray_read(r);
	num->num = iarray_read(r);
	num->space = iarray_read(r);
	num->show_comma = iarray_read(r);
	num->length = iarray_read(r);
	num->font_no = iarray_read(r);
	if (version >= 16) {
		num->show_type = iarray_read(r);
		num->full_pitch = iarray_read(r);
		num->show_padding = iarray_read(r);
		iarray_read_text_style(r, &num->ts);
		free_string(num->cg_name);
		num->cg_name = iarray_read_string(r);
	} else if (num->font_no >= 0 && num->font_no < parts_nr_numeral_fonts) {
		num->show_type = parts_numeral_fonts[num->font_no].type == PARTS_NUMERAL_FONT_COMBINED;
	}

	if (num->have_num)
		parts_numeral_set_number(parts, num, num->num);
}

static void save_parts_gauge(struct iarray_writer *w, struct parts_gauge *gauge)
{
	iarray_write(w, gauge->cg_no);
	iarray_write_float(w, gauge->rate);
	iarray_write_float(w, gauge->numerator);
	iarray_write_float(w, gauge->denominator);
	iarray_write(w, gauge->reverse);
	iarray_write_string_or_null(w, gauge->cg_name);
}

static void load_parts_gauge(struct iarray_reader *r, struct parts *parts,
		struct parts_gauge *gauge, bool vert, int version)
{
	gauge->cg_no = iarray_read(r);
	gauge->rate = iarray_read_float(r);
	struct string *cg_name = NULL;
	if (version >= 29) {
		gauge->numerator = iarray_read_float(r);
		gauge->denominator = iarray_read_float(r);
		gauge->reverse = !!iarray_read(r);
		cg_name = iarray_read_string_or_null(r);
	} else {
		gauge->numerator = gauge->rate * 100.0f;
		gauge->denominator = 100.0f;
		gauge->reverse = false;
	}

	if (cg_name && cg_name->size)
		parts_gauge_set_cg(parts, gauge, cg_name);
	else if (gauge->cg_no >= 0)
		parts_gauge_set_cg_by_index(parts, gauge, gauge->cg_no);
	if (cg_name)
		free_string(cg_name);

	if (gauge->common.texture.handle) {
		if (vert)
			parts_vgauge_set_rate(parts, gauge, gauge->rate);
		else
			parts_hgauge_set_rate(parts, gauge, gauge->rate);
	}
}

static void save_parts_cp_op(struct iarray_writer *w, struct parts_cp_op *op)
{
	iarray_write(w, op->type);
	switch (op->type) {
	case PARTS_CP_DRAW_LINE:
		iarray_write(w, op->line.x0);
		iarray_write(w, op->line.y0);
		iarray_write(w, op->line.x1);
		iarray_write(w, op->line.y1);
		iarray_write(w, op->line.r);
		iarray_write(w, op->line.g);
		iarray_write(w, op->line.b);
		iarray_write(w, op->line.a);
		break;
	case PARTS_CP_CREATE:
	case PARTS_CP_CREATE_PIXEL_ONLY:
		iarray_write(w, op->create.w);
		iarray_write(w, op->create.h);
		break;
	case PARTS_CP_CG:
		iarray_write_string(w, op->cg.name);
		break;
	case PARTS_CP_BLEND_CG:
		iarray_write_string(w, op->cg.name);
		iarray_write(w, op->cg.x);
		iarray_write(w, op->cg.y);
		break;
	case PARTS_CP_FILL:
	case PARTS_CP_FILL_ALPHA_COLOR:
	case PARTS_CP_FILL_AMAP:
	case PARTS_CP_FILL_WITH_ALPHA:
	case PARTS_CP_DRAW_RECT:
		iarray_write(w, op->fill.x);
		iarray_write(w, op->fill.y);
		iarray_write(w, op->fill.w);
		iarray_write(w, op->fill.h);
		iarray_write(w, op->fill.r);
		iarray_write(w, op->fill.g);
		iarray_write(w, op->fill.b);
		iarray_write(w, op->fill.a);
		break;
	case PARTS_CP_FILL_GRADATION_HORIZON:
	case PARTS_CP_MUL_AMAP_GRADATION_HORIZON:
	case PARTS_CP_MUL_AMAP_GRADATION_VERTICAL:
		iarray_write(w, op->fill.x);
		iarray_write(w, op->fill.y);
		iarray_write(w, op->fill.w);
		iarray_write(w, op->fill.h);
		iarray_write(w, op->fill.r);
		iarray_write(w, op->fill.g);
		iarray_write(w, op->fill.b);
		iarray_write(w, op->fill.a);
		iarray_write(w, op->fill.r2);
		iarray_write(w, op->fill.g2);
		iarray_write(w, op->fill.b2);
		iarray_write(w, op->fill.a2);
		break;
	case PARTS_CP_DRAW_CUT_CG:
	case PARTS_CP_COPY_CUT_CG:
	case PARTS_CP_ALPHA_BLEND_CUT_CG:
	case PARTS_CP_ONLY_ALPHA_CUT_CG:
		iarray_write_string(w, op->cut_cg.cg_name);
		iarray_write(w, op->cut_cg.dx);
		iarray_write(w, op->cut_cg.dy);
		iarray_write(w, op->cut_cg.dw);
		iarray_write(w, op->cut_cg.dh);
		iarray_write(w, op->cut_cg.sx);
		iarray_write(w, op->cut_cg.sy);
		iarray_write(w, op->cut_cg.sw);
		iarray_write(w, op->cut_cg.sh);
		iarray_write(w, op->cut_cg.interp_type);
		iarray_write(w, op->cut_cg.scale);
		break;
	case PARTS_CP_DRAW_TEXT:
	case PARTS_CP_COPY_TEXT:
	case PARTS_CP_ALPHA_BLEND_TEXT:
	case PARTS_CP_ONLY_ALPHA_TEXT:
		iarray_write_string(w, op->text.text);
		iarray_write(w, op->text.x);
		iarray_write(w, op->text.y);
		iarray_write(w, op->text.line_space);
		iarray_write_text_style(w, &op->text.style);
		break;
	case PARTS_CP_GRAY_FILTER:
		iarray_write(w, op->filter.x);
		iarray_write(w, op->filter.y);
		iarray_write(w, op->filter.w);
		iarray_write(w, op->filter.h);
		iarray_write(w, op->filter.full_size);
		break;
	case PARTS_CP_ADD_FILTER:
	case PARTS_CP_MUL_FILTER:
		iarray_write(w, op->filter.x);
		iarray_write(w, op->filter.y);
		iarray_write(w, op->filter.w);
		iarray_write(w, op->filter.h);
		iarray_write(w, op->filter.r);
		iarray_write(w, op->filter.g);
		iarray_write(w, op->filter.b);
		iarray_write(w, op->filter.full_size);
		break;
	case PARTS_CP_HBLUR_FILTER:
	case PARTS_CP_VBLUR_FILTER:
		iarray_write(w, op->filter.x);
		iarray_write(w, op->filter.y);
		iarray_write(w, op->filter.w);
		iarray_write(w, op->filter.h);
		iarray_write(w, op->filter.blur);
		iarray_write(w, op->filter.full_size);
		break;
	}
}

static struct string *load_cp_cg_name(struct iarray_reader *r, int version)
{
	if (version >= 5)
		return iarray_read_string(r);
	// In version 4 and earlier, the CG name was stored as an integer ID.
	// Convert it to a string name by looking up the asset.
	int cg_no = iarray_read(r);
	if (cg_no <= 0)
		return string_ref(&EMPTY_STRING);
	struct archive_data *data = asset_get(ASSET_CG, cg_no);
	if (!data)
		return string_ref(&EMPTY_STRING);
	struct string *name = cstr_to_string(data->name);
	archive_free_data(data);
	return name;
}

static struct parts_cp_op *load_parts_cp_op(struct iarray_reader *r, int version)
{
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = iarray_read(r);
	switch (op->type) {
	case PARTS_CP_DRAW_LINE:
		op->line.x0 = iarray_read(r);
		op->line.y0 = iarray_read(r);
		op->line.x1 = iarray_read(r);
		op->line.y1 = iarray_read(r);
		op->line.r = iarray_read(r);
		op->line.g = iarray_read(r);
		op->line.b = iarray_read(r);
		op->line.a = iarray_read(r);
		break;
	case PARTS_CP_CREATE:
	case PARTS_CP_CREATE_PIXEL_ONLY:
		op->create.w = iarray_read(r);
		op->create.h = iarray_read(r);
		break;
	case PARTS_CP_CG:
		op->cg.name = load_cp_cg_name(r, version);
		break;
	case PARTS_CP_BLEND_CG:
		op->cg.name = load_cp_cg_name(r, version);
		op->cg.x = iarray_read(r);
		op->cg.y = iarray_read(r);
		break;
	case PARTS_CP_FILL:
	case PARTS_CP_FILL_ALPHA_COLOR:
	case PARTS_CP_FILL_AMAP:
	case PARTS_CP_FILL_WITH_ALPHA:
	case PARTS_CP_DRAW_RECT:
		op->fill.x = iarray_read(r);
		op->fill.y = iarray_read(r);
		op->fill.w = iarray_read(r);
		op->fill.h = iarray_read(r);
		op->fill.r = iarray_read(r);
		op->fill.g = iarray_read(r);
		op->fill.b = iarray_read(r);
		op->fill.a = iarray_read(r);
		break;
	case PARTS_CP_FILL_GRADATION_HORIZON:
	case PARTS_CP_MUL_AMAP_GRADATION_HORIZON:
	case PARTS_CP_MUL_AMAP_GRADATION_VERTICAL:
		op->fill.x = iarray_read(r);
		op->fill.y = iarray_read(r);
		op->fill.w = iarray_read(r);
		op->fill.h = iarray_read(r);
		op->fill.r = iarray_read(r);
		op->fill.g = iarray_read(r);
		op->fill.b = iarray_read(r);
		op->fill.a = iarray_read(r);
		op->fill.r2 = iarray_read(r);
		op->fill.g2 = iarray_read(r);
		op->fill.b2 = iarray_read(r);
		op->fill.a2 = iarray_read(r);
		break;
	case PARTS_CP_DRAW_CUT_CG:
	case PARTS_CP_COPY_CUT_CG:
	case PARTS_CP_ALPHA_BLEND_CUT_CG:
	case PARTS_CP_ONLY_ALPHA_CUT_CG:
		op->cut_cg.cg_name = load_cp_cg_name(r, version);
		op->cut_cg.dx = iarray_read(r);
		op->cut_cg.dy = iarray_read(r);
		op->cut_cg.dw = iarray_read(r);
		op->cut_cg.dh = iarray_read(r);
		op->cut_cg.sx = iarray_read(r);
		op->cut_cg.sy = iarray_read(r);
		op->cut_cg.sw = iarray_read(r);
		op->cut_cg.sh = iarray_read(r);
		op->cut_cg.interp_type = iarray_read(r);
		op->cut_cg.scale = version >= 25 ? !!iarray_read(r)
			: (op->cut_cg.sw != op->cut_cg.dw || op->cut_cg.sh != op->cut_cg.dh);
		break;
	case PARTS_CP_DRAW_TEXT:
	case PARTS_CP_COPY_TEXT:
	case PARTS_CP_ALPHA_BLEND_TEXT:
	case PARTS_CP_ONLY_ALPHA_TEXT:
		op->text.text = iarray_read_string(r);
		op->text.x = iarray_read(r);
		op->text.y = iarray_read(r);
		op->text.line_space = iarray_read(r);
		iarray_read_text_style(r, &op->text.style);
		break;
	case PARTS_CP_GRAY_FILTER:
		op->filter.x = iarray_read(r);
		op->filter.y = iarray_read(r);
		op->filter.w = iarray_read(r);
		op->filter.h = iarray_read(r);
		op->filter.full_size = !!iarray_read(r);
		break;
	case PARTS_CP_ADD_FILTER:
	case PARTS_CP_MUL_FILTER:
		op->filter.x = iarray_read(r);
		op->filter.y = iarray_read(r);
		op->filter.w = iarray_read(r);
		op->filter.h = iarray_read(r);
		op->filter.r = iarray_read(r);
		op->filter.g = iarray_read(r);
		op->filter.b = iarray_read(r);
		op->filter.full_size = !!iarray_read(r);
		break;
	case PARTS_CP_HBLUR_FILTER:
	case PARTS_CP_VBLUR_FILTER:
		op->filter.x = iarray_read(r);
		op->filter.y = iarray_read(r);
		op->filter.w = iarray_read(r);
		op->filter.h = iarray_read(r);
		op->filter.blur = iarray_read(r);
		op->filter.full_size = !!iarray_read(r);
		break;
	}
	return op;
}

#define CP_PIXELS_MAGIC 0x43505031
#define CP_PIXELS_LIMIT (64u * 1024u * 1024u)

static bool cp_valid_filter(int v, bool is_min)
{
	return v == GL_NEAREST || v == GL_LINEAR ||
		(is_min && v >= GL_NEAREST_MIPMAP_NEAREST && v <= GL_LINEAR_MIPMAP_LINEAR);
}

static bool cp_valid_wrap(int v)
{
	return v == GL_REPEAT || v == GL_CLAMP_TO_EDGE || v == GL_MIRRORED_REPEAT;
}

static void save_construction_pixels(struct iarray_writer *w, struct parts_state *state,
		Texture *texture, bool needed)
{
	if (!needed) {
		iarray_write(w, CP_PIXELS_MAGIC);
		iarray_write(w, 0);
		return;
	}
	struct parts_texture_pixels image;
	int got = parts_copy_texture_pixels(state, texture, &image);
	if (got <= 0) {
		iarray_write(w, CP_PIXELS_MAGIC);
		iarray_write(w, 2);
		return;
	}
	size_t bytes = (size_t)image.w * image.h * 4;
	uLongf size = compressBound(bytes);
	uint8_t *compressed = malloc(size);
	if (!compressed || compress2(compressed, &size, image.rgba, bytes, Z_BEST_SPEED) != Z_OK ||
	    size > CP_PIXELS_LIMIT) {
		WARNING("Failed to snapshot non-replayable ConstructionProcess texture (%dx%d)", image.w, image.h);
		free(compressed);
		free(image.rgba);
		iarray_write(w, CP_PIXELS_MAGIC);
		iarray_write(w, 2);
		return;
	}
	iarray_write(w, CP_PIXELS_MAGIC);
	iarray_write(w, 1);
	iarray_write(w, image.w);
	iarray_write(w, image.h);
	iarray_write(w, image.min_filter);
	iarray_write(w, image.mag_filter);
	iarray_write(w, image.wrap_s);
	iarray_write(w, image.wrap_t);
	iarray_write(w, image.opaque ? 1 : 0);
	iarray_write(w, (int)bytes);
	iarray_write(w, (int)size);
	iarray_write(w, (int)(uint32_t)crc32(0, image.rgba, bytes));
	for (size_t i = 0; i < size; i += 4) {
		uint32_t word = 0;
		for (unsigned j = 0; j < 4 && i + j < size; j++)
			word |= (uint32_t)compressed[i + j] << (8 * j);
		iarray_write(w, (int32_t)word);
	}
	free(compressed);
	free(image.rgba);
}

static int load_construction_pixels(struct iarray_reader *r, Texture *texture)
{
	if (iarray_read(r) != CP_PIXELS_MAGIC)
		return -1;
	int present = iarray_read(r);
	if (r->error || present < 0 || present > 2)
		return -1;
	if (present == 0 || present == 2)
		return present;

	int w = iarray_read(r);
	int h = iarray_read(r);
	int min_filter = iarray_read(r);
	int mag_filter = iarray_read(r);
	int wrap_s = iarray_read(r);
	int wrap_t = iarray_read(r);
	int opaque = iarray_read(r);
	int bytes = iarray_read(r);
	int packed = iarray_read(r);
	uint32_t crc = (uint32_t)iarray_read(r);
	if (r->error || w < 1 || h < 1 || w > 8192 || h > 8192 || bytes < 1 || packed < 1 ||
	    (size_t)w * h * 4 > CP_PIXELS_LIMIT || (size_t)bytes != (size_t)w * h * 4 ||
	    (unsigned)packed > CP_PIXELS_LIMIT || !cp_valid_filter(min_filter, true) ||
	    !cp_valid_filter(mag_filter, false) || !cp_valid_wrap(wrap_s) || !cp_valid_wrap(wrap_t) ||
	    ((size_t)packed + 3) / 4 > r->size - r->pos) {
		return -1;
	}
	uint8_t *data = malloc(packed);
	uint8_t *raw = malloc(bytes);
	if (!data || !raw) {
		free(data);
		free(raw);
		return -1;
	}
	for (int i = 0; i < packed; i += 4) {
		uint32_t word = (uint32_t)iarray_read(r);
		for (int j = 0; j < 4 && i + j < packed; j++)
			data[i + j] = (uint8_t)(word >> (8 * j));
	}
	uLongf raw_len = (uLongf)bytes;
	bool ok = !r->error &&
		uncompress(raw, &raw_len, data, (uLong)packed) == Z_OK &&
		raw_len == (uLongf)bytes &&
		(uint32_t)crc32(0, raw, bytes) == crc;
	free(data);
	if (ok) {
		gfx_delete_texture(texture);
		gfx_init_texture_with_pixels(texture, w, h, raw);
		texture->opaque = !!opaque;
		if (texture->handle) {
			GLint prev_binding = 0;
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_binding);
			glBindTexture(GL_TEXTURE_2D, texture->handle);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag_filter);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_s);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_t);
			if (min_filter >= GL_NEAREST_MIPMAP_NEAREST && min_filter <= GL_LINEAR_MIPMAP_LINEAR)
				glGenerateMipmap(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, (GLuint)prev_binding);
		}
		ok = texture->handle != 0;
	}
	free(raw);
	return ok ? 1 : -1;
}

static void save_parts_construction_process(struct iarray_writer *w, struct parts *parts,
		struct parts_state *state, struct parts_construction_process *cproc)
{
	if (cproc->needs_pixel_snapshot && cproc->needs_build)
		parts_build_construction_process(parts, cproc);

	unsigned ops_count_pos = iarray_writer_pos(w);
	iarray_write(w, 0); // size of ops list

	unsigned ops_count = 0;
	struct parts_cp_op *op;
	TAILQ_FOREACH(op, &cproc->ops, entry) {
		save_parts_cp_op(w, op);
		ops_count++;
	}

	iarray_write_at(w, ops_count_pos, ops_count);
	save_construction_pixels(w, state, &cproc->common.texture, cproc->needs_pixel_snapshot);
}

static void load_parts_construction_process(struct iarray_reader *r, struct parts *parts,
		struct parts_construction_process *cproc, int version)
{
	int ops_count = iarray_read(r);
	if (r->error || ops_count < 0 || (unsigned)ops_count > r->size - r->pos) {
		r->error = 1;
		return;
	}
	for (int i = 0; i < ops_count; i++) {
		struct parts_cp_op *op = load_parts_cp_op(r, version);
		parts_add_cp_op(cproc, op);
	}
	int mode = version >= 28 ? load_construction_pixels(r, &cproc->common.texture) : 0;
	if (mode < 0) {
		WARNING("Failed to restore ConstructionProcess pixel snapshot");
		parts_clear_construction_process(cproc);
		r->error = 1;
		return;
	}
	if (mode == 0)
		parts_build_construction_process(parts, cproc);
	cproc->needs_pixel_snapshot = mode > 0;
}

static void save_parts_flash(struct iarray_writer *w, struct parts_flash *flash)
{
	iarray_write_string_or_null(w, flash->name);
	iarray_write(w, flash->stopped);
	iarray_write(w, flash->current_frame);
}

static void load_parts_flash(struct iarray_reader *r, struct parts *parts,
		struct parts_flash *flash)
{
	struct string *name = iarray_read_string_or_null(r);
	parts_flash_load(parts, flash, name);
	if (name)
		free_string(name);
	flash->stopped = !!iarray_read(r);
	parts_flash_seek(flash, iarray_read(r));
}

static void save_flat_key_graphic(struct iarray_writer *w, const struct flat_key_data_graphic *k)
{
	iarray_write_float(w, k->pos_x);
	iarray_write_float(w, k->pos_y);
	iarray_write_float(w, k->scale_x);
	iarray_write_float(w, k->scale_y);
	iarray_write_float(w, k->angle_x);
	iarray_write_float(w, k->angle_y);
	iarray_write_float(w, k->angle_z);
	iarray_write(w, k->add_r);
	iarray_write(w, k->add_g);
	iarray_write(w, k->add_b);
	iarray_write(w, k->mul_r);
	iarray_write(w, k->mul_g);
	iarray_write(w, k->mul_b);
	iarray_write(w, k->alpha);
	iarray_write(w, k->area_x);
	iarray_write(w, k->area_y);
	iarray_write(w, k->area_width);
	iarray_write(w, k->area_height);
	iarray_write(w, k->draw_filter);
	iarray_write(w, k->uk1);
	iarray_write(w, k->origin_x);
	iarray_write(w, k->origin_y);
	iarray_write(w, k->uk2);
	iarray_write(w, k->reverse_tb);
	iarray_write(w, k->reverse_lr);
}

static void load_flat_key_graphic(struct iarray_reader *r, struct flat_key_data_graphic *k)
{
	k->pos_x = iarray_read_float(r);
	k->pos_y = iarray_read_float(r);
	k->scale_x = iarray_read_float(r);
	k->scale_y = iarray_read_float(r);
	k->angle_x = iarray_read_float(r);
	k->angle_y = iarray_read_float(r);
	k->angle_z = iarray_read_float(r);
	k->add_r = iarray_read(r);
	k->add_g = iarray_read(r);
	k->add_b = iarray_read(r);
	k->mul_r = iarray_read(r);
	k->mul_g = iarray_read(r);
	k->mul_b = iarray_read(r);
	k->alpha = iarray_read(r);
	k->area_x = iarray_read(r);
	k->area_y = iarray_read(r);
	k->area_width = iarray_read(r);
	k->area_height = iarray_read(r);
	k->draw_filter = iarray_read(r);
	k->uk1 = iarray_read(r);
	k->origin_x = iarray_read(r);
	k->origin_y = iarray_read(r);
	k->uk2 = iarray_read(r);
	k->reverse_tb = !!iarray_read(r);
	k->reverse_lr = !!iarray_read(r);
}

static void save_flat_layer_state(struct iarray_writer *w, struct flat_layer_state *s)
{
	iarray_write(w, s != NULL);
	if (!s)
		return;
	iarray_write(w, s->current_frame);
	iarray_write(w, s->stopped);
	iarray_write(w, s->suppress_advance);
	iarray_write(w, s->jump_target);
	iarray_write(w, s->last_rendered_frame);
	iarray_write(w, (int)s->nr_timelines);
	for (size_t i = 0; i < s->nr_timelines; i++) {
		iarray_write(w, s->last_script_keys[i]);
		iarray_write(w, s->disabled_timelines ? s->disabled_timelines[i] : 0);
	}
	int birth_count = 0;
	for (struct flat_emitter_birth *b = s->emitter_births; b; b = b->next)
		birth_count++;
	iarray_write(w, birth_count);
	for (struct flat_emitter_birth *b = s->emitter_births; b; b = b->next) {
		iarray_write(w, b->timeline_idx);
		iarray_write(w, b->frame);
		iarray_write(w, (int)b->instance);
		iarray_write(w, b->has_position);
		iarray_write_float(w, b->position[0]);
		iarray_write_float(w, b->position[1]);
		iarray_write_float(w, b->position[2]);
		iarray_write(w, b->has_ancestor_keys);
		int ac = b->has_ancestor_keys ? b->ancestor_count : 0;
		iarray_write(w, ac);
		for (int d = 0; d < ac; d++)
			save_flat_key_graphic(w, &b->ancestor_keys[d]);
	}
	for (size_t i = 0; i < s->nr_timelines; i++)
		save_flat_layer_state(w, s->children[i]);
}

static struct flat_layer_state *load_flat_layer_state(struct iarray_reader *r,
		struct flat *fl, struct flat_timeline *timelines, size_t nr_timelines)
{
	if (!iarray_read(r))
		return NULL;
	int current_frame = iarray_read(r);
	bool stopped = !!iarray_read(r);
	bool suppress_advance = !!iarray_read(r);
	int jump_target = iarray_read(r);
	int last_rendered_frame = iarray_read(r);
	int saved_nr_timelines = max(0, iarray_read(r));
	size_t alloc_tl = fl ? nr_timelines : (size_t)saved_nr_timelines;
	struct flat_layer_state *s = parts_flat_layer_state_new(alloc_tl);
	s->current_frame = current_frame;
	s->stopped = stopped;
	s->suppress_advance = suppress_advance;
	s->jump_target = jump_target;
	s->last_rendered_frame = last_rendered_frame;
	for (int i = 0; i < saved_nr_timelines; i++) {
		int lsk = iarray_read(r);
		bool dis = !!iarray_read(r);
		if ((size_t)i < s->nr_timelines) {
			s->last_script_keys[i] = lsk;
			s->disabled_timelines[i] = dis;
		}
	}
	int birth_count = max(0, iarray_read(r));
	for (int i = 0; i < birth_count; i++) {
		int tl_idx = iarray_read(r);
		int frame = iarray_read(r);
		unsigned inst = (unsigned)iarray_read(r);
		bool has_pos = !!iarray_read(r);
		vec3 pos = { iarray_read_float(r), iarray_read_float(r), iarray_read_float(r) };
		bool has_anc = !!iarray_read(r);
		int ac = max(0, iarray_read(r));
		struct flat_key_data_graphic anc[FLAT_MAX_ANCESTOR_DEPTH];
		for (int d = 0; d < ac; d++) {
			struct flat_key_data_graphic tmp;
			load_flat_key_graphic(r, d < FLAT_MAX_ANCESTOR_DEPTH ? &anc[d] : &tmp);
		}
		if (timelines && tl_idx >= 0 && (size_t)tl_idx < nr_timelines) {
			struct flat_emitter_birth *b = parts_flat_get_emitter_birth(
					s, &timelines[tl_idx], tl_idx, frame, inst, true);
			b->has_position = has_pos;
			glm_vec3_copy(pos, b->position);
			b->has_ancestor_keys = has_anc;
			b->ancestor_count = min(ac, FLAT_MAX_ANCESTOR_DEPTH);
			for (int d = 0; d < b->ancestor_count; d++)
				b->ancestor_keys[d] = anc[d];
		}
	}
	for (int i = 0; i < saved_nr_timelines; i++) {
		struct flat_timeline *child_tls = NULL;
		size_t child_nr = 0;
		if (fl && timelines && (size_t)i < nr_timelines
				&& timelines[i].type == FLAT_TIMELINE_GRAPHIC) {
			int lib_idx = parts_flat_find_library(fl, timelines[i].library_name->text);
			if (lib_idx >= 0 && fl->libraries[lib_idx].type == FLAT_LIB_TIMELINE) {
				child_tls = fl->libraries[lib_idx].timeline.timelines;
				child_nr = fl->libraries[lib_idx].timeline.nr_timelines;
			}
		}
		struct flat_layer_state *child = load_flat_layer_state(r, fl, child_tls, child_nr);
		if ((size_t)i < s->nr_timelines)
			s->children[i] = child;
		else
			parts_flat_layer_state_free(child);
	}
	return s;
}

static void save_parts_flat(struct iarray_writer *w, struct parts_flat *flat)
{
	iarray_write_string_or_null(w, flat->name);
	iarray_write_float(w, flat->fps_rate);
	iarray_write(w, flat->stopped);
	iarray_write(w, flat->needs_advance);
	iarray_write_float(w, (float)flat->elapsed);
	iarray_write(w, flat->begin_seek_frame);
	iarray_write(w, flat->pending_seek_delta);
	save_flat_layer_state(w, flat->root_state);
}

static void load_parts_flat(struct iarray_reader *r, struct parts *parts,
		struct parts_flat *flat, int version)
{
	struct string *name = iarray_read_string_or_null(r);
	parts_flat_load(parts, flat, name);
	if (name)
		free_string(name);
	flat->needs_advance = true;
	flat->fps_rate = version >= 14 ? iarray_read_float(r) : 1.0f;
	if (version >= 26) {
		flat->stopped = !!iarray_read(r);
		flat->needs_advance = !!iarray_read(r);
		flat->elapsed = iarray_read_float(r);
		flat->begin_seek_frame = iarray_read(r);
		flat->pending_seek_delta = iarray_read(r);
		struct flat_layer_state *loaded = load_flat_layer_state(
				r, flat->flat,
				flat->flat ? flat->flat->timelines : NULL,
				flat->flat ? flat->flat->nr_timelines : 0);
		if (flat->flat && loaded) {
			parts_flat_layer_state_free(flat->root_state);
			flat->root_state = loaded;
		} else {
			parts_flat_layer_state_free(loaded);
		}
	}
}

static void save_parts_layout_box(struct iarray_writer *w, struct parts_layout_box *lb)
{
	iarray_write(w, lb->layout_type);
	iarray_write(w, lb->wrap);
	iarray_write_float(w, lb->wrap_size);
	iarray_write(w, lb->align);
	iarray_write(w, lb->padding_top);
	iarray_write(w, lb->padding_bottom);
	iarray_write(w, lb->padding_left);
	iarray_write(w, lb->padding_right);
}

static void load_parts_layout_box(struct iarray_reader *r, struct parts_layout_box *lb, int version)
{
	lb->layout_type = iarray_read(r);
	lb->wrap = iarray_read(r);
	lb->wrap_size = version >= 15 ? iarray_read_float(r) : iarray_read(r);
	lb->align = iarray_read(r);
	lb->padding_top = iarray_read(r);
	lb->padding_bottom = iarray_read(r);
	lb->padding_left = iarray_read(r);
	lb->padding_right = iarray_read(r);
}

static void save_parts_state(struct iarray_writer *w, struct parts *parts, struct parts_state *state)
{
	iarray_write(w, state->type);
	iarray_write(w, state->common.w);
	iarray_write(w, state->common.h);
	iarray_write_point(w, &state->common.origin_offset);
	iarray_write_rectangle(w, &state->common.hitbox);
	iarray_write_rectangle(w, &state->common.surface_area);
	switch (state->type) {
	case PARTS_UNINITIALIZED:
	case PARTS_MOVIE:
	case PARTS_3DLAYER:
		break;
	case PARTS_RECT_DETECTION:
		iarray_write(w, state->rect.custom);
		for (int i = 0; i < 4; i++) iarray_write_point(w, &state->rect.points[i]);
		break;
	case PARTS_CG:
		save_parts_cg(w, &state->cg);
		break;
	case PARTS_TEXT:
		save_parts_text(w, &state->text);
		break;
	case PARTS_ANIMATION:
		save_parts_animation(w, &state->anim);
		break;
	case PARTS_NUMERAL:
		save_parts_numeral(w, &state->num);
		break;
	case PARTS_HGAUGE:
	case PARTS_VGAUGE:
		save_parts_gauge(w, &state->gauge);
		break;
	case PARTS_CONSTRUCTION_PROCESS:
		save_parts_construction_process(w, parts, state, &state->cproc);
		break;
	case PARTS_FLASH:
		save_parts_flash(w, &state->flash);
		break;
	case PARTS_FLAT:
		save_parts_flat(w, &state->flat);
		break;
	case PARTS_LAYOUT_BOX:
		save_parts_layout_box(w, &state->layout_box);
		break;
	}
}

static void load_parts_state(struct iarray_reader *r, struct parts *parts,
		struct parts_state *state, int version)
{
	parts_state_reset(state, iarray_read(r));
	state->common.w = iarray_read(r);
	state->common.h = iarray_read(r);
	iarray_read_point(r, &state->common.origin_offset);
	iarray_read_rectangle(r, &state->common.hitbox);
	iarray_read_rectangle(r, &state->common.surface_area);
	switch (state->type) {
	case PARTS_UNINITIALIZED:
	case PARTS_MOVIE:
	case PARTS_3DLAYER:
		break;
	case PARTS_RECT_DETECTION:
		if (version >= 13) {
			state->rect.custom = iarray_read(r);
			for (int i = 0; i < 4; i++) iarray_read_point(r, &state->rect.points[i]);
		}
		break;
	case PARTS_CG:
		load_parts_cg(r, parts, &state->cg);
		break;
	case PARTS_TEXT:
		load_parts_text(r, parts, &state->text);
		break;
	case PARTS_ANIMATION:
		load_parts_animation(r, parts, &state->anim);
		break;
	case PARTS_NUMERAL:
		load_parts_numeral(r, parts, &state->num, version);
		break;
	case PARTS_HGAUGE:
		load_parts_gauge(r, parts, &state->gauge, false, version);
		break;
	case PARTS_VGAUGE:
		load_parts_gauge(r, parts, &state->gauge, true, version);
		break;
	case PARTS_CONSTRUCTION_PROCESS:
		load_parts_construction_process(r, parts, &state->cproc, version);
		break;
	case PARTS_FLASH:
		load_parts_flash(r, parts, &state->flash);
		break;
	case PARTS_FLAT:
		load_parts_flat(r, parts, &state->flat, version);
		break;
	case PARTS_LAYOUT_BOX:
		load_parts_layout_box(r, &state->layout_box, version);
		break;
	}
}

static void save_parts_motion(struct iarray_writer *w, struct parts_motion *motion)
{
	iarray_write(w, motion->type);
	switch (motion->type) {
	case PARTS_MOTION_POS:
		iarray_write(w, motion->begin.x);
		iarray_write(w, motion->begin.y);
		iarray_write(w, motion->end.x);
		iarray_write(w, motion->end.y);
		break;
	case PARTS_MOTION_ALPHA:
	case PARTS_MOTION_CG:
	case PARTS_MOTION_NUMERAL_NUMBER:
		iarray_write(w, motion->begin.i);
		iarray_write(w, motion->end.i);
		break;
	case PARTS_MOTION_HGAUGE_RATE:
	case PARTS_MOTION_VGAUGE_RATE:
	case PARTS_MOTION_MAG_X:
	case PARTS_MOTION_MAG_Y:
	case PARTS_MOTION_ROTATE_X:
	case PARTS_MOTION_ROTATE_Y:
	case PARTS_MOTION_ROTATE_Z:
		iarray_write_float(w, motion->begin.f);
		iarray_write_float(w, motion->end.f);
		break;
	case PARTS_MOTION_VIBRATION_SIZE:
		iarray_write(w, motion->begin.x);
		iarray_write(w, motion->begin.y);
		break;
	}
	iarray_write(w, motion->begin_time);
	iarray_write(w, motion->end_time);
}

static struct parts_motion *load_parts_motion(struct iarray_reader *r)
{
	struct parts_motion *motion = xcalloc(1, sizeof(struct parts_motion));
	motion->type = iarray_read(r);
	switch (motion->type) {
	case PARTS_MOTION_POS:
		motion->begin.x = iarray_read(r);
		motion->begin.y = iarray_read(r);
		motion->end.x = iarray_read(r);
		motion->end.y = iarray_read(r);
		break;
	case PARTS_MOTION_ALPHA:
	case PARTS_MOTION_CG:
	case PARTS_MOTION_NUMERAL_NUMBER:
		motion->begin.i = iarray_read(r);
		motion->end.i = iarray_read(r);
		break;
	case PARTS_MOTION_HGAUGE_RATE:
	case PARTS_MOTION_VGAUGE_RATE:
	case PARTS_MOTION_MAG_X:
	case PARTS_MOTION_MAG_Y:
	case PARTS_MOTION_ROTATE_X:
	case PARTS_MOTION_ROTATE_Y:
	case PARTS_MOTION_ROTATE_Z:
		motion->begin.f = iarray_read_float(r);
		motion->end.f = iarray_read_float(r);
		break;
	case PARTS_MOTION_VIBRATION_SIZE:
		motion->begin.x = iarray_read(r);
		motion->begin.y = iarray_read(r);
		break;
	}
	motion->begin_time = iarray_read(r);
	motion->end_time = iarray_read(r);
	return motion;
}

static void save_message_window(struct iarray_writer *w, struct parts *parts, struct parts_message_window *m)
{
	iarray_write(w, m != NULL);
	if (!m) return;
	iarray_write(w, m->active);
	iarray_write(w, m->fixed);
	iarray_write(w, m->key_show);
	for (int i = 0; i < 3; i++) iarray_write(w, m->inactive_color[i]);
	iarray_write(w, m->text_origin);
	iarray_write(w, m->text_speed);
	iarray_write(w, m->flat_wait_frame);
	iarray_write(w, m->text_area.x);
	iarray_write(w, m->text_area.y);
	iarray_write(w, m->text_area.w);
	iarray_write(w, m->text_area.h);
	iarray_write_string(w, m->cg_name);
	iarray_write_string(w, m->flat_name);
	iarray_write_string(w, m->text);
	iarray_write_string(w, m->function);
	iarray_write_string(w, m->key_cg_name);
	iarray_write_string(w, m->key_flat_name);
	iarray_write(w, m->message);
	iarray_write(w, m->version);
	iarray_write(w, m->step);
	iarray_write(w, m->key_x);
	iarray_write(w, m->key_y);
	iarray_write(w, m->key_z);
	iarray_write(w, m->key_start);
	iarray_write(w, m->key_count);
	iarray_write(w, m->key_time);
	iarray_write(w, m->elapsed);
	iarray_write_float(w, m->speed_rate);
	iarray_write_text_style(w, &m->label.text.ts);
	iarray_write(w, m->label.text.line_space);
	save_parts_state(w, parts, &m->key_wait);
}

static void load_message_window(struct iarray_reader *r, struct parts *parts, int version)
{
	if (!iarray_read(r)) return;
	// The background was loaded with the ordinary states; do not reset it.
	struct parts_message_window *m = parts->message_box = xcalloc(1, sizeof(*m));
	m->active = iarray_read(r);
	m->fixed = iarray_read(r);
	m->key_show = iarray_read(r);
	for (int i = 0; i < 3; i++) m->inactive_color[i] = iarray_read(r);
	m->text_origin = iarray_read(r);
	m->text_speed = iarray_read(r);
	m->flat_wait_frame = iarray_read(r);
	m->text_area.x = iarray_read(r);
	m->text_area.y = iarray_read(r);
	m->text_area.w = iarray_read(r);
	m->text_area.h = iarray_read(r);
	m->cg_name = iarray_read_string(r);
	m->flat_name = iarray_read_string(r);
	m->text = iarray_read_string(r);
	m->function = iarray_read_string(r);
	m->key_cg_name = iarray_read_string(r);
	m->key_flat_name = iarray_read_string(r);
	m->message = iarray_read(r);
	m->version = iarray_read(r);
	m->step = iarray_read(r);
	m->key_x = iarray_read(r);
	m->key_y = iarray_read(r);
	m->key_z = iarray_read(r);
	m->key_start = iarray_read(r);
	m->key_count = iarray_read(r);
	m->key_time = iarray_read(r);
	m->elapsed = iarray_read(r);
	m->speed_rate = iarray_read_float(r);
	parts_state_reset(&m->label, PARTS_TEXT);
	iarray_read_text_style(r, &m->label.text.ts);
	m->label.text.line_space = iarray_read(r);
	parts_message_window_rebuild_text(parts);
	load_parts_state(r, parts, &m->key_wait, version);
	// Sys43 resumes a loaded message fully visible, without its wait marker.
	m->fixed = true;
	m->key_show = false;
}

static void save_parts(struct iarray_writer *w, struct parts *parts)
{
	iarray_write(w, parts->no);
	iarray_write(w, parts->state);
	for (int i = 0; i < PARTS_NR_STATES; i++) {
		save_parts_state(w, parts, &parts->states[i]);
	}

	save_parts_params(w, &parts->local);
	save_parts_params(w, &parts->global);
	iarray_write(w, parts->parent ? parts->parent->no : -1);
	iarray_write(w, parts->delegate_index);
	iarray_write(w, parts->sprite_deform);
	iarray_write(w, parts->clickable);
	iarray_write(w, parts->wheelable);
	iarray_write(w, parts->enable_input);
	iarray_write(w, parts->on_cursor_sound);
	iarray_write(w, parts->on_click_sound);
	for (int i = 0; i < PARTS_NR_STATES; i++)
		iarray_write_string(w, parts->sound_names[i]);
	iarray_write(w, parts->origin_mode);
	iarray_write(w, parts->position_is_ratio);
	iarray_write(w, parts->want_save);
	// XXX: no need to save pending parent since we called UpdateComponent first
	iarray_write(w, parts->linked_to);
	iarray_write(w, parts->linked_from);
	iarray_write(w, parts->draw_filter);
	iarray_write(w, parts->message_window);
	iarray_write(w, parts->alpha_clipper_parts_no);
	iarray_write(w, parts->controller_no);
	iarray_write(w, parts->pass_cursor);
	iarray_write(w, parts->lock_input_state);
	iarray_write(w, parts->margin_top);
	iarray_write(w, parts->margin_bottom);
	iarray_write(w, parts->margin_left);
	iarray_write(w, parts->margin_right);
	iarray_write(w, parts->draggable);
	iarray_write(w, parts->component_type);
	iarray_write_string(w, parts->user_component_name);
	int user_values = 0;
	for (struct parts_user_value *v = parts->user_values; v; v = v->next) user_values++;
	iarray_write(w, user_values);
	for (struct parts_user_value *v = parts->user_values; v; v = v->next) {
		iarray_write_string(w, v->key);
		iarray_write_string(w, v->value);
	}
	iarray_write(w, parts->widget != NULL);
	if (parts->widget) {
		struct parts_widget *widget = parts->widget;
		iarray_write(w, widget->enabled);
		iarray_write(w, widget->width);
		iarray_write(w, widget->height);
		for (int i = 0; i < 3; i++) iarray_write(w, widget->color[i]);
		iarray_write(w, widget->text_origin);
		iarray_write_string(w, widget->cg_name);
		iarray_write_string(w, widget->flat_name);
		iarray_write_string(w, widget->text);
		iarray_write_text_style(w, &widget->label.text.ts);
		iarray_write(w, widget->label.text.line_space);
		iarray_write(w, widget->checked);
		iarray_write(w, widget->button_mode);
		for (int i = 0; i < 2; i++) {
			iarray_write_string(w, widget->override_cg[i]);
			iarray_write_string(w, widget->override_flat[i]);
		}
	}

	iarray_write(w, parts->textures_suspended);
	save_message_window(w, parts, parts->message_box);
	iarray_write(w, parts->panel != NULL);
	if (parts->panel) {
		iarray_write(w, parts->panel->width);
		iarray_write(w, parts->panel->height);
		for (int i = 0; i < 4; i++) iarray_write(w, parts->panel->color[i]);
		for (int i = 0; i < 4; i++) iarray_write(w, parts->panel->gradient[i]);
	}
	unsigned motion_count_pos = iarray_writer_pos(w);
	// v23: scrollbars and links belong to the saved scene, including BackScene.
	for (int i = 0; i < 3; i++) iarray_write(w, parts->scroll_links[i]);
	iarray_write(w, parts->scrollbar != NULL);
	if (parts->scrollbar) {
		struct parts_scrollbar *s = parts->scrollbar;
		iarray_write(w, s->horizontal);
		int values[] = {s->width, s->height, s->before, s->after, s->total, s->view,
			s->pos, s->step, s->total_link, s->view_link};
		for (int i = 0; i < 10; i++) iarray_write(w, values[i]);
		iarray_write_float(w, s->rate);
		for (int resource = 0; resource < 2; resource++)
			for (int piece = 0; piece < 5; piece++) iarray_write_string(w, s->names[resource][piece]);
	}
	iarray_write(w, parts->checkbox_show_link);
	iarray_write(w, parts->nr_radio_children);
	for (int i = 0; i < parts->nr_radio_children; i++)
		iarray_write(w, parts->radio_children[i]);
	iarray_write_string_or_null(w, parts->text_box_text);
	iarray_write(w, parts->enable_speed_up);
	iarray_write(w, parts->swipe_type);
	motion_count_pos = iarray_writer_pos(w);
	iarray_write(w, 0); // size of motion list

	unsigned motion_count = 0;
	struct parts_motion *motion;
	TAILQ_FOREACH(motion, &parts->motion, entry) {
		save_parts_motion(w, motion);
		motion_count++;
	}

	iarray_write_at(w, motion_count_pos, motion_count);
}

static void load_parts(struct iarray_reader *r, int version)
{
	int no = iarray_read(r);
	struct parts *parts = parts_get(no);
	// Allocation normally attaches new components to the active controller.
	// Saved components have their own parent IDs, including detached roots.
	if (ain->version >= 12 && parts->parent) {
		TAILQ_REMOVE(&parts->parent->children, parts, child_list_entry);
		parts->parent = NULL;
	}
	parts->state = iarray_read(r);
	for (int i = 0; i < PARTS_NR_STATES; i++) {
		load_parts_state(r, parts, &parts->states[i], version);
	}

	load_parts_params(r, &parts->local, version);
	load_parts_params(r, &parts->global, version);
	parts->pending_parent = iarray_read(r);
	parts->delegate_index = iarray_read(r);
	parts->sprite_deform = iarray_read(r);
	parts->clickable = iarray_read(r);
	parts->wheelable = version >= 11 ? iarray_read(r) : false;
	parts->enable_input = version >= 7 ? iarray_read(r) : true;
	parts->on_cursor_sound = iarray_read(r);
	parts->on_click_sound = iarray_read(r);
	if (version >= 8) {
		for (int i = 0; i < PARTS_NR_STATES; i++) {
			free_string(parts->sound_names[i]);
			parts->sound_names[i] = iarray_read_string(r);
		}
	}
	parts->origin_mode = iarray_read(r);
	parts->position_is_ratio = version >= 19 ? iarray_read(r) : false;
	parts->want_save = version >= 20 ? iarray_read(r) : true;
	parts->linked_to = iarray_read(r);
	parts->linked_from = iarray_read(r);
	parts->draw_filter = iarray_read(r);
	if (version > 0)
		parts->message_window = iarray_read(r);
	if (version > 2)
		parts->alpha_clipper_parts_no = iarray_read(r);
	if (version > 3 || (version == 3 && parts_multi_controller)) {
		parts->controller_no = iarray_read(r);
		parts->pass_cursor = iarray_read(r);
		parts->lock_input_state = iarray_read(r);
		parts->margin_top = iarray_read(r);
		parts->margin_bottom = iarray_read(r);
		parts->margin_left = iarray_read(r);
		parts->margin_right = iarray_read(r);
		parts->draggable = iarray_read(r);
	}

	if (version >= 9) {
		parts->component_type = iarray_read(r);
		free_string(parts->user_component_name);
		parts->user_component_name = iarray_read_string(r);
		int count = iarray_read(r);
		for (int i = 0; i < count; i++) {
			struct parts_user_value *v = xcalloc(1, sizeof(*v));
			v->key = iarray_read_string(r);
			v->value = iarray_read_string(r);
			v->next = parts->user_values;
			parts->user_values = v;
		}
	}
	if (version >= 12 && iarray_read(r)) {
		struct parts_widget *widget = parts->widget = xcalloc(1, sizeof(*widget));
		widget->enabled = iarray_read(r);
		widget->width = iarray_read(r);
		widget->height = iarray_read(r);
		for (int i = 0; i < 3; i++) widget->color[i] = iarray_read(r);
		widget->text_origin = iarray_read(r);
		widget->cg_name = iarray_read_string(r);
		widget->flat_name = iarray_read_string(r);
		widget->text = iarray_read_string(r);
		parts_state_reset(&widget->label, PARTS_TEXT);
		iarray_read_text_style(r, &widget->label.text.ts);
		widget->label.text.line_space = iarray_read(r);
		if (version >= 22) {
			widget->checked = iarray_read(r);
			widget->button_mode = iarray_read(r);
		}
		for (int i = 0; i < 2; i++) {
			widget->override_cg[i] = version >= 22 ? iarray_read_string(r) : string_ref(&EMPTY_STRING);
			widget->override_flat[i] = version >= 22 ? iarray_read_string(r) : string_ref(&EMPTY_STRING);
		}
		parts_widget_rebuild(parts);
	}
	bool textures_suspended = version >= 17 ? iarray_read(r) : false;
	if (version >= 18) load_message_window(r, parts, version);
	if (version >= 21 && iarray_read(r)) {
		struct parts_panel *p = parts->panel = xcalloc(1, sizeof(*p));
		p->width = iarray_read(r);
		p->height = iarray_read(r);
		for (int i = 0; i < 4; i++) p->color[i] = iarray_read(r);
		for (int i = 0; i < 4; i++) p->gradient[i] = iarray_read(r);
		parts_panel_rebuild(parts);
	}
	if (version >= 23) {
		for (int i = 0; i < 3; i++) parts->scroll_links[i] = iarray_read(r);
		if (iarray_read(r)) {
			struct parts_scrollbar *s = parts->scrollbar = xcalloc(1, sizeof(*s));
			s->horizontal = iarray_read(r);
			int *values[] = {&s->width, &s->height, &s->before, &s->after, &s->total, &s->view,
				&s->pos, &s->step, &s->total_link, &s->view_link};
			for (int i = 0; i < 10; i++) *values[i] = iarray_read(r);
			s->rate = iarray_read_float(r);
			s->hovered = s->pressed = -1;
			for (int resource = 0; resource < 2; resource++)
				for (int piece = 0; piece < 5; piece++) s->names[resource][piece] = iarray_read_string(r);
			parts_scrollbar_rebuild(parts);
		}
	}
	if (version >= 25) {
		parts->checkbox_show_link = iarray_read(r);
		free(parts->radio_children);
		parts->nr_radio_children = iarray_read(r);
		parts->radio_children = parts->nr_radio_children > 0
			? xmalloc(parts->nr_radio_children * sizeof(int)) : NULL;
		for (int i = 0; i < parts->nr_radio_children; i++)
			parts->radio_children[i] = iarray_read(r);
		if (parts->text_box_text)
			free_string(parts->text_box_text);
		parts->text_box_text = iarray_read_string_or_null(r);
		if (!parts->text_box_text)
			parts->text_box_text = string_ref(&EMPTY_STRING);
		parts->enable_speed_up = iarray_read(r);
	}
	if (version >= 27) {
		parts->swipe_type = iarray_read(r);
	}
	int motion_count = iarray_read(r);
	for (int i = 0; i < motion_count; i++) {
		struct parts_motion *motion = load_parts_motion(r);
		parts_add_motion(parts, motion);
	}

	parts_list_resort(parts);
	parts_component_dirty(parts);
	parts_recalculate_hitbox(parts);
	// Panels are regenerated when loaded, including a save made while their
	// previous texture was suspended (Sys43 behavior).
	if (textures_suspended && !parts->panel) PE_SuspendTexture(no);
}

static void save_numeral_fonts(struct iarray_writer *w)
{
	iarray_write(w, parts_nr_numeral_fonts);
	for (int i = 0; i < parts_nr_numeral_fonts; i++) {
		struct parts_numeral_font *font = &parts_numeral_fonts[i];
		iarray_write(w, font->type);
		iarray_write(w, font->cg_no);
		for (int i = 0; i < 12; i++) {
			iarray_write(w, font->width[i]);
		}
	}
}

static void load_numeral_fonts(struct iarray_reader *r)
{
	parts_nr_numeral_fonts = iarray_read(r);
	parts_numeral_fonts = xcalloc(parts_nr_numeral_fonts, sizeof(struct parts_numeral_font));
	for (int i = 0; i < parts_nr_numeral_fonts; i++) {
		struct parts_numeral_font *font = &parts_numeral_fonts[i];
		font->type = iarray_read(r);
		font->cg_no = iarray_read(r);
		for (int i = 0; i < 12; i++) {
			font->width[i] = iarray_read(r);
		}
		parts_numeral_font_init(font);
	}
}

static bool parts_wants_backscene(struct parts *parts)
{
	for (; parts; parts = parts->parent)
		if (!parts->want_save) return false;
	return true;
}

static bool parts_engine_save(struct page **buffer, bool save_hidden, bool backscene)
{
	// get parts into clean state first
	PE_UpdateComponent(0);

	struct iarray_writer w;
	iarray_init_writer(&w, "XPE");
	iarray_write(&w, CURRENT_SAVE_VERSION);
	if (CURRENT_SAVE_VERSION > 1)
		save_numeral_fonts(&w);

	iarray_write(&w, ctrl_stack.active);
	iarray_write(&w, ctrl_stack.nr_controllers);
	iarray_write(&w, ctrl_stack.overlay_id);
	for (int i = 0; i < ctrl_stack.nr_controllers; i++) iarray_write(&w, ctrl_stack.ids[i]);
	struct string *click_miss_sound = PE_GetClickMissSoundName();
	iarray_write_string(&w, click_miss_sound);
	free_string(click_miss_sound);

	unsigned count_pos = iarray_writer_pos(&w);
	iarray_write(&w, 0); // size of parts list

	unsigned count = 0;
	struct parts *parts;
	PARTS_LIST_FOREACH(parts) {
		if (!save_hidden && !parts->global.show)
			continue;
		if (backscene && !parts_wants_backscene(parts))
			continue;
		if (ain->version < 12 && !parts->want_save)
			continue;
		save_parts(&w, parts);
		count++;
	}

	iarray_write_at(&w, count_pos, count);
	iarray_write(&w, !backscene);
	if (!backscene) {
		parts_activities_save(&w);
		parts_input_save_state(&w);
	}

	if (*buffer) {
		delete_page_vars(*buffer);
		free_page(*buffer);
	}
	*buffer = iarray_to_page(&w);
	iarray_free_writer(&w);
	return true;

}

bool PE_SaveWithoutHideParts(struct page **buffer)
{
	return parts_engine_save(buffer, false, false);
}

bool PE_Save(struct page **buffer)
{
	return parts_engine_save(buffer, true, false);
}

bool PE_SaveBackScene(struct page **buffer)
{
	return parts_engine_save(buffer, true, true);
}

bool PE_Load(struct page **buffer)
{
	if (!(*buffer)) {
		WARNING("savedata array is empty");
		return false;
	}

	struct iarray_reader r;
	if (!iarray_init_reader(&r, *buffer, "XPE")) {
		WARNING("unrecognized savedata magic");
		return false;
	}
	int version = iarray_read(&r);
	if (version > CURRENT_SAVE_VERSION) {
		WARNING("unrecognized savedata version");
		return false;
	}

	if (!parts_current->background)
		PE_ReleaseMessage();
	parts_release_all();

	if (version > 1)
		load_numeral_fonts(&r);

	if (version > 3 || (version == 3 && parts_multi_controller)) {
		ctrl_stack.active = iarray_read(&r);
		ctrl_stack.nr_controllers = iarray_read(&r);
		if (ctrl_stack.nr_controllers < 0 || ctrl_stack.nr_controllers > PARTS_CONTROLLER_STACK_MAX)
			VM_ERROR("Invalid controller count in parts save");
		if (version >= 10) {
			ctrl_stack.overlay_id = iarray_read(&r);
			for (int i = 0; i < ctrl_stack.nr_controllers; i++) ctrl_stack.ids[i] = iarray_read(&r);
		}
	}

	if (version >= 12) {
		struct string *click_miss_sound = iarray_read_string(&r);
		if (!parts_current->background)
			PE_SetClickMissSoundName(click_miss_sound);
		free_string(click_miss_sound);
	}
	int nr_parts = iarray_read(&r);
	if (nr_parts < 0) {
		WARNING("invalid parts count");
		return false;
	}

	for (int i = 0; i < nr_parts; i++) {
		load_parts(&r, version);
	}
	if (ain->version >= 12) {
		// Resolve the complete hierarchy before recursive updates can remove
		// children from the dirty list and discard their pending parent links.
		struct parts *parts;
		PARTS_LIST_FOREACH(parts) {
			struct parts *parent = parts->pending_parent >= 0
				? parts_try_get(parts->pending_parent) : NULL;
			for (struct parts *p = parent; p; p = p->parent)
				if (p == parts) VM_ERROR("Cyclic parts parent relationship in save");
			parts->parent = parent;
			if (parent) TAILQ_INSERT_TAIL(&parent->children, parts, child_list_entry);
			parts->pending_parent = -1;
		}
	}

	if (!parts_current->background) {
		if (version >= 27) {
			if (iarray_read(&r)) {
				parts_activities_load(&r);
				parts_input_load_state(&r);
			}
		} else {
			parts_activities_reconcile_legacy();
			parts_input_load_legacy_state();
		}
	}

	parts_engine_clean();
	return true;
}

bool PE_LoadBackScene(struct page **buffer)
{
	// Build the replacement before discarding the previous snapshot. Live
	// controllers, activity handles, focus, messages and sounds stay untouched.
	struct parts_context *context = parts_context_new_background();
	struct parts_context *previous = parts_context_select(context);
	bool ok = PE_Load(buffer);
	if (ok) PE_UpdateComponent(0);
	parts_context_select(previous);
	if (!ok) {
		parts_context_free(context);
		return false;
	}
	PE_ClearBackScene();
	back_scene = context;
	scene_register_sprite(&context->sprite);
	scene_dirty();
	return true;
}

void PE_ClearBackScene(void)
{
	parts_context_free(back_scene);
	back_scene = NULL;
}

struct parts_context *parts_get_back_scene(void)
{
	return back_scene;
}

void PE_SetShowForBackScene(int no, bool show)
{
	if (!back_scene) return;
	struct parts_context *previous = parts_context_select(back_scene);
	if (parts_try_get(no)) {
		PE_SetShow(no, show);
		PE_UpdateComponent(0);
		scene_dirty();
	}
	parts_context_select(previous);
}

void PE_SetFontColorForBackScene(int no, int r, int g, int b, possibly_unused int state)
{
	if (!back_scene) return;
	struct parts_context *previous = parts_context_select(back_scene);
	struct parts *parts = parts_try_get(no);
	// Sys43 applies this API only to message windows, always using state 1.
	if (parts && parts->message_box) {
		parts->message_box->label.text.ts.color = (SDL_Color){r, g, b, 255};
		parts_message_window_rebuild_text(parts);
		scene_dirty();
	}
	parts_context_select(previous);
}
