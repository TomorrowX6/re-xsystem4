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

#include <limits.h>
#include <math.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <cglm/cglm.h>

#include "system4.h"
#include "system4/hashtable.h"
#include "system4/string.h"
#include "system4/flat.h"

#include "gfx/gfx.h"
#include "gfx/private.h"
#include "sact.h"
#include "scene.h"
#include "sprite.h"
#include "xsystem4.h"

#include "parts_internal.h"

static struct {
	struct shader shader;
	GLint uv_rect;
	GLint blend_rate;
	GLint add_color;
	GLint multiply_color;
	GLint draw_filter;
	GLint use_clipper;
	GLint clipper_tex;
	GLint inv_clipper_transform;
} parts_shader;

uint64_t parts_render_gen = 1;

static bool parts_pass_active = false;
static GLuint pass_tex0 = 0;
static GLuint pass_tex1 = 0;
static int pass_draw_filter = -1;
static int pass_use_clipper = -1;
static bool pass_blend_enabled = true;
static int pass_clip_no = 0;
static uint64_t pass_clip_gen = 0;
static struct parts_context *pass_clip_ctx = NULL;

static void apply_draw_filter_blend_func(int draw_filter)
{
	switch (draw_filter) {
	case PARTS_DRAW_FILTER_ADDITIVE:
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
		break;
	case PARTS_DRAW_FILTER_MULTIPLY:
		glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
		break;
	case PARTS_DRAW_FILTER_SCREEN:
		glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ZERO, GL_ONE);
		break;
	default:
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
		break;
	}
}

void parts_render_pass_begin(void)
{
	mat4 wv_transform = WV_TRANSFORM(config.view_width, config.view_height);
	glUseProgram(parts_shader.shader.program);
	glUniformMatrix4fv(parts_shader.shader.view_transform, 1, GL_FALSE, wv_transform[0]);
	glUniform1i(parts_shader.shader.texture, 0);
	glUniform1i(parts_shader.clipper_tex, 1);

	glEnable(GL_BLEND);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);

	glBindVertexArray(sdl.gl.vao);
	glEnableVertexAttribArray(parts_shader.shader.vertex_pos);
	glEnableVertexAttribArray(parts_shader.shader.vertex_uv);
	glBindBuffer(GL_ARRAY_BUFFER, sdl.gl.vbo);
	glVertexAttribPointer(parts_shader.shader.vertex_pos, 4, GL_FLOAT, GL_FALSE, sizeof(struct gfx_vertex), NULL);
	glVertexAttribPointer(parts_shader.shader.vertex_uv, 2, GL_FLOAT, GL_FALSE, sizeof(struct gfx_vertex), (void*)offsetof(struct gfx_vertex, u));
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sdl.gl.rect_ibo);

	glActiveTexture(GL_TEXTURE0);

	pass_tex0 = 0;
	pass_tex1 = 0;
	pass_blend_enabled = true;
	pass_draw_filter = PARTS_DRAW_FILTER_NORMAL;
	glUniform1i(parts_shader.draw_filter, PARTS_DRAW_FILTER_NORMAL);
	pass_use_clipper = 0;
	glUniform1i(parts_shader.use_clipper, 0);
	pass_clip_no = 0;
	pass_clip_gen = 0;
	pass_clip_ctx = NULL;
	parts_pass_active = true;
}

void parts_render_pass_end(void)
{
	if (!parts_pass_active)
		return;
	parts_pass_active = false;
	if (!pass_blend_enabled) {
		glEnable(GL_BLEND);
		pass_blend_enabled = true;
	}
	if (pass_draw_filter != PARTS_DRAW_FILTER_NORMAL) {
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
	}
	glActiveTexture(GL_TEXTURE0);
	glDisableVertexAttribArray(parts_shader.shader.vertex_pos);
	glDisableVertexAttribArray(parts_shader.shader.vertex_uv);
	glBindVertexArray(0);
	glUseProgram(0);
}

void parts_render_pass_rebind(void)
{
	if (parts_pass_active)
		parts_render_pass_begin();
}

static void quad_screen_aabb(mat4 m, float *min_x, float *max_x, float *min_y, float *max_y)
{
	float x0 = m[3][0], y0 = m[3][1];
	float dx0 = m[0][0], dy0 = m[0][1];
	float dx1 = m[1][0], dy1 = m[1][1];
	*min_x = x0 + (dx0 < 0.0f ? dx0 : 0.0f) + (dx1 < 0.0f ? dx1 : 0.0f);
	*max_x = x0 + (dx0 > 0.0f ? dx0 : 0.0f) + (dx1 > 0.0f ? dx1 : 0.0f);
	*min_y = y0 + (dy0 < 0.0f ? dy0 : 0.0f) + (dy1 < 0.0f ? dy1 : 0.0f);
	*max_y = y0 + (dy0 > 0.0f ? dy0 : 0.0f) + (dy1 > 0.0f ? dy1 : 0.0f);
}

static bool crop_quad_transform(mat4 mw_transform, int tex_w, int tex_h, const Rectangle *rect,
		mat4 out_mw, vec4 out_uv)
{
	if (tex_w <= 0 || tex_h <= 0)
		return false;
	if (!rect || (rect->x <= 0 && rect->y <= 0 &&
			rect->x + rect->w >= tex_w && rect->y + rect->h >= tex_h)) {
		if (rect && (rect->w <= 0 || rect->h <= 0))
			return false;
		glm_mat4_copy(mw_transform, out_mw);
		out_uv[0] = 0.0f;
		out_uv[1] = 0.0f;
		out_uv[2] = 1.0f;
		out_uv[3] = 1.0f;
		return true;
	}
	int rx0 = max(0, rect->x);
	int ry0 = max(0, rect->y);
	int rx1 = min(tex_w, rect->x + rect->w);
	int ry1 = min(tex_h, rect->y + rect->h);
	if (rx1 <= rx0 || ry1 <= ry0)
		return false;

	float u0 = (float)rx0 / (float)tex_w;
	float v0 = (float)ry0 / (float)tex_h;
	float du = (float)(rx1 - rx0) / (float)tex_w;
	float dv = (float)(ry1 - ry0) / (float)tex_h;
	out_uv[0] = u0;
	out_uv[1] = v0;
	out_uv[2] = du;
	out_uv[3] = dv;

	for (int r = 0; r < 4; r++) {
		float m0 = mw_transform[0][r];
		float m1 = mw_transform[1][r];
		out_mw[0][r] = du * m0;
		out_mw[1][r] = dv * m1;
		out_mw[2][r] = mw_transform[2][r];
		out_mw[3][r] = mw_transform[3][r] + u0 * m0 + v0 * m1;
	}
	return true;
}

