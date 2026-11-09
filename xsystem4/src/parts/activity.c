/* PACT v6 / parts v34 layouts and activity ownership, GPL-2.0-or-later. */
#include <stdlib.h>
#include <string.h>

#include "system4.h"
#include "system4/ain.h"
#include "system4/archive.h"
#include "system4/ex.h"
#include "system4/file.h"
#include "system4/string.h"
#include "system4/utfsjis.h"
#include "asset_manager.h"
#include "mainex_v12.h"
#include "parts.h"
#include "parts_activity.h"
#include "parts_widgets.h"
#include "parts_scrollbar.h"
#include "parts_message_window.h"
#include "vm/page.h"
#include "xsystem4.h"
#include "parts_internal.h"

struct activity_part {
	struct string *name;
	int no;
	bool closed, locked;
	struct activity_part *next;
};

struct activity {
	struct string *name, *background, *ex_text;
	struct activity_part *parts;
	int ex_id, nr_keys, *keys;
	struct activity *next;
};

static struct activity *activities;

static struct activity *find_activity(struct string *name)
{
	for (struct activity *a = activities; a; a = a->next)
		if (!strcmp(a->name->text, name->text)) return a;
	return NULL;
}

static struct activity_part *find_part(struct activity *a, struct string *name)
{
	if (a) for (struct activity_part *p = a->parts; p; p = p->next)
		if (!strcmp(p->name->text, name->text)) return p;
	return NULL;
}

bool PE_IsExistActivity(struct string *name) { return find_activity(name); }

bool PE_CreateActivity(struct string *name)
{
	if (find_activity(name)) return false;
	struct activity *a = xcalloc(1, sizeof(*a));
	a->name = string_dup(name);
	a->background = string_ref(&EMPTY_STRING);
	a->ex_text = string_ref(&EMPTY_STRING);
	a->ex_id = -1;
	a->next = activities;
	activities = a;
	return true;
}

bool PE_AddActivityParts(struct string *name, struct string *part_name, int no)
{
	struct activity *a = find_activity(name);
	if (!a || find_part(a, part_name)) return false;
	struct activity_part **at = &a->parts;
	while (*at && strcmp((*at)->name->text, part_name->text) < 0) at = &(*at)->next;
	struct activity_part *p = xcalloc(1, sizeof(*p));
	p->name = string_dup(part_name);
	p->no = no;
	p->next = *at;
	*at = p;
	parts_get(no);
	return true;
}

bool PE_RemoveActivityParts(struct string *name, struct string *part_name)
{
	struct activity *a = find_activity(name);
	if (!a) return false;
	for (struct activity_part **at = &a->parts; *at; at = &(*at)->next) {
		struct activity_part *p = *at;
		if (strcmp(p->name->text, part_name->text)) continue;
		*at = p->next;
		free_string(p->name);
		free(p);
		return true;
	}
	return false;
}

bool PE_RemoveAllActivityParts(struct string *name)
{
	struct activity *a = find_activity(name);
	if (!a) return false;
	while (a->parts) PE_RemoveActivityParts(name, a->parts->name);
	return true;
}

bool PE_ReleaseActivity(struct string *name, struct page **erased)
{
	for (struct activity **at = &activities; *at; at = &(*at)->next) {
		struct activity *a = *at;
		if (strcmp(a->name->text, name->text)) continue;
		*at = a->next;
		while (a->parts) {
			struct activity_part *p = a->parts;
			a->parts = p->next;
			struct parts *part = parts_try_get(p->no);
			if (part && erased)
				*erased = array_pushback(*erased, vm_int(part->delegate_index), AIN_ARRAY_INT, -1);
			parts_release(p->no);
			free_string(p->name);
			free(p);
		}
		mainex_v12_unregister(a->ex_id);
		free_string(a->name);
		free_string(a->background);
		free_string(a->ex_text);
		free(a->keys);
		free(a);
		return true;
	}
	return false;
}

void PE_ReleaseActivities(void)
{
	while (activities) PE_ReleaseActivity(activities->name, NULL);
}

int PE_NumofActivityParts(struct string *name)
{
	struct activity *a = find_activity(name);
	int count = 0;
	if (a) for (struct activity_part *p = a->parts; p; p = p->next) count++;
	return count;
}

bool PE_GetActivityParts(int index, struct string *name, struct string **part_name, int *no)
{
	struct activity *a = find_activity(name);
	if (a && index >= 0) for (struct activity_part *p = a->parts; p; p = p->next) {
		if (index--) continue;
		free_string(*part_name);
		*part_name = string_dup(p->name);
		*no = p->no;
		return true;
	}
	return false;
}

bool PE_IsExistActivityPartsByName(struct string *name, struct string *part_name)
{
	return find_part(find_activity(name), part_name);
}

bool PE_IsExistActivityPartsByNumber(struct string *name, int no)
{
	struct activity *a = find_activity(name);
	if (a) for (struct activity_part *p = a->parts; p; p = p->next)
		if (p->no == no) return true;
	return false;
}

int PE_GetActivityPartsNumber(struct string *name, struct string *part_name)
{
	struct activity_part *p = find_part(find_activity(name), part_name);
	return p ? p->no : 0;
}

struct string *PE_GetActivityPartsName(struct string *name, int no)
{
	struct activity *a = find_activity(name);
	if (a) for (struct activity_part *p = a->parts; p; p = p->next)
		if (p->no == no) return string_dup(p->name);
	return string_ref(&EMPTY_STRING);
}

