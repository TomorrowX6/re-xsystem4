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
#include <limits.h>
#include <math.h>
#include <cglm/cglm.h>

#include "system4.h"
#include "system4/cg.h"
#include "system4/hashtable.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

#include "asset_manager.h"
#include "audio.h"
#include "movie.h"
#include "vm/page.h"
#include "xsystem4.h"
#include "sact.h"
#include "sprite.h"
#include "parts.h"
#include "parts_activity.h"
#include "parts_internal.h"
#include "reign.h"

static struct parts_context live_context = {
	.list = TAILQ_HEAD_INITIALIZER(live_context.list),
	.dirty = TAILQ_HEAD_INITIALIZER(live_context.dirty),
	.next_free_no = 1000010000,
};
struct parts_context *parts_current = &live_context;
#define dirty_list (parts_current->dirty)
#define parts_table (parts_current->table)
#define root_pos (parts_current->origin)

bool parts_multi_controller;

static void ctrl_stack_init(void);
static void parts_change_controller(struct parts *parts, int index);

#define PARTS_PARAMS_INITIALIZER (struct parts_params) { \
	.z = 1, \
	.pos = { 0, 0 }, \
	.show = true, \
	.alpha = 255, \
	.scale = { 1.0f, 1.0f }, \
	.rotation = { 0.0f, 0.0f, 0.0f }, \
	.add_color = { 0, 0, 0, 0 }, \
	.multiply_color = { 255, 255, 255, 255 } \
}

static void parts_init(struct parts *parts)
{
	parts->sp.z = 1;
	parts->sp.has_pixel = true;
	parts->sp.has_alpha = true;
	parts->sp.render = parts_sprite_render;
	parts->sp.to_json = parts_sprite_to_json;
	parts->local = PARTS_PARAMS_INITIALIZER;
	parts->global = PARTS_PARAMS_INITIALIZER;
	if (ain && ain->version >= 12) {
		parts->local.z = 0;
		parts->global.z = 0;
	}
	parts->delegate_index = -1;
	parts->comment = string_ref(&EMPTY_STRING);
	parts->user_component_name = string_ref(&EMPTY_STRING);
	parts->text_box_text = string_ref(&EMPTY_STRING);
	parts->want_save = true;
	parts->enable_input = true;
	parts->enable_speed_up = true;
	parts->on_cursor_sound = -1;
	parts->on_click_sound = -1;
	for (int i = 0; i < PARTS_NR_STATES; i++)
		parts->sound_names[i] = string_ref(&EMPTY_STRING);
	parts->origin_mode = 1;
	parts->pending_parent = -1;
	parts->linked_to = -1;
	parts->linked_from = -1;
	TAILQ_INIT(&parts->children);
	TAILQ_INIT(&parts->motion);
}

static struct parts *parts_alloc(void)
{
	struct parts *parts = xcalloc(1, sizeof(struct parts));
	parts_init(parts);
	parts->world_matrix_self = parts;
	return parts;
}

void parts_component_dirty(struct parts *parts)
{
	parts_render_gen++;
	if (parts->dirty)
		return;
	parts->dirty = true;
	TAILQ_INSERT_TAIL(&dirty_list, parts, dirty_list_entry);
}

static void dirty_list_remove(struct parts *parts)
{
	if (parts->dirty)
		TAILQ_REMOVE(&dirty_list, parts, dirty_list_entry);
}

static int parts_get_sprite_z(struct parts *parts)
{
	if (!parts_multi_controller)
		return parts->global.z;
	// The system overlay controller sorts above any in-stack controller.
	return parts->controller_no;
}

static int parts_get_sprite_z2(struct parts *parts)
{
	if (!parts_multi_controller)
		return 0;
	return parts->global.z;
}

static void parts_list_insert(struct parts *parts)
{
	int z = parts_get_sprite_z(parts);
	int z2 = parts_get_sprite_z2(parts);
	parts->sp.z = z;
	parts->sp.z2 = z2;
	struct parts *p;
	PARTS_LIST_FOREACH_REVERSE(p) {
		int pz = parts_get_sprite_z(p);
		int pz2 = parts_get_sprite_z2(p);
		if (pz < z || (pz == z && pz2 <= z2)) {
			TAILQ_INSERT_AFTER(&parts_list, p, parts, parts_list_entry);
			goto done;
		}
	}
	TAILQ_INSERT_HEAD(&parts_list, parts, parts_list_entry);
done:
	parts_engine_dirty();
	if (!parts_current->background)
		scene_register_sprite(&parts->sp);
}

static void parts_list_remove(struct parts *parts)
{
	TAILQ_REMOVE(&parts_list, parts, parts_list_entry);
	scene_unregister_sprite(&parts->sp);
}

void parts_list_resort(struct parts *parts)
{
	int z = parts_get_sprite_z(parts);
	int z2 = parts_get_sprite_z2(parts);
	struct parts *prev = TAILQ_PREV(parts, parts_list_head, parts_list_entry);
	struct parts *next = TAILQ_NEXT(parts, parts_list_entry);
	bool ok_prev = !prev || prev->sp.z < z || (prev->sp.z == z && prev->sp.z2 <= z2);
	bool ok_next = !next || next->sp.z > z || (next->sp.z == z && next->sp.z2 >= z2);
	if (ok_prev && ok_next &&
			(parts_current->background || scene_sprite_check_z_order(&parts->sp, z, z2))) {
		parts->sp.z = z;
		parts->sp.z2 = z2;
		parts_engine_dirty();
		return;
	}
	parts_list_remove(parts);
	parts_list_insert(parts);
}

struct parts *parts_try_get(int parts_no)
{
	struct ht_slot *slot = ht_put_int(parts_table, parts_no, NULL);
	if (slot->value)
		return slot->value;
	return NULL;
}

struct parts *parts_get(int parts_no)
{
	struct ht_slot *slot = ht_put_int(parts_table, parts_no, NULL);
	if (slot->value)
		return slot->value;

	struct parts *parts = parts_alloc();
	parts->no = parts_no;
	parts->controller_no = ctrl_stack.active;
	slot->value = parts;
	parts_list_insert(parts);
	if (ain->version >= 12) {
		int root = ctrl_stack.active == PARTS_CONTROLLER_SYSTEM_OVERLAY
			? ctrl_stack.overlay_id : ctrl_stack.ids[ctrl_stack.active];
		struct parts *parent = root != parts_no ? parts_try_get(root) : NULL;
		if (parent) {
			parts->parent = parent;
			TAILQ_INSERT_TAIL(&parent->children, parts, child_list_entry);
			parts_component_dirty(parts);
		}
	}
	return parts;
}

bool parts_exists(int parts_no)
{
	return !!ht_get_int(parts_table, parts_no, NULL);
}

#define PARTS_CG_CACHE_SIZE 128

struct parts_cg_cache_entry {
	int cg_no;
	struct string *name;
	Texture texture;
	int w, h;
};

static struct parts_cg_cache_entry parts_cg_cache[PARTS_CG_CACHE_SIZE];
static int nr_parts_cg_cache = 0;

static void parts_cg_cache_put(struct parts_cg *cg)
{
	if (cg->no <= 0 || !cg->common.texture.handle || cg->common.w <= 0 || cg->common.h <= 0) {
		gfx_delete_texture(&cg->common.texture);
		if (cg->name) {
			free_string(cg->name);
			cg->name = NULL;
		}
		cg->no = 0;
		return;
	}
	if (nr_parts_cg_cache == PARTS_CG_CACHE_SIZE) {
		struct parts_cg_cache_entry *evict = &parts_cg_cache[PARTS_CG_CACHE_SIZE - 1];
		gfx_delete_texture(&evict->texture);
		if (evict->name)
			free_string(evict->name);
		nr_parts_cg_cache--;
	}
	if (nr_parts_cg_cache > 0) {
		memmove(&parts_cg_cache[1], &parts_cg_cache[0],
				nr_parts_cg_cache * sizeof(struct parts_cg_cache_entry));
	}
	parts_cg_cache[0] = (struct parts_cg_cache_entry) {
		.cg_no = cg->no,
		.name = cg->name,
		.texture = cg->common.texture,
		.w = cg->common.w,
		.h = cg->common.h,
	};
	nr_parts_cg_cache++;
	cg->common.texture.handle = 0;
	cg->name = NULL;
	cg->no = 0;
}

static bool parts_cg_cache_take(int cg_no, const char *cg_name,
		struct parts_cg_cache_entry *out)
{
	for (int i = 0; i < nr_parts_cg_cache; i++) {
		struct parts_cg_cache_entry *e = &parts_cg_cache[i];
		bool match = (cg_no > 0 && e->cg_no == cg_no) ||
			(cg_name && e->name && !strcmp(e->name->text, cg_name));
		if (match) {
			*out = *e;
			nr_parts_cg_cache--;
			if (i < nr_parts_cg_cache) {
				memmove(&parts_cg_cache[i], &parts_cg_cache[i + 1],
						(nr_parts_cg_cache - i) * sizeof(struct parts_cg_cache_entry));
			}
			return true;
		}
	}
	return false;
}

static void parts_cg_cache_clear(void)
{
	for (int i = 0; i < nr_parts_cg_cache; i++) {
		gfx_delete_texture(&parts_cg_cache[i].texture);
		if (parts_cg_cache[i].name)
			free_string(parts_cg_cache[i].name);
	}
	nr_parts_cg_cache = 0;
}

void PE_FlushCGCache(void)
{
	parts_cg_cache_clear();
	parts_cproc_cache_clear();
	parts_flat_cache_clear();
}

static void parts_state_free(struct parts_state *state)
{
	parts_state_discard_texture_backups(state);
	switch (state->type) {
	case PARTS_UNINITIALIZED:
	case PARTS_LAYOUT_BOX:
		break;
	case PARTS_CG:
		parts_cg_cache_put(&state->cg);
		break;
	case PARTS_TEXT:
		parts_text_free(&state->text);
		break;
	case PARTS_ANIMATION:
		for (unsigned i = 0; i < state->anim.nr_frames; i++) {
			gfx_delete_texture(&state->anim.frames[i]);
		}
		free(state->anim.frames);
		if (state->anim.cg_name)
			free_string(state->anim.cg_name);
		break;
	case PARTS_NUMERAL:
		gfx_delete_texture(&state->common.texture);
		if (state->num.cg_name) free_string(state->num.cg_name);
		break;
	case PARTS_HGAUGE:
	case PARTS_VGAUGE:
		gfx_delete_texture(&state->common.texture);
		gfx_delete_texture(&state->gauge.cg);
		if (state->gauge.cg_name)
			free_string(state->gauge.cg_name);
		break;
	case PARTS_RECT_DETECTION:
		if (state->rect.cg_name)
			free_string(state->rect.cg_name);
		break;
	case PARTS_CONSTRUCTION_PROCESS:
		gfx_delete_texture(&state->common.texture);
		parts_clear_construction_process(&state->cproc);
		break;
	case PARTS_FLASH:
		parts_flash_free(&state->flash);
		break;
	case PARTS_FLAT:
		parts_flat_free(&state->flat);
		break;
	case PARTS_MOVIE:
		// The texture is owned by the SACT sprite.
		state->common.texture.handle = 0;
		if (state->movie.mc)
			movie_free(state->movie.mc);
		if (state->movie.sprite_no >= 0)
			sact_SP_Delete(state->movie.sprite_no);
		break;
	case PARTS_3DLAYER:
		state->common.texture.handle = 0;
		if (state->layer3d.plugin >= 0)
			ReignEngine_ReleasePlugin(state->layer3d.plugin);
		if (state->layer3d.sprite_no >= 0)
			sact_SP_Delete(state->layer3d.sprite_no);
		break;
	}
	memset(state, 0, sizeof(struct parts_state));
}

static struct text_style default_text_style = {
	.face = FONT_GOTHIC,
	.size = 16.0f,
	.bold_width = 0.0f,
	.weight = FW_NORMAL,
	.edge_left = 0.0f,
	.edge_up = 0.0f,
	.edge_right = 0.0f,
	.edge_down = 0.0f,
	.color = { .r = 255, .g = 255, .b = 255, .a = 255 },
	.edge_color = { .r = 0, .g = 0, .b = 0, .a = 255 },
	.scale_x = 1.0f,
	.space_scale_x = 1.0f,
	.font_spacing = 0.0f,
	.font_size = NULL
};

void parts_state_reset(struct parts_state *state, enum parts_type type)
{
	parts_state_free(state);
	state->type = type;
	switch (type) {
	case PARTS_TEXT:
		state->text.ts = default_text_style;
		state->text.ruby_ts = default_text_style;
		state->text.ruby_ts.size = 10.0f;
		break;
	case PARTS_NUMERAL:
		state->num.length = ain->version >= 12 ? 0 : 1;
		state->num.font_no = -1;
		state->num.show_padding = true;
		state->num.ts = default_text_style;
		state->num.cg_name = string_ref(&EMPTY_STRING);
		break;
	case PARTS_CONSTRUCTION_PROCESS:
		TAILQ_INIT(&state->cproc.ops);
		break;
	case PARTS_HGAUGE:
	case PARTS_VGAUGE:
		state->gauge.cg_no = -1;
		state->gauge.cg_name = string_ref(&EMPTY_STRING);
		state->gauge.rate = 0.0f;
		state->gauge.numerator = 0.0f;
		state->gauge.denominator = 1.0f;
		break;
	case PARTS_3DLAYER:
		state->layer3d.plugin = -1;
		state->layer3d.sprite_no = -1;
		break;
	case PARTS_UNINITIALIZED:
	case PARTS_CG:
	case PARTS_ANIMATION:
	case PARTS_FLASH:
	case PARTS_RECT_DETECTION:
		break;
	case PARTS_FLAT:
		state->flat.fps_rate = 1.0f;
		break;
	case PARTS_MOVIE:
		state->movie.sprite_no = -1;
		break;
	case PARTS_LAYOUT_BOX:
		state->layout_box.layout_type = PARTS_LAYOUT_VERTICAL;
		state->layout_box.align = 1;
		break;
	}
}

struct parts_cg *parts_get_cg(struct parts *parts, int state)
{
	parts_state_resume_textures(&parts->states[state]);
	if (parts->states[state].type != PARTS_CG) {
		parts_state_reset(&parts->states[state], PARTS_CG);
	}
	return &parts->states[state].cg;
}

struct parts_text *parts_get_text(struct parts *parts, int state)
{
	parts_state_resume_textures(&parts->states[state]);
	if (parts->states[state].type != PARTS_TEXT) {
		parts_state_reset(&parts->states[state], PARTS_TEXT);
	}
	return &parts->states[state].text;
}

