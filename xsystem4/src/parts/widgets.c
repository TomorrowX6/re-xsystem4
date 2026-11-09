/* Native Parts button rendering and properties, GPL-2.0-or-later. */
#include <stdlib.h>
#include <limits.h>
#include <string.h>

#include "system4.h"
#include "system4/string.h"
#include "system4/utfsjis.h"
#include "parts.h"
#include "parts_activity.h"
#include "parts_widgets.h"
#include "vm.h"
#include "parts_internal.h"

static void set_string(struct string **out, struct string *value)
{
	struct string *copy = string_dup(value);
	free_string(*out);
	*out = copy;
}

void parts_widget_free(struct parts *parts)
{
	struct parts_widget *w = parts->widget;
	if (!w) return;
	parts_state_reset(&w->disabled, PARTS_UNINITIALIZED);
	parts_state_reset(&w->checked_disabled, PARTS_UNINITIALIZED);
	for (int i = 0; i < PARTS_NR_STATES; i++)
		parts_state_reset(&w->checked_states[i], PARTS_UNINITIALIZED);
	parts_state_reset(&w->label, PARTS_UNINITIALIZED);
	free_string(w->cg_name);
	free_string(w->flat_name);
	free_string(w->text);
	for (int i = 0; i < 2; i++) {
		free_string(w->override_cg[i]);
		free_string(w->override_flat[i]);
	}
	free(w);
	parts->widget = NULL;
}

static bool load_skin(struct parts *parts, struct parts_state *state,
		struct string *base, bool flat, const char *suffix)
{
	char *encoded = utf2sjis(suffix, strlen(suffix));
	struct string *name = string_dup(base);
	string_append_cstr(&name, encoded, strlen(encoded));
	free(encoded);
	parts_state_reset(state, flat ? PARTS_FLAT : PARTS_CG);
	bool ok = flat ? parts_flat_load(parts, &state->flat, name)
		: parts_cg_set(parts, &state->cg, name);
	free_string(name);
	if (!ok) parts_state_reset(state, PARTS_UNINITIALIZED);
	return ok;
}

static void draw_plain_button(struct parts *parts, struct parts_state *state, bool hovered, bool depressed)
{
	struct parts_widget *w = parts->widget;
	parts_state_reset(state, PARTS_CG);
	int width = max(0, w->width), height = max(0, w->height);
	parts_set_dims(parts, &state->common, width, height);
	if (!width || !height) return;
	int r = min(255, max(0, w->color[0]));
	int g = min(255, max(0, w->color[1]));
	int b = min(255, max(0, w->color[2]));
	if (hovered) {
		r = min(255, r + 16); g = min(255, g + 16); b = min(255, b + 16);
	}
	gfx_init_texture_rgba(&state->common.texture, width, height, (SDL_Color){r, g, b, 255});
	int upper = depressed ? 96 : 240;
	int lower = depressed ? 240 : 96;
	gfx_fill(&state->common.texture, 0, 0, width, 1, upper, upper, upper);
	gfx_fill(&state->common.texture, 0, 0, 1, height, upper, upper, upper);
	gfx_fill(&state->common.texture, 0, height - 1, width, 1, lower, lower, lower);
	gfx_fill(&state->common.texture, width - 1, 0, 1, height, lower, lower, lower);
}