int PE_GetActivityEXID(struct string *name)
{
	struct activity *a = find_activity(name);
	return a ? a->ex_id : -1;
}

void PE_SetActivityEXText(struct string *name, struct string *text)
{
	struct activity *a = find_activity(name);
	if (!a) return;
	if (text->size) VM_ERROR("Nonempty PACT EX text is not yet supported");
	free_string(a->ex_text);
	a->ex_text = string_dup(text);
	mainex_v12_unregister(a->ex_id);
	a->ex_id = mainex_v12_register(xcalloc(1, sizeof(struct ex)));
}

struct string *PE_GetActivityEXText(struct string *name)
{
	struct activity *a = find_activity(name);
	return string_ref(a ? a->ex_text : &EMPTY_STRING);
}

void PE_SetActivityBG(struct string *name, struct string *cg)
{
	struct activity *a = find_activity(name);
	if (!a) return;
	free_string(a->background);
	a->background = string_dup(cg);
}

struct string *PE_GetActivityBG(struct string *name)
{
	struct activity *a = find_activity(name);
	return string_ref(a ? a->background : &EMPTY_STRING);
}

#define PART_FLAG_API(label, member) \
	void PE_AddActivity##label##Parts(struct string *name, struct string *part_name) { \
		struct activity_part *p = find_part(find_activity(name), part_name); \
		if (p) p->member = true; \
	} \
	bool PE_IsExistActivity##label##Parts(struct string *name, struct string *part_name) { \
		struct activity_part *p = find_part(find_activity(name), part_name); \
		return p && p->member; \
	}
PART_FLAG_API(Close, closed)
PART_FLAG_API(Locked, locked)

bool PE_IsExistActivityEndKey(struct string *name, int key)
{
	struct activity *a = find_activity(name);
	if (a) for (int i = 0; i < a->nr_keys; i++) if (a->keys[i] == key) return true;
	return false;
}

void PE_SetActivityEndKey(struct string *name, int key)
{
	struct activity *a = find_activity(name);
	if (!a || PE_IsExistActivityEndKey(name, key)) return;
	a->keys = xrealloc(a->keys, (a->nr_keys + 1) * sizeof(*a->keys));
	int i = a->nr_keys;
	while (i && a->keys[i - 1] > key) { a->keys[i] = a->keys[i - 1]; i--; }
	a->keys[i] = key;
	a->nr_keys++;
}

void PE_EraseActivityEndKey(struct string *name, int key)
{
	struct activity *a = find_activity(name);
	if (a) for (int i = 0; i < a->nr_keys; i++) if (a->keys[i] == key) {
		memmove(a->keys + i, a->keys + i + 1, (--a->nr_keys - i) * sizeof(*a->keys));
		return;
	}
}

int PE_NumofActivityEndKey(struct string *name)
{
	struct activity *a = find_activity(name);
	return a ? a->nr_keys : 0;
}

int PE_GetActivityEndKey(struct string *name, int index)
{
	struct activity *a = find_activity(name);
	return a && index >= 0 && index < a->nr_keys ? a->keys[index] : 0;
}

void PE_SetSpecialComponentType(int no, int type)
{
	if (type == 2 || type == 3) {
		parts_scrollbar_create(parts_get(no), type == 3);
		return;
	}
	if (type == 1) {
		parts_checkbox_create(parts_get(no));
		return;
	}
	if (type == 12) {
		parts_panel_create(parts_get(no));
		return;
	}
	if (type == 10) {
		parts_message_window_create(parts_get(no));
		return;
	}
	if (type == 0) {
		parts_widget_create(parts_get(no));
		return;
	}
	if (type < 0 || type > 16 || type == 8) VM_ERROR("Unsupported special component %d", type);
	struct parts *p = parts_get(no);
	parts_widget_free(p);
	parts_panel_free(p);
	parts_scrollbar_free(p);
	parts_message_window_free(p);
	if (p->component_type != type) {
		PE_ClearRadioButtonBoxChild(no);
		for (int i = 0; i < PARTS_NR_STATES; i++) parts_state_reset(&p->states[i], PARTS_UNINITIALIZED);
	}
	p->component_type = type;
}

int PE_GetSpecialComponentType(int no)
{
	struct parts *p = parts_try_get(no);
	return p && (p->widget || p->component_type) ? p->component_type : -1;
}

void PE_SetUserComponentName(int no, struct string *name)
{
	struct parts *p = parts_get(no);
	free_string(p->user_component_name);
	p->user_component_name = string_dup(name);
}

struct string *PE_GetUserComponentName(int no)
{
	return string_ref(parts_get(no)->user_component_name);
}

void PE_SetUserComponentData(int no, struct string *key, struct string *value)
{
	struct parts *p = parts_get(no);
	for (struct parts_user_value *v = p->user_values; v; v = v->next) {
		if (strcmp(v->key->text, key->text)) continue;
		free_string(v->value);
		v->value = string_dup(value);
		return;
	}
	struct parts_user_value *v = xcalloc(1, sizeof(*v));
	v->key = string_dup(key);
	v->value = string_dup(value);
	v->next = p->user_values;
	p->user_values = v;
}

struct string *PE_GetUserComponentData(int no, struct string *key)
{
	for (struct parts_user_value *v = parts_get(no)->user_values; v; v = v->next)
		if (!strcmp(v->key->text, key->text)) return string_ref(v->value);
	return string_ref(&EMPTY_STRING);
}

