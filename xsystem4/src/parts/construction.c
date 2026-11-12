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

#include <assert.h>
#include "system4.h"
#include "system4/ain.h"
#include "system4/cg.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

#include "xsystem4.h"
#include "gfx/gl.h"
#include "vm.h"
#include "asset_manager.h"
#include "parts.h"
#include "parts_internal.h"

static struct parts_construction_process *get_cproc(int parts_no, int state)
{
	return parts_get_construction_process(parts_get(parts_no), state);
}

void parts_cp_op_free(struct parts_cp_op *op)
{
	switch (op->type) {
	case PARTS_CP_CREATE:
	case PARTS_CP_CREATE_PIXEL_ONLY:
	case PARTS_CP_FILL:
	case PARTS_CP_FILL_ALPHA_COLOR:
	case PARTS_CP_FILL_AMAP:
	case PARTS_CP_FILL_WITH_ALPHA:
	case PARTS_CP_FILL_GRADATION_HORIZON:
	case PARTS_CP_MUL_AMAP_GRADATION_HORIZON:
	case PARTS_CP_MUL_AMAP_GRADATION_VERTICAL:
	case PARTS_CP_DRAW_RECT:
	case PARTS_CP_GRAY_FILTER:
	case PARTS_CP_ADD_FILTER:
	case PARTS_CP_MUL_FILTER:
	case PARTS_CP_HBLUR_FILTER:
	case PARTS_CP_VBLUR_FILTER:
	case PARTS_CP_DRAW_LINE:
		break;
	case PARTS_CP_CG:
	case PARTS_CP_BLEND_CG:
		free_string(op->cg.name);
		break;
	case PARTS_CP_DRAW_CUT_CG:
	case PARTS_CP_COPY_CUT_CG:
	case PARTS_CP_ALPHA_BLEND_CUT_CG:
	case PARTS_CP_ONLY_ALPHA_CUT_CG:
		free_string(op->cut_cg.cg_name);
		break;
	case PARTS_CP_DRAW_TEXT:
	case PARTS_CP_COPY_TEXT:
	case PARTS_CP_ALPHA_BLEND_TEXT:
	case PARTS_CP_ONLY_ALPHA_TEXT:
		free_string(op->text.text);
		break;
	}
	free(op);
}

void parts_add_cp_op(struct parts_construction_process *cproc, struct parts_cp_op *op)
{
	TAILQ_INSERT_TAIL(&cproc->ops, op, entry);
}

bool PE_ClearPartsConstructionProcess(int parts_no, int state);