static void rebuild_background(struct parts *parts)
{
	struct parts_widget *w = parts->widget;
	static const char *suffixes[4][2] = {
		{ "／一般", "／通常" }, { "／オン", "／オン" },
		{ "／DOWN", "／DOWN" }, { "／无效", "／無効" }
	};
	bool checkbox = parts->component_type == 1;
	for (int checked = 0; checked < (checkbox ? 2 : 1); checked++) {
	bool flat = w->override_flat[checked]->size || (!w->override_cg[checked]->size && w->flat_name->size);
	struct string *override = flat ? w->override_flat[checked] : w->override_cg[checked];
	struct string *base = flat ? w->flat_name : w->cg_name;
	if (override->size) base = override;
	struct string *checked_base = string_dup(base);
	if (checked && !override->size) {
		char *suffix = utf2sjis("／检查", strlen("／检查"));
		string_append_cstr(&checked_base, suffix, strlen(suffix));
		free(suffix);
	}
	for (int i = 0; i < 4; i++) {
		struct parts_state *state = checked
			? (i < PARTS_NR_STATES ? &w->checked_states[i] : &w->checked_disabled)
			: (i < PARTS_NR_STATES ? &parts->states[i] : &w->disabled);
		bool loaded = false;
		if (base->size) {
			loaded = load_skin(parts, state, checked_base, flat, suffixes[i][0]);
			if (!loaded && strcmp(suffixes[i][0], suffixes[i][1]))
				loaded = load_skin(parts, state, checked_base, flat, suffixes[i][1]);
			if (!loaded && i && !(checkbox && i == 3)) {
				loaded = load_skin(parts, state, checked_base, flat, suffixes[0][0]);
				if (!loaded) loaded = load_skin(parts, state, checked_base, flat, suffixes[0][1]);
			}
		}
		if (!base->size) {
			bool depressed = (i == PARTS_STATE_CLICKED) || (checked && w->button_mode);
			draw_plain_button(parts, state, i == PARTS_STATE_HOVERED, depressed);
			if (checked && !w->button_mode && state->common.texture.handle) {
				int width = state->common.w, height = state->common.h;
				for (int x = 3; x < width - 3; x++) {
					int y = x < width / 2 ? height / 2 + x - 3
						: height - 4 - (x - width / 2) * 2;
					gfx_fill(&state->common.texture, x, max(2, min(height - 4, y)), 2, 2, 0, 0, 0);
				}
			}
		}
	}
	free_string(checked_base);
	}
	parts_recalculate_hitbox(parts);
	parts_component_dirty(parts);
	parts_dirty(parts);
}

static void rebuild_label(struct parts *parts)
{
	struct parts_widget *w = parts->widget;
	struct text_style style = w->label.text.ts;
	int line_space = w->label.text.line_space;
	parts_state_reset(&w->label, PARTS_TEXT);
	w->label.text.ts = style;
	w->label.text.ts.font_size = NULL;
	w->label.text.line_space = line_space;
	parts_text_append(parts, &w->label.text, w->text);
	parts_recalculate_hitbox(parts);
	parts_component_dirty(parts);
	parts_dirty(parts);
}

void parts_widget_rebuild(struct parts *parts)
{
	if (!parts->widget) return;
	rebuild_background(parts);
	rebuild_label(parts);
}

static void parts_widget_create_type(struct parts *parts, int type)
{
	if (parts->widget && parts->component_type == type) return;
	parts_widget_free(parts);
	parts_panel_free(parts);
	parts_scrollbar_free(parts);
	parts_message_window_free(parts);
	struct parts_widget *w = parts->widget = xcalloc(1, sizeof(*w));
	w->enabled = true;
	w->width = type == 1 ? 16 : 200;
	w->height = type == 1 ? 16 : 30;
	w->color[0] = w->color[1] = w->color[2] = 200;
	w->text_origin = type == 1 ? 4 : 5;
	w->cg_name = string_ref(&EMPTY_STRING);
	w->flat_name = string_ref(&EMPTY_STRING);
	w->text = string_ref(&EMPTY_STRING);
	for (int i = 0; i < 2; i++) {
		w->override_cg[i] = string_ref(&EMPTY_STRING);
		w->override_flat[i] = string_ref(&EMPTY_STRING);
	}
	parts_state_reset(&w->label, PARTS_TEXT);
	w->label.text.ts.color = type == 1 ? (SDL_Color){255, 255, 255, 255} : (SDL_Color){0, 0, 0, 255};
	parts->component_type = type;
	parts_widget_rebuild(parts);
}

void parts_widget_create(struct parts *parts) { parts_widget_create_type(parts, 0); }
void parts_checkbox_create(struct parts *parts) { parts_widget_create_type(parts, 1); }

void parts_widget_get_size(struct parts *parts, int *width, int *height)
{
	struct parts_widget *w = parts->widget;
	struct parts_state *state = w->enabled ? &parts->states[parts->state] : &w->disabled;
	*width = state->common.w;
	*height = state->common.h;
	if (parts->component_type == 1 && !w->button_mode && w->text->size) {
		*width += w->label.common.w;
		*height = max(*height, w->label.common.h);
	}
}