// Layout keys in the Chinese edition are GBK, including the Japanese field names.
static struct ex_tree *node(struct ex_tree *tree, const char *key)
{
	if (!tree || tree->is_leaf) return NULL;
	char *encoded = utf2sjis(key, strlen(key));
	struct ex_tree *result = ex_tree_get_child(tree, encoded);
	free(encoded);
	return result;
}

static struct ex_value *field(struct ex_tree *tree, const char *key)
{
	struct ex_tree *n = node(tree, key);
	return n && n->is_leaf ? &n->leaf.value : NULL;
}

static struct string *str(struct ex_tree *tree, const char *key)
{
	struct ex_value *v = field(tree, key);
	return v && v->type == EX_STRING ? v->s : &EMPTY_STRING;
}

static float number(struct ex_tree *tree, const char *key, float fallback)
{
	struct ex_value *v = field(tree, key);
	return !v ? fallback : v->type == EX_INT ? v->i : v->type == EX_FLOAT ? v->f : fallback;
}

static float item(struct ex_tree *tree, const char *key, int index, float fallback)
{
	struct ex_value *v = field(tree, key);
	v = v && v->type == EX_LIST ? ex_list_get(v->list, index) : NULL;
	return !v ? fallback : v->type == EX_INT ? v->i : v->type == EX_FLOAT ? v->f : fallback;
}