static bool get_alpha_clipper_info(int alpha_clipper, GLuint *tex_out, vec4 **inv_out,
		float *min_x, float *max_x, float *min_y, float *max_y)
{
	if (alpha_clipper <= 0)
		return false;
	struct parts *clipper = parts_try_get(alpha_clipper);
	if (!clipper)
		return false;

	static uint64_t cached_clip_gen = 0;
	static int cached_clip_no = 0;
	static struct parts_context *cached_clip_ctx = NULL;
	static mat4 cached_clip_inv;
	static GLuint cached_clip_tex = 0;
	static float clip_min_x, clip_max_x, clip_min_y, clip_max_y;

	if (cached_clip_gen != parts_render_gen || cached_clip_no != alpha_clipper || cached_clip_ctx != parts_current) {
		struct parts_state *c_state = &clipper->states[clipper->state];
		if (c_state->type == PARTS_CONSTRUCTION_PROCESS && c_state->cproc.needs_build && !clipper->textures_suspended) {
			parts_build_construction_process(clipper, &c_state->cproc);
			parts_render_pass_rebind();
		}
		struct parts_common *c_common = &c_state->common;
		mat4 clip_mw;
		parts_world_matrix(clipper, clip_mw);
		glm_translate(clip_mw, (vec3){ c_common->origin_offset.x, c_common->origin_offset.y, 0 });
		glm_scale(clip_mw, (vec3){ c_common->w, c_common->h, 1.0 });
		glm_mat4_inv(clip_mw, cached_clip_inv);
		quad_screen_aabb(clip_mw, &clip_min_x, &clip_max_x, &clip_min_y, &clip_max_y);
		cached_clip_tex = c_common->texture.handle;
		cached_clip_gen = parts_render_gen;
		cached_clip_no = alpha_clipper;
		cached_clip_ctx = parts_current;
	}

	if (tex_out) *tex_out = cached_clip_tex;
	if (inv_out) *inv_out = cached_clip_inv;
	*min_x = clip_min_x;
	*max_x = clip_max_x;
	*min_y = clip_min_y;
	*max_y = clip_max_y;
	return true;
}

static void parts_render_texture(struct texture *texture, mat4 mw_transform, Rectangle *rect, float blend_rate, vec3 add_color, vec3 multiply_color, int draw_filter, int alpha_clipper)
{
	if (!texture || !texture->handle || blend_rate <= 0.0f)
		return;

	mat4 sub_mw;
	vec4 uv_rect;
	if (!crop_quad_transform(mw_transform, texture->w, texture->h, rect, sub_mw, uv_rect))
		return;

	float q_min_x, q_max_x, q_min_y, q_max_y;
	quad_screen_aabb(sub_mw, &q_min_x, &q_max_x, &q_min_y, &q_max_y);
	if (q_max_x <= 0.0f || q_min_x >= (float)config.view_width ||
			q_max_y <= 0.0f || q_min_y >= (float)config.view_height)
		return;

	GLuint clip_tex = 0;
	vec4 *clip_inv = NULL;
	float clip_min_x, clip_max_x, clip_min_y, clip_max_y;
	bool has_clipper = get_alpha_clipper_info(alpha_clipper, &clip_tex, &clip_inv,
			&clip_min_x, &clip_max_x, &clip_min_y, &clip_max_y);
	if (has_clipper) {
		if (q_max_x <= clip_min_x || q_min_x >= clip_max_x ||
				q_max_y <= clip_min_y || q_min_y >= clip_max_y)
			return;
	}

	bool auto_pass = !parts_pass_active;
	if (auto_pass)
		parts_render_pass_begin();

	sub_mw[0][2] = 0.0f;
	sub_mw[1][2] = 0.0f;
	sub_mw[2][2] = 0.0f;
	sub_mw[3][2] = 0.0f;

	bool want_blend = !(texture->opaque && blend_rate >= 1.0f &&
			draw_filter == PARTS_DRAW_FILTER_NORMAL && !has_clipper);
	if (want_blend != pass_blend_enabled) {
		if (want_blend)
			glEnable(GL_BLEND);
		else
			glDisable(GL_BLEND);
		pass_blend_enabled = want_blend;
	}

	if (draw_filter != pass_draw_filter) {
		apply_draw_filter_blend_func(draw_filter);
		glUniform1i(parts_shader.draw_filter, draw_filter);
		pass_draw_filter = draw_filter;
	}

	if (pass_tex0 != texture->handle) {
		glBindTexture(GL_TEXTURE_2D, texture->handle);
		pass_tex0 = texture->handle;
	}

	glUniformMatrix4fv(parts_shader.shader.world_transform, 1, GL_FALSE, sub_mw[0]);
	glUniform4fv(parts_shader.uv_rect, 1, uv_rect);
	glUniform1f(parts_shader.blend_rate, blend_rate);
	glUniform3fv(parts_shader.add_color, 1, add_color);
	glUniform3fv(parts_shader.multiply_color, 1, multiply_color);

	if (has_clipper) {
		if (pass_tex1 != clip_tex) {
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, clip_tex);
			glActiveTexture(GL_TEXTURE0);
			pass_tex1 = clip_tex;
		}
		if (pass_use_clipper != 1) {
			glUniform1i(parts_shader.use_clipper, 1);
			pass_use_clipper = 1;
		}
		if (pass_clip_no != alpha_clipper || pass_clip_gen != parts_render_gen || pass_clip_ctx != parts_current) {
			glUniformMatrix4fv(parts_shader.inv_clipper_transform, 1, GL_FALSE, clip_inv[0]);
			pass_clip_no = alpha_clipper;
			pass_clip_gen = parts_render_gen;
			pass_clip_ctx = parts_current;
		}
	} else if (pass_use_clipper != 0) {
		glUniform1i(parts_shader.use_clipper, 0);
		pass_use_clipper = 0;
	}

	glDrawElements(GL_TRIANGLE_FAN, 4, GL_UNSIGNED_INT, NULL);

	if (auto_pass)
		parts_render_pass_end();
}

static void parts_render_text(struct parts *parts, struct parts_text *t,
		struct parts_message_window *message)
{
	vec3 add_color = {
		parts->global.add_color.r / 255.0f,
		parts->global.add_color.g / 255.0f,
		parts->global.add_color.b / 255.0f
	};
	vec3 multiply_color = {
		parts->global.multiply_color.r / 255.0f,
		parts->global.multiply_color.g / 255.0f,
		parts->global.multiply_color.b / 255.0f,
	};
	float blend_rate = parts->global.alpha / 255.0;
	int alpha_clipper = parts_get_effective_alpha_clipper(parts);