struct parts_state *parts_display_state(struct parts *parts)
{
	if (parts->message_box || parts->panel) return &parts->states[0];
	if (parts->component_type == 1 && parts->widget && parts->widget->checked)
		return parts->widget->enabled ? &parts->widget->checked_states[parts->state] : &parts->widget->checked_disabled;
	return parts->widget && !parts->widget->enabled
		? &parts->widget->disabled : &parts->states[parts->state];
}

static struct parts_widget *button(int no)
{
	struct parts *parts = parts_get(no);
	parts_widget_create(parts);
	return parts->widget;
}

void PE_SetButtonSize(int no, int width, int height)
{
	struct parts_widget *w = button(no);
	if (w->width == width && w->height == height) return;
	w->width = width; w->height = height;
	rebuild_background(parts_get(no));
}

void PE_SetButtonEnable(int no, bool enabled)
{
	struct parts_widget *w = button(no);
	if (w->enabled == enabled) return;
	w->enabled = enabled;
	if (!enabled) parts_set_state(parts_get(no), PARTS_STATE_DEFAULT);
	parts_dirty(parts_get(no));
}

bool PE_IsButtonEnable(int no) { return button(no)->enabled; }

void PE_SetButtonColor(int no, int r, int g, int b)
{
	struct parts_widget *w = button(no);
	if (w->color[0] == r && w->color[1] == g && w->color[2] == b) return;
	w->color[0] = r; w->color[1] = g; w->color[2] = b;
	if (!w->cg_name->size && !w->flat_name->size) rebuild_background(parts_get(no));
}

int PE_GetButtonR(int no) { return button(no)->color[0]; }
int PE_GetButtonG(int no) { return button(no)->color[1]; }
int PE_GetButtonB(int no) { return button(no)->color[2]; }

static void widget_set_font(struct parts *parts, int type, int size, int r, int g, int b,
		float bold, int edge_r, int edge_g, int edge_b, float edge)
{
	struct text_style *ts = &parts->widget->label.text.ts;
	SDL_Color color = {r, g, b, 255};
	SDL_Color edge_color = {edge_r, edge_g, edge_b, 255};
	float old_left = ts->edge_left, old_up = ts->edge_up, old_right = ts->edge_right, old_down = ts->edge_down;
	text_style_set_edge_width(ts, edge);
	if (ts->face == type && ts->size == size && !memcmp(&ts->color, &color, sizeof(color))
			&& ts->bold_width == bold && !memcmp(&ts->edge_color, &edge_color, sizeof(edge_color))
			&& ts->edge_left == old_left && ts->edge_up == old_up
			&& ts->edge_right == old_right && ts->edge_down == old_down)
		return;
	ts->face = type; ts->size = size;
	ts->color = color;
	ts->bold_width = bold; ts->weight = bold * 1000;
	ts->edge_color = edge_color;
	rebuild_label(parts);
}

static void widget_get_font(struct parts *parts, int *type, int *size, int *r, int *g, int *b,
		float *bold, int *edge_r, int *edge_g, int *edge_b, float *edge)
{
	struct text_style *ts = &parts->widget->label.text.ts;
	*type = ts->face; *size = ts->size;
	*r = ts->color.r; *g = ts->color.g; *b = ts->color.b;
	*bold = ts->bold_width; *edge = ts->edge_left;
	*edge_r = ts->edge_color.r; *edge_g = ts->edge_color.g; *edge_b = ts->edge_color.b;
}

void PE_SetButtonFontProperty(int no, int type, int size, int r, int g, int b,
		float bold, int edge_r, int edge_g, int edge_b, float edge)
{
	button(no);
	widget_set_font(parts_get(no), type, size, r, g, b, bold, edge_r, edge_g, edge_b, edge);
}

void PE_GetButtonFontProperty(int no, int *type, int *size, int *r, int *g, int *b,
		float *bold, int *edge_r, int *edge_g, int *edge_b, float *edge)
{
	button(no);
	widget_get_font(parts_get(no), type, size, r, g, b, bold, edge_r, edge_g, edge_b, edge);
}