static void load_state(struct parts *p, struct ex_tree *tree, int state)
{
	if (!tree) return;
	struct string *type = str(tree, "パーツ类型");
	char *kind = sjis2utf(type->text, type->size);
	int no = p->no;
	if (!strcmp(kind, "ＣＧパーツ")) {
		struct string *cg = str(tree, "ＣＧ名");
		parts_get_cg(p, state - 1);
		if (cg->size) PE_SetPartsCG(no, cg, number(tree, "变形", 0), state);
	} else if (!strcmp(kind, "フラットパーツ")) {
		struct string *flat = str(tree, "フラット名");
		if (flat->size) {
			PE_SetPartsFlat(no, flat, state);
			int frame = number(tree, "开始時シーク量", 0);
			p->states[state - 1].flat.begin_seek_frame = frame;
			if (frame) PE_GoFramePartsFlat(no, frame, state);
		}
	} else if (!strcmp(kind, "矩形パーツ")) {
		PE_SetRectangleDetectionPoint(no, item(tree, "左上", 0, 0), item(tree, "左上", 1, 0),
				item(tree, "右上", 0, 0), item(tree, "右上", 1, 0),
				item(tree, "左下", 0, 0), item(tree, "左下", 1, 0),
				item(tree, "右下", 0, 0), item(tree, "右下", 1, 0), state);
	} else if (!strcmp(kind, "横向计量パーツ")) {
		PE_SetHGaugeCG(no, str(tree, "ＣＧ名"), state);
		PE_SetHGaugeRate(no, number(tree, "分子", 0), number(tree, "分母", 100), state);
	} else if (!strcmp(kind, "数字パーツ")) {
		PE_SetNumeralFont(no, number(tree, "字体类型", 0), number(tree, "字体尺寸", 16),
				item(tree, "字体色", 0, 255), item(tree, "字体色", 1, 255), item(tree, "字体色", 2, 255),
				number(tree, "字体粗度", 0), item(tree, "字体镶边色", 0, 0),
				item(tree, "字体镶边色", 1, 0), item(tree, "字体镶边色", 2, 0),
				number(tree, "字体镶边", 0), state);
		int show_type = number(tree, "表示类型", 0);
		struct string *cg = str(tree, "ＣＧ名");
		if (cg->size && show_type == 0) PE_SetNumeralCG(no, cg, state);
		else if (cg->size && show_type == 1)
			PE_SetNumeralLinkedCGNumberWidthWidthList(no, cg,
				item(tree, "幅リスト", 0, 0), item(tree, "幅リスト", 1, 0), item(tree, "幅リスト", 2, 0),
				item(tree, "幅リスト", 3, 0), item(tree, "幅リスト", 4, 0), item(tree, "幅リスト", 5, 0),
				item(tree, "幅リスト", 6, 0), item(tree, "幅リスト", 7, 0), item(tree, "幅リスト", 8, 0),
				item(tree, "幅リスト", 9, 0), item(tree, "幅リスト", 10, 0), item(tree, "幅リスト", 11, 0), state);
		PE_SetNumeralShowType(no, show_type, state);
		PE_SetNumeralSpace(no, number(tree, "字间距", 0), state);
		PE_SetNumeralShowComma(no, number(tree, "コンマ表示", 0), state);
		PE_SetNumeralFullPitch(no, number(tree, "全角", 0), state);
		PE_SetNumeralShowPadding(no, number(tree, "ゼロパディング", 1), state);
		PE_SetNumeralLength(no, number(tree, "桁数", 0), state);
		PE_SetNumeralNumber(no, number(tree, "数值", 0), state);
	} else if (!strcmp(kind, "文本パーツ")) {
		struct ex_tree *style = node(tree, "文本装飾");
		PE_SetPartsFontType(no, number(style, "字体类型", 0), state);
		PE_SetPartsFontSize(no, number(style, "字体尺寸", 16), state);
		PE_SetPartsFontColor(no, item(style, "字体色", 0, 255), item(style, "字体色", 1, 255), item(style, "字体色", 2, 255), state);
		PE_SetPartsFontBoldWeight(no, number(style, "字体粗度", 0), state);
		PE_SetPartsFontEdgeWeight(no, number(style, "字体镶边", 0), state);
		PE_SetPartsFontEdgeColor(no, item(style, "字体镶边色", 0, 0), item(style, "字体镶边色", 1, 0), item(style, "字体镶边色", 2, 0), state);
		PE_SetTextCharSpace(no, number(style, "字间距", 0), state);
		PE_SetTextLineSpace(no, number(style, "行间距", 0), state);
		PE_SetText(no, str(tree, "文本"), state);
	} else if (!strcmp(kind, "構築パーツ")) {
		parts_get_construction_process(p, state - 1);
		struct ex_tree *steps = node(tree, "手順リスト");
		if (steps && !steps->is_leaf) {
			for (unsigned i = 0; i < steps->nr_children; i++) {
				struct ex_tree *step = &steps->children[i];
				int cmd = number(step, "コマンド", 0);
				int interp = number(step, "补间类型", 0);
				int sx = item(step, "原矩形", 0, 0), sy = item(step, "原矩形", 1, 0);
				int sw = item(step, "原矩形", 2, 0), sh = item(step, "原矩形", 3, 0);
				int dx = item(step, "先矩形", 0, 0), dy = item(step, "先矩形", 1, 0);
				int dx2 = item(step, "先矩形", 2, 0), dy2 = item(step, "先矩形", 3, 0);
				int dw = item(step, "先矩形", 4, 0), dh = item(step, "先矩形", 5, 0);
				int r = item(step, "色１", 0, 0), g = item(step, "色１", 1, 0);
				int b = item(step, "色１", 2, 0), a = item(step, "色１", 3, 0);
				int r2 = item(step, "色２", 0, 0), g2 = item(step, "色２", 1, 0);
				int b2 = item(step, "色２", 2, 0), a2 = item(step, "色２", 3, 0);
				int cspace = number(step, "文字间距", 0), lspace = number(step, "行间距", 0);
				int ftype = number(step, "字体类型", 0), fsize = number(step, "字体尺寸", 16);
				int fr = item(step, "字体色", 0, 255), fg = item(step, "字体色", 1, 255), fb = item(step, "字体色", 2, 255);
				int er = item(step, "字体镶边色", 0, 0), eg = item(step, "字体镶边色", 1, 0), eb = item(step, "字体镶边色", 2, 0);
				bool full = number(step, "全体", 0);
				int blur = number(step, "ぼかし長さ", 0);
				float bold = number(step, "字体粗度", 0), edge = number(step, "字体镶边", 0);
				struct string *txt = str(step, "文本"), *cg = str(step, "ＣＧ名");
				switch (cmd) {
				case 0: PE_ClearPartsConstructionProcess(no, state); break;
				case 1: PE_AddCreateToPartsConstructionProcess(no, dw, dh, state); break;
				case 2: PE_AddCreateCGToProcess(no, cg, state); break;
				case 3: PE_AddFillToPartsConstructionProcess(no, dx, dy, dw, dh, r, g, b, state); break;
				case 4: PE_AddFillAlphaColorToPartsConstructionProcess(no, dx, dy, dw, dh, r, g, b, a, state); break;
				case 5: PE_AddFillAMapToPartsConstructionProcess(no, dx, dy, dw, dh, a, state); break;
				case 6: PE_AddFillWithAlphaToPartsConstructionProcess(no, dx, dy, dw, dh, r, g, b, a, state); break;
				case 7: PE_AddDrawTextToPartsConstructionProcess(no, dx, dy, txt, ftype, fsize, fr, fg, fb, bold, er, eg, eb, edge, cspace, lspace, state); break;
				case 8: PE_AddCopyTextToPartsConstructionProcess(no, dx, dy, txt, ftype, fsize, fr, fg, fb, bold, er, eg, eb, edge, cspace, lspace, state); break;
				case 9: PE_AddFillGradationHorizonToPartsConstructionProcess(no, dx, dy, dw, dh, r, g, b, r2, g2, b2, state); break;
				case 10: PE_AddDrawRectToPartsConstructionProcess(no, dx, dy, dw, dh, r, g, b, state); break;
				case 11: PE_AddDrawCutCGToPartsConstructionProcess(no, cg, dx, dy, dw, dh, sx, sy, dw, dh, interp, state); break;
				case 12: PE_AddCopyCutCGToPartsConstructionProcess(no, cg, dx, dy, dw, dh, sx, sy, dw, dh, interp, state); break;
				case 13: PE_AddDrawCutCGToPartsConstructionProcess(no, cg, dx, dy, dw, dh, sx, sy, sw, sh, interp, state); break;
				case 14: PE_AddCopyCutCGToPartsConstructionProcess(no, cg, dx, dy, dw, dh, sx, sy, sw, sh, interp, state); break;
				case 15: PE_AddGrayFilterToPartsConstructionProcess(no, dx, dy, dw, dh, full, state); break;
				case 16: PE_AddAddFilterToPartsConstructionProcess(no, dx, dy, dw, dh, r, g, b, full, state); break;
				case 17: PE_AddMulFilterToPartsConstructionProcess(no, dx, dy, dw, dh, r, g, b, full, state); break;
				case 18: PE_AddDrawLineToPartsConstructionProcess(no, dx, dy, dx2, dy2, r, g, b, a, state); break;
				case 19: PE_AddCutCGAlphaBlendToPartsConstructionProcess(no, cg, dx, dy, dw, dh, sx, sy, dw, dh, interp, false, state); break;
				case 20: PE_AddCutCGAlphaBlendToPartsConstructionProcess(no, cg, dx, dy, dw, dh, sx, sy, sw, sh, interp, true, state); break;
				case 21: PE_AddCutCGOnlyAlphaToPartsConstructionProcess(no, cg, dx, dy, dw, dh, sx, sy, dw, dh, interp, false, state); break;
				case 22: PE_AddCutCGOnlyAlphaToPartsConstructionProcess(no, cg, dx, dy, dw, dh, sx, sy, sw, sh, interp, true, state); break;
				case 23: PE_AddAlphaBlendTextToPartsConstructionProcess(no, dx, dy, txt, ftype, fsize, fr, fg, fb, bold, er, eg, eb, edge, cspace, lspace, state); break;
				case 24: PE_AddOnlyAlphaTextToPartsConstructionProcess(no, dx, dy, txt, ftype, fsize, fr, fg, fb, bold, er, eg, eb, edge, cspace, lspace, state); break;
				case 25: PE_AddMulAMapGradationHorizonToPartsConstructionProcess(no, dx, dy, dw, dh, a, a2, state); break;
				case 26: PE_AddMulAMapGradationVerticalToPartsConstructionProcess(no, dx, dy, dw, dh, a, a2, state); break;
				case 27: PE_AddHBlurFilterToPartsConstructionProcess(no, dx, dy, dw, dh, blur, full, state); break;
				case 28: PE_AddCreatePixelOnlyToPartsConstructionProcess(no, dw, dh, state); break;
				case 29: PE_AddCGBlendToPartsConstructionProcess(no, cg, dx, dy, state); break;
				default: WARNING("Unsupported PACT construction step command: %d", cmd); break;
				}
			}
			PE_BuildPartsConstructionProcess(no, state);
		}
	} else if (*kind) {
		VM_ERROR("Unsupported PACT state component: %s", kind);
	}
	int x = item(tree, "サーフェイスエリア", 0, 0), y = item(tree, "サーフェイスエリア", 1, 0);
	int w = item(tree, "サーフェイスエリア", 2, 0), h = item(tree, "サーフェイスエリア", 3, 0);
	if (w > 0 && h > 0) p->states[state - 1].common.surface_area = (Rectangle){ x, y, w, h };
	free(kind);
}