	mat4 base;
	parts_world_matrix(parts, base);
	int x = t->common.origin_offset.x;
	int y = t->common.origin_offset.y;
	for (unsigned i = 0; i < t->nr_lines; i++) {
		struct parts_text_line *line = &t->lines[i];
		for (int j = 0; j < line->nr_chars; j++) {
			struct parts_text_char *ch = &line->chars[j];
			float alpha = blend_rate;
			if (message && !message->fixed) {
				double time = message->elapsed - ch->fade_start;
				alpha *= time < 0 ? 0 : ch->fade_time > 0 ? min(1., time / ch->fade_time) : 1.;
			}
			Rectangle r = { 0, 0, ch->t.w, ch->t.h };
			if (message) {
				Rectangle a = message->text_area;
				a.x += parts->states[0].common.origin_offset.x;
				a.y += parts->states[0].common.origin_offset.y;
				if (a.w > 0) { r.x = max(0, a.x - x); r.w = min(ch->t.w, a.x + a.w - x) - r.x; }
				if (a.h > 0) { r.y = max(0, a.y - y); r.h = min(ch->t.h, a.y + a.h - y) - r.y; }
			}
			if (alpha > 0 && r.w > 0 && r.h > 0 && ch->t.handle) {
				mat4 mw_transform;
				for (int row = 0; row < 4; row++) {
					float b0 = base[0][row];
					float b1 = base[1][row];
					mw_transform[0][row] = ch->t.w * b0;
					mw_transform[1][row] = ch->t.h * b1;
					mw_transform[2][row] = base[2][row];
					mw_transform[3][row] = base[3][row] + x * b0 + y * b1;
				}
				parts_render_texture(&ch->t, mw_transform, &r, alpha, add_color,
						multiply_color, 0, alpha_clipper);
			}
			x += ch->advance;
		}
		x = t->common.origin_offset.x;
		y += line->height + line->line_space;
	}
}

static void parts_render_cg(struct parts *parts, struct parts_common *common)
{
	mat4 mw_transform;
	parts_world_matrix(parts, mw_transform);
	glm_translate(mw_transform, (vec3){ common->origin_offset.x, common->origin_offset.y, 0 });

	switch (parts->sprite_deform) {
	// Flip horizontally
	case 1:
		glm_translate(mw_transform, (vec3){ common->w, 0.0f, 0.0f });
		glm_scale(mw_transform, (vec3){ -common->w, common->h, 1.0f });
		break;
	// Flip vertically
	case 2:
		glm_translate(mw_transform, (vec3){ 0.0f, common->h, 0.0f });
		glm_scale(mw_transform, (vec3){ common->w, -common->h, 1.0f });
		break;
	default:
		WARNING("Invalid sprite_deform: %d", parts->sprite_deform);
	// No deform
	case 0:
		glm_scale(mw_transform, (vec3){ common->w, common->h, 1.0 });
		break;
	}

	Rectangle r = common->surface_area;
	if (!r.w && !r.h) {
		r = (Rectangle) { 0, 0, common->texture.w, common->texture.h };
	}

	vec3 add_color = {
		parts->global.add_color.r / 255.0f,
		parts->global.add_color.g / 255.0f,
		parts->global.add_color.b / 255.0f
	};
	vec3 multiply_color = {
		parts->global.multiply_color.r / 255.0f,
		parts->global.multiply_color.g / 255.0f,
		parts->global.multiply_color.b / 255.0f,
	};
	parts_render_texture(&common->texture, mw_transform, &r, parts->global.alpha / 255.0, add_color, multiply_color, parts->draw_filter, parts_get_effective_alpha_clipper(parts));
}

struct emitter_render_ud {
	struct parts_flat *f;
	mat4 transform;       // root * base * layer for this emitter+birth_frame
	float parent_alpha;
	vec2 align;
	int alpha_clipper;
	int draw_filter;
	vec3 add_color;
	vec3 mul_color;
};

static void render_emitter_particle_cb(const struct flat_emitter_particle *p,
		void *ud)
{
	struct emitter_render_ud *d = ud;
	if (p->cg_lib_idx < 0 || (size_t)p->cg_lib_idx >= d->f->nr_libraries)
		return;
	Texture *tex = &d->f->textures[p->cg_lib_idx];
	if (!tex->handle)
		return;

	mat4 m = GLM_MAT4_IDENTITY_INIT;
	glm_translate(m, (vec3){ p->pos[0], p->pos[1], 0 });
	if (p->rot[2] != 0)
		glm_rotate_z(m, glm_rad(p->rot[2]), m);
	if (p->rot[0] != 0)
		glm_rotate_x(m, glm_rad(-p->rot[0]), m);
	if (p->rot[1] != 0)
		glm_rotate_y(m, glm_rad(p->rot[1]), m);
	glm_scale(m, (vec3){ p->scale[0], p->scale[1], 1.0f });
	glm_translate(m, (vec3){ -d->align[0], -d->align[1], 0 });

	// Bring particle into screen space, then kill the Z-output row so
	// clip_z stays at the near plane.
	glm_mat4_mul(d->transform, m, m);
	m[0][2] = m[1][2] = m[2][2] = m[3][2] = 0.0f;
	glm_scale(m, (vec3){ tex->w, tex->h, 1.0f });

	float blend_rate = d->parent_alpha * p->fade_alpha;
	Rectangle rect = { 0, 0, tex->w, tex->h };
	parts_render_texture(tex, m, &rect, blend_rate,
			d->add_color, d->mul_color, d->draw_filter, d->alpha_clipper);
}

static void render_flat_emitter(struct parts *parts, struct parts_flat *f,
		int emitter_lib_idx, int tl_idx, int local, int frame_count,
		const struct flat_timeline *timeline,
		struct flat_layer_state *state,
		mat4 root, float parent_alpha,
		struct flat_key_stack *key_stack)
{
	vec2 align;
	if (!parts_flat_emitter_get_align_offset(f, emitter_lib_idx, align))
		return;

	struct flat_emitter *em = &f->flat->libraries[emitter_lib_idx].emitter;
	int active_frames = max(1, frame_count - em->particle_lifetime + 1);
	float parts_alpha = parts->global.alpha / 255.0f;

	mat4 current_base;
	parts_flat_build_emitter_base_matrix(em, key_stack, current_base);