struct parts_animation *parts_get_animation(struct parts *parts, int state)
{
	parts_state_resume_textures(&parts->states[state]);
	if (parts->states[state].type != PARTS_ANIMATION) {
		parts_state_reset(&parts->states[state], PARTS_ANIMATION);
	}
	return &parts->states[state].anim;
}

struct parts_numeral *parts_get_numeral(struct parts *parts, int state)
{
	parts_state_resume_textures(&parts->states[state]);
	if (parts->states[state].type != PARTS_NUMERAL) {
		parts_state_reset(&parts->states[state], PARTS_NUMERAL);
	}
	return &parts->states[state].num;
}

struct parts_gauge *parts_get_hgauge(struct parts *parts, int state)
{
	parts_state_resume_textures(&parts->states[state]);
	if (parts->states[state].type != PARTS_HGAUGE) {
		parts_state_reset(&parts->states[state], PARTS_HGAUGE);
	}
	return &parts->states[state].gauge;
}

struct parts_gauge *parts_get_vgauge(struct parts *parts, int state)
{
	parts_state_resume_textures(&parts->states[state]);
	if (parts->states[state].type != PARTS_VGAUGE) {
		parts_state_reset(&parts->states[state], PARTS_VGAUGE);
	}
	return &parts->states[state].gauge;
}

struct parts_construction_process *parts_get_construction_process(struct parts *parts, int state)
{
	parts_state_resume_textures(&parts->states[state]);
	if (parts->states[state].type != PARTS_CONSTRUCTION_PROCESS) {
		parts_state_reset(&parts->states[state], PARTS_CONSTRUCTION_PROCESS);
	}
	return &parts->states[state].cproc;
}

struct parts_flash *parts_get_flash(struct parts *parts, int state)
{
	parts_state_resume_textures(&parts->states[state]);
	if (parts->states[state].type != PARTS_FLASH) {
		parts_state_reset(&parts->states[state], PARTS_FLASH);
	}
	return &parts->states[state].flash;
}

struct parts_flat *parts_get_flat(struct parts *parts, int state)
{
	parts_state_resume_textures(&parts->states[state]);
	if (parts->states[state].type != PARTS_FLAT) {
		parts_state_reset(&parts->states[state], PARTS_FLAT);
	}
	return &parts->states[state].flat;
}

struct parts_movie *parts_get_movie(struct parts *parts, int state)
{
	if (parts->states[state].type != PARTS_MOVIE) {
		parts_state_reset(&parts->states[state], PARTS_MOVIE);
	}
	return &parts->states[state].movie;
}

struct parts_layout_box *parts_get_layout_box(struct parts *parts)
{
	if (parts->states[0].type != PARTS_LAYOUT_BOX) {
		parts_state_reset(&parts->states[0], PARTS_LAYOUT_BOX);
	}
	return &parts->states[0].layout_box;
}

struct parts_3dlayer *parts_get_3dlayer(struct parts *parts, int state)
{
	if (parts->states[state].type != PARTS_3DLAYER) {
		parts_state_reset(&parts->states[state], PARTS_3DLAYER);
	}
	return &parts->states[state].layer3d;
}

static Point calculate_offset(int mode, int w, int h)
{
	switch (mode) {
	case 1:  return (Point) {    0, 0    }; // top-left
	case 2:  return (Point) { -w/2, 0    }; // top-center
	case 3:  return (Point) {   -w, 0    }; // top-right
	case 4:  return (Point) {    0, -h/2 }; // middle-left
	case 5:  return (Point) { -w/2, -h/2 }; // middle-center
	case 6:  return (Point) {   -w, -h/2 }; // middle-right
	case 7:  return (Point) {    0, -h   }; // bottom-left
	case 8:  return (Point) { -w/2, -h   }; // bottom-center
	case 9:  return (Point) {   -w, -h   }; // bottom-right
	default: return (Point) { mode, (3*h)/4 }; // why...
	}
}

/*
 * Should be called when:
 *   - position (parts->pos) changes
 *   - width or height changes
 *   - origin mode changes
 */
static void parts_common_recalculate_hitbox(struct parts *parts, struct parts_common *common)
{
	if (parts->states[0].type == PARTS_LAYOUT_BOX)
		parts_render_gen++;
	// v12 hitboxes stay in component space; transforms retain subpixel positions.
	float x = ain->version >= 12 ? 0 : parts->local.pos.x;
	float y = ain->version >= 12 ? 0 : parts->local.pos.y;
	if (parts->component_type == 1 && parts->widget && common != &parts->widget->label.common) {
		int width, height;
		parts_widget_get_size(parts, &width, &height);
		common->origin_offset = calculate_offset(parts->origin_mode, width, height);
		common->hitbox = (Rectangle){common->origin_offset.x, common->origin_offset.y, width, height};
		return;
	}
	if (common->surface_area.w || common->surface_area.h) {
		common->origin_offset = calculate_offset(parts->origin_mode,
				common->surface_area.w, common->surface_area.h);
		Rectangle r = { 0, 0, common->w, common->h };
		SDL_IntersectRect(&r, &common->surface_area, &common->hitbox);
		common->origin_offset.x -= common->surface_area.x;
		common->origin_offset.y -= common->surface_area.y;
		common->hitbox.x += x + common->origin_offset.x;
		common->hitbox.y += y + common->origin_offset.y;
	} else {
		common->origin_offset = calculate_offset(parts->origin_mode, common->w, common->h);
		common->hitbox = (Rectangle) {
			.x = x + common->origin_offset.x,
			.y = y + common->origin_offset.y,
			.w = common->w,
			.h = common->h,
		};
	}
}

void parts_recalculate_hitbox(struct parts *parts)
{
	for (int i = 0; i < PARTS_NR_STATES; i++) {
		parts_common_recalculate_hitbox(parts, &parts->states[i].common);
		if (parts->states[i].type == PARTS_RECT_DETECTION && parts->states[i].rect.custom) {
			struct parts_rect *rect = &parts->states[i].rect;
			int x = rect->points[0].x, y = rect->points[0].y;
			for (int j = 1; j < 4; j++) { x = min(x, rect->points[j].x); y = min(y, rect->points[j].y); }
			rect->common.hitbox.x += x;
			rect->common.hitbox.y += y;
		}
	}
}

SDL_FPoint parts_local_position(struct parts *parts)
{
	SDL_FPoint pos = parts->local.pos;
	if (parts->position_is_ratio) {
		pos.x *= config.view_width;
		pos.y *= config.view_height;
	}
	return pos;
}

static void parts_apply_local_matrix(struct parts *parts, mat4 out)
{
	float tx = 0.0f, ty = 0.0f;
	if (parts->parent && parts->parent->states[0].type == PARTS_LAYOUT_BOX &&
			parts->parent->states[0].layout_box.layout_type == PARTS_LAYOUT_FREE) {
		Point off = parts->parent->states[0].common.origin_offset;
		tx += off.x;
		ty += off.y;
	}
	SDL_FPoint pos = parts_local_position(parts);
	for (int axis = 0; axis < 2; axis++) {
		int link_no = parts->scroll_links[axis];
		if (link_no > 0) {
			struct parts *link = parts_try_get(link_no);
			if (link && link->scrollbar) {
				if (axis) pos.y -= link->scrollbar->pos;
				else pos.x -= link->scrollbar->pos;
			}
		}
	}
	tx += pos.x;
	ty += pos.y;
	if (parts->local.rotation.x == 0.0f && parts->local.rotation.y == 0.0f &&
			parts->local.rotation.z == 0.0f) {
		float sx = parts->local.scale.x;
		float sy = parts->local.scale.y;
		if (tx != 0.0f || ty != 0.0f || sx != 1.0f || sy != 1.0f) {
			for (int r = 0; r < 4; r++) {
				float m0 = out[0][r];
				float m1 = out[1][r];
				out[3][r] += tx * m0 + ty * m1;
				out[0][r] = sx * m0;
				out[1][r] = sy * m1;
			}
		}
		return;
	}
	if (tx != 0.0f || ty != 0.0f)
		glm_translate(out, (vec3){tx, ty, 0});
	if (parts->local.rotation.z != 0.0f)
		glm_rotate_z(out, glm_rad(parts->local.rotation.z), out);
	if (parts->local.rotation.x != 0.0f)
		glm_rotate_x(out, glm_rad(-parts->local.rotation.x), out);
	if (parts->local.rotation.y != 0.0f)
		glm_rotate_y(out, glm_rad(parts->local.rotation.y), out);
	if (parts->local.scale.x != 1.0f || parts->local.scale.y != 1.0f)
		glm_scale(out, (vec3){parts->local.scale.x, parts->local.scale.y, 1});
}

static void parts_update_global_pos_mat(struct parts *parts, SDL_FPoint parent_pos, mat4 parent_matrix)
{
	parts->global.pos = (SDL_FPoint) {
		parent_pos.x + parts->local.pos.x,
		parent_pos.y + parts->local.pos.y
	};
	mat4 matrix;
	if (ain->version >= 12) {
		glm_mat4_copy(parent_matrix, matrix);
		parts_apply_local_matrix(parts, matrix);
		parts->global.pos = (SDL_FPoint){matrix[3][0], matrix[3][1]};
		if (parts->world_matrix_self == parts) {
			glm_mat4_copy(matrix, parts->world_matrix);
			parts->world_matrix_gen = parts_render_gen;
		}
	}

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_pos_mat(child, parts->global.pos, matrix);
	}
}

static void parts_update_global_pos(struct parts *parts, SDL_FPoint parent_pos)
{
	mat4 parent_matrix = GLM_MAT4_IDENTITY_INIT;
	if (ain->version >= 12) {
		if (parts->parent) {
			parts_world_matrix(parts->parent, parent_matrix);
		} else if (root_pos.x != 0 || root_pos.y != 0) {
			glm_translate(parent_matrix, (vec3){root_pos.x, root_pos.y, 0});
		}
	}
	parts_update_global_pos_mat(parts, parent_pos, parent_matrix);
}

void parts_set_pos(struct parts *parts, Point pos)
{
	parts_set_posf(parts, (SDL_FPoint){pos.x, pos.y});
}

void parts_set_posf(struct parts *parts, SDL_FPoint pos)
{
	parts->local.pos.x = pos.x;
	parts->local.pos.y = pos.y;
	parts_recalculate_hitbox(parts);
	parts_dirty(parts);
	parts_update_global_pos(parts, parts->parent ? parts->parent->global.pos
			: (SDL_FPoint){root_pos.x, root_pos.y});
}

void parts_set_global_pos(Point pos)
{
	root_pos = pos;
	parts_engine_dirty();
	struct parts *parts;
	PARTS_LIST_FOREACH(parts) {
		if (!parts->parent)
			parts_update_global_pos(parts, (SDL_FPoint){root_pos.x, root_pos.y});
	}
}

static void parts_update_global_z(struct parts *parts, int parent_z)
{
	parts->global.z = parent_z + parts->local.z;
	parts_list_resort(parts);

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_z(child, parts->global.z);
	}
}

void parts_set_z(struct parts *parts, int z)
{
	if (parts->local.z == z)
		return;

	parts->local.z = z;
	parts_update_global_z(parts, parts->parent ? parts->parent->global.z : 0);
	parts_dirty(parts);
}

static void parts_update_global_show(struct parts *parts, bool parent_show)
{
	parts->global.show = parent_show && parts->local.show;

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_show(child, parts->global.show);
	}
}

void parts_set_show(struct parts *parts, bool show)
{
	if (parts->local.show == show)
		return;

	parts->local.show = show;
	parts_update_global_show(parts, parts->parent ? parts->parent->global.show : true);
	parts_dirty(parts);
}

bool parts_is_visible(struct parts *parts)
{
	if (!parts || !parts->global.show || parts->global.alpha <= 0)
		return false;
	float rot_x = 0.0f;
	float rot_y = 0.0f;
	for (struct parts *p = parts; p; p = p->parent) {
		if (!p->local.show || p->local.alpha <= 0)
			return false;
		if (p->message_window && !parts_message_window_show)
			return false;
		if (p->linked_to > 0) {
			struct parts *link_parts = parts_try_get(p->linked_to);
			if (!link_parts || !link_parts->is_hovered)
				return false;
		}
		if (p->checkbox_show_link > 0) {
			struct parts *cb = parts_try_get(p->checkbox_show_link);
			if (!cb || !cb->widget || !cb->widget->checked)
				return false;
		}
		rot_x += p->local.rotation.x;
		rot_y += p->local.rotation.y;
	}
	if ((rot_x != 0.0f || rot_y != 0.0f) &&
			cosf(glm_rad(rot_x)) * cosf(glm_rad(rot_y)) <= 1e-6f)
		return false;
	return true;
}

static void parts_update_global_alpha(struct parts *parts, int parent_alpha)
{
	parts->global.alpha = parent_alpha * (parts->local.alpha / 255.0f);

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_alpha(child, parts->global.alpha);
	}
}

void parts_set_alpha(struct parts *parts, int alpha)
{
	uint8_t clamped = max(0, min(255, alpha));
	if (parts->local.alpha == clamped)
		return;
	parts->local.alpha = clamped;
	parts_update_global_alpha(parts, parts->parent ? parts->parent->global.alpha : 255);
	parts_dirty(parts);
}

static void parts_update_global_add_color(struct parts *parts, SDL_Color parent_color)
{
	parts->global.add_color = (SDL_Color) {
		min(255, parent_color.r + parts->local.add_color.r),
		min(255, parent_color.g + parts->local.add_color.g),
		min(255, parent_color.b + parts->local.add_color.b),
		0
	};

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_add_color(child, parts->global.add_color);
	}
}

void parts_set_add_color(struct parts *parts, SDL_Color color)
{
	if (!memcmp(&parts->local.add_color, &color, sizeof(SDL_Color)))
		return;
	parts->local.add_color = color;
	parts_update_global_add_color(parts, parts->parent ? parts->parent->global.add_color
			: (SDL_Color){0,0,0,0});
	parts_dirty(parts);
}

static void parts_update_global_multiply_color(struct parts *parts, SDL_Color parent_color)
{
	parts->global.multiply_color = (SDL_Color) {
		parent_color.r * (parts->local.multiply_color.r / 255.0f),
		parent_color.g * (parts->local.multiply_color.g / 255.0f),
		parent_color.b * (parts->local.multiply_color.b / 255.0f),
		255
	};

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_multiply_color(child, parts->global.multiply_color);
	}
}

void parts_set_multiply_color(struct parts *parts, SDL_Color color)
{
	if (!memcmp(&parts->local.multiply_color, &color, sizeof(SDL_Color)))
		return;
	parts->local.multiply_color = color;
	parts_update_global_multiply_color(parts, parts->parent ? parts->parent->global.multiply_color
			: (SDL_Color){255,255,255,255});
	parts_dirty(parts);
}