static void load_part(struct activity *a, struct ex_tree *tree, int parent, bool edit)
{
	if (!edit && !number(tree, "エディタ上表示", 1))
		return;
	int no = PE_GetFreeNumber();
	struct parts *p = parts_get(no);
	if (!PE_AddActivityParts(a->name, tree->name, no))
		VM_ERROR("Duplicate PACT parts name: %s", display_sjis0(tree->name->text));
	struct activity_part *binding = find_part(a, tree->name);
	binding->closed = number(tree, "閉じている", 0);
	binding->locked = number(tree, "ロックしている", 0);
	p->position_is_ratio = number(tree, "座標を率として認識する", 0);
	PE_SetComponentPos(no, item(tree, "座標", 0, 0), item(tree, "座標", 1, 0));
	PE_SetZ(no, item(tree, "座標", 2, 1));
	PE_SetShow(no, number(tree, "表示", 1));
	PE_SetAlpha(no, number(tree, "アルファ", 255));
	PE_SetPartsOriginPosMode(no, number(tree, "原点座標模式", 1));
	PE_SetPartsMagX(no, item(tree, "拡大縮小", 0, 1) * (number(tree, "左右反转", 0) ? -1 : 1));
	PE_SetPartsMagY(no, item(tree, "拡大縮小", 1, 1) * (number(tree, "上下反转", 0) ? -1 : 1));
	PE_SetPartsRotateX(no, item(tree, "旋转", 0, 0));
	PE_SetPartsRotateY(no, item(tree, "旋转", 1, 0));
	PE_SetPartsRotateZ(no, item(tree, "旋转", 2, 0));
	PE_SetAddColor(no, item(tree, "加法色", 0, 0), item(tree, "加法色", 1, 0), item(tree, "加法色", 2, 0));
	PE_SetMultiplyColor(no, item(tree, "乘法色", 0, 255), item(tree, "乘法色", 1, 255), item(tree, "乘法色", 2, 255));
	PE_SetPartsDrawFilter(no, number(tree, "描画フィルタ", 0));
	PE_SetClickable(no, number(tree, "点击許可", 0));
	PE_SetPassCursor(no, number(tree, "光标选中透過", 0));
	PE_SetDrag(no, number(tree, "ドラッグ", 0));
	PE_SetComment(no, str(tree, "批注"));
	PE_SetPartsMessageWindowShowLink(no, number(tree, "消息窗口表示連動", 0));
	PE_SetComponentMargin(no, item(tree, "边距", 0, 0), item(tree, "边距", 1, 0), item(tree, "边距", 2, 0), item(tree, "边距", 3, 0));
	PE_Parts_SetSoundName(no, str(tree, "光标选中效果音"), 2);
	PE_Parts_SetSoundName(no, str(tree, "点击效果音"), 3);
	if (parent) PE_SetParentPartsNumber(no, parent);
	struct ex_tree *info = node(tree, "种类別情报");
	struct string *type = str(info, "パーツ类型");
	char *kind = sjis2utf(type->text, type->size);
	if (!strcmp(kind, "レイアウトボックス")) {
		PE_SetLayoutBoxLayoutType(no, number(info, "レイアウト类型", 0));
		PE_SetLayoutBoxAlign(no, number(info, "配置", 1));
		PE_SetLayoutBoxReturnF(no, number(info, "翻折許可", 0), number(info, "翻折尺寸", 200));
		PE_set_layoutbox_padding(no, item(info, "パディング", 0, 0), item(info, "パディング", 1, 0), item(info, "パディング", 2, 0), item(info, "パディング", 3, 0));
	} else if (!strcmp(kind, "低レベルパーツ")) {
		load_state(p, node(info, "一般状态"), 1);
		load_state(p, node(info, "光标选中状态"), 2);
		load_state(p, node(info, "键DOWN状态"), 3);
	} else if (!strcmp(kind, "按钮")) {
		PE_SetSpecialComponentType(no, 0);
		PE_SetButtonSize(no, item(info, "尺寸", 0, 200), item(info, "尺寸", 1, 30));
		PE_SetButtonEnable(no, number(info, "有效", 1));
		PE_SetButtonColor(no, item(info, "按钮の色", 0, 200), item(info, "按钮の色", 1, 200), item(info, "按钮の色", 2, 200));
		PE_SetButtonFontProperty(no, number(info, "字体类型", 0), number(info, "字体尺寸", 16),
				item(info, "字体色", 0, 0), item(info, "字体色", 1, 0), item(info, "字体色", 2, 0),
				number(info, "字体粗度", 0), item(info, "字体镶边色", 0, 0), item(info, "字体镶边色", 1, 0),
				item(info, "字体镶边色", 2, 0), number(info, "字体镶边", 0));
		PE_SetButtonTextOriginPosMode(no, number(info, "文本位置", 5));
		PE_SetButtonCharSpace(no, number(info, "文字间距", 0));
		PE_SetButtonLineSpace(no, number(info, "行间距", 0));
		PE_SetButtonText(no, str(info, "文本"));
		struct string *flat = str(info, "フラット名"), *cg = str(info, "ＣＧ名");
		if (cg->size) PE_SetButtonCGName(no, cg);
		if (flat->size) PE_SetButtonFlatName(no, flat);
	} else if (!strcmp(kind, "复选框")) {
		parts_checkbox_create(p);
		p->widget->checked = number(info, "选择状态", 0);
		p->widget->button_mode = number(info, "按钮模式", 0);
		PE_SetCheckBoxSize(no, item(info, "尺寸", 0, 16), item(info, "尺寸", 1, 16));
		PE_SetCheckBoxEnable(no, number(info, "有效状态", 1));
		PE_SetCheckBoxColor(no, item(info, "按钮の色", 0, 200), item(info, "按钮の色", 1, 200), item(info, "按钮の色", 2, 200));
		PE_SetCheckBoxFontProperty(no, number(info, "字体类型", 0), number(info, "字体尺寸", 16),
				item(info, "字体色", 0, 255), item(info, "字体色", 1, 255), item(info, "字体色", 2, 255),
				number(info, "字体粗度", 0), item(info, "字体镶边色", 0, 0), item(info, "字体镶边色", 1, 0),
				item(info, "字体镶边色", 2, 0), number(info, "字体镶边", 0));
		PE_SetCheckBoxTextOriginPosMode(no, number(info, "文本位置", 4));
		PE_SetCheckBoxCharSpace(no, number(info, "文字间距", 0));
		PE_SetCheckBoxLineSpace(no, number(info, "行间距", 0));
		PE_SetCheckBoxButtonMode(no, number(info, "按钮模式", 0));
		PE_SetCheckBoxText(no, str(info, "文本"));
		struct string *cg = str(info, "ＣＧ名"), *flat = str(info, "フラット名");
		if (cg->size) PE_SetCheckBoxCGName(no, cg);
		if (flat->size) PE_SetCheckBoxFlatName(no, flat);
		cg = str(info, "上書きＣＧ名");
		if (cg->size) PE_SetCheckBoxOverrideCGName(no, cg);
		cg = str(info, "上書き检查ＣＧ名");
		if (cg->size) PE_SetCheckBoxOverrideCheckedCGName(no, cg);
		flat = str(info, "上書きフラット名");
		if (flat->size) PE_SetCheckBoxOverrideFlatName(no, flat);
		flat = str(info, "上書き检查フラット名");
		if (flat->size) PE_SetCheckBoxOverrideCheckedFlatName(no, flat);
	} else if (!strcmp(kind, "纵向滚动条栏") || !strcmp(kind, "横向滚动条栏")) {
		bool horizontal = !strcmp(kind, "横向滚动条栏");
		parts_scrollbar_create(p, horizontal);
		struct parts_scrollbar *s = p->scrollbar;
		int length = number(info, "長さ", 256), width = number(info, "幅", 24);
		s->width = horizontal ? length : width;
		s->height = horizontal ? width : length;
		s->before = number(info, "前尺寸", 24);
		s->after = number(info, "次尺寸", 24);
		s->total = number(info, "全体滚动条量", 100);
		s->view = number(info, "表示量", 1);
		s->pos = min(parts_scrollbar_range(s), max(0, number(info, "滚动条位置", 0)));
		s->rate = number(info, "滚动率", parts_scrollbar_range(s) ? (float)s->pos / parts_scrollbar_range(s) : 1.f);
		s->rate = min(1.f, max(0.f, s->rate));
		s->step = number(info, "按钮点击移动量", 1);
		const char *fields[2][5] = {
			{"ＣＧ名", "上書き前ＣＧ名", "上書き次ＣＧ名", "上書きバーＣＧ名", "上書き背景ＣＧ名"},
			{"フラット名", "上書き前フラット名", "上書き次フラット名", "上書きバーフラット名", "上書き背景フラット名"}
		};
		for (int resource = 0; resource < 2; resource++) for (int piece = 0; piece < 5; piece++) {
			free_string(s->names[resource][piece]);
			s->names[resource][piece] = string_dup(str(info, fields[resource][piece]));
		}
		parts_scrollbar_rebuild(p);
	} else if (!strcmp(kind, "面板")) {
		PE_SetPanelSize(no, item(info, "尺寸", 0, 200), item(info, "尺寸", 1, 200));
		PE_SetPanelColor(no, item(info, "色", 0, 220), item(info, "色", 1, 220),
				item(info, "色", 2, 220), item(info, "色", 3, 255));
		PE_SetPanelAlphaGradationTop(no, item(info, "アルファ色彩渐变", 0, 0));
		PE_SetPanelAlphaGradationBottom(no, item(info, "アルファ色彩渐变", 1, 0));
		PE_SetPanelAlphaGradationLeft(no, item(info, "アルファ色彩渐变", 2, 0));
		PE_SetPanelAlphaGradationRight(no, item(info, "アルファ色彩渐变", 3, 0));
	} else if (!strcmp(kind, "消息窗口")) {
		PE_SetSpecialComponentType(no, 10);
		PE_SetMessageWindowActive(no, number(info, "アクティブ", 0));
		PE_SetMessageWindowInactiveMultipleColor(no, item(info, "非アクティブ時の乘法颜色", 0, 255),
				item(info, "非アクティブ時の乘法颜色", 1, 255), item(info, "非アクティブ時の乘法颜色", 2, 255));
		PE_SetMessageWindowTextArea(no, item(info, "文本エリア", 0, 10), item(info, "文本エリア", 1, 15),
				item(info, "文本エリア", 2, 0), item(info, "文本エリア", 3, 0));
		PE_SetMessageWindowTextOriginPosMode(no, number(info, "文本位置", 1));
		PE_SetMessageWindowTextFont(no, number(info, "字体类型", 0), number(info, "字体尺寸", 26),
				item(info, "字体色", 0, 255), item(info, "字体色", 1, 255), item(info, "字体色", 2, 255),
				number(info, "字体粗度", 0), item(info, "字体镶边色", 0, 0), item(info, "字体镶边色", 1, 0),
				item(info, "字体镶边色", 2, 0), number(info, "字体镶边", 0));
		PE_SetMessageWindowTextSpeed(no, number(info, "字速度", 23));
		PE_SetMessageWindowTextSpace(no, number(info, "文字间距", 0), number(info, "行间距", 3));
		struct string *flat = str(info, "フラット名"), *cg = str(info, "ＣＧ名");
		if (cg->size) PE_SetMessageWindowCGName(no, cg);
		if (flat->size) PE_SetMessageWindowFlatName(no, flat);
		PE_SetMessageWindowText(no, str(info, "文本"), number(info, "消息编号", -1), &EMPTY_STRING, 0, 0);
		struct ex_tree *key = node(info, "キー待ちマーク");
		if (key) {
			PE_SetKeyWaitPos(no, item(key, "座標", 0, 0), item(key, "座標", 1, 0), item(key, "座標", 2, 0));
			PE_SetKeyWaitCGName(no, str(key, "ＣＧ名"), number(key, "循环ＣＧ开始编号", 0),
					number(key, "循环ＣＧ张数", 0), number(key, "循环ＣＧ切り替え时间", 0));
			flat = str(key, "フラット名");
			if (flat->size) PE_SetKeyWaitFlatName(no, flat);
		}
	} else if (!strcmp(kind, "ラジオ按钮ボックス")) {
		PE_SetSpecialComponentType(no, 9);
	} else if (!strcmp(kind, "文本框")) {
		PE_SetSpecialComponentType(no, 4);
		parts_set_dims(p, &p->states[0].common, item(info, "尺寸", 0, 200), item(info, "尺寸", 1, 30));
		PE_SetTextBoxText(no, str(info, "文本"));
	} else if (!strcmp(kind, "リストボックス")) {
		PE_SetSpecialComponentType(no, 5);
		parts_set_dims(p, &p->states[0].common, item(info, "尺寸", 0, 200), item(info, "尺寸", 1, 200));
	} else if (!strcmp(kind, "コンボボックス")) {
		PE_SetSpecialComponentType(no, 6);
		parts_set_dims(p, &p->states[0].common, item(info, "尺寸", 0, 200), item(info, "尺寸", 1, 30));
	} else if (!strcmp(kind, "多行文本框")) {
		PE_SetSpecialComponentType(no, 7);
		parts_set_dims(p, &p->states[0].common, item(info, "尺寸", 0, 200), item(info, "尺寸", 1, 200));
		PE_SetTextBoxText(no, str(info, "文本"));
	} else if (!strcmp(kind, "スピンボックス")) {
		PE_SetSpecialComponentType(no, 11);
		parts_set_dims(p, &p->states[0].common, item(info, "尺寸", 0, 100), item(info, "尺寸", 1, 24));
	} else if (!strcmp(kind, "フォーム")) {
		PE_SetSpecialComponentType(no, 13);
		parts_set_dims(p, &p->states[0].common, item(info, "尺寸", 0, 200), item(info, "尺寸", 1, 200));
	} else if (!strcmp(kind, "用户组件")) {
		PE_SetSpecialComponentType(no, 15);
		PE_SetUserComponentName(no, str(info, "用户组件名"));
		struct ex_value *data = field(info, "数据");
		if (data && data->type == EX_LIST) for (unsigned i = 0; i + 1 < data->list->nr_items; i += 2) {
			struct ex_value *key = ex_list_get(data->list, i), *value = ex_list_get(data->list, i + 1);
			if (key->type == EX_STRING && value->type == EX_STRING)
				PE_SetUserComponentData(no, key->s, value->s);
		}
	} else {
		VM_ERROR("Unsupported PACT component: %s", kind);
	}
	free(kind);
	struct ex_tree *children = node(tree, "子パーツ");
	if (children) for (unsigned i = 0; i < children->nr_children; i++)
		load_part(a, &children->children[i], no, edit);
}