void PE_SetButtonCGName(int no, struct string *name)
{
	struct parts_widget *w = button(no);
	if (!strcmp(w->cg_name->text, name->text)) return;
	set_string(&w->cg_name, name);
	rebuild_background(parts_get(no));
}
struct string *PE_GetButtonCGName(int no) { return string_ref(button(no)->cg_name); }

void PE_SetButtonFlatName(int no, struct string *name)
{
	struct parts_widget *w = button(no);
	if (!strcmp(w->flat_name->text, name->text)) return;
	set_string(&w->flat_name, name);
	rebuild_background(parts_get(no));
}
struct string *PE_GetButtonFlatName(int no) { return string_ref(button(no)->flat_name); }

void PE_SetButtonText(int no, struct string *text)
{
	struct parts_widget *w = button(no);
	if (!strcmp(w->text->text, text->text)) return;
	set_string(&w->text, text);
	rebuild_label(parts_get(no));
}
struct string *PE_GetButtonText(int no) { return string_ref(button(no)->text); }

void PE_SetButtonTextOriginPosMode(int no, int mode)
{
	struct parts_widget *w = button(no);
	if (w->text_origin == mode) return;
	w->text_origin = mode;
	parts_dirty(parts_get(no));
}
int PE_GetButtonTextOriginPosMode(int no) { return button(no)->text_origin; }

void PE_SetButtonCharSpace(int no, int space)
{
	struct parts_widget *w = button(no);
	if (w->label.text.ts.font_spacing == space) return;
	w->label.text.ts.font_spacing = space;
	rebuild_label(parts_get(no));
}
int PE_GetButtonCharSpace(int no) { return button(no)->label.text.ts.font_spacing; }

void PE_SetButtonLineSpace(int no, int space)
{
	struct parts_widget *w = button(no);
	if (w->label.text.line_space == space) return;
	w->label.text.line_space = space;
	rebuild_label(parts_get(no));
}
int PE_GetButtonLineSpace(int no) { return button(no)->label.text.line_space; }

static struct parts_widget *checkbox(int no)
{
	struct parts *parts = parts_get(no);
	parts_checkbox_create(parts);
	return parts->widget;
}

void PE_SetCheckBoxSize(int no, int width, int height)
{
	struct parts_widget *w = checkbox(no);
	if (w->width == width && w->height == height) return;
	w->width = width;
	w->height = height;
	rebuild_background(parts_get(no));
}
int PE_GetCheckBoxButtonWidth(int no) { checkbox(no); return parts_get(no)->states[0].common.w; }
int PE_GetCheckBoxButtonHeight(int no) { checkbox(no); return parts_get(no)->states[0].common.h; }

void PE_SetCheckBoxEnable(int no, bool enabled)
{
	struct parts_widget *w = checkbox(no);
	if (w->enabled == enabled) return;
	w->enabled = enabled;
	struct parts *parts = parts_get(no);
	if (!enabled) parts_set_state(parts, PARTS_STATE_DEFAULT);
	parts_recalculate_hitbox(parts);
	parts_dirty(parts);
}
bool PE_IsCheckBoxEnable(int no) { return checkbox(no)->enabled; }

static struct parts *find_radio_button_box_parent(int child_no)
{
	struct parts *p;
	PARTS_LIST_FOREACH(p) {
		if (p->component_type != 9)
			continue;
		for (int i = 0; i < p->nr_radio_children; i++) {
			if (p->radio_children[i] == child_no)
				return p;
		}
	}
	return NULL;
}

void parts_widget_check(struct parts *parts, bool checked)
{
	struct parts_widget *w = parts->widget;
	if (!w || !w->enabled || parts->component_type != 1 || w->checked == checked) return;
	struct parts *rbox = find_radio_button_box_parent(parts->no);
	if (rbox) {
		if (!checked)
			return;
		parts_msg_push(rbox, PARTS_MSG_RADIO_BUTTON_BOX, "");
		for (int i = 0; i < rbox->nr_radio_children; i++) {
			int sib_no = rbox->radio_children[i];
			if (sib_no == parts->no)
				continue;
			struct parts *sib = parts_try_get(sib_no);
			if (sib && sib->widget && sib->widget->checked) {
				sib->widget->checked = false;
				parts_msg_push(sib, PARTS_MSG_CHECK_CHANGED, "b", false);
				parts_dirty(sib);
			}
		}
	}
	w->checked = checked;
	parts_msg_push(parts, PARTS_MSG_CHECK_CHANGED, "b", checked);
	parts_dirty(parts);
}
void PE_CheckBoxChecked(int no, bool checked) { checkbox(no); parts_widget_check(parts_get(no), checked); }
bool PE_IsCheckBoxChecked(int no) { return checkbox(no)->checked; }