	int min_birth = max(0, local - em->particle_lifetime + 1);
	int max_birth = min(active_frames - 1, local);
	// Retain only live birth positions, rather than growing a history for
	// every frame of long animations.
	for (struct flat_emitter_birth **at = &state->emitter_births; *at;) {
		struct flat_emitter_birth *birth = *at;
		if ((birth->timeline == timeline || birth->timeline_idx == tl_idx)
				&& (birth->frame < min_birth || birth->frame > local)) {
			*at = birth->next;
			free(birth);
		} else {
			at = &birth->next;
		}
	}
	for (int birth_frame = min_birth; birth_frame <= max_birth; birth_frame++) {
		int age = local - birth_frame;
		for (unsigned instance = 0; instance < parts_flat_key_count(timeline, birth_frame); instance++) {

		// Color, opacity and inherited scale/rotation follow the current key.
		// Position tracking is independent: 0 = world origin, 1 = current
		// emitter position, 2 = emitter position when the particle was born.
		const struct flat_key_data_graphic *birth_key = parts_flat_key(timeline, birth_frame, instance);
		const struct flat_key_data_graphic *key = parts_flat_key(timeline, local, instance);
		if (!key)
			continue;

		bool need_birth_record = (em->pos_track_mode == 2)
				|| (key_stack->count > 0 && em->parent_key_mode == 2);
		struct flat_emitter_birth *birth = parts_flat_get_emitter_birth(
				state, timeline, tl_idx, birth_frame, instance, need_birth_record);
		if (birth && !birth->has_ancestor_keys && key_stack->count > 0) {
			birth->ancestor_count = key_stack->count;
			for (int d = 0; d < key_stack->count; d++) {
				const struct flat_key_data_graphic *ak = NULL;
				const struct flat_timeline *atl = key_stack->timelines[d];
				if (atl && atl->frame_count > 0) {
					int anc_local = key_stack->locals[d] - age;
					anc_local = min(max(0, anc_local), (int)atl->frame_count - 1);
					ak = parts_flat_key(atl, anc_local, key_stack->instances[d]);
				}
				birth->ancestor_keys[d] = ak ? *ak : *key_stack->keys[d];
			}
			birth->has_ancestor_keys = true;
		}

		mat4 base;
		if (em->parent_key_mode == 2 && birth && birth->has_ancestor_keys) {
			struct flat_key_stack birth_stack = { .count = birth->ancestor_count };
			for (int d = 0; d < birth->ancestor_count; d++)
				birth_stack.keys[d] = &birth->ancestor_keys[d];
			parts_flat_build_emitter_base_matrix(em, &birth_stack, base);
		} else {
			glm_mat4_copy(current_base, base);
		}

		float layer_alpha = parent_alpha * key->alpha / 255.0f;

		struct flat_emitter_layer_effective eff;
		parts_flat_emitter_resolve_layer(em, key,
				parts_alpha, layer_alpha, &eff);
		if (em->pos_track_mode == 2 && birth_key) {
			eff.pos[0] = birth_key->pos_x;
			eff.pos[1] = birth_key->pos_y;
		}

		// Build the layer matrix with the selected position and the current
		// key's remaining properties, filtered by the inherit_* flags.
		mat4 layer_m;
		parts_flat_build_layer_matrix(key, eff.pos,
				eff.use_rotation, eff.use_scale, eff.use_origin,
				eff.reverse_lr, eff.reverse_tb,
				layer_m);

		struct emitter_render_ud ud = {
			.f = f,
			.parent_alpha = eff.alpha,
			.alpha_clipper = parts_get_effective_alpha_clipper(parts),
			.draw_filter = eff.draw_filter,
		};
		glm_vec2_copy(align, ud.align);
		glm_vec3_copy(eff.add_color, ud.add_color);
		glm_vec3_copy(eff.mul_color, ud.mul_color);
		glm_mat4_mul(base, layer_m, ud.transform);
		glm_mat4_mul(root, ud.transform, ud.transform);
		if (em->pos_track_mode == 0) {
			ud.transform[3][0] = 0;
			ud.transform[3][1] = 0;
			ud.transform[3][2] = 0;
		} else if (em->pos_track_mode == 2 && birth) {
			if (!birth->has_position) {
				if (key_stack->count > 0 && birth->has_ancestor_keys && em->parent_key_mode != 2) {
					struct flat_key_stack birth_stack = { .count = birth->ancestor_count };
					for (int d = 0; d < birth->ancestor_count; d++)
						birth_stack.keys[d] = &birth->ancestor_keys[d];
					mat4 birth_base, birth_transform;
					parts_flat_build_emitter_base_matrix(em, &birth_stack, birth_base);
					glm_mat4_mul(birth_base, layer_m, birth_transform);
					glm_mat4_mul(root, birth_transform, birth_transform);
					glm_vec3_copy(birth_transform[3], birth->position);
				} else {
					glm_vec3_copy(ud.transform[3], birth->position);
				}
				birth->has_position = true;
			}
			glm_vec3_copy(birth->position, ud.transform[3]);
		}
		parts_flat_foreach_emitter_particle(f, emitter_lib_idx, timeline, instance,
				birth_frame, age, frame_count,
				render_emitter_particle_cb, &ud);
		}
	}
}

struct flat_draw_ctx {
	mat4 matrix;
	float alpha;
	vec3 add_color;
	vec3 mul_color;
	int draw_filter;
};

static void render_flat_layer(struct parts *parts, struct parts_flat *f,
		struct flat_layer_state *state,
		struct flat_timeline *timelines, size_t nr_timelines,
		struct flat_draw_ctx *ctx, mat4 root,
		struct flat_key_stack *key_stack);

static void render_flat_cg(struct parts *parts, Texture *tex,
		const struct flat_key_data_graphic *key, struct flat_draw_ctx *ctx)
{
	if (!tex->handle)
		return;

	mat4 render_m;
	glm_mat4_copy(ctx->matrix, render_m);
	// uk2 is the texture anchor in the 1..9 grid, independent of explicit origin.
	if (key->uk2 >= 1 && key->uk2 <= 9) {
		int width = key->area_width > 0 ? key->area_width : tex->w;
		int height = key->area_height > 0 ? key->area_height : tex->h;
		int anchor = key->uk2 - 1;
		glm_translate(render_m, (vec3){-(anchor % 3) * width / 2.0f,
				-(anchor / 3) * height / 2.0f, 0});
	}
	// Kill the Z-output row to pin clip_z at the near plane, avoiding
	// near/far clipping of 3D-rotated sprites.
	render_m[0][2] = render_m[1][2] = render_m[2][2] = render_m[3][2] = 0.0f;
	// area_x/area_y select a sub-rectangle of the texture atlas, but
	// should not shift the on-screen position. This translation cancels
	// the offset that the sub-rect's top-left would otherwise introduce.
	glm_translate(render_m, (vec3){ -(float)key->area_x, -(float)key->area_y, 0.0f });
	glm_scale(render_m, (vec3){ tex->w, tex->h, 1.0f });

	Rectangle rect;
	if (key->area_width && key->area_height) {
		rect = (Rectangle){ key->area_x, key->area_y, key->area_width, key->area_height };
	} else {
		rect = (Rectangle){ 0, 0, tex->w, tex->h };
	}

	parts_render_texture(tex, render_m, &rect, ctx->alpha, ctx->add_color, ctx->mul_color,
			ctx->draw_filter, parts_get_effective_alpha_clipper(parts));
}