void parts_set_origin_mode(struct parts *parts, int origin_mode)
{
	parts->origin_mode = origin_mode;
	parts_recalculate_hitbox(parts);
	parts_dirty(parts);
	parts_update_global_pos(parts, parts->parent ? parts->parent->global.pos
			: (SDL_FPoint){root_pos.x, root_pos.y});
}

static void parts_update_global_scale_x(struct parts *parts, float parent_scale_x)
{
	parts->global.scale.x = parent_scale_x * parts->local.scale.x;

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_scale_x(child, parts->global.scale.x);
	}
}

void parts_set_scale_x(struct parts *parts, float mag)
{
	parts->local.scale.x = mag;
	parts_recalculate_hitbox(parts);
	parts_update_global_scale_x(parts, parts->parent ? parts->parent->global.scale.x : 1.0f);
	parts_dirty(parts);
}

static void parts_update_global_scale_y(struct parts *parts, float parent_scale_y)
{
	parts->global.scale.y = parent_scale_y * parts->local.scale.y;

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_scale_y(child, parts->global.scale.y);
	}
}

void parts_set_scale_y(struct parts *parts, float mag)
{
	parts->local.scale.y = mag;
	parts_recalculate_hitbox(parts);
	parts_update_global_scale_y(parts, parts->parent ? parts->parent->global.scale.y : 1.0f);
	parts_dirty(parts);
}

static void parts_update_global_rotate_x(struct parts *parts, float parent_rot_x)
{
	parts->global.rotation.x = parent_rot_x + parts->local.rotation.x;

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_rotate_x(child, parts->global.rotation.x);
	}
}

void parts_set_rotation_x(struct parts *parts, float rot)
{
	parts->local.rotation.x = rot;
	parts_update_global_rotate_x(parts, parts->parent ? parts->parent->global.rotation.x : 0.0f);
	parts_dirty(parts);
}

static void parts_update_global_rotate_y(struct parts *parts, float parent_rot_y)
{
	parts->global.rotation.y = parent_rot_y + parts->local.rotation.y;

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_rotate_y(child, parts->global.rotation.y);
	}
}

void parts_set_rotation_y(struct parts *parts, float rot)
{
	parts->local.rotation.y = rot;
	parts_update_global_rotate_y(parts, parts->parent ? parts->parent->global.rotation.y : 0.0f);
	parts_dirty(parts);
}

static void parts_update_global_rotate_z(struct parts *parts, float parent_rot_z)
{
	parts->global.rotation.z = parent_rot_z + parts->local.rotation.z;

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_global_rotate_z(child, parts->global.rotation.z);
	}
}

void parts_set_rotation_z(struct parts *parts, float rot)
{
	parts->local.rotation.z = rot;
	parts_update_global_rotate_z(parts, parts->parent ? parts->parent->global.rotation.z : 0.0f);
	parts_dirty(parts);
}

void parts_set_dims(struct parts *parts, struct parts_common *common, int w, int h)
{
	common->w = w;
	common->h = h;
	parts_common_recalculate_hitbox(parts, common);
}

bool _parts_cg_set(struct parts *parts, struct parts_cg *parts_cg, struct cg *cg, int cg_no,
		struct string *name)
{
	if (!cg) {
		if (name)
			free_string(name);
		return false;
	}
	parts_cg_cache_put(parts_cg);
	gfx_init_texture_with_cg(&parts_cg->common.texture, cg);
	parts_set_dims(parts, &parts_cg->common, cg->metrics.w, cg->metrics.h);
	parts_cg->no = cg_no;
	if (parts_cg->name)
		free_string(parts_cg->name);
	parts_cg->name = name;
	parts_frame_did_heavy_load = true;
	parts_dirty(parts);
	cg_free(cg);
	return true;
}

static void parts_cg_apply_cached(struct parts *parts, struct parts_cg *parts_cg,
		struct parts_cg_cache_entry *cached, struct string *name)
{
	parts_cg_cache_put(parts_cg);
	parts_cg->common.texture = cached->texture;
	parts_set_dims(parts, &parts_cg->common, cached->w, cached->h);
	parts_cg->no = cached->cg_no;
	if (cached->name)
		free_string(cached->name);
	parts_cg->name = name;
	parts_dirty(parts);
}

bool parts_cg_set_by_index(struct parts *parts, struct parts_cg *cg, int cg_no)
{
	assert(cg_no);
	if (cg->no == cg_no && cg->common.texture.handle) {
		if (cg->name) {
			free_string(cg->name);
			cg->name = NULL;
		}
		parts_set_dims(parts, &cg->common, cg->common.texture.w, cg->common.texture.h);
		parts_dirty(parts);
		return true;
	}
	struct parts_cg_cache_entry cached;
	if (parts_cg_cache_take(cg_no, NULL, &cached)) {
		parts_cg_apply_cached(parts, cg, &cached, NULL);
		return true;
	}
	return _parts_cg_set(parts, cg, asset_cg_load(cg_no), cg_no, NULL);
}

bool parts_cg_set(struct parts *parts, struct parts_cg *parts_cg, struct string *cg_name)
{
	assert(cg_name && *(cg_name->text) != '\0');
	int no;
	struct cg *cg;
	if (!memcmp(cg_name->text, "<save>", 6)) {
		char *path = savedir_path(cg_name->text + 6);
		cg = cg_load_file(path);
		no = 0;
		free(path);
	} else {
		if (parts_cg->no > 0 && parts_cg->common.texture.handle &&
				parts_cg->name && !strcmp(parts_cg->name->text, cg_name->text)) {
			parts_set_dims(parts, &parts_cg->common,
					parts_cg->common.texture.w, parts_cg->common.texture.h);
			parts_dirty(parts);
			return true;
		}
		struct parts_cg_cache_entry cached;
		if (parts_cg_cache_take(0, cg_name->text, &cached)) {
			parts_cg_apply_cached(parts, parts_cg, &cached, string_dup(cg_name));
			return true;
		}
		cg = asset_cg_load_by_name(cg_name->text, &no);
	}
	return _parts_cg_set(parts, parts_cg, cg, no, string_dup(cg_name));
}


void parts_numeral_font_init(struct parts_numeral_font *font)
{
	if (font->type == PARTS_NUMERAL_FONT_SEPARATE) {
		for (int i = 0; i < 12; i++) {
			struct cg *cg = asset_cg_load(font->cg_no + i);
			if (!cg) {
				font->cg[i].handle = 0;
				continue;
			}
			gfx_init_texture_with_cg(&font->cg[i], cg);
			cg_free(cg);
		}
	} else if (font->type == PARTS_NUMERAL_FONT_SEPARATE2) {
		for (int i = 0; i < 12; i++) {
			if (font->width[i] < 0)
				continue;
			struct cg *cg = asset_cg_load(font->width[i]);
			if (!cg) {
				font->cg[i].handle = 0;
				continue;
			}
			gfx_init_texture_with_cg(&font->cg[i], cg);
			cg_free(cg);
		}
	} else if (font->type == PARTS_NUMERAL_FONT_COMBINED) {
		int x = 0;
		Texture t = {0};
		struct cg *cg = asset_cg_load(font->cg_no);
		if (!cg) return;
		gfx_init_texture_with_cg(&t, cg);
		for (int i = 0; i < 12; i++) {
			if (font->width[i] <= 0)
				continue;
			gfx_init_texture_blank(&font->cg[i], font->width[i], t.h);
			gfx_copy_with_alpha_map(&font->cg[i], 0, 0, &t, x, 0, font->width[i], t.h);
			x += font->width[i];
		}
		gfx_delete_texture(&t);
		cg_free(cg);
	}
}

static int parts_load_numeral_font_separate(int cg_no)
{
	// find existing font
	for (int i = 0; i < parts_nr_numeral_fonts; i++) {
		if (parts_numeral_fonts[i].type == PARTS_NUMERAL_FONT_SEPARATE
				&& parts_numeral_fonts[i].cg_no == cg_no)
			return i;
	}

	// load new font
	int font_no = parts_nr_numeral_fonts++;
	parts_numeral_fonts = xrealloc_array(parts_numeral_fonts, font_no, font_no + 1,
			sizeof(struct parts_numeral_font));
	struct parts_numeral_font *font = &parts_numeral_fonts[font_no];
	font->cg_no = cg_no;
	font->type = PARTS_NUMERAL_FONT_SEPARATE;
	parts_numeral_font_init(font);
	return font_no;
}

static int parts_load_numeral_font_separate_string(struct string *cg_name)
{
	// convert name to CG indices
	int indices[12];
	for (int i = 0; i < 12; i++) {
		struct string *name = string_format(cg_name, (union vm_value){.i = i}, STRFMT_INT);
		if (!asset_exists_by_name(ASSET_CG, name->text, &indices[i])) {
			WARNING("numeral cg doesn't exist: %s", display_sjis0(name->text));
			indices[i] = -1;
		}
		free_string(name);
	}

	// find existing font
	for (int i = 0; i < parts_nr_numeral_fonts; i++) {
		struct parts_numeral_font *font = &parts_numeral_fonts[i];
		if (font->type != PARTS_NUMERAL_FONT_SEPARATE2)
			continue;
		bool not_match = false;
		for (int d = 0; d < 12; d++) {
			if (font->width[d] != indices[d]) {
				not_match = true;
				break;
			}
		}
		if (not_match)
			continue;
		return i;
	}

	// load new font
	int font_no = parts_nr_numeral_fonts++;
	parts_numeral_fonts = xrealloc_array(parts_numeral_fonts, font_no, font_no + 1,
			sizeof(struct parts_numeral_font));
	struct parts_numeral_font *font = &parts_numeral_fonts[font_no];
	font->cg_no = indices[0];
	font->type = PARTS_NUMERAL_FONT_SEPARATE2;
	memcpy(font->width, indices, sizeof(int) * 12);
	parts_numeral_font_init(font);
	return font_no;
}

static int parts_load_numeral_font_combined(int cg_no, int w[12])
{
	// find existing font
	for (int i = 0; i < parts_nr_numeral_fonts; i++) {
		struct parts_numeral_font *font = &parts_numeral_fonts[i];
		if (font->type != PARTS_NUMERAL_FONT_COMBINED || font->cg_no != cg_no)
			continue;
		if (!memcmp(w, font->width, sizeof(font->width)))
			return i;
	}

	// load new font
	int font_no = parts_nr_numeral_fonts++;
	parts_numeral_fonts = xrealloc_array(parts_numeral_fonts, font_no, font_no + 1,
			sizeof(struct parts_numeral_font));
	struct parts_numeral_font *font = &parts_numeral_fonts[font_no];
	font->cg_no = cg_no;
	font->type = PARTS_NUMERAL_FONT_COMBINED;
	memcpy(font->width, w, sizeof(int) * 12);
	parts_numeral_font_init(font);
	return font_no;
}

static bool parts_numeral_update(struct parts *parts, struct parts_numeral *num)
{
	if (!num->have_num || (num->show_type != 2 && num->font_no < 0))
		return true;
	// Length counts digits, excluding the sign and inserted grouping separators.
	// Hidden padding still occupies the width of its zero glyph.
	char digits[24];
	int64_t value = num->num;
	bool negative = value < 0;
	if (negative) value = -value;
	int count = snprintf(digits, sizeof(digits), "%lld", (long long)value);
	int total = max(count, min(num->length, 128));
	int padding = total - count;
	char characters[192];
	bool hidden[192] = {0};
	int length = 0;
	if (negative) characters[length++] = '-';
	for (int i = 0; i < total; i++) {
		if (num->show_comma && i && (total - i) % 3 == 0)
			characters[length++] = ',';
		hidden[length] = i < padding && !num->show_padding;
		characters[length++] = i < padding ? '0' : digits[i - padding];
	}
	characters[length] = 0;

	float advances[192];
	char glyphs[192][4];
	struct parts_numeral_font *font = num->font_no >= 0
		? &parts_numeral_fonts[num->font_no] : NULL;
	int height = 0;
	float width = 0;
	for (int i = 0; i < length; i++) {
		if (num->show_type == 2) {
			glyphs[i][0] = characters[i];
			glyphs[i][1] = 0;
			if (num->full_pitch) {
				// Convert through the selected game encoding (GBK or Shift-JIS).
				unsigned cp = 0xff00 + (unsigned char)characters[i] - 0x20;
				char utf8[4] = {0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f),
					0x80 | (cp & 0x3f), 0};
				char *encoded = utf2sjis(utf8, 3);
				snprintf(glyphs[i], sizeof(glyphs[i]), "%s", encoded);
				free(encoded);
			}
			advances[i] = gfx_size_char(&num->ts, glyphs[i]);
			height = ceilf(text_style_height(&num->ts));
		} else {
			int index = characters[i] == '-' ? 10 : characters[i] == ',' ? 11 : characters[i] - '0';
			advances[i] = font->cg[index].w;
			height = max(height, font->cg[index].h);
		}
		width += advances[i];
	}
	width += (length - 1) * num->space;
	int w = max(0, (int)ceilf(width));
	gfx_delete_texture(&num->common.texture);
	if (w > 0 && height > 0) {
		gfx_init_texture_rgba(&num->common.texture, w, height, (SDL_Color){0, 0, 0, 0});
		gfx_begin_draw_target(&num->common.texture);
		float x = 0;
		for (int i = 0; i < length; i++) {
			if (!hidden[i]) {
				if (num->show_type == 2) {
					gfx_render_textf(&num->common.texture, x, 0, glyphs[i], &num->ts, false);
				} else {
					int index = characters[i] == '-' ? 10 : characters[i] == ',' ? 11 : characters[i] - '0';
					Texture *cg = &font->cg[index];
					if (cg->handle) gfx_copy_with_alpha_map(&num->common.texture, x, 0, cg, 0, 0, cg->w, cg->h);
				}
			}
			x += advances[i] + num->space;
		}
		gfx_end_draw_target();
	}
	parts_set_dims(parts, &num->common, w, height);
	parts_dirty(parts);
	return true;
}

bool parts_numeral_set_number(struct parts *parts, struct parts_numeral *num, int n)
{
	if (num->have_num && num->num == n && num->common.texture.handle)
		return true;
	num->have_num = true;
	num->num = n;
	return parts_numeral_update(parts, num);
}

void parts_set_state(struct parts *parts, enum parts_state_type state)
{
	if (parts->lock_input_state)
		return;
	while (state > PARTS_STATE_DEFAULT) {
		struct parts_state *s = &parts->states[state];
		if (s->type != PARTS_UNINITIALIZED && !(ain->version >= 12
				&& s->type == PARTS_CG && !s->common.texture.handle)) break;
		state--;
	}
	if (parts->state != state) {
		parts->state = state;
		parts_dirty(parts);
	}
}