static void load_part_links(struct activity *a, struct ex_tree *tree)
{
	struct activity_part *binding = find_part(a, tree->name);
	struct parts *p = binding ? parts_try_get(binding->no) : NULL;
	if (!p) return;
	struct activity_part *clipper = find_part(a, str(tree, "α剪切器ー"));
	if (clipper) PE_SetPartsAlphaClipperPartsNumber(p->no, clipper->no);
	struct activity_part *cursor_link = find_part(a, str(tree, "光标选中表示連動"));
	if (cursor_link) PE_SetOnCursorShowLinkPartsNumber(p->no, cursor_link->no);
	struct activity_part *cb_link = find_part(a, str(tree, "复选框表示連動"));
	if (cb_link) PE_SetComponentCheckBoxShowLinkNumber(p->no, cb_link->no);
	if (p->component_type == 9) {
		struct ex_tree *info = node(tree, "种类別情报");
		struct ex_value *list = field(info, "复选框リスト");
		if (list && list->type == EX_LIST) {
			for (unsigned i = 0; i < list->list->nr_items; i++) {
				struct ex_value *item = ex_list_get(list->list, i);
				if (item && item->type == EX_STRING) {
					struct activity_part *cb = find_part(a, item->s);
					if (cb) PE_AddRadioButtonBoxChild(p->no, cb->no);
				}
			}
		}
	}
	const char *links[] = {"滚动条Ｘ座標連動", "滚动条Ｙ座標連動", "滚动条アルファ連動"};
	for (int i = 0; i < 3; i++) {
		struct activity_part *target = find_part(a, str(tree, links[i]));
		p->scroll_links[i] = target ? target->no : 0;
	}
	if (p->scrollbar) {
		struct ex_tree *info = node(tree, "种类別情报");
		struct activity_part *total = find_part(a, str(info, "全体滚动条量尺寸連動"));
		struct activity_part *view = find_part(a, str(info, "表示量尺寸連動"));
		p->scrollbar->total_link = total ? total->no : 0;
		p->scrollbar->view_link = view ? view->no : 0;
	}
	struct ex_tree *children = node(tree, "子パーツ");
	if (children) for (unsigned i = 0; i < children->nr_children; i++) load_part_links(a, &children->children[i]);
}