static void render_flat_item(struct parts *parts, struct parts_flat *f,
		int lib_idx, struct flat_layer_state *state, size_t tl_idx,
		struct flat_timeline *tl, int local,
		const struct flat_key_data_graphic *key,
		struct flat_draw_ctx *parent, mat4 root,
		struct flat_key_stack *key_stack)
{
	if (lib_idx < 0 || (size_t)lib_idx >= f->flat->nr_libraries)
		return;

	struct flat_library *lib = &f->flat->libraries[lib_idx];
	mat4 layer_m;
	vec2 pos = { key->pos_x, key->pos_y };
	parts_flat_build_layer_matrix(key, pos,
			true, true, true,
			key->reverse_lr, key->reverse_tb,
			layer_m);

	struct flat_draw_ctx ctx;
	glm_mat4_mul(parent->matrix, layer_m, ctx.matrix);
	ctx.alpha = parent->alpha * key->alpha / 255.0f;
	vec3 key_add = { key->add_r / 255.0f, key->add_g / 255.0f, key->add_b / 255.0f };
	vec3 key_mul = { key->mul_r / 255.0f, key->mul_g / 255.0f, key->mul_b / 255.0f };
	glm_vec3_add(parent->add_color, key_add, ctx.add_color);
	glm_vec3_mul(parent->mul_color, key_mul, ctx.mul_color);
	ctx.draw_filter = key->draw_filter != PARTS_DRAW_FILTER_NORMAL
			? key->draw_filter : parent->draw_filter;

	switch (lib->type) {
	case FLAT_LIB_CG:
		render_flat_cg(parts, &f->textures[lib_idx], key, &ctx);
		break;
	case FLAT_LIB_TIMELINE: {
		struct flat_layer_state *child = state->children[tl_idx];
		if (child && key_stack->count < FLAT_MAX_ANCESTOR_DEPTH) {
			int d = key_stack->count++;
			key_stack->keys[d] = key;
			key_stack->timelines[d] = tl;
			key_stack->locals[d] = local;
			key_stack->instances[d] = 0;
			render_flat_layer(parts, f, child,
					lib->timeline.timelines, lib->timeline.nr_timelines,
					&ctx, root, key_stack);
			key_stack->count--;
		}
		break;
	}
	case FLAT_LIB_STOP_MOTION: {
		int cg_idx = parts_flat_stop_motion_get_cg_lib(f, lib_idx, local);
		if (cg_idx >= 0 && (size_t)cg_idx < f->nr_libraries)
			render_flat_cg(parts, &f->textures[cg_idx], key, &ctx);
		break;
	}
	case FLAT_LIB_EMITTER:
		// cannot happen, handled above
		break;
	case FLAT_LIB_MEMORY:
		// not implemented
		break;
	}
}

static void render_flat_layer(struct parts *parts, struct parts_flat *f,
		struct flat_layer_state *state,
		struct flat_timeline *timelines, size_t nr_timelines,
		struct flat_draw_ctx *ctx, mat4 root,
		struct flat_key_stack *key_stack)
{
	if (state->current_frame < state->last_rendered_frame)
		parts_flat_clear_emitter_births(state);
	state->last_rendered_frame = state->current_frame;
	// reverse order for correct z-ordering
	for (size_t i = nr_timelines; i-- > 0;) {
		if (state->disabled_timelines && i < state->nr_timelines && state->disabled_timelines[i])
			continue;
		struct flat_timeline *tl = &timelines[i];
		if (tl->type != FLAT_TIMELINE_GRAPHIC)
			continue;
		int local = state->current_frame - tl->begin_frame;
		if (local < 0 || local >= tl->frame_count)
			continue;

		int lib_idx = parts_flat_find_library(f->flat, tl->library_name->text);
		if (lib_idx < 0)
			continue;
		if (f->flat->libraries[lib_idx].type == FLAT_LIB_EMITTER) {
			render_flat_emitter(parts, f, lib_idx, (int)i, local, tl->frame_count, tl, state,
					root, ctx->alpha, key_stack);
			continue;
		}
		for (unsigned instance = 0; instance < parts_flat_key_count(tl, local); instance++)
			render_flat_item(parts, f, lib_idx, state, i, tl, local, parts_flat_key(tl, local, instance),
					ctx, root, key_stack);
	}
}

static void parts_render_flat(struct parts *parts, struct parts_flat *f)
{
	if (!f->flat || !f->root_state)
		return;

	struct flat_draw_ctx ctx;
	parts_world_matrix(parts, ctx.matrix);
	glm_translate(ctx.matrix, (vec3){f->common.origin_offset.x, f->common.origin_offset.y, 0});
	ctx.alpha = parts->global.alpha / 255.0f;
	ctx.add_color[0] = parts->global.add_color.r / 255.0f;
	ctx.add_color[1] = parts->global.add_color.g / 255.0f;
	ctx.add_color[2] = parts->global.add_color.b / 255.0f;
	ctx.mul_color[0] = parts->global.multiply_color.r / 255.0f;
	ctx.mul_color[1] = parts->global.multiply_color.g / 255.0f;
	ctx.mul_color[2] = parts->global.multiply_color.b / 255.0f;
	ctx.draw_filter = parts->draw_filter;

	struct flat_key_stack key_stack = { .count = 0 };
	render_flat_layer(parts, f, f->root_state,
			f->flat->timelines, f->flat->nr_timelines,
			&ctx, ctx.matrix, &key_stack);
}

static void parts_render_3dlayer(struct parts *parts, struct parts_3dlayer *l)
{
	if (l->sprite_no < 0)
		return;
	struct sact_sprite *sp = sact_try_get_sprite(l->sprite_no);
	if (!sp)
		return;
	struct texture *tex = sprite_get_texture(sp);
	if (!tex->handle)
		return;
	// The bound sprite's texture is recreated by RE_set_viewport, so
	// refresh the parts_common's view of it each frame.
	l->common.texture = *tex;
	l->common.w = tex->w;
	l->common.h = tex->h;
	parts_render_cg(parts, &l->common);
}