void parts_set_surface_area(struct parts *parts, struct parts_common *common, int x, int y, int w, int h)
{
	if (x < 0 || y < 0)
		return;
	common->surface_area = (Rectangle) { x, y, w, h };
	parts_common_recalculate_hitbox(parts, common);
}

static bool parts_animation_update(struct parts_animation *anim, int passed_time)
{
	if (passed_time <= 0 || !anim->nr_frames)
		return false;

	const unsigned elapsed = anim->elapsed + passed_time;
	const unsigned frame_diff = elapsed / anim->frame_time;
	const unsigned remainder = elapsed % anim->frame_time;

	if (frame_diff > 0) {
		anim->elapsed = remainder;
		anim->current_frame = (anim->current_frame + frame_diff) % anim->nr_frames;
		anim->common.texture = anim->frames[anim->current_frame];
		return true;
	} else {
		anim->elapsed = elapsed;
		return false;
	}
}

bool parts_state_update(struct parts_state *state, int passed_time)
{
	bool dirty = false;
	switch (state->type) {
	case PARTS_ANIMATION:
		dirty = parts_animation_update(&state->anim, passed_time);
		break;
	case PARTS_FLASH:
		dirty = parts_flash_update(&state->flash, passed_time);
		break;
	case PARTS_FLAT:
		dirty = parts_flat_update(&state->flat, passed_time);
		break;
	case PARTS_MOVIE:
		if (state->movie.mc && state->movie.sprite_no >= 0) {
			movie_draw(state->movie.mc, sact_try_get_sprite(state->movie.sprite_no));
			dirty = true;
		}
		break;
	default:
		break;
	}
	return dirty;
}

static void parts_update_loop(struct parts *parts, int passed_time, bool occluded)
{
	bool visible = !occluded && parts_is_visible(parts);
	struct parts_state *display = parts_display_state(parts);
	bool state_dirty = false;
	if (!visible && display->type == PARTS_FLAT &&
			!display->flat.needs_advance && display->flat.pending_seek_delta <= 0) {
		state_dirty = false;
	} else {
		state_dirty = parts_state_update(display, passed_time);
	}
	if (display != &parts->states[PARTS_STATE_DEFAULT]
			&& parts->states[PARTS_STATE_DEFAULT].type == PARTS_FLAT
			&& parts->states[PARTS_STATE_DEFAULT].flat.needs_advance) {
		state_dirty |= parts_state_update(&parts->states[PARTS_STATE_DEFAULT], 0);
	}
	bool ui_dirty = false;
	if (parts->scrollbar) ui_dirty |= parts_scrollbar_update(parts, passed_time);
	if (parts->message_box) ui_dirty |= parts_message_window_update(parts, passed_time);
	if ((state_dirty && visible) || (ui_dirty && !occluded))
		parts_dirty(parts);
}

static void parts_update_animation(int passed_time, int scaled_time)
{
	struct parts *cover = NULL;
	struct parts *p;
	PARTS_LIST_FOREACH_REVERSE(p) {
		if (parts_sprite_is_fullscreen_opaque(&p->sp)) {
			cover = p;
			break;
		}
	}
	bool occluded = (cover != NULL);
	struct parts *parts;
	PARTS_LIST_FOREACH(parts) {
		if (parts == cover)
			occluded = false;
		parts_update_loop(parts, parts->enable_speed_up ? scaled_time : passed_time, occluded);
	}
}

void parts_release(int parts_no)
{
	struct ht_slot *slot = ht_put_int(parts_table, parts_no, NULL);
	if (!slot->value)
		return;

	struct parts *parts = slot->value;
	struct parts *root = parts;
	while (root->parent) root = root->parent;
	if (root->active_part == parts_no) root->active_part = 0;
	parts_input_reset_part(parts);
	parts_widget_free(parts);
	parts_panel_free(parts);
	parts_scrollbar_free(parts);
	parts_message_window_free(parts);
	parts_clear_motion(parts);
	for (int i = 0; i < PARTS_NR_STATES; i++) {
		parts_state_free(&parts->states[i]);
		free_string(parts->sound_names[i]);
	}

	// break parent/child relationships
	while (!TAILQ_EMPTY(&parts->children)) {
		struct parts *child = TAILQ_FIRST(&parts->children);
		TAILQ_REMOVE(&parts->children, child, child_list_entry);
		child->parent = NULL;
	}
	if (parts->parent) {
		TAILQ_REMOVE(&parts->parent->children, parts, child_list_entry);
		parts->parent = NULL;
	}

	parts_list_remove(parts);
	dirty_list_remove(parts);
	free_string(parts->comment);
	free_string(parts->user_component_name);
	if (parts->text_box_text)
		free_string(parts->text_box_text);
	free(parts->radio_children);
	while (parts->user_values) {
		struct parts_user_value *v = parts->user_values;
		parts->user_values = v->next;
		free_string(v->key);
		free_string(v->value);
		free(v);
	}
	free(parts);
	slot->value = NULL;
	if (parts_no >= 1000010000 && parts_no < parts_current->next_free_no)
		parts_current->next_free_no = parts_no;
	parts_engine_dirty();
}

void parts_release_all(void)
{
	while (!TAILQ_EMPTY(&parts_list)) {
		struct parts *parts = TAILQ_FIRST(&parts_list);
		parts_release(parts->no);
	}
	parts_current->next_free_no = 1000010000;

	for (int i = 0; i < parts_nr_numeral_fonts; i++) {
		struct parts_numeral_font *font = &parts_numeral_fonts[i];
		for (int i = 0; i < 12; i++) {
			if (font->cg[i].handle)
				gfx_delete_texture(&font->cg[i]);
		}
	}

	free(parts_numeral_fonts);
	parts_numeral_fonts = NULL;
	parts_nr_numeral_fonts = 0;
}

struct parts_context *parts_context_select(struct parts_context *context)
{
	struct parts_context *previous = parts_current;
	parts_current = context;
	parts_render_gen++;
	return previous;
}

void parts_background_render(struct sprite *sprite)
{
	struct parts_context *context = (void*)((char*)sprite - offsetof(struct parts_context, sprite));
	struct parts_context *previous = parts_context_select(context);
	struct parts *parts;
	PARTS_LIST_FOREACH(parts) parts_render(parts);
	parts_context_select(previous);
}

struct parts_context *parts_context_new_background(void)
{
	struct parts_context *context = xcalloc(1, sizeof(*context));
	TAILQ_INIT(&context->list);
	TAILQ_INIT(&context->dirty);
	context->table = ht_create(16384);
	context->next_free_no = 1000010000;
	context->origin = root_pos;
	context->background = true;
	// BackScene is one layer below the live graph, regardless of component Z.
	context->sprite.z = INT_MIN;
	context->sprite.z2 = INT_MIN;
	context->sprite.has_pixel = true;
	context->sprite.has_alpha = true;
	context->sprite.render = parts_background_render;
	return context;
}

void parts_context_free(struct parts_context *context)
{
	if (!context) return;
	assert(context != parts_current);
	struct parts_context *previous = parts_context_select(context);
	parts_release_all();
	ht_free_int(context->table);
	scene_unregister_sprite(&context->sprite);
	parts_context_select(previous);
	free(context);
}

static bool parts_engine_initialized = false;

void PE_enable_multi_controller(void)
{
	if (parts_multi_controller)
		return;
	assert(!parts_engine_initialized);
	parts_multi_controller = true;
}

bool PE_Init(void)
{
	if (parts_engine_initialized)
		return true;
	// XXX: Oyako Rankan doesn't call ChipmunkSpriteEngine.Init
	sact_init(16, CHIPMUNK_SPRITE_ENGINE);
	parts_table = ht_create(16384);
	parts_current->next_free_no = 1000010000;
	parts_render_init();
	parts_debug_init();
	ctrl_stack_init();
	parts_engine_initialized = true;
	return true;
}

void PE_Reset(void)
{
	PE_ClearBackScene();
	parts_input_reset();
	PE_ReleaseActivities();
	PE_ReleaseAllParts();
	parts_cg_cache_clear();
	parts_cproc_cache_clear();
	parts_flat_cache_clear();
	PE_ReleaseMessage();
	ctrl_stack_init();
	sact_ModuleFini();
}

static bool parts_has_dirty_parent(struct parts *parts)
{
	for (struct parts *parent = parts->parent; parent; parent = parent->parent) {
		if (parent->dirty)
			return true;
	}
	return false;
}

static void parts_combine_params(struct parts_params *parent, struct parts_params *child,
		struct parts_params *out)
{
	out->z = parent->z + child->z;
	out->pos = (SDL_FPoint) { parent->pos.x + child->pos.x, parent->pos.y + child->pos.y };
	out->show = parent->show && child->show;
	out->alpha = parent->alpha * (child->alpha / 255.0f);
	out->scale.x = parent->scale.x * child->scale.x;
	out->scale.y = parent->scale.y * child->scale.y;
	out->rotation.x = parent->rotation.x + child->rotation.x;
	out->rotation.y = parent->rotation.y + child->rotation.y;
	out->rotation.z = parent->rotation.z + child->rotation.z;
	out->add_color.r = min(255, parent->add_color.r + child->add_color.r);
	out->add_color.g = min(255, parent->add_color.g + child->add_color.g);
	out->add_color.b = min(255, parent->add_color.b + child->add_color.b);
	out->multiply_color.r = parent->multiply_color.r * (child->multiply_color.r / 255.0f);
	out->multiply_color.g = parent->multiply_color.g * (child->multiply_color.g / 255.0f);
	out->multiply_color.b = parent->multiply_color.b * (child->multiply_color.b / 255.0f);
}

static void parts_update_component_mat(struct parts *parts, mat4 parent_matrix)
{
	if (parts->parent) {
		parts_combine_params(&parts->parent->global, &parts->local, &parts->global);
	}
	mat4 matrix;
	if (ain->version >= 12) {
		glm_mat4_copy(parent_matrix, matrix);
		parts_apply_local_matrix(parts, matrix);
		parts->global.pos = (SDL_FPoint){matrix[3][0], matrix[3][1]};
		if (parts->world_matrix_self == parts) {
			glm_mat4_copy(matrix, parts->world_matrix);
			parts->world_matrix_gen = parts_render_gen;
		}
	}
	if (parts_get_sprite_z(parts) != parts->sp.z
			|| parts_get_sprite_z2(parts) != parts->sp.z2) {
		parts_list_resort(parts);
	}

	if (parts->dirty) {
		TAILQ_REMOVE(&dirty_list, parts, dirty_list_entry);
		parts->dirty = false;
	}

	if (ain->version < 12) parts_do_layout(parts);

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_update_component_mat(child, matrix);
	}
}

static void parts_update_component(struct parts *parts)
{
	mat4 parent_matrix = GLM_MAT4_IDENTITY_INIT;
	if (ain->version >= 12) {
		if (parts->parent) {
			parts_world_matrix(parts->parent, parent_matrix);
		} else if (root_pos.x != 0 || root_pos.y != 0) {
			glm_translate(parent_matrix, (vec3){root_pos.x, root_pos.y, 0});
		}
	}
	parts_update_component_mat(parts, parent_matrix);
}

void PE_UpdateComponent(possibly_unused int passed_time)
{
	// After loading a save, the game script may compute a negative time delta
	// because it stores an absolute system.GetTime() value in the save data,
	// which is meaningless across process restarts.
	if (passed_time < 0)
		passed_time = 0;
	while (!TAILQ_EMPTY(&dirty_list)) {
		// pop parts object from dirty list
		struct parts *parts = TAILQ_FIRST(&dirty_list);
		TAILQ_REMOVE(&dirty_list, parts, dirty_list_entry);
		parts->dirty = false;

		// update parent
		struct parts *parent;
		if (parts->pending_parent >= 0 && (parent = parts_try_get(parts->pending_parent))) {
			if (parts->parent) {
				TAILQ_REMOVE(&parts->parent->children, parts, child_list_entry);
			}
			parts->parent = parent;
			TAILQ_INSERT_TAIL(&parent->children, parts, child_list_entry);

			// if parent is layout box, mark it dirty so that it can re-layout its children
			if (parent->states[0].type == PARTS_LAYOUT_BOX)
				parts_component_dirty(parent);
		}
		// TODO: should the child be orphaned if it already has a parent and an invalid
		//       parent no is given?
		parts->pending_parent = -1;

		// don't do anything here if parent is dirty
		if (parts_has_dirty_parent(parts))
			continue;

		// update child params
		parts_update_component(parts);
	}
	if (ain->version >= 12) {
		// Resolve all pending parent links first, then measure children before parents.
		struct parts *parts;
		PARTS_LIST_FOREACH(parts) {
			if (!parts->parent) {
				parts_update_layout_tree(parts);
			}
		}
		parts_scrollbar_update_links();
		uint64_t gen_start = ++parts_render_gen;
		PARTS_LIST_FOREACH(parts) {
			if (!parts->parent) {
				parts_update_component(parts);
			}
		}
		if (parts_render_gen != gen_start) {
			PARTS_LIST_FOREACH(parts) {
				if (parts->world_matrix_self == parts && parts->world_matrix_gen >= gen_start)
					parts->world_matrix_gen = parts_render_gen;
			}
		}
	}
}

static void parts_prewarm_offscreen_cproc(void)
{
	uint64_t freq = SDL_GetPerformanceFrequency();
	uint64_t budget = freq ? (freq * 4) / 1000 : 0;
	uint64_t t_start = SDL_GetPerformanceCounter();
	int built = 0;
	struct parts *parts;
	PARTS_LIST_FOREACH(parts) {
		if (parts->textures_suspended || !parts_is_visible(parts))
			continue;
		struct parts_state *state = parts_display_state(parts);
		if (state->type != PARTS_CONSTRUCTION_PROCESS || !state->cproc.needs_build)
			continue;
		if (parts_cg_on_screen(parts, &state->common))
			continue;
		parts_build_construction_process(parts, &state->cproc);
		built++;
		if (built >= 4 || (budget && SDL_GetPerformanceCounter() - t_start >= budget))
			break;
	}
}

bool parts_message_window_show = true;
bool parts_frame_did_heavy_load = false;

void PE_Update(int passed_time, bool message_window_show)
{
	PE_UpdateWithTimeScale(passed_time, passed_time, message_window_show);
}