void PE_SetCheckBoxColor(int no, int r, int g, int b)
{
	struct parts_widget *w = checkbox(no);
	if (w->color[0] == r && w->color[1] == g && w->color[2] == b) return;
	w->color[0] = r;
	w->color[1] = g;
	w->color[2] = b;
	if (!w->cg_name->size && !w->flat_name->size
			&& (!w->override_cg[0]->size || !w->override_cg[1]->size)
			&& (!w->override_flat[0]->size || !w->override_flat[1]->size))
		rebuild_background(parts_get(no));
}
int PE_GetCheckBoxR(int no) { return checkbox(no)->color[0]; }
int PE_GetCheckBoxG(int no) { return checkbox(no)->color[1]; }
int PE_GetCheckBoxB(int no) { return checkbox(no)->color[2]; }

void PE_SetCheckBoxFontProperty(int no, int type, int size, int r, int g, int b,
		float bold, int edge_r, int edge_g, int edge_b, float edge)
{
	checkbox(no);
	widget_set_font(parts_get(no), type, size, r, g, b, bold, edge_r, edge_g, edge_b, edge);
}
void PE_GetCheckBoxFontProperty(int no, int *type, int *size, int *r, int *g, int *b,
		float *bold, int *edge_r, int *edge_g, int *edge_b, float *edge)
{
	checkbox(no);
	widget_get_font(parts_get(no), type, size, r, g, b, bold, edge_r, edge_g, edge_b, edge);
}

#define CHECKBOX_SKIN(property, member) \
void PE_SetCheckBox##property(int no, struct string *name) \
{ \
	struct parts_widget *w = checkbox(no); \
	if (!strcmp(w->member->text, name->text)) return; \
	set_string(&w->member, name); \
	rebuild_background(parts_get(no)); \
} \
struct string *PE_GetCheckBox##property(int no) { return string_ref(checkbox(no)->member); }
CHECKBOX_SKIN(CGName, cg_name)
CHECKBOX_SKIN(FlatName, flat_name)
CHECKBOX_SKIN(OverrideCGName, override_cg[0])
CHECKBOX_SKIN(OverrideCheckedCGName, override_cg[1])
CHECKBOX_SKIN(OverrideFlatName, override_flat[0])
CHECKBOX_SKIN(OverrideCheckedFlatName, override_flat[1])
#undef CHECKBOX_SKIN

void PE_SetCheckBoxText(int no, struct string *text)
{
	struct parts_widget *w = checkbox(no);
	if (!strcmp(w->text->text, text->text)) return;
	set_string(&w->text, text);
	rebuild_label(parts_get(no));
}
struct string *PE_GetCheckBoxText(int no) { return string_ref(checkbox(no)->text); }
void PE_SetCheckBoxTextOriginPosMode(int no, int mode)
{
	struct parts_widget *w = checkbox(no);
	if (w->text_origin == mode) return;
	w->text_origin = mode;
	parts_dirty(parts_get(no));
}
int PE_GetCheckBoxTextOriginPosMode(int no) { return checkbox(no)->text_origin; }
void PE_SetCheckBoxButtonMode(int no, bool enabled)
{
	struct parts_widget *w = checkbox(no);
	if (w->button_mode == enabled) return;
	w->button_mode = enabled;
	struct parts *parts = parts_get(no);
	if (!w->cg_name->size && !w->flat_name->size && !w->override_cg[1]->size && !w->override_flat[1]->size)
		rebuild_background(parts);
	else {
		parts_recalculate_hitbox(parts);
		parts_dirty(parts);
	}
}
bool PE_IsCheckBoxButtonMode(int no) { return checkbox(no)->button_mode; }
void PE_SetCheckBoxCharSpace(int no, int space)
{
	struct parts_widget *w = checkbox(no);
	if (w->label.text.ts.font_spacing == space) return;
	w->label.text.ts.font_spacing = space;
	rebuild_label(parts_get(no));
}
int PE_GetCheckBoxCharSpace(int no) { return checkbox(no)->label.text.ts.font_spacing; }
void PE_SetCheckBoxLineSpace(int no, int space)
{
	struct parts_widget *w = checkbox(no);
	if (w->label.text.line_space == space) return;
	w->label.text.line_space = space;
	rebuild_label(parts_get(no));
}
int PE_GetCheckBoxLineSpace(int no) { return checkbox(no)->label.text.line_space; }