static void parts_render_flash_shape(struct parts *parts, struct parts_flash *f, struct parts_flash_object *obj, struct swf_tag_define_shape *tag)
{
	struct texture *src = ht_get_int(f->bitmaps, tag->fill_style.bitmap_id, NULL);
	if (!src)
		ERROR("undefined bitmap id %d", tag->fill_style.bitmap_id);

	mat4 mw_transform = GLM_MAT4_IDENTITY_INIT;
	glm_translate(mw_transform, (vec3) { parts->global.pos.x, parts->global.pos.y, 0 });
	glm_rotate_z(mw_transform, parts->local.rotation.z, mw_transform);
	glm_scale(mw_transform, (vec3){ parts->global.scale.x, parts->global.scale.y, 1.0 });
	glm_translate(mw_transform, (vec3){ f->common.origin_offset.x, f->common.origin_offset.y, 0 });

	glm_mul(mw_transform, obj->matrix, mw_transform);

	glm_translate(mw_transform, (vec3){
		twips_to_float(tag->bounds.x_min),
		twips_to_float(tag->bounds.y_min),
		0});
	glm_scale(mw_transform, (vec3){
		twips_to_float(tag->bounds.x_max - tag->bounds.x_min),
		twips_to_float(tag->bounds.y_max - tag->bounds.y_min),
		1.0});

	Rectangle r = { 0, 0, src->w, src->h };

	float blend_rate = (parts->global.alpha / 255.0f) * fixed16_to_float(obj->color_transform.mult_terms[3]);
	vec3 add_color = {
		(parts->global.add_color.r + obj->color_transform.add_terms[0]) / 255.0f,
		(parts->global.add_color.g + obj->color_transform.add_terms[1]) / 255.0f,
		(parts->global.add_color.b + obj->color_transform.add_terms[2]) / 255.0f
	};
	vec3 multiply_color = {
		(parts->global.multiply_color.r / 255.0f) * fixed16_to_float(obj->color_transform.mult_terms[0]),
		(parts->global.multiply_color.g / 255.0f) * fixed16_to_float(obj->color_transform.mult_terms[1]),
		(parts->global.multiply_color.b / 255.0f) * fixed16_to_float(obj->color_transform.mult_terms[2])
	};
	parts_render_texture(src, mw_transform, &r, blend_rate, add_color, multiply_color, 0, parts_get_effective_alpha_clipper(parts));
}

static void parts_render_flash_sprite(struct parts *parts, struct parts_flash *f, struct parts_flash_object *obj, struct swf_tag_define_sprite *tag)
{
	struct parts_flash_object *obj2 = ht_get_int(f->sprites, tag->sprite_id, NULL);
	if (!obj2)
		return;  // sprite has no visual elements.

	struct swf_tag *child = ht_get_int(f->dictionary, obj2->character_id, NULL);
	if (!child)
		ERROR("character %d is not defined", obj2->character_id);
	if (child->type != TAG_DEFINE_SHAPE)
		ERROR("unexpected child type %d", child->type);

	struct parts_flash_object transform;
	glm_mul(obj->matrix, obj2->matrix, transform.matrix);
	for (int i = 0; i < 4; i++) {
		transform.color_transform.add_terms[i] =
			obj->color_transform.add_terms[i] + obj2->color_transform.add_terms[i];
	}
	for (int i = 0; i < 4; i++) {
		transform.color_transform.mult_terms[i] =
			fixed16_mul(obj->color_transform.mult_terms[i], obj2->color_transform.mult_terms[i]);
	}
	parts_render_flash_shape(parts, f, &transform, (struct swf_tag_define_shape*)child);
}

static void parts_render_flash(struct parts *parts, struct parts_flash *f)
{
	struct parts_flash_object *obj;
	TAILQ_FOREACH(obj, &f->display_list, entry) {
		struct swf_tag *tag = ht_get_int(f->dictionary, obj->character_id, NULL);
		if (!tag) {
			WARNING("character %d is not defined", obj->character_id);
			continue;
		}
		if (parts_pass_active && pass_draw_filter != PARTS_DRAW_FILTER_NORMAL) {
			glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
			glUniform1i(parts_shader.draw_filter, PARTS_DRAW_FILTER_NORMAL);
			pass_draw_filter = PARTS_DRAW_FILTER_NORMAL;
		}
		// set blend mode
		switch (obj->blend_mode) {
		case PARTS_FLASH_BLEND_MULTIPLY:
			glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
			break;
		case PARTS_FLASH_BLEND_SCREEN:
			glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ZERO, GL_ONE);
			break;
		case PARTS_FLASH_BLEND_ADD:
			glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
			break;
		default:
			break;
		}
		// render object
		switch (tag->type) {
		case TAG_DEFINE_SHAPE:
			parts_render_flash_shape(parts, f, obj, (struct swf_tag_define_shape*)tag);
			break;
		case TAG_DEFINE_SPRITE:
			parts_render_flash_sprite(parts, f, obj, (struct swf_tag_define_sprite*)tag);
			break;
		default:
			ERROR("unknown tag %d", tag->type);
		}
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
	}
}

bool parts_cg_on_screen(struct parts *parts, struct parts_common *common)
{
	if (!parts || !common || common->w <= 0 || common->h <= 0 || parts->global.alpha <= 0)
		return false;

	mat4 mw_transform;
	parts_world_matrix(parts, mw_transform);
	glm_translate(mw_transform, (vec3){ common->origin_offset.x, common->origin_offset.y, 0 });
	switch (parts->sprite_deform) {
	case 1:
		glm_translate(mw_transform, (vec3){ common->w, 0.0f, 0.0f });
		glm_scale(mw_transform, (vec3){ -common->w, common->h, 1.0f });
		break;
	case 2:
		glm_translate(mw_transform, (vec3){ 0.0f, common->h, 0.0f });
		glm_scale(mw_transform, (vec3){ common->w, -common->h, 1.0f });
		break;
	default:
	case 0:
		glm_scale(mw_transform, (vec3){ common->w, common->h, 1.0f });
		break;
	}

	Rectangle r = common->surface_area;
	if (!r.w && !r.h) {
		r = (Rectangle){ 0, 0, common->w, common->h };
	}

	mat4 sub_mw;
	vec4 uv_rect;
	if (!crop_quad_transform(mw_transform, common->w, common->h, &r, sub_mw, uv_rect))
		return false;

	float q_min_x, q_max_x, q_min_y, q_max_y;
	quad_screen_aabb(sub_mw, &q_min_x, &q_max_x, &q_min_y, &q_max_y);
	if (q_max_x <= 0.0f || q_min_x >= (float)config.view_width ||
			q_max_y <= 0.0f || q_min_y >= (float)config.view_height)
		return false;

	int alpha_clipper = parts_get_effective_alpha_clipper(parts);
	if (alpha_clipper > 0) {
		float clip_min_x, clip_max_x, clip_min_y, clip_max_y;
		if (get_alpha_clipper_info(alpha_clipper, NULL, NULL,
				&clip_min_x, &clip_max_x, &clip_min_y, &clip_max_y)) {
			if (q_max_x <= clip_min_x || q_min_x >= clip_max_x ||
					q_max_y <= clip_min_y || q_min_y >= clip_max_y)
				return false;
		}
	}
	return true;
}