static struct ex_value *block(struct ex *ex, const char *key)
{
	char *encoded = utf2sjis(key, strlen(key));
	struct ex_value *v = ex_get(ex, encoded);
	free(encoded);
	return v;
}

static char *activity_file_path(struct string *file)
{
	char *path = gamedir_path_icase(file->text);
	if (path && file_exists(path)) return path;
	free(path);
	char *name = xmalloc(file->size + 8);
	memcpy(name, file->text, file->size);
	strcpy(name + file->size, ".pactex");
	path = gamedir_path_icase(name);
	free(name);
	return path;
}

bool PE_IsExistActivityFile(struct string *file)
{
	if (asset_exists_by_name(ASSET_PACT, file->text, NULL)) return true;
	char *path = activity_file_path(file);
	bool exists = path && file_exists(path);
	free(path);
	return exists;
}

bool PE_ReadActivityFile(struct string *name, struct string *file, bool edit)
{
	struct archive_data *data = asset_get_by_name(ASSET_PACT, file->text, NULL);
	struct ex *ex;
	if (data) {
		ex = ex_read(data->data, data->size);
		archive_free_data(data);
	} else {
		char *path = activity_file_path(file);
		ex = path ? ex_read_file(path) : NULL;
		free(path);
	}
	if (!ex) return false;
	struct ex_value *layout = block(ex, "アクティビティ");
	if (!layout || layout->type != EX_TREE || layout->tree->is_leaf || !PE_CreateActivity(name)) {
		ex_free(ex);
		return false;
	}
	struct activity *a = find_activity(name);
	struct ex_value *v = block(ex, "扩大");
	PE_SetActivityEXText(name, v && v->type == EX_STRING ? v->s : &EMPTY_STRING);
	v = block(ex, "背景图片");
	if (v && v->type == EX_STRING) PE_SetActivityBG(name, v->s);
	for (unsigned i = 0; i < layout->tree->nr_children; i++) load_part(a, &layout->tree->children[i], 0, edit);
	for (unsigned i = 0; i < layout->tree->nr_children; i++) load_part_links(a, &layout->tree->children[i]);
	PE_UpdateComponent(0);
	ex_free(ex);
	return true;
}