bool PE_AddCreateToPartsConstructionProcess(int parts_no, int w, int h, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_CREATE;
	op->create.w = w;
	op->create.h = h;
	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddCreatePixelOnlyToPartsConstructionProcess(int parts_no, int w, int h, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_CREATE_PIXEL_ONLY;
	op->create.w = w;
	op->create.h = h;
	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddCreateCGToProcess(int parts_no, struct string *cg_name, int state)
{
	if (!parts_state_valid(--state))
		return false;

	if ((!ain || ain->version < 12) && !asset_exists_by_name(ASSET_CG, cg_name->text, NULL)) {
		WARNING("Invalid CG name: %s", display_sjis0(cg_name->text));
		return false;
	}

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_CG;
	op->cg.name = string_ref(cg_name);
	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddFillToPartsConstructionProcess(int parts_no, int x, int y, int w, int h, int r, int g, int b, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_FILL;
	op->fill = (struct parts_cp_fill) {
		.x = x, .y = y, .w = w, .h = h,
		.r = r, .g = g, .b = b, .a = 255
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddFillAlphaColorToPartsConstructionProcess(int parts_no, int x, int y, int w, int h, int r, int g, int b, int a, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_FILL_ALPHA_COLOR;
	op->fill = (struct parts_cp_fill) {
		.x = x, .y = y, .w = w, .h = h,
		.r = r, .g = g, .b = b, .a = a
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddFillAMapToPartsConstructionProcess(int parts_no, int x, int y, int w, int h, int a, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_FILL_AMAP;
	op->fill = (struct parts_cp_fill) {
		.x = x, .y = y, .w = w, .h = h,
		.r = 0, .g = 0, .b = 0, .a = a
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddFillWithAlphaToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int r, int g, int b, int a, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_FILL_WITH_ALPHA;
	op->fill = (struct parts_cp_fill) {
		.x = x, .y = y, .w = w, .h = h,
		.r = r, .g = g, .b = b, .a = a
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddFillGradationHorizonToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int top_r, int top_g, int top_b, int bot_r, int bot_g, int bot_b, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_FILL_GRADATION_HORIZON;
	op->fill = (struct parts_cp_fill) {
		.x = x, .y = y, .w = w, .h = h,
		.r = top_r, .g = top_g, .b = top_b, .a = 255,
		.r2 = bot_r, .g2 = bot_g, .b2 = bot_b, .a2 = 255
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddMulAMapGradationHorizonToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int top_a, int bot_a, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_MUL_AMAP_GRADATION_HORIZON;
	op->fill = (struct parts_cp_fill) {
		.x = x, .y = y, .w = w, .h = h,
		.a = top_a, .a2 = bot_a
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddMulAMapGradationVerticalToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int left_a, int right_a, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_MUL_AMAP_GRADATION_VERTICAL;
	op->fill = (struct parts_cp_fill) {
		.x = x, .y = y, .w = w, .h = h,
		.a = left_a, .a2 = right_a
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddCutCGAlphaBlendToPartsConstructionProcess(int parts_no, struct string *cg_name,
		int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, int interp_type, bool scale, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_ALPHA_BLEND_CUT_CG;
	op->cut_cg = (struct parts_cp_cut_cg) {
		.cg_name = string_ref(cg_name),
		.dx = dx, .dy = dy, .dw = dw, .dh = dh,
		.sx = sx, .sy = sy, .sw = sw, .sh = sh,
		.interp_type = interp_type,
		.scale = scale
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddCutCGOnlyAlphaToPartsConstructionProcess(int parts_no, struct string *cg_name,
		int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, int interp_type, bool scale, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_ONLY_ALPHA_CUT_CG;
	op->cut_cg = (struct parts_cp_cut_cg) {
		.cg_name = string_ref(cg_name),
		.dx = dx, .dy = dy, .dw = dw, .dh = dh,
		.sx = sx, .sy = sy, .sw = sw, .sh = sh,
		.interp_type = interp_type,
		.scale = scale
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddCGBlendToPartsConstructionProcess(int parts_no, struct string *cg_name,
		int dx, int dy, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_BLEND_CG;
	op->cg = (struct parts_cp_cg) {
		.name = string_ref(cg_name),
		.x = dx, .y = dy
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddDrawRectToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int r, int g, int b, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_DRAW_RECT;
	op->fill = (struct parts_cp_fill) {
		.x = x, .y = y, .w = w, .h = h,
		.r = r, .g = g, .b = b, .a = 255
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddDrawLineToPartsConstructionProcess(int parts_no, int x0, int y0, int x1, int y1,
		int r, int g, int b, int a, int state)
{
	if (!parts_state_valid(--state))
		return false;
	struct parts_cp_op *op = xcalloc(1, sizeof(*op));
	op->type = PARTS_CP_DRAW_LINE;
	op->line = (struct parts_cp_line) { x0, y0, x1, y1, r, g, b, a };
	parts_add_cp_op(get_cproc(parts_no, state), op);
	return true;
}

bool PE_AddDrawCutCGToPartsConstructionProcess(int parts_no, struct string *cg_name,
		int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, int interp_type, int state)
{
	if (!parts_state_valid(--state))
		return false;

	if ((!ain || ain->version < 12) && !asset_exists_by_name(ASSET_CG, cg_name->text, NULL)) {
		WARNING("Invalid CG name: %s", display_sjis0(cg_name->text));
		return false;
	}

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_DRAW_CUT_CG;
	op->cut_cg = (struct parts_cp_cut_cg) {
		.cg_name = string_ref(cg_name),
		.dx = dx, .dy = dy, .dw = dw, .dh = dh,
		.sx = sx, .sy = sy, .sw = sw, .sh = sh,
		.interp_type = interp_type,
		.scale = (sw != dw || sh != dh)
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddCopyCutCGToPartsConstructionProcess(int parts_no, struct string *cg_name,
		int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh, int interp_type, int state)
{
	if (!parts_state_valid(--state))
		return false;

	if ((!ain || ain->version < 12) && !asset_exists_by_name(ASSET_CG, cg_name->text, NULL)) {
		WARNING("Invalid CG name: %s", display_sjis0(cg_name->text));
		return false;
	}

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_COPY_CUT_CG;
	op->cut_cg = (struct parts_cp_cut_cg) {
		.cg_name = string_ref(cg_name),
		.dx = dx, .dy = dy, .dw = dw, .dh = dh,
		.sx = sx, .sy = sy, .sw = sw, .sh = sh,
		.interp_type = interp_type,
		.scale = (sw != dw || sh != dh)
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddGrayFilterToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		bool full_size, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_GRAY_FILTER;
	op->filter = (struct parts_cp_filter) {
		.x = x, .y = y, .w = w, .h = h,
		.full_size = full_size
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddAddFilterToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int r, int g, int b, bool full_size, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_ADD_FILTER;
	op->filter = (struct parts_cp_filter) {
		.x = x, .y = y, .w = w, .h = h,
		.r = r, .g = g, .b = b,
		.full_size = full_size
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddMulFilterToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int r, int g, int b, bool full_size, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_MUL_FILTER;
	op->filter = (struct parts_cp_filter) {
		.x = x, .y = y, .w = w, .h = h,
		.r = r, .g = g, .b = b,
		.full_size = full_size
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddHBlurFilterToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int blur, bool full_size, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_HBLUR_FILTER;
	op->filter = (struct parts_cp_filter) {
		.x = x, .y = y, .w = w, .h = h,
		.blur = blur,
		.full_size = full_size
	};

	parts_add_cp_op(cproc, op);
	return true;
}

bool PE_AddVBlurFilterToPartsConstructionProcess(int parts_no, int x, int y, int w, int h,
		int blur, bool full_size, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_construction_process *cproc = get_cproc(parts_no, state);
	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = PARTS_CP_VBLUR_FILTER;
	op->filter = (struct parts_cp_filter) {
		.x = x, .y = y, .w = w, .h = h,
		.blur = blur,
		.full_size = full_size
	};

	parts_add_cp_op(cproc, op);
	return true;
}

static bool add_text_to_cproc(int parts_no, int x, int y, struct string *text,
		int type, int size, int r, int g, int b, float bold_weight,
		int edge_r, int edge_g, int edge_b, float edge_weight,
		int char_space, int line_space, int state, enum parts_cp_op_type op_type)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts_cp_op *op = xcalloc(1, sizeof(struct parts_cp_op));
	op->type = op_type;
	op->text = (struct parts_cp_text) {
		.text = string_dup(text),
		.x = x,
		.y = y,
		.line_space = line_space,
		.style = {
			.face = type,
			.size = size,
			.bold_width = bold_weight,
			.weight = 0,
			.edge_left = edge_weight,
			.edge_up = edge_weight,
			.edge_right = edge_weight,
			.edge_down = edge_weight,
			.color = { r, g, b, 255 },
			.edge_color = { edge_r, edge_g, edge_b, 255 },
			.scale_x = 1.0f,
			.space_scale_x = 1.0f,
			.font_spacing = char_space
		}
	};

	parts_add_cp_op(get_cproc(parts_no, state), op);
	return true;

}


bool PE_AddDrawTextToPartsConstructionProcess(int parts_no, int x, int y, struct string *text,
		int type, int size, int r, int g, int b, float bold_weight,
		int edge_r, int edge_g, int edge_b, float edge_weight,
		int char_space, int line_space, int state)
{
	return add_text_to_cproc(parts_no, x, y, text, type, size, r, g, b, bold_weight,
			edge_r, edge_g, edge_b, edge_weight, char_space, line_space, state,
			PARTS_CP_DRAW_TEXT);
}

bool PE_AddCopyTextToPartsConstructionProcess(int parts_no, int x, int y, struct string *text,
		int type, int size, int r, int g, int b, float bold_weight,
		int edge_r, int edge_g, int edge_b, float edge_weight,
		int char_space, int line_space, int state)
{
	return add_text_to_cproc(parts_no, x, y, text, type, size, r, g, b, bold_weight,
			edge_r, edge_g, edge_b, edge_weight, char_space, line_space, state,
			PARTS_CP_COPY_TEXT);
}

bool PE_AddAlphaBlendTextToPartsConstructionProcess(int parts_no, int x, int y, struct string *text,
		int type, int size, int r, int g, int b, float bold_weight,
		int edge_r, int edge_g, int edge_b, float edge_weight,
		int char_space, int line_space, int state)
{
	return add_text_to_cproc(parts_no, x, y, text, type, size, r, g, b, bold_weight,
			edge_r, edge_g, edge_b, edge_weight, char_space, line_space, state,
			PARTS_CP_ALPHA_BLEND_TEXT);
}

bool PE_AddOnlyAlphaTextToPartsConstructionProcess(int parts_no, int x, int y, struct string *text,
		int type, int size, int r, int g, int b, float bold_weight,
		int edge_r, int edge_g, int edge_b, float edge_weight,
		int char_space, int line_space, int state)
{
	return add_text_to_cproc(parts_no, x, y, text, type, size, r, g, b, bold_weight,
			edge_r, edge_g, edge_b, edge_weight, char_space, line_space, state,
			PARTS_CP_ONLY_ALPHA_TEXT);
}

static bool build_create(struct parts *parts, struct parts_construction_process *cproc,
		struct parts_cp_create *op)
{
	gfx_delete_texture(&cproc->common.texture);
	gfx_init_texture_rgba(&cproc->common.texture, op->w, op->h, (SDL_Color){0,0,0,255});
	if (!cproc->common.texture.handle)
		return false;
	parts_set_dims(parts, &cproc->common, op->w, op->h);
	return true;
}

static bool build_create_pixel_only(struct parts *parts, struct parts_construction_process *cproc,
		struct parts_cp_create *op)
{
	gfx_delete_texture(&cproc->common.texture);
	gfx_init_texture_rgb(&cproc->common.texture, op->w, op->h, (SDL_Color){0,0,0,255});
	if (!cproc->common.texture.handle)
		return false;
	parts_set_dims(parts, &cproc->common, op->w, op->h);
	return true;
}

static bool build_cg(struct parts *parts, struct parts_construction_process *cproc, struct parts_cp_cg *op)
{
	struct cg *cg = asset_cg_load_by_name(op->name->text, NULL);
	if (!cg)
		return false;
	gfx_delete_texture(&cproc->common.texture);
	gfx_init_texture_with_cg(&cproc->common.texture, cg);
	parts_set_dims(parts, &cproc->common, cg->metrics.w, cg->metrics.h);
	cg_free(cg);
	return true;
}

static bool build_fill(struct parts_construction_process *cproc, struct parts_cp_fill *op)
{
	if (!cproc->common.texture.handle)
		return false;
	gfx_fill(&cproc->common.texture, op->x, op->y, op->w, op->h, op->r, op->g, op->b);
	return true;
}

static bool build_fill_alpha_color(struct parts_construction_process *cproc, struct parts_cp_fill *op)
{
	if (!cproc->common.texture.handle)
		return false;
	gfx_fill_alpha_color(&cproc->common.texture, op->x, op->y, op->w, op->h, op->r, op->g, op->b, op->a);
	return true;
}

static bool build_fill_amap(struct parts_construction_process *cproc, struct parts_cp_fill *op)
{
	if (!cproc->common.texture.handle)
		return false;
	gfx_fill_amap(&cproc->common.texture, op->x, op->y, op->w, op->h, op->a);
	return true;
}

static bool build_fill_with_alpha(struct parts_construction_process *cproc, struct parts_cp_fill *op)
{
	if (!cproc->common.texture.handle)
		return false;
	gfx_fill_with_alpha(&cproc->common.texture, op->x, op->y, op->w, op->h, op->r, op->g, op->b, op->a);
	return true;
}

static bool build_fill_gradation_horizon(struct parts_construction_process *cproc, struct parts_cp_fill *op)
{
	if (!cproc->common.texture.handle)
		return false;
	if (op->w <= 0 || op->h <= 0)
		return true;
	SDL_Color *pixels = xmalloc((size_t)op->w * op->h * sizeof(*pixels));
	int denom = max(1, op->h - 1);
	for (int row = 0; row < op->h; row++) {
		uint8_t r = min(255, max(0, op->r + (op->r2 - op->r) * row / denom));
		uint8_t g = min(255, max(0, op->g + (op->g2 - op->g) * row / denom));
		uint8_t b = min(255, max(0, op->b + (op->b2 - op->b) * row / denom));
		SDL_Color c = { r, g, b, 255 };
		for (int col = 0; col < op->w; col++)
			pixels[(size_t)row * op->w + col] = c;
	}
	Texture src;
	gfx_init_texture_with_pixels(&src, op->w, op->h, pixels);
	free(pixels);
	gfx_copy(&cproc->common.texture, op->x, op->y, &src, 0, 0, op->w, op->h);
	gfx_delete_texture(&src);
	return true;
}

static bool build_mul_amap_gradation(struct parts_construction_process *cproc, struct parts_cp_fill *op, bool vertical)
{
	if (!cproc->common.texture.handle)
		return false;
	if (op->w <= 0 || op->h <= 0)
		return true;
	SDL_Color *pixels = xmalloc((size_t)op->w * op->h * sizeof(*pixels));
	int denom = max(1, (vertical ? op->w : op->h) - 1);
	for (int row = 0; row < op->h; row++) {
		for (int col = 0; col < op->w; col++) {
			int pos = vertical ? col : row;
			uint8_t a = min(255, max(0, op->a + (op->a2 - op->a) * pos / denom));
			pixels[(size_t)row * op->w + col] = (SDL_Color){ 255, 255, 255, a };
		}
	}
	Texture src;
	gfx_init_texture_with_pixels(&src, op->w, op->h, pixels);
	free(pixels);
	gfx_blend_da_daxsa(&cproc->common.texture, op->x, op->y, &src, 0, 0, op->w, op->h);
	gfx_delete_texture(&src);
	return true;
}

static bool build_draw_rect(struct parts_construction_process *cproc, struct parts_cp_fill *op)
{
	if (!cproc->common.texture.handle)
		return false;
	int x2 = op->x + op->w - 1;
	int y2 = op->y + op->h - 1;
	gfx_draw_line(&cproc->common.texture, op->x, op->y, x2, op->y, op->r, op->g, op->b);
	gfx_draw_line(&cproc->common.texture, op->x, op->y, op->x, y2, op->r, op->g, op->b);
	gfx_draw_line(&cproc->common.texture, x2, op->y, x2, y2, op->r, op->g, op->b);
	gfx_draw_line(&cproc->common.texture, op->x, y2, x2, y2, op->r, op->g, op->b);
	return true;
}

#define PARTS_CPROC_CACHE_SIZE 256

struct cproc_cg_cache_entry {
	struct string *name;
	Texture texture;
};

static struct cproc_cg_cache_entry cproc_cg_cache[PARTS_CPROC_CACHE_SIZE];
static int nr_cproc_cg_cache = 0;

void parts_cproc_cache_clear(void)
{
	for (int i = 0; i < nr_cproc_cg_cache; i++) {
		gfx_delete_texture(&cproc_cg_cache[i].texture);
		if (cproc_cg_cache[i].name)
			free_string(cproc_cg_cache[i].name);
	}
	nr_cproc_cg_cache = 0;
}

static Texture *cproc_get_cached_texture(struct string *cg_name)
{
	if (!cg_name || !cg_name->size)
		return NULL;
	for (int i = 0; i < nr_cproc_cg_cache; i++) {
		if (!strcmp(cproc_cg_cache[i].name->text, cg_name->text)) {
			if (i > 0) {
				struct cproc_cg_cache_entry hit = cproc_cg_cache[i];
				memmove(&cproc_cg_cache[1], &cproc_cg_cache[0],
						i * sizeof(struct cproc_cg_cache_entry));
				cproc_cg_cache[0] = hit;
			}
			return &cproc_cg_cache[0].texture;
		}
	}

	struct cg *cg = asset_cg_load_by_name(cg_name->text, NULL);
	if (!cg)
		return NULL;

	if (nr_cproc_cg_cache == PARTS_CPROC_CACHE_SIZE) {
		struct cproc_cg_cache_entry *evict = &cproc_cg_cache[PARTS_CPROC_CACHE_SIZE - 1];
		gfx_delete_texture(&evict->texture);
		if (evict->name)
			free_string(evict->name);
		nr_cproc_cg_cache--;
	}
	if (nr_cproc_cg_cache > 0) {
		memmove(&cproc_cg_cache[1], &cproc_cg_cache[0],
				nr_cproc_cg_cache * sizeof(struct cproc_cg_cache_entry));
	}
	memset(&cproc_cg_cache[0], 0, sizeof(cproc_cg_cache[0]));
	cproc_cg_cache[0].name = string_dup(cg_name);
	gfx_init_texture_with_cg(&cproc_cg_cache[0].texture, cg);
	cg_free(cg);
	nr_cproc_cg_cache++;
	return &cproc_cg_cache[0].texture;
}

static bool build_draw_cut_cg(struct parts_construction_process *cproc, struct parts_cp_cut_cg *op)
{
	if (!cproc->common.texture.handle)
		return false;

	Texture *src = cproc_get_cached_texture(op->cg_name);
	if (!src)
		return false;

	gfx_copy_stretch_blend_amap(&cproc->common.texture, op->dx, op->dy, op->dw, op->dh,
			src, op->sx, op->sy, op->sw, op->sh);
	return true;
}

static bool build_copy_cut_cg(struct parts_construction_process *cproc, struct parts_cp_cut_cg *op)
{
	if (!cproc->common.texture.handle)
		return false;

	Texture *src = cproc_get_cached_texture(op->cg_name);
	if (!src)
		return false;

	gfx_copy_stretch_with_alpha_map(&cproc->common.texture, op->dx, op->dy, op->dw, op->dh,
			src, op->sx, op->sy, op->sw, op->sh);
	return true;
}

static bool build_only_alpha_cut_cg(struct parts_construction_process *cproc, struct parts_cp_cut_cg *op)
{
	if (!cproc->common.texture.handle)
		return false;

	Texture *src = cproc_get_cached_texture(op->cg_name);
	if (!src)
		return false;

	gfx_copy_stretch_amap(&cproc->common.texture, op->dx, op->dy, op->dw, op->dh,
			src, op->sx, op->sy, op->sw, op->sh);
	return true;
}

static bool build_blend_cg(struct parts_construction_process *cproc, struct parts_cp_cg *op)
{
	if (!cproc->common.texture.handle)
		return false;

	Texture *src = cproc_get_cached_texture(op->name);
	if (!src)
		return false;

	gfx_blend_amap(&cproc->common.texture, op->x, op->y, src, 0, 0, src->w, src->h);
	return true;
}

static bool build_text(struct parts_construction_process *cproc, struct parts_cp_text *op,
		enum text_render_mode mode)
{
	if (!cproc->common.texture.handle)
		return false;
	int h = ceilf(text_style_height(&op->style));
	int y = op->y;
	char *text = xstrdup(op->text->text);
	for (char *line = text; line;) {
		char *next = strchr(line, '\n');
		if (next)
			*next++ = '\0';
		if (*line) {
			if (mode == RENDER_COPY || mode == RENDER_ALPHA_ONLY) {
				int w = ceilf(gfx_size_text(&op->style, line)
						+ (sjis_count_char(line) - 1) * op->style.font_spacing);
				if (mode == RENDER_COPY)
					gfx_fill_with_alpha(&cproc->common.texture, op->x, y, w, h,
							op->style.edge_color.r, op->style.edge_color.g, op->style.edge_color.b, 0);
				else
					gfx_fill_amap(&cproc->common.texture, op->x, y, w, h, 0);
			}
			gfx_render_text_mode(&cproc->common.texture, op->x, y, line, &op->style, mode);
		}
		y += h + op->line_space;
		line = next;
	}
	free(text);
	return true;
}

static bool build_gray_filter(struct parts_construction_process *cproc, struct parts_cp_filter *op)
{
	if (!cproc->common.texture.handle)
		return false;
	int x = op->x, y = op->y, w = op->w, h = op->h;
	if (op->full_size) {
		x = 0; y = 0;
		w = cproc->common.texture.w;
		h = cproc->common.texture.h;
	}

	gfx_copy_grayscale(&cproc->common.texture, x, y, &cproc->common.texture, x, y, w, h);
	return true;
}

static bool build_add_filter(struct parts_construction_process *cproc, struct parts_cp_filter *op)
{
	if (!cproc->common.texture.handle)
		return false;
	int x = op->x, y = op->y, w = op->w, h = op->h;
	if (op->full_size) {
		x = 0; y = 0;
		w = cproc->common.texture.w;
		h = cproc->common.texture.h;
	}

	gfx_fill_add(&cproc->common.texture, x, y, w, h, op->r, op->g, op->b);
	return true;
}

static bool build_mul_filter(struct parts_construction_process *cproc, struct parts_cp_filter *op)
{
	if (!cproc->common.texture.handle)
		return false;
	int x = op->x, y = op->y, w = op->w, h = op->h;
	if (op->full_size) {
		x = 0; y = 0;
		w = cproc->common.texture.w;
		h = cproc->common.texture.h;
	}

	gfx_fill_multiply(&cproc->common.texture, x, y, w, h, op->r, op->g, op->b);
	return true;
}

static bool build_blur_filter(struct parts_construction_process *cproc, struct parts_cp_filter *op, bool vertical)
{
	if (!cproc->common.texture.handle)
		return false;
	int x = op->x, y = op->y, w = op->w, h = op->h;
	if (op->full_size) {
		x = 0; y = 0;
		w = cproc->common.texture.w;
		h = cproc->common.texture.h;
	}
	if (w <= 0 || h <= 0 || op->blur <= 0)
		return true;

	if (vertical)
		gfx_copy_height_blur(&cproc->common.texture, x, y, &cproc->common.texture, x, y, w, h, op->blur);
	else
		gfx_copy_width_blur(&cproc->common.texture, x, y, &cproc->common.texture, x, y, w, h, op->blur);
	return true;
}

bool parts_build_construction_process(struct parts *parts,
		struct parts_construction_process *cproc)
{
	parts_frame_did_heavy_load = true;
	cproc->needs_build = false;
	bool resume_pass = parts_render_pass_suspend();
	glEnable(GL_BLEND);
	glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
	bool ok = false;
	bool is_rgb_tex = false;
	struct parts_cp_op *first_op = TAILQ_FIRST(&cproc->ops);
	if (first_op && (first_op->type == PARTS_CP_CREATE ||
	                 first_op->type == PARTS_CP_CREATE_PIXEL_ONLY ||
	                 first_op->type == PARTS_CP_CG)) {
		cproc->needs_pixel_snapshot = false;
	} else if (first_op && cproc->common.texture.handle) {
		gfx_begin_draw_target(&cproc->common.texture);
	}
	struct parts_cp_op *op;
	TAILQ_FOREACH(op, &cproc->ops, entry) {
		switch (op->type) {
		case PARTS_CP_CREATE:
			is_rgb_tex = false;
			gfx_end_draw_target();
			if (!build_create(parts, cproc, &op->create))
				goto done;
			gfx_begin_draw_target(&cproc->common.texture);
			break;
		case PARTS_CP_CREATE_PIXEL_ONLY:
			gfx_end_draw_target();
			if (!build_create_pixel_only(parts, cproc, &op->create))
				goto done;
			is_rgb_tex = true;
			gfx_begin_draw_target(&cproc->common.texture);
			break;
		case PARTS_CP_CG:
			is_rgb_tex = false;
			gfx_end_draw_target();
			if (!build_cg(parts, cproc, &op->cg))
				goto done;
			gfx_begin_draw_target(&cproc->common.texture);
			break;
		case PARTS_CP_FILL:
			if (!build_fill(cproc, &op->fill))
				goto done;
			break;
		case PARTS_CP_FILL_ALPHA_COLOR:
			if (!build_fill_alpha_color(cproc, &op->fill))
				goto done;
			break;
		case PARTS_CP_FILL_AMAP:
			if (!build_fill_amap(cproc, &op->fill))
				goto done;
			break;
		case PARTS_CP_FILL_WITH_ALPHA:
			if (!build_fill_with_alpha(cproc, &op->fill))
				goto done;
			break;
		case PARTS_CP_FILL_GRADATION_HORIZON:
			if (!build_fill_gradation_horizon(cproc, &op->fill))
				goto done;
			break;
		case PARTS_CP_MUL_AMAP_GRADATION_HORIZON:
			if (!build_mul_amap_gradation(cproc, &op->fill, false))
				goto done;
			break;
		case PARTS_CP_MUL_AMAP_GRADATION_VERTICAL:
			if (!build_mul_amap_gradation(cproc, &op->fill, true))
				goto done;
			break;
		case PARTS_CP_DRAW_RECT:
			if (!build_draw_rect(cproc, &op->fill))
				goto done;
			break;
		case PARTS_CP_DRAW_LINE:
			if (!cproc->common.texture.handle)
				goto done;
			gfx_draw_line(&cproc->common.texture, op->line.x0, op->line.y0,
					op->line.x1, op->line.y1, op->line.r, op->line.g, op->line.b);
			gfx_draw_line_to_amap(&cproc->common.texture, op->line.x0, op->line.y0,
					op->line.x1, op->line.y1, op->line.a);
			break;
		case PARTS_CP_DRAW_CUT_CG:
		case PARTS_CP_ALPHA_BLEND_CUT_CG:
			if (!build_draw_cut_cg(cproc, &op->cut_cg))
				goto done;
			break;
		case PARTS_CP_COPY_CUT_CG:
			if (!build_copy_cut_cg(cproc, &op->cut_cg))
				goto done;
			break;
		case PARTS_CP_ONLY_ALPHA_CUT_CG:
			if (!build_only_alpha_cut_cg(cproc, &op->cut_cg))
				goto done;
			break;
		case PARTS_CP_BLEND_CG:
			if (!build_blend_cg(cproc, &op->cg))
				goto done;
			break;
		case PARTS_CP_DRAW_TEXT:
			if (!build_text(cproc, &op->text, RENDER_BLENDED))
				goto done;
			break;
		case PARTS_CP_COPY_TEXT:
			if (!build_text(cproc, &op->text, RENDER_COPY))
				goto done;
			break;
		case PARTS_CP_ALPHA_BLEND_TEXT:
			if (!build_text(cproc, &op->text, RENDER_BLEND_ALPHA))
				goto done;
			break;
		case PARTS_CP_ONLY_ALPHA_TEXT:
			if (!build_text(cproc, &op->text, RENDER_ALPHA_ONLY))
				goto done;
			break;
		case PARTS_CP_GRAY_FILTER:
			if (!build_gray_filter(cproc, &op->filter))
				goto done;
			break;
		case PARTS_CP_ADD_FILTER:
			if (!build_add_filter(cproc, &op->filter))
				goto done;
			break;
		case PARTS_CP_MUL_FILTER:
			if (!build_mul_filter(cproc, &op->filter))
				goto done;
			break;
		case PARTS_CP_HBLUR_FILTER:
			if (!build_blur_filter(cproc, &op->filter, false))
				goto done;
			break;
		case PARTS_CP_VBLUR_FILTER:
			if (!build_blur_filter(cproc, &op->filter, true))
				goto done;
			break;
		}
	}
	parts_dirty(parts);
	ok = true;
done:
	gfx_end_draw_target();
	if (ok && is_rgb_tex)
		cproc->common.texture.opaque = true;
	parts_render_pass_resume(resume_pass);
	return ok;
}

static bool cproc_determine_dims(struct parts_construction_process *cproc, int *w_out, int *h_out)
{
	bool found = false;
	int w = 0, h = 0;
	struct parts_cp_op *op;
	TAILQ_FOREACH(op, &cproc->ops, entry) {
		switch (op->type) {
		case PARTS_CP_CREATE:
		case PARTS_CP_CREATE_PIXEL_ONLY:
			w = op->create.w;
			h = op->create.h;
			found = (w > 0 && h > 0);
			break;
		case PARTS_CP_CG: {
			Texture *cached = cproc_get_cached_texture(op->cg.name);
			if (!cached)
				return false;
			w = cached->w;
			h = cached->h;
			found = (w > 0 && h > 0);
			break;
		}
		default:
			break;
		}
	}
	if (!found)
		return false;
	*w_out = w;
	*h_out = h;
	return true;
}

bool PE_BuildPartsConstructionProcess(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_construction_process *cproc = parts_get_construction_process(parts, state);
	return parts_build_construction_process(parts, cproc);
}

bool PE_BuildPartsConstructionProcessThread(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_construction_process *cproc = parts_get_construction_process(parts, state);
	int w = 0, h = 0;
	if (!cproc_determine_dims(cproc, &w, &h))
		return parts_build_construction_process(parts, cproc);

	gfx_delete_texture(&cproc->common.texture);
	parts_set_dims(parts, &cproc->common, w, h);
	cproc->needs_build = true;
	parts_frame_did_heavy_load = true;
	parts_dirty(parts);
	return true;
}

bool parts_clear_construction_process(struct parts_construction_process *cproc)
{
	cproc->needs_build = false;
	cproc->needs_pixel_snapshot = false;
	while (!TAILQ_EMPTY(&cproc->ops)) {
		struct parts_cp_op *op = TAILQ_FIRST(&cproc->ops);
		TAILQ_REMOVE(&cproc->ops, op, entry);
		parts_cp_op_free(op);
	}
	return true;
}

bool PE_ClearPartsConstructionProcess(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return false;
	struct parts *parts = parts_get(parts_no);
	struct parts_construction_process *cproc = parts_get_construction_process(parts, state);
	if (cproc->needs_build)
		parts_build_construction_process(parts, cproc);
	bool keep_snapshot = (cproc->common.texture.handle != 0 || parts->textures_suspended) &&
		(cproc->needs_pixel_snapshot || !TAILQ_EMPTY(&cproc->ops));
	parts_clear_construction_process(cproc);
	cproc->needs_pixel_snapshot = keep_snapshot;
	return true;
}

bool PE_SetPartsConstructionSurfaceArea(int parts_no, int x, int y, int w, int h, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_construction_process *cproc = parts_get_construction_process(parts, state);
	parts_set_surface_area(parts, &cproc->common, x, y, w, h);
	return true;
}