bool parts_sprite_is_fullscreen_opaque(struct sprite *sp)
{
	if (!sp || sp->render != parts_sprite_render)
		return false;
	struct parts *parts = (struct parts *)sp;
	if (parts->textures_suspended || !parts_is_visible(parts))
		return false;
	if (parts->scrollbar || parts->message_box)
		return false;
	if (parts->component_type == 1 && parts->widget)
		return false;
	if (parts->global.alpha < 255 || parts->draw_filter != PARTS_DRAW_FILTER_NORMAL)
		return false;
	if (parts_get_effective_alpha_clipper(parts) > 0)
		return false;

	struct parts_state *state = parts_display_state(parts);
	if (state->type == PARTS_CG || state->type == PARTS_ANIMATION ||
			state->type == PARTS_CONSTRUCTION_PROCESS) {
		struct parts_common *common = &state->common;
		if (!common->texture.handle || !common->texture.opaque ||
				common->w <= 0 || common->h <= 0)
			return false;
		if (common->surface_area.w || common->surface_area.h || parts->sprite_deform != 0)
			return false;
		mat4 m;
		parts_world_matrix(parts, m);
		if (m[0][1] != 0.0f || m[1][0] != 0.0f || m[0][0] <= 0.0f || m[1][1] <= 0.0f)
			return false;
		float x0 = m[3][0] + m[0][0] * common->origin_offset.x;
		float y0 = m[3][1] + m[1][1] * common->origin_offset.y;
		float x1 = x0 + m[0][0] * common->w;
		float y1 = y0 + m[1][1] * common->h;
		return x0 <= 0.0f && y0 <= 0.0f &&
			x1 >= (float)config.view_width && y1 >= (float)config.view_height;
	}

	if (state->type == PARTS_FLAT && state->flat.flat && state->flat.root_state) {
		struct parts_flat *f = &state->flat;
		mat4 root_m;
		parts_world_matrix(parts, root_m);
		glm_translate(root_m, (vec3){ f->common.origin_offset.x, f->common.origin_offset.y, 0 });
		if (root_m[0][1] != 0.0f || root_m[1][0] != 0.0f ||
				root_m[0][0] <= 0.0f || root_m[1][1] <= 0.0f)
			return false;

		for (size_t i = f->flat->nr_timelines; i-- > 0;) {
			if (f->root_state->disabled_timelines && i < f->root_state->nr_timelines &&
					f->root_state->disabled_timelines[i])
				continue;
			struct flat_timeline *tl = &f->flat->timelines[i];
			if (tl->type != FLAT_TIMELINE_GRAPHIC)
				continue;
			int local = f->root_state->current_frame - tl->begin_frame;
			if (local < 0 || local >= tl->frame_count || parts_flat_key_count(tl, local) == 0)
				continue;
			const struct flat_key_data_graphic *key = parts_flat_key(tl, local, 0);
			if (!key || key->alpha < 255 || key->draw_filter != PARTS_DRAW_FILTER_NORMAL ||
					key->reverse_lr || key->reverse_tb ||
					key->area_width != 0 || key->area_height != 0)
				continue;
			int lib_idx = parts_flat_find_library(f->flat, tl->library_name->text);
			if (lib_idx < 0 || (size_t)lib_idx >= f->nr_libraries)
				continue;
			if (f->flat->libraries[lib_idx].type == FLAT_LIB_STOP_MOTION) {
				lib_idx = parts_flat_stop_motion_get_cg_lib(f, lib_idx, local);
				if (lib_idx < 0 || (size_t)lib_idx >= f->nr_libraries)
					continue;
			} else if (f->flat->libraries[lib_idx].type != FLAT_LIB_CG) {
				continue;
			}
			Texture *tex = &f->textures[lib_idx];
			if (!tex->handle || !tex->opaque || tex->w <= 0 || tex->h <= 0)
				continue;

			mat4 layer_m, render_m;
			vec2 pos = { key->pos_x, key->pos_y };
			parts_flat_build_layer_matrix(key, pos, true, true, true, false, false, layer_m);
			glm_mat4_mul(root_m, layer_m, render_m);
			if (key->uk2 >= 1 && key->uk2 <= 9) {
				int anchor = key->uk2 - 1;
				glm_translate(render_m, (vec3){ -(anchor % 3) * tex->w / 2.0f,
						-(anchor / 3) * tex->h / 2.0f, 0 });
			}
			render_m[0][2] = render_m[1][2] = render_m[2][2] = render_m[3][2] = 0.0f;
			glm_translate(render_m, (vec3){ -(float)key->area_x, -(float)key->area_y, 0.0f });
			glm_scale(render_m, (vec3){ tex->w, tex->h, 1.0f });
			if (render_m[0][1] == 0.0f && render_m[1][0] == 0.0f &&
					render_m[0][0] >= (float)config.view_width &&
					render_m[1][1] >= (float)config.view_height &&
					render_m[3][0] <= 0.0f && render_m[3][1] <= 0.0f &&
					render_m[3][0] + render_m[0][0] >= (float)config.view_width &&
					render_m[3][1] + render_m[1][1] >= (float)config.view_height) {
				return true;
			}
		}
	}
	return false;
}

static void parts_render_state(struct parts *parts, struct parts_state *state)
{
	switch (state->type) {
	case PARTS_UNINITIALIZED:
	case PARTS_RECT_DETECTION:
	case PARTS_LAYOUT_BOX:
		break;
	case PARTS_CONSTRUCTION_PROCESS:
		if (state->cproc.needs_build && !parts->textures_suspended) {
			if (!parts_cg_on_screen(parts, &state->common))
				break;
			parts_build_construction_process(parts, &state->cproc);
			parts_render_pass_rebind();
		}
		if (state->common.texture.handle)
			parts_render_cg(parts, &state->common);
		break;
	case PARTS_CG:
	case PARTS_ANIMATION:
	case PARTS_NUMERAL:
	case PARTS_HGAUGE:
	case PARTS_VGAUGE:
	case PARTS_MOVIE:
		if (state->common.texture.handle)
			parts_render_cg(parts, &state->common);
		break;
	case PARTS_TEXT:
		parts_render_text(parts, &state->text, NULL);
		break;
	case PARTS_FLASH:
		parts_render_flash(parts, &state->flash);
		break;
	case PARTS_FLAT:
		parts_render_flat(parts, &state->flat);
		break;
	case PARTS_3DLAYER:
		parts_render_3dlayer(parts, &state->layer3d);
		break;
	}
}

static void parts_render_message_key(struct parts *parts)
{
	struct parts_message_window *w = parts->message_box;
	if (!w->key_show) return;
	struct parts_state key = w->key_wait;
	key.common.origin_offset.x = parts->states[0].common.origin_offset.x + w->key_x;
	key.common.origin_offset.y = parts->states[0].common.origin_offset.y + w->key_y;
	parts_render_state(parts, &key);
}