void parts_panel_free(struct parts *parts)
{
	free(parts->panel);
	parts->panel = NULL;
}

static int panel_gradient(int coordinate, int dimension, int before, int after)
{
	int alpha = 255;
	if (before > 0 && coordinate < before)
		alpha = (int64_t)255 * coordinate / before;
	if (after > 0 && coordinate > dimension - (int64_t)after)
		alpha = alpha * (255 - (int64_t)255 * (coordinate - (dimension - (int64_t)after)) / after) / 255;
	return alpha;
}

void parts_panel_rebuild(struct parts *parts)
{
	struct parts_panel *p = parts->panel;
	struct parts_state *state = &parts->states[0];
	parts_state_reset(state, PARTS_CG);
	parts_set_dims(parts, &state->common, p->width, p->height);
	if (p->width > 0 && p->height > 0) {
		if ((size_t)p->width > INT_MAX / 4 / (size_t)p->height)
			VM_ERROR("Parts panel texture is too large");
		SDL_Color color = {min(255, max(0, p->color[0])), min(255, max(0, p->color[1])),
			min(255, max(0, p->color[2])), min(255, max(0, p->color[3]))};
		if (!(p->gradient[0] || p->gradient[1] || p->gradient[2] || p->gradient[3])) {
			gfx_init_texture_rgba(&state->common.texture, p->width, p->height, color);
		} else {
			SDL_Color *pixels = xmalloc((size_t)p->width * p->height * sizeof(*pixels));
			for (int y = 0; y < p->height; y++) {
				int ay = panel_gradient(y, p->height, p->gradient[0], p->gradient[1]);
				for (int x = 0; x < p->width; x++) {
					int ax = panel_gradient(x, p->width, p->gradient[2], p->gradient[3]);
					SDL_Color c = color;
					c.a = color.a * (ay * ax / 255) / 255;
					pixels[(size_t)y * p->width + x] = c;
				}
			}
			gfx_init_texture_with_pixels(&state->common.texture, p->width, p->height, pixels);
			free(pixels);
		}
	}
	parts_component_dirty(parts);
	parts_dirty(parts);
}

void parts_panel_create(struct parts *parts)
{
	if (parts->panel) return;
	parts_widget_free(parts);
	parts_scrollbar_free(parts);
	parts_message_window_free(parts);
	for (int i = 0; i < PARTS_NR_STATES; i++) parts_state_reset(&parts->states[i], PARTS_UNINITIALIZED);
	parts->panel = xcalloc(1, sizeof(*parts->panel));
	parts->panel->width = parts->panel->height = 200;
	parts->panel->color[0] = parts->panel->color[1] = parts->panel->color[2] = 220;
	parts->panel->color[3] = 255;
	parts->component_type = 12;
	parts->state = PARTS_STATE_DEFAULT;
	parts_panel_rebuild(parts);
}

static struct parts_panel *panel(int no)
{
	struct parts *parts = parts_get(no);
	parts_panel_create(parts);
	return parts->panel;
}

void PE_SetPanelSize(int no, int width, int height)
{
	struct parts_panel *p = panel(no);
	if (p->width == width && p->height == height) return;
	p->width = width;
	p->height = height;
	parts_panel_rebuild(parts_get(no));
}