void PE_UpdateWithTimeScale(int passed_time, int scaled_time, bool message_window_show)
{
	parts_message_window_show = message_window_show;
	PE_UpdateComponent(passed_time);
	audio_update();
	parts_update_animation(passed_time, scaled_time);
	PE_UpdateInputState(passed_time);
	parts_scrollbar_notify_changes();
	if (!parts_frame_did_heavy_load && !parts_input_is_busy()) {
		bool was_dirty = parts_is_dirty();
		parts_prewarm_offscreen_cproc();
		if (!was_dirty)
			parts_engine_clean();
	}
	parts_frame_did_heavy_load = false;
	parts_render_update();
}

void PE_UpdateParts(int passed_time, possibly_unused bool is_skip, bool message_window_show)
{
	parts_message_window_show = message_window_show;
	audio_update();
	parts_update_animation(passed_time, passed_time);
	parts_render_update();
}

void PE_SetEnableSpeedUp(int parts_no, bool enable)
{
	parts_get(parts_no)->enable_speed_up = enable;
}

bool PE_IsEnableSpeedUp(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? parts->enable_speed_up : true;
}

void PE_SetDelegateIndex(int parts_no, int delegate_index)
{
	parts_get(parts_no)->delegate_index = delegate_index;
}

int PE_GetDelegateIndex(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? parts->delegate_index : -1;
}

bool PE_SetPartsCG(int parts_no, struct string *cg_name, int sprite_deform, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	parts->sprite_deform = sprite_deform;
	if (!cg_name || *(cg_name->text) == '\0') {
		parts_state_reset(&parts->states[state], PARTS_CG);
		parts_dirty(parts);
		return true;
	}

	struct parts_cg *cg = parts_get_cg(parts, state);
	return parts_cg_set(parts, cg, cg_name);
}

bool PE_SetPartsCG_by_index(int parts_no, int cg_no, int sprite_deform, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	parts->sprite_deform = sprite_deform;
	if (!cg_no) {
		parts_state_reset(&parts->states[state], PARTS_CG);
		parts_dirty(parts);
		return true;
	}

	struct parts_cg *cg = parts_get_cg(parts, state);
	return parts_cg_set_by_index(parts, cg, cg_no);
}

// XXX: Rance Quest
bool PE_SetPartsCG_by_string_index(int parts_no, struct string *cg_name,
		int sprite_deform, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	parts->sprite_deform = sprite_deform;
	if (!cg_name) {
		parts_state_reset(&parts->states[state], PARTS_CG);
		parts_dirty(parts);
		return true;
	}

	struct parts_cg *cg = parts_get_cg(parts, state);
	int cg_no = atoi(cg_name->text);
	if (cg_no) {
		if (!parts_cg_set_by_index(parts, cg, atoi(cg_name->text)))
			return false;
		cg->name = string_ref(cg_name);
		return true;
	} else if (!memcmp(cg_name->text, "<save>SaveData\\", 15)) {
		char *path = savedir_path(cg_name->text + 15);
		bool result = _parts_cg_set(parts, cg, cg_load_file(path), 0, string_ref(cg_name));
		free(path);
		return result;
	} else {
		VM_ERROR("Invalid CG name: %s", display_sjis0(cg_name->text));
	}
}

void PE_GetPartsCGName(int parts_no, struct string **cg_name, int state)
{
	if (!parts_state_valid(--state))
		return;
	struct parts_cg *cg = parts_get_cg(parts_get(parts_no), state);
	if (cg->name) {
		if (*cg_name)
			free_string(*cg_name);
		*cg_name = string_dup(cg->name);
	}
}

bool PE_SetPartsCGSurfaceArea(int parts_no, int x, int y, int w, int h, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_cg *cg = parts_get_cg(parts, state);
	parts_set_surface_area(parts, &cg->common, x, y, w, h);
	return true;
}

void PE_GetPartsCGSurfaceArea(int parts_no, int *x, int *y, int *w, int *h, int state)
{
	if (!parts_state_valid(--state))
		return;

	struct parts_cg *cg = parts_get_cg(parts_get(parts_no), state);
	*x = cg->common.surface_area.x;
	*y = cg->common.surface_area.y;
	*w = cg->common.surface_area.w;
	*h = cg->common.surface_area.h;
}

int PE_GetPartsCGNumber(int parts_no, int state)
{
	if (!parts_state_valid(--state)) {
		return false;
	}

	return parts_get_cg(parts_get(parts_no), state)->no;
}

static bool _parts_animation_set_cg(struct parts *parts, struct parts_animation *anim,
		int start_no, int nr_frames, int frame_time,
		struct cg *(*load_cg)(int no, void *data), void *data)
{
	int w = 0, h = 0;
	Texture *frames = xcalloc(nr_frames, sizeof(Texture));
	for (int i = 0; i < nr_frames; i++) {
		struct cg *cg = load_cg(start_no + i, data);
		if (!cg) {
			for (int j = 0; j < i; j++) {
				gfx_delete_texture(&frames[j]);
			}
			free(frames);
			return false;
		}
		gfx_init_texture_with_cg(&frames[i], cg);
		w = max(w, cg->metrics.w);
		h = max(h, cg->metrics.h);
		cg_free(cg);
	}

	parts_set_dims(parts, &anim->common, w, h);
	for (unsigned i = 0; i < anim->nr_frames; i++) {
		gfx_delete_texture(&anim->frames[i]);
	}
	free(anim->frames);
	anim->start_no = start_no;
	anim->frames = frames;
	anim->nr_frames = nr_frames;
	anim->frame_time = frame_time;
	anim->elapsed = 0;
	anim->current_frame = 0;
	anim->common.texture = frames[0];
	return true;
}

static struct cg *load_loop_cg_by_index(int no, void *_)
{
	return asset_cg_load(no);
}

bool parts_animation_set_cg_by_index(struct parts *parts, struct parts_animation *anim,
		int cg_no, int nr_frames, int frame_time)
{
	return _parts_animation_set_cg(parts, anim, cg_no, nr_frames, frame_time,
			load_loop_cg_by_index, NULL);
}

bool PE_SetLoopCG_by_index(int parts_no, int cg_no, int nr_frames, int frame_time, int state)
{
	if (!parts_state_valid(--state))
		return false;
	if (nr_frames <= 0 || nr_frames > 10000) {
		WARNING("Invalid frame count: %d", nr_frames);
		return false;
	}

	struct parts *parts = parts_get(parts_no);
	struct parts_animation *anim = parts_get_animation(parts, state);
	return parts_animation_set_cg_by_index(parts, anim, cg_no, nr_frames, frame_time);
}

static struct cg *load_loop_cg_by_name(int no, void *data)
{
	int unused_no;
	struct string *cg_name = string_format((struct string*)data, (union vm_value){.i=no}, STRFMT_INT);
	struct cg *cg = asset_cg_load_by_name(cg_name->text, &unused_no);
	free_string(cg_name);
	return cg;
}

bool parts_animation_set_cg(struct parts *parts, struct parts_animation *anim,
		struct string *cg_name, int start_no, int nr_frames, int frame_time)
{
	bool r = _parts_animation_set_cg(parts, anim, start_no, nr_frames, frame_time,
			load_loop_cg_by_name, cg_name);
	if (r) {
		if (anim->cg_name)
			free_string(anim->cg_name);
		anim->cg_name = string_dup(cg_name);
	}
	return r;
}

bool PE_SetLoopCG(int parts_no, struct string *cg_name, int start_no, int nr_frames,
	int frame_time, int state)
{
	if (!parts_state_valid(--state))
		return false;
	if (nr_frames <= 0 || nr_frames > 10000) {
		WARNING("Invalid frame count: %d", nr_frames);
		return false;
	}

	struct parts *parts = parts_get(parts_no);
	struct parts_animation *anim = parts_get_animation(parts, state);
	return parts_animation_set_cg(parts, anim, cg_name, start_no, nr_frames, frame_time);
}

bool PE_SetLoopCGSurfaceArea(int parts_no, int x, int y, int w, int h, int state)
{
	if (!parts_state_valid(--state)) {
		WARNING("Invalid parts state: %d", state);
		return false;
	}

	struct parts *parts = parts_get(parts_no);
	struct parts_animation *anim = parts_get_animation(parts, state);
	parts_set_surface_area(parts, &anim->common, x, y, w, h);
	return true;
}

static void _parts_set_gauge_cg(struct parts *parts, struct parts_gauge *g, struct cg *cg)
{
	gfx_delete_texture(&g->cg);
	gfx_delete_texture(&g->common.texture);
	gfx_init_texture_with_cg(&g->cg, cg);

	gfx_init_texture_rgba(&g->common.texture, g->cg.w, g->cg.h, (SDL_Color){0,0,0,255});
	gfx_copy_with_alpha_map(&g->common.texture, 0, 0, &g->cg, 0, 0, g->cg.w, g->cg.h);

	parts_set_dims(parts, &g->common, g->cg.w, g->cg.h);

	parts_dirty(parts);
}

bool parts_gauge_set_cg(struct parts *parts, struct parts_gauge *g, struct string *cg_name)
{
	int cg_no;
	struct cg *cg = asset_cg_load_by_name(cg_name->text, &cg_no);
	if (!cg)
		return false;
	_parts_set_gauge_cg(parts, g, cg);
	cg_free(cg);
	g->cg_no = cg_no;
	if (g->cg_name)
		free_string(g->cg_name);
	g->cg_name = string_dup(cg_name);
	return true;
}

bool parts_gauge_set_cg_by_index(struct parts *parts, struct parts_gauge *g, int cg_no)
{
	struct cg *cg = asset_cg_load(cg_no);
	if (!cg)
		return false;
	_parts_set_gauge_cg(parts, g, cg);
	cg_free(cg);
	g->cg_no = cg_no;
	return true;
}

bool PE_SetHGaugeCG(int parts_no, struct string *cg_name, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_gauge *g = parts_get_hgauge(parts, state);
	return parts_gauge_set_cg(parts, g, cg_name);
}

bool PE_SetHGaugeCG_by_index(int parts_no, int cg_no, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_gauge *g = parts_get_hgauge(parts, state);
	return parts_gauge_set_cg_by_index(parts, g, cg_no);
}

bool PE_SetVGaugeCG(int parts_no, struct string *cg_name, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_gauge *g = parts_get_vgauge(parts, state);
	return parts_gauge_set_cg(parts, g, cg_name);
}

bool PE_SetVGaugeCG_by_index(int parts_no, int cg_no, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_gauge *g = parts_get_vgauge(parts, state);
	return parts_gauge_set_cg_by_index(parts, g, cg_no);
}

void parts_hgauge_set_rate(struct parts *parts, struct parts_gauge *g, float rate)
{
	if (!g->common.texture.handle) {
		WARNING("HGauge texture uninitialized");
		return;
	}
	int pixels = rate * g->cg.w;
	gfx_copy_with_alpha_map(&g->common.texture, 0, 0, &g->cg, 0, 0, pixels, g->cg.h);
	gfx_fill_amap(&g->common.texture, pixels, 0, g->cg.w - pixels, g->cg.h, 0);
	parts_dirty(parts);
	g->rate = rate;
}

void parts_vgauge_set_rate(struct parts *parts, struct parts_gauge *g, float rate)
{
	if (!g->common.texture.handle) {
		WARNING("VGauge texture uninitialized");
		return;
	}
	int pixels = rate * g->cg.h;
	gfx_copy_with_alpha_map(&g->common.texture, 0, pixels, &g->cg, 0, pixels, g->cg.w, g->cg.h - pixels);
	gfx_fill_amap(&g->common.texture, 0, 0, g->cg.w, pixels, 0);
	parts_dirty(parts);
	g->rate = rate;
}

bool PE_SetHGaugeRate(int parts_no, float numerator, float denominator, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_gauge *g = parts_get_hgauge(parts, state);
	g->numerator = numerator;
	g->denominator = denominator;
	float rate = denominator != 0.0f ? max(0.0f, min(1.0f, numerator / denominator)) : 0.0f;
	g->rate = rate;
	if (g->common.texture.handle)
		parts_hgauge_set_rate(parts, g, rate);
	return true;
}

bool PE_SetHGaugeRate_int(int parts_no, int numerator, int denominator, int state)
{
	return PE_SetHGaugeRate(parts_no, numerator, denominator, state);
}

bool PE_SetVGaugeRate(int parts_no, float numerator, float denominator, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_gauge *g = parts_get_vgauge(parts, state);
	g->numerator = numerator;
	g->denominator = denominator;
	float rate = denominator != 0.0f ? max(0.0f, min(1.0f, numerator / denominator)) : 0.0f;
	g->rate = rate;
	if (g->common.texture.handle)
		parts_vgauge_set_rate(parts, g, rate);
	return true;
}

bool PE_SetVGaugeRate_int(int parts_no, int numerator, int denominator, int state)
{
	return PE_SetVGaugeRate(parts_no, numerator, denominator, state);
}

float PE_GetHGaugeNumerator(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return 0.0f;
	struct parts *parts = parts_try_get(parts_no);
	if (!parts || parts->states[state].type != PARTS_HGAUGE)
		return 0.0f;
	return parts->states[state].gauge.numerator;
}

float PE_GetHGaugeDenominator(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return 1.0f;
	struct parts *parts = parts_try_get(parts_no);
	if (!parts || parts->states[state].type != PARTS_HGAUGE)
		return 1.0f;
	return parts->states[state].gauge.denominator;
}

float PE_GetVGaugeNumerator(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return 0.0f;
	struct parts *parts = parts_try_get(parts_no);
	if (!parts || parts->states[state].type != PARTS_VGAUGE)
		return 0.0f;
	return parts->states[state].gauge.numerator;
}

float PE_GetVGaugeDenominator(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return 1.0f;
	struct parts *parts = parts_try_get(parts_no);
	if (!parts || parts->states[state].type != PARTS_VGAUGE)
		return 1.0f;
	return parts->states[state].gauge.denominator;
}

bool PE_SetHGaugeSurfaceArea(int parts_no, int x, int y, int w, int h, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_gauge *g = parts_get_hgauge(parts, state);
	parts_set_surface_area(parts, &g->common, x, y, w, h);
	return true;
}

bool PE_SetVGaugeSurfaceArea(int parts_no, int x, int y, int w, int h, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_gauge *g = parts_get_vgauge(parts, state);
	parts_set_surface_area(parts, &g->common, x, y, w, h);
	return true;
}

bool PE_SetNumeralCG(int parts_no, struct string *cg_name, int state)
{
	if (!parts_state_valid(--state))
		return false;
	struct parts *p = parts_get(parts_no);
	struct parts_numeral *n = parts_get_numeral(p, state);
	n->font_no = parts_load_numeral_font_separate_string(cg_name);
	n->show_type = 0;
	free_string(n->cg_name);
	n->cg_name = string_dup(cg_name);
	return parts_numeral_update(p, n);
}