static void parts_render_scrollbar(struct parts *parts)
{
	struct parts_scrollbar *s = parts->scrollbar;
	Rectangle rects[4];
	parts_scrollbar_rects(parts, rects);
	const int order[] = {3, 0, 1, 2};
	for (int i = 0; i < 4; i++) {
		int piece = order[i], index = parts_scrollbar_skin_state(s, piece);
		Rectangle rect = rects[piece];
		if (rect.w <= 0 || rect.h <= 0 || (piece == 2 && !parts_scrollbar_range(s))) continue;
		struct parts_state state = s->skins[piece][index];
		if (state.common.w <= 0 || state.common.h <= 0) continue;
		struct parts proxy = *parts;
		proxy.parent = parts;
		proxy.position_is_ratio = false;
		memset(proxy.scroll_links, 0, sizeof(proxy.scroll_links));
		proxy.local.pos = (SDL_FPoint){rect.x + parts->states[0].common.origin_offset.x,
			rect.y + parts->states[0].common.origin_offset.y};
		proxy.local.rotation.x = proxy.local.rotation.y = proxy.local.rotation.z = 0;
		proxy.local.scale.x = !s->loaded[piece][index] ? (float)rect.w / state.common.w : 1.f;
		proxy.local.scale.y = !s->loaded[piece][index] ? (float)rect.h / state.common.h : 1.f;
		if (piece == 3 && state.type == PARTS_CG && s->loaded[piece][index]) {
			// Sys43 clips a background skin to the track; it does not stretch it.
			state.common.surface_area = (Rectangle){0, 0,
				min(rect.w, state.common.w), min(rect.h, state.common.h)};
		}
		state.common.origin_offset = (Point){0, 0};
		parts_render_state(&proxy, &state);
	}
}

void parts_render(struct parts *parts)
{
	if (parts->textures_suspended || !parts_is_visible(parts))
		return;
	if (parts->scrollbar) {
		parts_render_scrollbar(parts);
		return;
	}
	struct parts_state *state = parts_display_state(parts);
	if (parts->message_box) {
		struct parts_message_window *w = parts->message_box;
		struct parts p = *parts;
		if (!w->active) {
			p.global.multiply_color.r = p.global.multiply_color.r * min(255, max(0, w->inactive_color[0])) / 255;
			p.global.multiply_color.g = p.global.multiply_color.g * min(255, max(0, w->inactive_color[1])) / 255;
			p.global.multiply_color.b = p.global.multiply_color.b * min(255, max(0, w->inactive_color[2])) / 255;
		}
		if (w->key_z < 0) parts_render_message_key(&p);
		parts_render_state(&p, state);
		struct parts_text label = w->label.text;
		int mode = w->text_origin >= 1 && w->text_origin <= 9 ? w->text_origin - 1 : 0;
		label.common.origin_offset.x = state->common.origin_offset.x + w->text_area.x
			+ (w->text_area.w - label.common.w) * (mode % 3) / 2;
		label.common.origin_offset.y = state->common.origin_offset.y + w->text_area.y
			+ (w->text_area.h - label.common.h) * (mode / 3) / 2;
		parts_render_text(&p, &label, w);
		if (w->key_z >= 0) parts_render_message_key(&p);
		return;
	}
	struct parts_state checkbox_state;
	struct parts_common *label_box = &state->common;
	int label_width = state->common.w, label_height = state->common.h;
	if (parts->component_type == 1 && parts->widget) {
		struct parts_widget *w = parts->widget;
		struct parts_state *normal = w->enabled ? &parts->states[parts->state] : &w->disabled;
		label_box = &normal->common;
		parts_widget_get_size(parts, &label_width, &label_height);
		checkbox_state = *state;
		checkbox_state.common.origin_offset = normal->common.origin_offset;
		checkbox_state.common.origin_offset.y += (label_height - state->common.h) / 2;
		state = &checkbox_state;
	}
	parts_render_state(parts, state);
	if (parts->widget && parts->widget->text->size) {
		struct parts_widget *w = parts->widget;
		bool has_skin = w->cg_name->size || w->flat_name->size
			|| (parts->component_type == 1 && (w->override_cg[w->checked]->size || w->override_flat[w->checked]->size));
		if ((parts->component_type == 1 && !w->button_mode) || !has_skin) {
			struct parts_text label = w->label.text;
			int mode = w->text_origin >= 1 && w->text_origin <= 9 ? w->text_origin - 1 : 4;
			label.common.origin_offset.x = label_box->origin_offset.x
				+ (parts->component_type == 1 && !w->button_mode ? label_box->w
					: (label_width - label.common.w) * (mode % 3) / 2);
			label.common.origin_offset.y = label_box->origin_offset.y
				+ (label_height - label.common.h) * (mode / 3) / 2;
			parts_render_text(parts, &label, NULL);
		}
	}
}

void parts_render_family(struct parts *parts)
{
	parts_render(parts);

	struct parts *child;
	PARTS_FOREACH_CHILD(child, parts) {
		parts_render_family(child);
	}
}

void parts_engine_render(possibly_unused struct sprite *_)
{
	struct parts *parts;
	PARTS_LIST_FOREACH(parts) {
		parts_render(parts);
	}
}

void parts_sprite_render(struct sprite *sp)
{
	parts_render((struct parts*)sp);
}

static bool pe_dirty = false;

bool parts_is_dirty(void)
{
	return pe_dirty;
}

void parts_render_update(void)
{
	if (pe_dirty) {
		scene_dirty();
		pe_dirty = false;
	}
}

void parts_engine_dirty(void)
{
	pe_dirty = true;
	parts_render_gen++;
}

void parts_engine_clean(void)
{
	pe_dirty = false;
}

void parts_dirty(possibly_unused struct parts *parts)
{
	pe_dirty = true;
	parts_render_gen++;
}

void parts_render_init(void)
{
	gfx_load_shader(&parts_shader.shader, "shaders/parts.v.glsl", "shaders/parts.f.glsl");
	parts_shader.uv_rect = glGetUniformLocation(parts_shader.shader.program, "uv_rect");
	parts_shader.blend_rate = glGetUniformLocation(parts_shader.shader.program, "blend_rate");
	parts_shader.add_color = glGetUniformLocation(parts_shader.shader.program, "add_color");
	parts_shader.multiply_color = glGetUniformLocation(parts_shader.shader.program, "multiply_color");
	parts_shader.draw_filter = glGetUniformLocation(parts_shader.shader.program, "draw_filter");
	parts_shader.use_clipper = glGetUniformLocation(parts_shader.shader.program, "use_clipper");
	parts_shader.clipper_tex = glGetUniformLocation(parts_shader.shader.program, "clipper_tex");
	parts_shader.inv_clipper_transform = glGetUniformLocation(parts_shader.shader.program, "inv_clipper_transform");
}