void PE_SetPanelColor(int no, int r, int g, int b, int a)
{
	struct parts_panel *p = panel(no);
	int color[4] = {r, g, b, a};
	if (!memcmp(p->color, color, sizeof(color))) return;
	memcpy(p->color, color, sizeof(color));
	parts_panel_rebuild(parts_get(no));
}
int PE_GetPanelR(int no) { return panel(no)->color[0]; }
int PE_GetPanelG(int no) { return panel(no)->color[1]; }
int PE_GetPanelB(int no) { return panel(no)->color[2]; }
int PE_GetPanelA(int no) { return panel(no)->color[3]; }

#define PANEL_GRADIENT(direction, index) \
void PE_SetPanelAlphaGradation##direction(int no, int size) \
{ \
	struct parts_panel *p = panel(no); \
	if (p->gradient[index] == max(0, size)) return; \
	p->gradient[index] = max(0, size); \
	parts_panel_rebuild(parts_get(no)); \
} \
int PE_GetPanelAlphaGradation##direction(int no) { return panel(no)->gradient[index]; }
PANEL_GRADIENT(Top, 0)
PANEL_GRADIENT(Bottom, 1)
PANEL_GRADIENT(Left, 2)
PANEL_GRADIENT(Right, 3)
#undef PANEL_GRADIENT

void PE_SetComponentCheckBoxShowLinkNumber(int no, int cb_no)
{
	struct parts *parts = parts_get(no);
	int link = max(0, cb_no);
	if (parts->checkbox_show_link == link) return;
	parts->checkbox_show_link = link;
	parts_dirty(parts);
}

int PE_GetComponentCheckBoxShowLinkNumber(int no)
{
	return parts_get(no)->checkbox_show_link;
}

void PE_AddRadioButtonBoxChild(int no, int child_no)
{
	struct parts *p = parts_get(no);
	if (p->component_type != 9)
		PE_SetSpecialComponentType(no, 9);
	bool has_checked = false;
	for (int i = 0; i < p->nr_radio_children; i++) {
		if (p->radio_children[i] == child_no)
			return;
		struct parts *sib = parts_try_get(p->radio_children[i]);
		if (sib && sib->widget && sib->widget->checked)
			has_checked = true;
	}
	struct parts *child = parts_try_get(child_no);
	if (has_checked && child && child->widget && child->widget->checked) {
		child->widget->checked = false;
		parts_dirty(child);
	}
	p->radio_children = xrealloc(p->radio_children, (p->nr_radio_children + 1) * sizeof(int));
	p->radio_children[p->nr_radio_children++] = child_no;
}

int PE_NumofRadioButtonBoxChild(int no)
{
	struct parts *p = parts_try_get(no);
	return p ? p->nr_radio_children : 0;
}

int PE_GetRadioButtonBoxChild(int no, int index)
{
	struct parts *p = parts_try_get(no);
	if (!p || index < 0 || index >= p->nr_radio_children)
		return 0;
	return p->radio_children[index];
}

bool PE_IsRadioButtonBoxExistGUI(int no, int child_no)
{
	struct parts *p = parts_try_get(no);
	if (!p)
		return false;
	for (int i = 0; i < p->nr_radio_children; i++) {
		if (p->radio_children[i] == child_no)
			return true;
	}
	return false;
}

void PE_RemoveRadioButtonBoxChild(int no, int child_no)
{
	struct parts *p = parts_try_get(no);
	if (!p)
		return;
	for (int i = 0; i < p->nr_radio_children; i++) {
		if (p->radio_children[i] == child_no) {
			memmove(&p->radio_children[i], &p->radio_children[i + 1],
					(p->nr_radio_children - i - 1) * sizeof(int));
			p->nr_radio_children--;
			return;
		}
	}
}

void PE_ClearRadioButtonBoxChild(int no)
{
	struct parts *p = parts_try_get(no);
	if (!p)
		return;
	free(p->radio_children);
	p->radio_children = NULL;
	p->nr_radio_children = 0;
}

void PE_SetTextBoxText(int no, struct string *text)
{
	struct parts *p = parts_get(no);
	set_string(&p->text_box_text, text ? text : &EMPTY_STRING);
}

struct string *PE_GetTextBoxText(int no)
{
	struct parts *p = parts_get(no);
	return string_ref(p->text_box_text ? p->text_box_text : &EMPTY_STRING);
}