bool PE_SetNumeralCG_by_index(int parts_no, int cg_no, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *p = parts_get(parts_no);
	struct parts_numeral *n = parts_get_numeral(p, state);
	n->font_no = parts_load_numeral_font_separate(cg_no);
	n->show_type = 0;
	return parts_numeral_update(p, n);
}

bool PE_SetNumeralLinkedCGNumberWidthWidthList_by_index(int parts_no, int cg_no,
		int w0, int w1, int w2, int w3, int w4, int w5, int w6, int w7, int w8,
		int w9, int w_minus, int w_comma, int state)
{
	if (!parts_state_valid(--state))
		return false;

	if (!asset_exists(ASSET_CG, cg_no))
		return false;

	int w[12] = { w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w_minus, w_comma };
	struct parts *p = parts_get(parts_no);
	struct parts_numeral *n = parts_get_numeral(p, state);
	n->font_no = parts_load_numeral_font_combined(cg_no, w);
	n->show_type = 1;
	return parts_numeral_update(p, n);
}

bool PE_SetNumeralLinkedCGNumberWidthWidthList(int parts_no, struct string *cg_name,
		int w0, int w1, int w2, int w3, int w4, int w5, int w6, int w7, int w8,
		int w9, int w_minus, int w_comma, int state)
{
	if (!parts_state_valid(--state))
		return false;

	int no;
	if (!asset_exists_by_name(ASSET_CG, cg_name->text, &no))
		return false;

	int w[12] = { w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w_minus, w_comma };
	struct parts *p = parts_get(parts_no);
	struct parts_numeral *n = parts_get_numeral(p, state);
	n->font_no = parts_load_numeral_font_combined(no, w);
	n->show_type = 1;
	free_string(n->cg_name);
	n->cg_name = string_dup(cg_name);
	return parts_numeral_update(p, n);
}

bool PE_SetNumeralNumber(int parts_no, int n, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_numeral *numeral = parts_get_numeral(parts, state);
	return parts_numeral_set_number(parts, numeral, n);
}

bool PE_SetNumeralShowComma(int parts_no, bool show_comma, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_numeral *num = parts_get_numeral(parts, state);
	if (num->show_comma == show_comma)
		return true;

	num->show_comma = show_comma;
	parts_numeral_update(parts, num);
	return true;
}

bool PE_SetNumeralSpace(int parts_no, int space, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_numeral *num = parts_get_numeral(parts, state);
	if (num->space == space)
		return true;

	num->space = space;
	parts_numeral_update(parts, num);
	return true;
}

bool PE_SetNumeralLength(int parts_no, int length, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_numeral *num = parts_get_numeral(parts, state);
	if (num->length == length)
		return true;

	num->length = max(ain->version >= 12 ? 0 : 1, length);
	num->have_num = true;
	parts_numeral_update(parts, num);
	return true;
}

bool PE_SetNumeralSurfaceArea(int parts_no, int x, int y, int w, int h, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	struct parts_numeral *n = parts_get_numeral(parts, state);
	parts_set_surface_area(parts, &n->common, x, y, w, h);
	return true;
}

static struct parts_numeral *try_get_numeral(int no, int state)
{
	if (!parts_state_valid(--state)) return NULL;
	struct parts *p = parts_try_get(no);
	return p && p->states[state].type == PARTS_NUMERAL ? &p->states[state].num : NULL;
}

void PE_SetNumeralFont(int no, int face, int size, int r, int g, int b, float bold,
		int er, int eg, int eb, float edge, int state)
{
	if (!parts_state_valid(--state)) return;
	struct parts *p = parts_get(no);
	struct parts_numeral *n = parts_get_numeral(p, state);
	n->show_type = 2;
	n->ts.face = face;
	n->ts.size = size;
	n->ts.font_size = NULL;
	n->ts.color = (SDL_Color){r, g, b, 255};
	n->ts.bold_width = bold;
	n->ts.weight = bold * 1000;
	n->ts.edge_color = (SDL_Color){er, eg, eb, 255};
	text_style_set_edge_width(&n->ts, edge);
	parts_numeral_update(p, n);
}

void PE_GetNumeralFont(int no, int *face, int *size, int *r, int *g, int *b, float *bold,
		int *er, int *eg, int *eb, float *edge, int state)
{
	struct parts_numeral *n = try_get_numeral(no, state);
	struct text_style *ts = n ? &n->ts : &default_text_style;
	*face = ts->face;
	*size = ts->size;
	*r = ts->color.r; *g = ts->color.g; *b = ts->color.b;
	*bold = ts->bold_width;
	*er = ts->edge_color.r; *eg = ts->edge_color.g; *eb = ts->edge_color.b;
	*edge = ts->edge_left;
}

void PE_SetNumeralFullPitch(int no, bool full_pitch, int state)
{
	if (!parts_state_valid(--state)) return;
	struct parts *p = parts_get(no);
	struct parts_numeral *n = parts_get_numeral(p, state);
	n->full_pitch = full_pitch;
	parts_numeral_update(p, n);
}

void PE_SetNumeralShowPadding(int no, bool show, int state)
{
	if (!parts_state_valid(--state)) return;
	struct parts *p = parts_get(no);
	struct parts_numeral *n = parts_get_numeral(p, state);
	n->show_padding = show;
	parts_numeral_update(p, n);
}

void PE_SetNumeralShowType(int no, int type, int state)
{
	if (!parts_state_valid(--state) || type < 0 || type > 2) return;
	struct parts *p = parts_get(no);
	struct parts_numeral *n = parts_get_numeral(p, state);
	n->show_type = type;
	parts_numeral_update(p, n);
}

#define NUMERAL_GETTER(type, name, member, fallback) \
	type PE_##name(int no, int state) { \
		struct parts_numeral *n = try_get_numeral(no, state); \
		return n ? n->member : fallback; \
	}
NUMERAL_GETTER(bool, IsNumeralShowPadding, show_padding, true)
NUMERAL_GETTER(int, GetNumeralCGType, show_type, 0)
NUMERAL_GETTER(int, GetNumeralNumber, num, 0)
NUMERAL_GETTER(bool, IsNumeralShowComma, show_comma, false)
NUMERAL_GETTER(int, GetNumeralSpace, space, 0)
NUMERAL_GETTER(int, GetNumeralLength, length, 0)
#undef NUMERAL_GETTER

struct string *PE_GetNumeralCGName(int no, int state)
{
	struct parts_numeral *n = try_get_numeral(no, state);
	return string_ref(n ? n->cg_name : &EMPTY_STRING);
}

void PE_GetNumeralCGNumberWidthList(int no, int *w0, int *w1, int *w2, int *w3,
		int *w4, int *w5, int *w6, int *w7, int *w8, int *w9, int *minus, int *comma, int state)
{
	struct parts_numeral *n = try_get_numeral(no, state);
	struct parts_numeral_font *font = n && n->font_no >= 0 && n->font_no < parts_nr_numeral_fonts
		? &parts_numeral_fonts[n->font_no] : NULL;
	int *widths[12] = {w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, minus, comma};
	for (int i = 0; i < 12; i++)
		*widths[i] = font && font->type == PARTS_NUMERAL_FONT_COMBINED ? font->width[i] : 0;
}

void PE_GetNumeralSurfaceArea(int no, int *x, int *y, int *w, int *h, int state)
{
	struct parts_numeral *n = try_get_numeral(no, state);
	Rectangle area = n ? n->common.surface_area : (Rectangle){0};
	*x = area.x; *y = area.y; *w = area.w; *h = area.h;
}

void PE_ReleaseParts(int parts_no)
{
	parts_release(parts_no);
}

void parts_world_matrix(struct parts *parts, mat4 out)
{
	if (ain->version < 12) {
		glm_mat4_identity(out);
		glm_translate(out, (vec3){parts->global.pos.x, parts->global.pos.y, 0});
		glm_rotate_z(out, glm_rad(parts->local.rotation.z), out);
		if (parts->local.rotation.x != 0.0f)
			glm_rotate_x(out, glm_rad(-parts->local.rotation.x), out);
		if (parts->local.rotation.y != 0.0f)
			glm_rotate_y(out, glm_rad(parts->local.rotation.y), out);
		glm_scale(out, (vec3){parts->global.scale.x, parts->global.scale.y, 1});
		return;
	}
	if (parts->world_matrix_self == parts && parts->world_matrix_gen == parts_render_gen) {
		glm_mat4_copy(parts->world_matrix, out);
		return;
	}
	// Child positions live in their parent's transformed coordinate system.
	// Multiplying full matrices also retains shear from nonuniform scaling.
	if (parts->parent) {
		parts_world_matrix(parts->parent, out);
	} else {
		glm_mat4_identity(out);
		if (root_pos.x != 0 || root_pos.y != 0)
			glm_translate(out, (vec3){root_pos.x, root_pos.y, 0});
	}
	parts_apply_local_matrix(parts, out);
	if (parts->world_matrix_self == parts) {
		glm_mat4_copy(out, parts->world_matrix);
		parts->world_matrix_gen = parts_render_gen;
	}
}

void PE_GetComponentAbsoluteSquarePos(int no, float *x0, float *y0,
		float *x1, float *y1, float *x2, float *y2, float *x3, float *y3, int state)
{
	*x0 = *y0 = *x1 = *y1 = *x2 = *y2 = *x3 = *y3 = 0;
	if (!parts_state_valid(--state)) return;
	struct parts *parts = parts_try_get(no);
	if (!parts) return;
	struct parts_common *c = &parts->states[state].common;
	mat4 matrix;
	parts_world_matrix(parts, matrix);
	float x = c->hitbox.x - (ain->version < 12 ? parts->local.pos.x : 0);
	float y = c->hitbox.y - (ain->version < 12 ? parts->local.pos.y : 0);
	float *xs[] = {x0, x1, x2, x3}, *ys[] = {y0, y1, y2, y3};
	for (int i = 0; i < 4; i++) {
		vec4 v = {x + (i & 1 ? c->hitbox.w : 0), y + (i & 2 ? c->hitbox.h : 0), 0, 1};
		if (parts->states[state].type == PARTS_RECT_DETECTION && parts->states[state].rect.custom) {
			v[0] = parts->states[state].rect.points[i].x + c->origin_offset.x;
			v[1] = parts->states[state].rect.points[i].y + c->origin_offset.y;
		}
		glm_mat4_mulv(matrix, v, v);
		*xs[i] = v[0]; *ys[i] = v[1];
	}
}

void PE_SetComment(int parts_no, struct string *comment)
{
	struct parts *parts = parts_get(parts_no);
	struct string *copy = string_ref(comment);
	free_string(parts->comment);
	parts->comment = copy;
}

struct string *PE_GetComment(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return string_ref(parts ? parts->comment : &EMPTY_STRING);
}

void PE_ReleaseAllParts(void)
{
	parts_release_all();
}

void PE_ReleaseAllPartsWithoutSystem(void)
{
	// FIXME: what's the difference?
	parts_release_all();
}

void PE_ReleaseAllWithoutSystem(struct page **erase_number_list)
{
	// Release all parts not belonging to the system overlay controller
	struct parts *parts = TAILQ_FIRST(&parts_list);
	while (parts) {
		struct parts *next = TAILQ_NEXT(parts, parts_list_entry);
		if (parts->controller_no != PARTS_CONTROLLER_SYSTEM_OVERLAY) {
			*erase_number_list = array_pushback(*erase_number_list,
					(union vm_value){.i = parts->no}, AIN_ARRAY_INT, -1);
			parts_release(parts->no);
		}
		parts = next;
	}

	// Drop all normal controllers and add a fresh default one.
	ctrl_stack.nr_controllers = 0;
	PE_AddController(-1);
}

void PE_SetPos(int parts_no, int x, int y)
{
	parts_set_pos(parts_get(parts_no), (Point){ x, y });
}

void PE_SetZ(int parts_no, int z)
{
	parts_set_z(parts_get(parts_no), z);
}

void PE_SetShow(int parts_no, bool show)
{
	parts_set_show(parts_get(parts_no), show);
}

void PE_SetAlpha(int parts_no, int alpha)
{
	parts_set_alpha(parts_get(parts_no), alpha);
}

void PE_SetPartsDrawFilter(int parts_no, int draw_filter)
{
	parts_get(parts_no)->draw_filter = draw_filter;
}

void PE_SetAddColor(int parts_no, int r, int g, int b)
{
	SDL_Color add_color = {
		min(255, max(0, r)),
		min(255, max(0, g)),
		min(255, max(0, b)),
		255
	};
	parts_set_add_color(parts_get(parts_no), add_color);
}

void PE_SetMultiplyColor(int parts_no, int r, int g, int b)
{
	SDL_Color multiply_color = {
		min(255, max(0, r)),
		min(255, max(0, g)),
		min(255, max(0, b)),
		255
	};
	parts_set_multiply_color(parts_get(parts_no), multiply_color);
}

int PE_GetPartsX(int parts_no)
{
	return parts_get(parts_no)->local.pos.x;
}

int PE_GetPartsY(int parts_no)
{
	return parts_get(parts_no)->local.pos.y;
}

void PE_SetComponentPos(int no, float x, float y)
{
	parts_set_posf(parts_get(no), (SDL_FPoint){x, y});
}

float PE_GetComponentPosX(int no)
{
	return parts_get(no)->local.pos.x;
}

float PE_GetComponentPosY(int no)
{
	return parts_get(no)->local.pos.y;
}

void PE_GetComponentAbsolutePos(int no, float *x, float *y)
{
	struct parts *parts = parts_try_get(no);
	*x = parts ? parts->global.pos.x : 0;
	*y = parts ? parts->global.pos.y : 0;
}

float PE_GetComponentAbsolutePosX(int no)
{
	float x, y;
	PE_GetComponentAbsolutePos(no, &x, &y);
	return x;
}

float PE_GetComponentAbsolutePosY(int no)
{
	float x, y;
	PE_GetComponentAbsolutePos(no, &x, &y);
	return y;
}

int PE_GetPartsWidth(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return 0;
	struct parts *parts = parts_get(parts_no);
	if (ain->version >= 12 && (parts->component_type == 15 || parts->widget
			|| parts->states[state].type == PARTS_LAYOUT_BOX)) {
		int w, h;
		parts_get_layout_size(parts, &w, &h);
		return w;
	}
	return parts->states[state].common.w;
}

int PE_GetPartsHeight(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return 0;
	struct parts *parts = parts_get(parts_no);
	if (ain->version >= 12 && (parts->component_type == 15 || parts->widget
			|| parts->states[state].type == PARTS_LAYOUT_BOX)) {
		int w, h;
		parts_get_layout_size(parts, &w, &h);
		return h;
	}
	return parts->states[state].common.h;
}

int PE_GetPartsUpperLeftPosX(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return 0;
	struct parts *parts = parts_get(parts_no);
	int x = parts->states[state].common.hitbox.x;
	if (ain->version >= 12) {
		mat4 matrix;
		parts_world_matrix(parts, matrix);
		vec4 pos = {x, parts->states[state].common.hitbox.y, 0, 1};
		glm_mat4_mulv(matrix, pos, pos);
		return pos[0];
	}
	if (parts->parent)
		x += parts->parent->global.pos.x;
	return x;
}

int PE_GetPartsUpperLeftPosY(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return 0;
	struct parts *parts = parts_get(parts_no);
	int y = parts->states[state].common.hitbox.y;
	if (ain->version >= 12) {
		mat4 matrix;
		parts_world_matrix(parts, matrix);
		vec4 pos = {parts->states[state].common.hitbox.x, y, 0, 1};
		glm_mat4_mulv(matrix, pos, pos);
		return pos[1];
	}
	if (parts->parent)
		y += parts->parent->global.pos.y;
	return y;
}

int PE_GetPartsZ(int parts_no)
{
	return parts_get(parts_no)->local.z;
}

bool PE_GetPartsShow(int parts_no)
{
	return parts_get(parts_no)->local.show;
}

int PE_GetPartsAlpha(int parts_no)
{
	return parts_get(parts_no)->local.alpha;
}

int PE_GetPartsDrawFilter(int parts_no)
{
	return parts_get(parts_no)->draw_filter;
}

void PE_GetAddColor(int parts_no, int *r, int *g, int *b)
{
	struct parts *parts = parts_get(parts_no);
	*r = parts->local.add_color.r;
	*g = parts->local.add_color.g;
	*b = parts->local.add_color.b;
}

void PE_GetMultiplyColor(int parts_no, int *r, int *g, int *b)
{
	struct parts *parts = parts_get(parts_no);
	*r = parts->local.multiply_color.r;
	*g = parts->local.multiply_color.g;
	*b = parts->local.multiply_color.b;
}

void PE_SetPartsOriginPosMode(int parts_no, int origin_pos_mode)
{
	struct parts *parts = parts_get(parts_no);
	parts_set_origin_mode(parts, origin_pos_mode);
	parts_dirty(parts);
}

int PE_GetPartsOriginPosMode(int parts_no)
{
	return parts_get(parts_no)->origin_mode;
}

void PE_SetParentPartsNumber(int parts_no, int parent_parts_no)
{
	struct parts *parts = parts_get(parts_no);
	if (ain->version >= 12) {
		if (parent_parts_no <= 0)
			parent_parts_no = parts->controller_no == PARTS_CONTROLLER_SYSTEM_OVERLAY
				? ctrl_stack.overlay_id : ctrl_stack.ids[parts->controller_no];
		struct parts *parent = parent_parts_no == parts_no ? NULL : parts_get(parent_parts_no);
		for (struct parts *p = parent; p; p = p->parent)
			if (p == parts) VM_ERROR("Cyclic parts parent relationship");
		if (parts->parent == parent) return;
		if (parts->parent) {
			parts_component_dirty(parts->parent);
			TAILQ_REMOVE(&parts->parent->children, parts, child_list_entry);
		}
		parts->parent = parent;
		if (parent) {
			TAILQ_INSERT_TAIL(&parent->children, parts, child_list_entry);
			parts_change_controller(parts, parent->controller_no);
			parts_component_dirty(parent);
		} else parts->global = parts->local;
		parts->pending_parent = -1;
		parts_component_dirty(parts);
		return;
	}
	parts->pending_parent = parent_parts_no;
	parts_component_dirty(parts);
}

int PE_GetParentPartsNumber(int parts_no)
{
	if (ain->version >= 12 && parts_no <= 0)
		return 0;
	struct parts *parts = parts_get(parts_no);
	if (parts->parent)
		return parts->parent->no;
	return ain->version >= 12 ? 0 : -1;
}

int PE_NumofChild(int no)
{
	struct parts *parent = parts_get(no), *p;
	int count = 0;
	PARTS_FOREACH_CHILD(p, parent) count++;
	return count;
}

int PE_GetChildIndex(int no, int child)
{
	struct parts *parent = parts_get(no), *p;
	int index = 0;
	PARTS_FOREACH_CHILD(p, parent) {
		if (p->no == child) return index;
		index++;
	}
	return -1;
}

int PE_GetChild(int no, int index)
{
	struct parts *parent = parts_get(no), *p;
	if (index >= 0) PARTS_FOREACH_CHILD(p, parent) if (!index--) return p->no;
	return 0;
}

bool PE_IsExistChild(int no, int child) { return PE_GetChildIndex(no, child) >= 0; }
void PE_AddChild(int no, int child) { PE_SetParentPartsNumber(child, no); }

void PE_InsertChild(int no, int index, int child)
{
	PE_SetParentPartsNumber(child, no);
	struct parts *parent = parts_get(no), *p = parts_get(child), *at;
	TAILQ_REMOVE(&parent->children, p, child_list_entry);
	PARTS_FOREACH_CHILD(at, parent) {
		if (index--) continue;
		TAILQ_INSERT_BEFORE(at, p, child_list_entry);
		parts_component_dirty(parent);
		return;
	}
	TAILQ_INSERT_TAIL(&parent->children, p, child_list_entry);
	parts_component_dirty(parent);
}

void PE_RemoveChild(int no, int child)
{
	struct parts *p = parts_try_get(child);
	if (p && p->parent && p->parent->no == no) PE_SetParentPartsNumber(child, -1);
}

void PE_ClearChild(int no)
{
	struct parts *parent = parts_get(no), *p = TAILQ_FIRST(&parent->children);
	while (p) {
		struct parts *next = TAILQ_NEXT(p, child_list_entry);
		PE_RemoveChild(no, p->no);
		p = next;
	}
}

bool PE_SetPartsGroupNumber(possibly_unused int PartsNumber, possibly_unused int GroupNumber)
{
	UNIMPLEMENTED("(%d, %d)", PartsNumber, GroupNumber);
	return true;
}

void PE_SetPartsMessageWindowShowLink(possibly_unused int parts_no, bool message_window_show_link)
{
	struct parts *parts = parts_get(parts_no);
	parts->message_window = message_window_show_link;
}

bool PE_GetPartsMessageWindowShowLink(int parts_no)
{
	return parts_get(parts_no)->message_window;
}

void PE_SetPartsMagX(int parts_no, float scale_x)
{
	struct parts *parts = parts_get(parts_no);
	parts_set_scale_x(parts, scale_x);
}

float PE_GetPartsMagX(int parts_no)
{
	return parts_get(parts_no)->local.scale.x;
}

void PE_SetPartsMagY(int parts_no, float scale_y)
{
	struct parts *parts = parts_get(parts_no);
	parts_set_scale_y(parts, scale_y);
}

float PE_GetPartsMagY(int parts_no)
{
	return parts_get(parts_no)->local.scale.y;
}

void PE_SetPartsRotateX(int parts_no, float rot_x)
{
	parts_set_rotation_x(parts_get(parts_no), rot_x);
}

void PE_SetPartsRotateY(int parts_no, float rot_y)
{
	parts_set_rotation_y(parts_get(parts_no), rot_y);
}

void PE_SetPartsRotateZ(int parts_no, float rot_z)
{
	parts_set_rotation_z(parts_get(parts_no), rot_z);
}

float PE_GetPartsRotateZ(int parts_no)
{
	return parts_get(parts_no)->local.rotation.z;
}

void PE_SetPartsAlphaClipperPartsNumber(int parts_no, int alpha_clipper_parts_no)
{
	struct parts *parts = parts_get(parts_no);
	parts->alpha_clipper_parts_no = alpha_clipper_parts_no;
	parts_dirty(parts);
}

void PE_SetPartsPixelDecide(int parts_no, bool pixel_decide)
{
	parts_get(parts_no)->pixel_decide = pixel_decide;
}

bool PE_SetThumbnailReductionSize(int reduction_size)
{
	UNIMPLEMENTED("(%d)", reduction_size);
	return true;
}

bool PE_SetThumbnailMode(bool mode)
{
	UNIMPLEMENTED("(%s)", mode ? "true" : "false");
	return true;
}

bool PE_save_thumbnail(struct string *filename, int reduction_factor)
{
	if (reduction_factor < 1)
		reduction_factor = 1;

	Texture *src = gfx_main_surface();
	int w, h;
	if (ain->version >= 12) {
		w = reduction_factor;
		h = src->w > 0 ? (int)((int64_t)src->h * w / src->w) : 1;
		if (h < 1)
			h = 1;
	} else {
		w = src->w / reduction_factor;
		h = src->h / reduction_factor;
	}

	// Downscale by repeatedly halving until we are within a factor of two of
	// the target size, then do the final stretch. This avoids the aliasing
	// that a single bilinear minification would otherwise produce.
	Texture tmp, *cur = src;
	bool have_tmp = false;
	while (cur->w / 2 > w && cur->h / 2 > h) {
		Texture next;
		gfx_init_texture_blank(&next, cur->w / 2, cur->h / 2);
		gfx_copy_stretch_with_alpha_map(&next, 0, 0, next.w, next.h, cur, 0, 0, cur->w, cur->h);
		if (have_tmp)
			gfx_delete_texture(&tmp);
		tmp = next;
		cur = &tmp;
		have_tmp = true;
	}

	Texture dst;
	gfx_init_texture_blank(&dst, w, h);
	gfx_copy_stretch_with_alpha_map(&dst, 0, 0, w, h, cur, 0, 0, cur->w, cur->h);
	if (have_tmp)
		gfx_delete_texture(&tmp);

	char *path = savedir_path(filename->text);
	int r = gfx_save_texture(&dst, path, ALCG_QNT);
	free(path);
	gfx_delete_texture(&dst);
	return !!r;
}

void PE_SetInputState(int parts_no, int state)
{
	if (!parts_state_valid(--state)) {
		WARNING("invalid input state: %d", state);
		return;
	}
	parts_set_state(parts_get(parts_no), state);
}

int PE_GetInputState(int parts_no)
{
	return parts_get(parts_no)->state + 1;
}

void PE_SetComponentType(int parts_no, int type, int state)
{
	if (!parts_state_valid(--state))
		return;
	struct parts *parts = parts_get(parts_no);
	parts_widget_free(parts);
	parts_panel_free(parts);
	parts_scrollbar_free(parts);
	parts_message_window_free(parts);
	parts->component_type = 0;
	enum parts_type pt = PARTS_UNINITIALIZED;
	switch (type) {
	case 8:  pt = PARTS_LAYOUT_BOX; break;
	case 11: pt = PARTS_CG; break;
	case 12: pt = PARTS_ANIMATION; break;
	case 13: pt = PARTS_TEXT; break;
	case 14: pt = PARTS_HGAUGE; break;
	case 15: pt = PARTS_VGAUGE; break;
	case 16: pt = PARTS_NUMERAL; break;
	case 17: pt = PARTS_RECT_DETECTION; break;
	case 18: pt = PARTS_CONSTRUCTION_PROCESS; break;
	case 20: pt = PARTS_FLAT; break;
	case 21: pt = PARTS_3DLAYER; break;
	case 22: pt = PARTS_MOVIE; break;
	default:
		VM_ERROR("unknown component type %d", type);
	}
	if (parts->states[state].type != pt)
		parts_state_reset(&parts->states[state], pt);
}

int PE_GetComponentType(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return -1;
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return -1;

	switch (parts->states[state].type) {
	case PARTS_LAYOUT_BOX: return 8;
	case PARTS_UNINITIALIZED:  // defaluts to CG
	case PARTS_CG:
		return 11;
	case PARTS_ANIMATION: return 12;
	case PARTS_TEXT: return 13;
	case PARTS_HGAUGE: return 14;
	case PARTS_VGAUGE: return 15;
	case PARTS_NUMERAL: return 16;
	case PARTS_RECT_DETECTION: return 17;
	case PARTS_CONSTRUCTION_PROCESS: return 18;
	case PARTS_FLAT: return 20;
	case PARTS_3DLAYER: return 21;
	case PARTS_MOVIE: return 22;
	case PARTS_FLASH:
		break;
	}
	VM_ERROR("unsupported component type %d", parts->states[state].type);
}

bool PE_SetPartsRectangleDetectionSize(int parts_no, int w, int h, int state)
{
	if (!parts_state_valid(--state))
		return false;
	struct parts *parts = parts_try_get(parts_no);
	if (!parts)
		return false;
	if (parts->states[state].type != PARTS_RECT_DETECTION)
		parts_state_reset(&parts->states[state], PARTS_RECT_DETECTION);
	parts->states[state].rect.custom = false;
	parts_set_dims(parts, &parts->states[state].common, w, h);
	return true;
}

void PE_SetRectangleDetectionPoint(int no, int x0, int y0, int x1, int y1,
		int x2, int y2, int x3, int y3, int state)
{
	if (!parts_state_valid(--state)) return;
	struct parts *parts = parts_get(no);
	if (parts->states[state].type != PARTS_RECT_DETECTION)
		parts_state_reset(&parts->states[state], PARTS_RECT_DETECTION);
	struct parts_rect *rect = &parts->states[state].rect;
	rect->custom = true;
	rect->points[0] = (Point){x0, y0}; rect->points[1] = (Point){x1, y1};
	rect->points[2] = (Point){x2, y2}; rect->points[3] = (Point){x3, y3};
	rect->common.w = max(max(x0, x1), max(x2, x3)) - min(min(x0, x1), min(x2, x3));
	rect->common.h = max(max(y0, y1), max(y2, y3)) - min(min(y0, y1), min(y2, y3));
	parts_recalculate_hitbox(parts);
	parts_component_dirty(parts);
}

void PE_GetRectangleDetectionPoint(int no, int *x0, int *y0, int *x1, int *y1,
		int *x2, int *y2, int *x3, int *y3, int state)
{
	*x0 = *y0 = *x1 = *y1 = *x2 = *y2 = *x3 = *y3 = 0;
	if (!parts_state_valid(--state)) return;
	struct parts *parts = parts_try_get(no);
	if (!parts || parts->states[state].type != PARTS_RECT_DETECTION) return;
	struct parts_rect *rect = &parts->states[state].rect;
	if (!rect->custom) { *x1 = *x3 = rect->common.w; *y2 = *y3 = rect->common.h; return; }
	*x0 = rect->points[0].x; *y0 = rect->points[0].y;
	*x1 = rect->points[1].x; *y1 = rect->points[1].y;
	*x2 = rect->points[2].x; *y2 = rect->points[2].y;
	*x3 = rect->points[3].x; *y3 = rect->points[3].y;
}

bool PE_SetPartsCGDetectionSize(int parts_no, struct string *cg_name, int state)
{
	if (!parts_state_valid(--state))
		return false;
	struct parts *parts = parts_get(parts_no);
	if (parts->states[state].type != PARTS_RECT_DETECTION)
		parts_state_reset(&parts->states[state], PARTS_RECT_DETECTION);
	struct parts_rect *rect = &parts->states[state].rect;
	if (rect->cg_name)
		free_string(rect->cg_name);
	rect->cg_name = cg_name ? string_dup(cg_name) : string_ref(&EMPTY_STRING);
	if (cg_name && cg_name->size > 0) {
		int cg_no;
		struct cg *cg = asset_cg_load_by_name(cg_name->text, &cg_no);
		if (cg) {
			parts_set_dims(parts, &rect->common, cg->metrics.w, cg->metrics.h);
			cg_free(cg);
			return true;
		}
	}
	return false;
}

int PE_GetFreeNumber(void)
{
	if (ain->version >= 12) {
		if (parts_current->next_free_no < 1000010000)
			parts_current->next_free_no = 1000010000;
		int no = parts_current->next_free_no;
		while (PE_IsExist(no)) no++;
		parts_current->next_free_no = no;
		return no;
	}
	// NOTE: per GUIEngine.dll (Rance 01)
	static int first_free = 1000001000;
	while (PE_IsExist(first_free)) {
		first_free++;
	}
	// XXX: the ID is incremented even if the parts is not created
	return first_free++;
}

bool PE_IsExist(int parts_no)
{
	return !!ht_get_int(parts_table, parts_no, NULL);
}

static void ctrl_stack_init(void)
{
	memset(&ctrl_stack, 0, sizeof(ctrl_stack));
	if (ain->version >= 12) {
		ctrl_stack.active = PARTS_CONTROLLER_SYSTEM_OVERLAY;
		ctrl_stack.overlay_id = PE_GetFreeNumber();
		parts_get_layout_box(parts_get(ctrl_stack.overlay_id))->layout_type = PARTS_LAYOUT_FREE;
	}
	// Add initial default controller
	PE_AddController(-1);
}

static void parts_change_controller(struct parts *parts, int index)
{
	if (parts->controller_no != index) {
		parts->controller_no = index;
		parts_list_resort(parts);
	}
	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) parts_change_controller(child, index);
}

static void ctrl_stack_reindex(void)
{
	for (int i = 0; i < ctrl_stack.nr_controllers; i++) {
		struct parts *root = parts_try_get(ctrl_stack.ids[i]);
		if (root) parts_change_controller(root, i);
	}
}

// Adds a new controller to the stack and makes it active. Before v12, `index`
// parameter specifies the position in the stack at which to insert the new
// controller; -1 means "insert directly after the currently active
// controller". In practice the game only ever passes -1, and the active
// controller is always the top of the stack at that point, so this
// degenerates to a simple push.
int PE_AddController(int index)
{
	if (ain->version >= 12) {
		if (ctrl_stack.nr_controllers >= PARTS_CONTROLLER_STACK_MAX)
			VM_ERROR("controller stack overflow");
		// v12's misleadingly named Index is the ID to insert after.
		// An unknown ID (including -1) means after the active controller.
		int after = PE_GetControllerIndex(index);
		if (after < 0) after = ctrl_stack.active == PARTS_CONTROLLER_SYSTEM_OVERLAY
			? ctrl_stack.nr_controllers - 1 : ctrl_stack.active;
		int pos = min(after + 1, ctrl_stack.nr_controllers);
		memmove(&ctrl_stack.ids[pos + 1], &ctrl_stack.ids[pos],
				(ctrl_stack.nr_controllers - pos) * sizeof(*ctrl_stack.ids));
		ctrl_stack.nr_controllers++;
		ctrl_stack.active = pos;
		int id = ctrl_stack.ids[pos] = PE_GetFreeNumber();
		parts_get_layout_box(parts_get(id))->layout_type = PARTS_LAYOUT_FREE;
		ctrl_stack_reindex();
		return id;
	}
	if (index != -1)
		VM_ERROR("index != -1 not supported (got %d)", index);
	if (ctrl_stack.nr_controllers > 0 &&
			ctrl_stack.active != ctrl_stack.nr_controllers - 1)
		VM_ERROR("active controller is not at the top of the stack");
	if (ctrl_stack.nr_controllers >= PARTS_CONTROLLER_STACK_MAX)
		VM_ERROR("controller stack overflow");

	int no = ctrl_stack.nr_controllers++;
	ctrl_stack.active = no;
	return no;
}

static void parts_release_controller_children(struct parts *root, struct page **erased)
{
	while (!TAILQ_EMPTY(&root->children)) {
		struct parts *child = TAILQ_FIRST(&root->children);
		*erased = array_pushback(*erased, vm_int(child->delegate_index), AIN_ARRAY_INT, -1);
		parts_release_controller_children(child, erased);
		parts_release(child->no);
	}
}

static void parts_clear_int_output(struct page **output)
{
	if (!*output) return;
	struct page *old = *output;
	if (old->type != ARRAY_PAGE || (old->a_type != AIN_ARRAY_INT
			&& (old->a_type != AIN_ARRAY || old->array.element_type.data != AIN_INT)))
		VM_ERROR("Expected integer array output");
	*output = alloc_page(ARRAY_PAGE, old->a_type, 0);
	(*output)->array = old->array;
	delete_page_vars(old);
	free_page(old);
}

// Before v12, removes the controller at position `index`, releases all
// parts belonging to it, and returns their parts numbers in
// `erase_number_list`. `index == -1` means "remove the currently active
// controller". In practice the game only ever passes -1, and the active
// controller is always the top of the stack at that point, so this
// degenerates to a simple pop.
void PE_RemoveController(struct page **erase_number_list, int index)
{
	if (ain->version >= 12) {
		// The original retains its last controller and omits the controller
		// root itself from the list of erased delegates.
		if (ctrl_stack.nr_controllers <= 1) return;
		int pos = PE_GetControllerIndex(index);
		if (pos < 0) pos = ctrl_stack.active == PARTS_CONTROLLER_SYSTEM_OVERLAY
			? ctrl_stack.nr_controllers - 1 : ctrl_stack.active;
		struct parts *root = parts_try_get(ctrl_stack.ids[pos]);
		if (root) {
			// The v12 HLL leaves the caller's array unchanged for an empty result.
			if (!TAILQ_EMPTY(&root->children)) parts_clear_int_output(erase_number_list);
			parts_release_controller_children(root, erase_number_list);
			parts_release(root->no);
		}
		ctrl_stack.nr_controllers--;
		memmove(&ctrl_stack.ids[pos], &ctrl_stack.ids[pos + 1],
				(ctrl_stack.nr_controllers - pos) * sizeof(*ctrl_stack.ids));
		if (ctrl_stack.active == pos) ctrl_stack.active = max(0, pos - 1);
		else if (ctrl_stack.active != PARTS_CONTROLLER_SYSTEM_OVERLAY && ctrl_stack.active > pos)
			ctrl_stack.active--;
		ctrl_stack_reindex();
		return;
	}
	if (index != -1)
		VM_ERROR("index != -1 not supported (got %d)", index);
	if (ctrl_stack.nr_controllers == 0 ||
			ctrl_stack.active != ctrl_stack.nr_controllers - 1)
		VM_ERROR("active controller is not at the top of the stack");

	int ctrl_no = ctrl_stack.active;

	// Collect and release parts belonging to this controller
	struct parts *p = TAILQ_FIRST(&parts_list);
	while (p) {
		struct parts *next = TAILQ_NEXT(p, parts_list_entry);
		if (p->controller_no == ctrl_no) {
			*erase_number_list = array_pushback(*erase_number_list,
					(union vm_value){.i = p->delegate_index}, AIN_ARRAY_INT, -1);
			parts_release(p->no);
		}
		p = next;
	}

	ctrl_stack.nr_controllers--;
	if (ctrl_stack.nr_controllers == 0) {
		PE_AddController(-1);
	} else {
		ctrl_stack.active = ctrl_stack.nr_controllers - 1;
	}
}

void PE_MoveController(int id, int index)
{
	int from = PE_GetControllerIndex(id);
	if (from < 0) return;
	int active = PE_get_active_controller();
	// Index is an insertion position in the original order, unlike Add/Remove.
	int to = max(0, min(index, ctrl_stack.nr_controllers));
	if (from < to) to--;
	if (from == to) return;
	if (from < to)
		memmove(&ctrl_stack.ids[from], &ctrl_stack.ids[from + 1], (to - from) * sizeof(*ctrl_stack.ids));
	else
		memmove(&ctrl_stack.ids[to + 1], &ctrl_stack.ids[to], (from - to) * sizeof(*ctrl_stack.ids));
	ctrl_stack.ids[to] = id;
	ctrl_stack.active = active == ctrl_stack.overlay_id ? PARTS_CONTROLLER_SYSTEM_OVERLAY
		: PE_GetControllerIndex(active);
	ctrl_stack_reindex();
}

void PE_set_active_controller(int controller_no)
{
	if (ain->version >= 12) {
		if (controller_no == ctrl_stack.overlay_id) {
			ctrl_stack.active = PARTS_CONTROLLER_SYSTEM_OVERLAY;
			return;
		}
		int index = PE_GetControllerIndex(controller_no);
		if (index < 0) VM_ERROR("Invalid controller ID: %d", controller_no);
		ctrl_stack.active = index;
		return;
	}
	if (controller_no == PARTS_CONTROLLER_SYSTEM_OVERLAY ||
			(controller_no >= 0 && controller_no < ctrl_stack.nr_controllers))
		ctrl_stack.active = controller_no;
	else
		VM_ERROR("Invalid controller number: %d", controller_no);
}

int PE_get_active_controller(void)
{
	if (ain->version >= 12)
		return ctrl_stack.active == PARTS_CONTROLLER_SYSTEM_OVERLAY
			? ctrl_stack.overlay_id : ctrl_stack.ids[ctrl_stack.active];
	return ctrl_stack.active;
}

int PE_get_system_controller(void)
{
	if (ain->version >= 12) return ctrl_stack.overlay_id;
	return PARTS_CONTROLLER_SYSTEM_OVERLAY;
}

int PE_GetControllerIndex(int id)
{
	for (int i = 0; i < ctrl_stack.nr_controllers; i++) if (ctrl_stack.ids[i] == id) return i;
	return -1;
}

int PE_GetControllerID(int index)
{
	if (!ctrl_stack.nr_controllers) return -1;
	if (index < 0) index = ctrl_stack.active == PARTS_CONTROLLER_SYSTEM_OVERLAY
		? ctrl_stack.nr_controllers - 1 : ctrl_stack.active;
	return ctrl_stack.ids[min(index, ctrl_stack.nr_controllers - 1)];
}

int PE_GetControllerLength(void) { return ctrl_stack.nr_controllers; }

void PE_parts_set_want_save(int parts_no, bool want_save)
{
	parts_get(parts_no)->want_save = want_save;
}

bool PE_parts_get_want_save(int parts_no)
{
	return parts_get(parts_no)->want_save;
}

float PE_parts_get_absolute_x(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? (float)parts->global.pos.x : 0.0f;
}

float PE_parts_get_absolute_y(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? (float)parts->global.pos.y : 0.0f;
}

int PE_parts_get_absolute_z(int parts_no)
{
	struct parts *parts = parts_try_get(parts_no);
	return parts ? parts->global.z : 0;
}

void PE_parts_set_lock_input_state(int parts_no, bool lock)
{
	parts_get(parts_no)->lock_input_state = lock;
}

bool PE_init_parts_movie(int parts_no, int width, int height, int bg_r, int bg_g, int bg_b, int state)
{
	if (!parts_state_valid(--state))
		return false;

	struct parts *parts = parts_get(parts_no);
	parts_state_reset(&parts->states[state], PARTS_MOVIE);
	struct parts_movie *movie = &parts->states[state].movie;

	int sp_no = sact_SP_GetUnuseNum(0);
	struct sact_sprite *sp = sact_create_sprite(sp_no, width, height, bg_r, bg_g, bg_b, 255);
	if (!sp)
		return false;
	// The movie frames are composited by the parts engine via parts_render(),
	// so hide the bound sprite from the scene to avoid double-drawing.
	sprite_set_show(sp, false);

	struct texture *tex = sprite_get_texture(sp);
	movie->sprite_no = sp_no;
	movie->common.texture = *tex; // XXX: textures normally shouldn't be copied like this...
	parts_set_dims(parts, &movie->common, width, height);
	parts_dirty(parts);
	return true;
}

int PE_get_movie_sprite(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return -1;

	struct parts *parts = parts_try_get(parts_no);
	if (!parts || parts->states[state].type != PARTS_MOVIE)
		return -1;

	return parts->states[state].movie.sprite_no;
}

bool PE_CreateParts3DLayerPluginID(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return false;
	struct parts *parts = parts_get(parts_no);
	struct parts_3dlayer *l = parts_get_3dlayer(parts, state);

	if (l->plugin >= 0)
		return false;

	int handle = ReignEngine_create_plugin(RE_SEAL_PLUGIN);
	if (handle < 0)
		return false;

	int sp_no = sact_SP_GetUnuseNum(0);
	struct sact_sprite *sp = sact_create_sprite(sp_no, 1, 1, 0, 0, 0, 255);
	if (!sp) {
		ReignEngine_ReleasePlugin(handle);
		return false;
	}

	if (!ReignEngine_BindPlugin(handle, sp_no)) {
		sact_SP_Delete(sp_no);
		ReignEngine_ReleasePlugin(handle);
		return false;
	}
	// The 3D content is composited by the parts engine via parts_render(), so
	// hide the bound sprite from the scene to avoid double-drawing.
	sprite_set_show(sp, false);

	l->plugin = handle;
	l->sprite_no = sp_no;
	return true;
}

int PE_GetParts3DLayerPluginID(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return -1;
	struct parts *parts = parts_try_get(parts_no);
	if (!parts || parts->states[state].type != PARTS_3DLAYER)
		return -1;
	return parts->states[state].layer3d.plugin;
}

bool PE_ReleaseParts3DLayerPluginID(int parts_no, int state)
{
	if (!parts_state_valid(--state))
		return false;
	struct parts *parts = parts_try_get(parts_no);
	if (!parts || parts->states[state].type != PARTS_3DLAYER)
		return false;
	struct parts_3dlayer *l = &parts->states[state].layer3d;
	if (l->plugin < 0)
		return false;
	ReignEngine_ReleasePlugin(l->plugin);
	l->plugin = -1;
	if (l->sprite_no >= 0) {
		sact_SP_Delete(l->sprite_no);
		l->sprite_no = -1;
	}
	return true;
}
