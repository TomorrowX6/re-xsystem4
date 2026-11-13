#define VM_PRIVATE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

#ifdef __ANDROID__
#include <jni.h>
#endif

#include "cJSON.h"
#include "system4.h"
#include "system4/ain.h"
#include "system4/file.h"
#include "system4/instructions.h"
#include "system4/savefile.h"
#include "system4/string.h"
#include "system4/utfsjis.h"
#include "vm.h"
#include "vm/heap.h"
#include "vm/page.h"
#include "input.h"
#include "modifier.h"
#include "parts.h"
#include "savedata.h"
#include "xsystem4.h"

extern bool key_state[VK_NR_KEYCODES];
static int s_pulse_key0_frames = 0;
static char *s_pending_cheat_gbk = NULL;
static int s_cheat_wait_frames = 0;
static bool s_pending_is_streamer_cheat = false;
static bool s_wait_post_rcs_streamer_refresh = false;
static bool s_pending_clear_card_cache = false;
static bool s_pending_live_ui_refresh = false;
static bool s_in_modifier_frame = false;
static bool s_game_config_ready = false;
static int s_last_observed_summary_mode = -1;

/* Lock bitmask flags */
#define LOCK_PARTY_MAX_HP       (1 << 0)
#define LOCK_INFINITE_AP        (1 << 1)
#define LOCK_BATTLE_COMBO       (1 << 2)
#define LOCK_BATTLE_ROUND_1     (1 << 3)
#define LOCK_ONE_HIT_KILL       (1 << 4)
#define LOCK_ENEMY_ZERO_ATK     (1 << 5)
#define LOCK_LEADER_BOOST_CRIT  (1 << 6)
#define LOCK_INFINITE_SKILL_USE (1 << 7)
#define LOCK_INFINITE_ZIPPO     (1 << 8)
#define LOCK_MAX_RESOURCES      (1 << 9)
#define LOCK_TREASURE_100       (1 << 10)
#define LOCK_DISABLE_STREAMER_MODE (1 << 11)

/* GBK / SJIS names for special Rance 10 globals */
#define GBK_BATTLE_COUNT  "\xa1\xf4\xd5\xbd\xb6\xb7\xbb\xd8\xca\xfd"
#define SJIS_BATTLE_COUNT "\x81\x9f\x90\xed\x93\xac\x89\xf1\x90\x94"
#define GBK_DIFFICULTY    "\xa1\xf4\xc4\xd1\xb6\xc8"
#define SJIS_DIFFICULTY   "\x81\x9f\x93\xef\x88\xd5\x93\x78"
/* GBK byte sequences for MOD streamer-mode detection & in-game dialog */
#define GBK_STREAMER_PASSPHRASE "\xb9\xe0\xd7\xa2\x45\x69\x6e\x73\xb3\xc2\xd0\xbb\xd0\xbb\xdf\xf7"
#define GBK_CARD_STREAMER       "\xbf\xa8\xc5\xc6\xa3\xaf\xd6\xf7\xb2\xa5"
#define GBK_STREAMER_MODE       "\xd6\xf7\xb2\xa5\xc4\xa3\xca\xbd"
#define GBK_XIEXIEMIAO          "\xd0\xbb\xd0\xbb\xdf\xf7"
#define GBK_ZHUBO               "\xd6\xf7\xb2\xa5"
#define GBK_CHEAT_INVALID_TITLE "\xd7\xf7\xb1\xd7\xc2\xeb\xce\xde\xd0\xa7"
#define GBK_CHEAT_INVALID_MSG   "\xce\xb4\xd5\xd2\xb5\xbd\xb6\xd4\xd3\xa6\xd7\xf7\xb1\xd7\xc2\xeb\xa3\xa1"
#define GBK_CHEAT_OK_TITLE      "\xd7\xf7\xb1\xd7\xc2\xeb\xc9\xfa\xd0\xa7\xa3\xa1"
#define GBK_STREAMER_OFF_MSG    "\xd6\xf7\xb2\xa5\xc4\xa3\xca\xbd\xd2\xd1\xb9\xd8\xb1\xd5\xa3\xa1"

static pthread_mutex_t g_mod_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mod_cond = PTHREAD_COND_INITIALIZER;
static int g_live_lock_mask = 0;
static bool g_has_lock_mask_init = false;
static int g_pending_lock_streamer_mode = -1;
static char *g_pending_action_json = NULL;
static char *g_cached_live_state_json = NULL;
static char *g_modifier_config_path = NULL;
static uint32_t g_live_state_req_seq = 0;
static uint32_t g_live_state_built_seq = 0;

/* =========================================================================
 * Cross-Platform & Out-of-Game gsave (.asd) Helpers
 * ========================================================================= */

static struct gsave_global *gs_find_global(struct gsave *gs, const char *name1, const char *name2)
{
	if (!gs || !gs->globals) return NULL;
	for (int i = 0; i < gs->nr_globals; i++) {
		const char *gn = gs->globals[i].name;
		if (!gn) continue;
		if (name1 && !strcmp(gn, name1)) return &gs->globals[i];
		if (name2 && !strcmp(gn, name2)) return &gs->globals[i];
	}
	return NULL;
}

static struct gsave_keyval *gs_get_record_kv(struct gsave *gs, int32_t rec_idx, const char *name1, const char *name2)
{
	if (!gs || rec_idx < 0 || rec_idx >= gs->nr_records) return NULL;
	struct gsave_record *rec = &gs->records[rec_idx];
	struct gsave_struct_def *sd = (gs->version >= 7 && rec->struct_index >= 0 && rec->struct_index < gs->nr_struct_defs)
		? &gs->struct_defs[rec->struct_index] : NULL;
	for (int i = 0; i < rec->nr_indices; i++) {
		int32_t kv_idx = rec->indices[i];
		if (kv_idx < 0 || kv_idx >= gs->nr_keyvals) continue;
		struct gsave_keyval *kv = &gs->keyvals[kv_idx];
		const char *fname = (sd && i < sd->nr_fields) ? sd->fields[i].name : kv->name;
		if (!fname) continue;
		if (name1 && !strcmp(fname, name1)) return kv;
		if (name2 && !strcmp(fname, name2)) return kv;
	}
	return NULL;
}

static bool gs_ensure_adv_shortcut_5_unlocked(struct gsave *gs, int32_t cfg_rec, bool *out_modified)
{
	struct gsave_keyval *kv_adv = (cfg_rec >= 0) ? gs_get_record_kv(gs, cfg_rec, "m_isShowAdvShortcut", NULL) : NULL;
	if (!kv_adv || kv_adv->value < 0 || kv_adv->value >= gs->nr_arrays)
		return false;
	struct gsave_array *ga = &gs->arrays[kv_adv->value];
	if (ga->rank < 1) {
		free(ga->dimensions);
		free(ga->flat_arrays);
		ga->rank = 1;
		ga->dimensions = xcalloc(1, sizeof(int32_t));
		ga->dimensions[0] = 7;
		ga->nr_flat_arrays = 1;
		ga->flat_arrays = xcalloc(1, sizeof(struct gsave_flat_array));
		ga->flat_arrays[0].type = AIN_BOOL;
		ga->flat_arrays[0].nr_values = 7;
		ga->flat_arrays[0].values = xcalloc(7, sizeof(struct gsave_array_value));
		for (int i = 0; i < 7; i++)
			ga->flat_arrays[0].values[i].type = AIN_BOOL;
		ga->flat_arrays[0].values[5].value = 1;
		if (out_modified)
			*out_modified = true;
		return true;
	}
	if (ga->rank != 1 || ga->nr_flat_arrays < 1 || !ga->flat_arrays)
		return false;
	struct gsave_flat_array *fa = &ga->flat_arrays[0];
	enum ain_data_type et = fa->type ? fa->type : AIN_BOOL;
	if (fa->nr_values < 6) {
		int old_n = fa->nr_values;
		int new_n = 7;
		fa->values = xrealloc_array(fa->values, old_n, new_n, sizeof(struct gsave_array_value));
		for (int i = old_n; i < new_n; i++) {
			fa->values[i].type = et;
			fa->values[i].value = 0;
		}
		fa->nr_values = new_n;
		fa->type = et;
		if (ga->dimensions)
			ga->dimensions[0] = new_n;
		if (out_modified)
			*out_modified = true;
	}
	if (fa->values[5].value != 1) {
		fa->values[5].type = et;
		fa->values[5].value = 1;
		if (out_modified)
			*out_modified = true;
	}
	return true;
}

static bool write_game_config_streamer_mode(const char *config_path, bool streamer_mode_enabled)
{
	if (!config_path || !config_path[0] || !file_exists(config_path))
		return false;
	bool prev_gbk = sys4_text_is_gbk;
	if (!ain)
		sys4_set_text_gbk(true);
	enum savefile_error serr;
	struct gsave *gs = gsave_read(config_path, &serr);
	if (!gs) {
		if (!ain)
			sys4_set_text_gbk(prev_gbk);
		return false;
	}
	struct gsave_global *g_cfg = gs_find_global(gs, "g_gameConfig", NULL);
	int32_t cfg_rec = g_cfg ? g_cfg->value : -1;
	struct gsave_keyval *kv = (cfg_rec >= 0) ? gs_get_record_kv(gs, cfg_rec, "m_isSummaryMode", NULL) : NULL;
	if (!kv) {
		gsave_free(gs);
		if (!ain)
			sys4_set_text_gbk(prev_gbk);
		return false;
	}
	int32_t target = streamer_mode_enabled ? 1 : 0;
	bool modified = false;
	if (target == 0) {
		gs_ensure_adv_shortcut_5_unlocked(gs, cfg_rec, &modified);
	}
	if (kv->value != target) {
		kv->value = target;
		modified = true;
	}
	if (!modified) {
		gsave_free(gs);
		if (!ain)
			sys4_set_text_gbk(prev_gbk);
		return true;
	}

	char *tmp_path = NULL;
	FILE *fp = atomic_save_open(config_path, "wb", &tmp_path);
	bool ok = false;
	if (fp) {
		if (gsave_write(gs, fp, false, 1) == SAVEFILE_SUCCESS &&
		    atomic_save_commit(fp, config_path, tmp_path, true))
			ok = true;
		else
			atomic_save_abort(fp, tmp_path);
	}
	gsave_free(gs);
	if (!ain)
		sys4_set_text_gbk(prev_gbk);
	return ok;
}

static void persist_streamer_mode_to_savedir(bool streamer_mode_enabled)
{
	if (!config.save_dir || !config.save_dir[0])
		return;
	char *cfg_path = savedir_path("GameConfig.asd");
	if (!cfg_path)
		return;
	write_game_config_streamer_mode(cfg_path, streamer_mode_enabled);
	free(cfg_path);
}

static void persist_modifier_config_from_native(int lock_mask)
{
	char *cfg_path = NULL;
	pthread_mutex_lock(&g_mod_mutex);
	if (g_modifier_config_path && g_modifier_config_path[0])
		cfg_path = strdup(g_modifier_config_path);
	pthread_mutex_unlock(&g_mod_mutex);

#ifndef __ANDROID__
	if (!cfg_path) {
		const char *env_cfg = getenv("XSYS4_MOD_CONFIG_FILE");
		if (env_cfg && env_cfg[0])
			cfg_path = strdup(env_cfg);
	}
#endif
	if (!cfg_path)
		return;

	cJSON *root = NULL;
	if (file_exists(cfg_path)) {
		size_t sz = 0;
		char *raw = file_read(cfg_path, &sz);
		if (raw) {
			root = cJSON_Parse(raw);
			free(raw);
		}
	}
	if (!root || !cJSON_IsObject(root)) {
		cJSON_Delete(root);
		root = cJSON_CreateObject();
		cJSON_AddBoolToObject(root, "floating_enabled", true);
		cJSON_AddBoolToObject(root, "floating_default_on_v1", true);
		cJSON_AddBoolToObject(root, "ctrl_overlay_enabled", true);
		cJSON_AddBoolToObject(root, "ctrl_default_on_v1", true);
		cJSON_AddBoolToObject(root, "ctrl_overlay_visible", true);
	}

	bool want_dis = (lock_mask & LOCK_DISABLE_STREAMER_MODE) != 0;
	cJSON *j_dis = cJSON_GetObjectItem(root, "disable_streamer_mode");
	cJSON *j_mask = cJSON_GetObjectItem(root, "lock_mask");
	bool cur_dis = j_dis ? cJSON_IsTrue(j_dis) : false;
	int cur_mask = (j_mask && cJSON_IsNumber(j_mask)) ? j_mask->valueint : -1;
	if (j_dis && j_mask && cur_dis == want_dis && cur_mask == lock_mask) {
		cJSON_Delete(root);
		free(cfg_path);
		return;
	}

	cJSON *j_seq = cJSON_GetObjectItem(root, "config_seq");
	int next_seq = ((j_seq && cJSON_IsNumber(j_seq)) ? j_seq->valueint : 0) + 1;

	cJSON_DeleteItemFromObject(root, "disable_streamer_mode");
	cJSON_AddBoolToObject(root, "disable_streamer_mode", want_dis);
	cJSON_DeleteItemFromObject(root, "lock_mask");
	cJSON_AddNumberToObject(root, "lock_mask", lock_mask);
	cJSON_DeleteItemFromObject(root, "config_seq");
	cJSON_AddNumberToObject(root, "config_seq", next_seq);
	if (!cJSON_GetObjectItem(root, "floating_default_on_v1"))
		cJSON_AddBoolToObject(root, "floating_default_on_v1", true);
	if (!cJSON_GetObjectItem(root, "floating_enabled"))
		cJSON_AddBoolToObject(root, "floating_enabled", true);
	if (!cJSON_GetObjectItem(root, "ctrl_default_on_v1"))
		cJSON_AddBoolToObject(root, "ctrl_default_on_v1", true);
	if (!cJSON_GetObjectItem(root, "ctrl_overlay_enabled"))
		cJSON_AddBoolToObject(root, "ctrl_overlay_enabled", true);
	if (!cJSON_GetObjectItem(root, "ctrl_overlay_visible")) {
		cJSON *j_ctrl_en = cJSON_GetObjectItem(root, "ctrl_overlay_enabled");
		bool ctrl_en = j_ctrl_en ? cJSON_IsTrue(j_ctrl_en) : true;
		cJSON_AddBoolToObject(root, "ctrl_overlay_visible", ctrl_en);
	}

	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (out) {
		char *tmp_path = NULL;
		FILE *fp = atomic_save_open(cfg_path, "wb", &tmp_path);
		if (fp) {
			size_t len = strlen(out);
			if (fwrite(out, 1, len, fp) == len)
				atomic_save_commit(fp, cfg_path, tmp_path, true);
			else
				atomic_save_abort(fp, tmp_path);
		}
		free(out);
	}
	free(cfg_path);
}

#ifdef __ANDROID__

static int32_t gs_get_field_int(struct gsave *gs, int32_t rec_idx, const char *name1, const char *name2, int32_t def_val)
{
	struct gsave_keyval *kv = gs_get_record_kv(gs, rec_idx, name1, name2);
	return kv ? kv->value : def_val;
}

static void gs_set_field_int(struct gsave *gs, int32_t rec_idx, const char *name1, const char *name2, int32_t val)
{
	struct gsave_keyval *kv = gs_get_record_kv(gs, rec_idx, name1, name2);
	if (kv) kv->value = val;
}

static struct gsave_flat_array *gs_get_flat_array(struct gsave *gs, int32_t arr_idx)
{
	if (!gs || arr_idx < 0 || arr_idx >= gs->nr_arrays) return NULL;
	struct gsave_array *a = &gs->arrays[arr_idx];
	if (a->rank < 1 || a->nr_flat_arrays < 1 || !a->flat_arrays) return NULL;
	return &a->flat_arrays[0];
}

static int32_t gs_get_tt(struct gsave *gs, int idx, int32_t def_val)
{
	struct gsave_global *g = gs_find_global(gs, "tt", NULL);
	if (!g) return def_val;
	struct gsave_flat_array *fa = gs_get_flat_array(gs, g->value);
	if (!fa || idx < 0 || idx >= fa->nr_values) return def_val;
	return fa->values[idx].value;
}

static void gs_set_tt(struct gsave *gs, int idx, int32_t val)
{
	struct gsave_global *g = gs_find_global(gs, "tt", NULL);
	if (!g) return;
	struct gsave_flat_array *fa = gs_get_flat_array(gs, g->value);
	if (!fa || idx < 0 || idx >= fa->nr_values) return;
	fa->values[idx].value = val;
}

static void gs_boost_card_array(struct gsave *gs, int32_t arr_idx, int set_count, int add_count, int atk_bonus, int hp_bonus, int *total_cards)
{
	struct gsave_flat_array *fa = gs_get_flat_array(gs, arr_idx);
	if (!fa) return;
	for (int i = 0; i < fa->nr_values; i++) {
		int32_t c_rec = fa->values[i].value;
		if (c_rec < 0 || c_rec >= gs->nr_records) continue;
		if (total_cards) (*total_cards)++;
		if (set_count > 0) {
			gs_set_field_int(gs, c_rec, "m_count", NULL, set_count);
			gs_set_field_int(gs, c_rec, "m_lastCount", NULL, set_count);
		} else if (add_count > 0) {
			int32_t cur = gs_get_field_int(gs, c_rec, "m_count", NULL, 1);
			gs_set_field_int(gs, c_rec, "m_count", NULL, cur + add_count);
			gs_set_field_int(gs, c_rec, "m_lastCount", NULL, cur + add_count);
		}
		if (atk_bonus > 0) {
			int32_t cur_atk = gs_get_field_int(gs, c_rec, "m_totalAtk", NULL, 100);
			if (cur_atk < atk_bonus)
				gs_set_field_int(gs, c_rec, "m_totalAtk", NULL, atk_bonus);
		}
		if (hp_bonus > 0) {
			int32_t cur_hp = gs_get_field_int(gs, c_rec, "m_totalHp", NULL, 100);
			if (cur_hp < hp_bonus)
				gs_set_field_int(gs, c_rec, "m_totalHp", NULL, hp_bonus);
		}
	}
}

static int gs_process_all_cards(struct gsave *gs, int set_count, int add_count, int atk_bonus, int hp_bonus)
{
	int total = 0;
	struct gsave_global *g_pc = gs_find_global(gs, "g_playerCard", NULL);
	if (!g_pc) return 0;
	int32_t pc_rec = g_pc->value;
	int32_t org_arr = gs_get_field_int(gs, pc_rec, "m_org", NULL, -1);
	struct gsave_flat_array *fa_org = gs_get_flat_array(gs, org_arr);
	if (fa_org) {
		for (int i = 0; i < fa_org->nr_values; i++) {
			int32_t org_rec = fa_org->values[i].value;
			int32_t card_arr = gs_get_field_int(gs, org_rec, "m_card", NULL, -1);
			gs_boost_card_array(gs, card_arr, set_count, add_count, atk_bonus, hp_bonus, &total);
		}
	}
	int32_t stock_arr = gs_get_field_int(gs, pc_rec, "m_stock", NULL, -1);
	gs_boost_card_array(gs, stock_arr, set_count, add_count, atk_bonus, hp_bonus, &total);
	int32_t extra_arr = gs_get_field_int(gs, pc_rec, "m_extraCard", NULL, -1);
	gs_boost_card_array(gs, extra_arr, set_count, add_count, atk_bonus, hp_bonus, &total);
	int32_t old_arr = gs_get_field_int(gs, pc_rec, "m_card", "m_oldCard", -1);
	gs_boost_card_array(gs, old_arr, set_count, add_count, atk_bonus, hp_bonus, NULL);
	return total;
}

static int gs_process_all_characters(struct gsave *gs, int set_star, int set_exp)
{
	struct gsave_global *g_ch = gs_find_global(gs, "g_character", NULL);
	if (!g_ch) return 0;
	int32_t ch_rec = g_ch->value;
	int32_t arr_idx = gs_get_field_int(gs, ch_rec, "m_character", NULL, -1);
	struct gsave_flat_array *fa = gs_get_flat_array(gs, arr_idx);
	if (!fa) return 0;
	for (int i = 0; i < fa->nr_values; i++) {
		int32_t c_rec = fa->values[i].value;
		if (set_star > 0) {
			int32_t cur = gs_get_field_int(gs, c_rec, "m_star", NULL, 0);
			if (cur < set_star)
				gs_set_field_int(gs, c_rec, "m_star", NULL, set_star);
		}
		if (set_exp > 0) {
			gs_set_field_int(gs, c_rec, "<Exp>", NULL, set_exp);
		}
	}
	return fa->nr_values;
}

static void json_read_common_status_gs(cJSON *root, struct gsave *gs, int32_t cs_rec)
{
	if (cs_rec < 0) return;
	cJSON_AddNumberToObject(root, "partyAtk", gs_get_field_int(gs, cs_rec, "<Atk>", NULL, 0));
	cJSON_AddNumberToObject(root, "atkPerPhys", gs_get_field_int(gs, cs_rec, "m_AtkPerPhysical", NULL, 0));
	cJSON_AddNumberToObject(root, "atkPerMag", gs_get_field_int(gs, cs_rec, "m_AtkPerMagical", NULL, 0));
	cJSON_AddNumberToObject(root, "defPerPhys", gs_get_field_int(gs, cs_rec, "m_DefPerPhysical", NULL, 0));
	cJSON_AddNumberToObject(root, "defPerMag", gs_get_field_int(gs, cs_rec, "m_DefPerMagical", NULL, 0));
	cJSON_AddNumberToObject(root, "perAvoid", gs_get_field_int(gs, cs_rec, "m_PerAvoidance", NULL, 0));
	cJSON_AddNumberToObject(root, "decBadCond", gs_get_field_int(gs, cs_rec, "m_DecPerBadCondition", NULL, 0));
	cJSON_AddNumberToObject(root, "perAutoAtk", gs_get_field_int(gs, cs_rec, "m_perAutoBattleAtk", NULL, 0));
	cJSON_AddNumberToObject(root, "perCrit", gs_get_field_int(gs, cs_rec, "m_PerCritical", NULL, 0));
	cJSON_AddNumberToObject(root, "perMulti", gs_get_field_int(gs, cs_rec, "m_PerMultiAttack", NULL, 0));
	cJSON_AddNumberToObject(root, "perExpUp", gs_get_field_int(gs, cs_rec, "m_PerExpUp", NULL, 0));
	cJSON_AddNumberToObject(root, "perHpUp", gs_get_field_int(gs, cs_rec, "m_PerHpUp", NULL, 0));
	cJSON_AddNumberToObject(root, "perAutoHeal", gs_get_field_int(gs, cs_rec, "m_PerAutoHeal", NULL, 0));
	cJSON_AddNumberToObject(root, "perFindTreasure", gs_get_field_int(gs, cs_rec, "m_PerFindTreasure", NULL, 0));
	cJSON_AddNumberToObject(root, "perInjection", gs_get_field_int(gs, cs_rec, "m_PerInjection", NULL, 0));
	cJSON_AddNumberToObject(root, "addPerHit", gs_get_field_int(gs, cs_rec, "m_AddPerHit", NULL, 0));
	cJSON_AddNumberToObject(root, "statusMaxPer", gs_get_field_int(gs, cs_rec, "StatusMaxPer", NULL, 0));
}

static void json_write_int_if_present(cJSON *root, const char *key, struct gsave *gs, int32_t rec_idx, const char *f1, const char *f2)
{
	cJSON *item = cJSON_GetObjectItem(root, key);
	if (item && cJSON_IsNumber(item)) {
		gs_set_field_int(gs, rec_idx, f1, f2, item->valueint);
	}
}

static char *read_local_save_summary_str(const char *path)
{
	sys4_set_text_gbk(true);
	enum savefile_error serr;
	struct gsave *gs = gsave_read(path, &serr);
	cJSON *root = cJSON_CreateObject();
	if (!gs) {
		cJSON_AddBoolToObject(root, "valid", false);
		cJSON_AddStringToObject(root, "error", savefile_strerror(serr));
		char *out = cJSON_PrintUnformatted(root);
		cJSON_Delete(root);
		return out;
	}
	cJSON_AddBoolToObject(root, "valid", true);
	cJSON_AddBoolToObject(root, "ok", true);
	if (gs->nr_comments > 0 && gs->comments[0] && gs->comments[0]->size > 0) {
		char *utf = sjis2utf(gs->comments[0]->text, gs->comments[0]->size);
		cJSON_AddStringToObject(root, "comment", utf ? utf : "");
		free(utf);
	} else {
		cJSON_AddStringToObject(root, "comment", "");
	}

	struct gsave_global *g_gc = gs_find_global(gs, "g_gameContext", NULL);
	int32_t gc_rec = g_gc ? g_gc->value : -1;
	cJSON_AddNumberToObject(root, "turn", gs_get_field_int(gs, gc_rec, "m_turn", NULL, 0));
	cJSON_AddNumberToObject(root, "chapter", gs_get_field_int(gs, gc_rec, "m_chapter", NULL, 0));

	struct gsave_global *g_diff = gs_find_global(gs, GBK_DIFFICULTY, SJIS_DIFFICULTY);
	struct gsave_global *g_bc = gs_find_global(gs, GBK_BATTLE_COUNT, SJIS_BATTLE_COUNT);
	cJSON_AddNumberToObject(root, "difficulty", g_diff ? g_diff->value : 1);
	cJSON_AddNumberToObject(root, "battleCount", g_bc ? g_bc->value : 0);

	struct gsave_global *g_pcp = gs_find_global(gs, "g_playerCommonParam", NULL);
	int32_t pcp_rec = g_pcp ? g_pcp->value : -1;
	cJSON_AddNumberToObject(root, "foodTicket", gs_get_field_int(gs, pcp_rec, "m_foodTicket", NULL, 0));
	cJSON_AddNumberToObject(root, "honor", gs_get_field_int(gs, pcp_rec, "m_honor", NULL, 0));
	cJSON_AddNumberToObject(root, "friendPoint", gs_get_field_int(gs, pcp_rec, "m_friendPoint", NULL, 0));
	cJSON_AddNumberToObject(root, "totalExp", gs_get_field_int(gs, pcp_rec, "<TotalExp>", NULL, 0));
	cJSON_AddNumberToObject(root, "partyRank", gs_get_field_int(gs, pcp_rec, "<PartyRank>", NULL, 0));
	cJSON_AddNumberToObject(root, "treasureNotGetCount", gs_get_field_int(gs, pcp_rec, "<TreasureNotGetCount>", NULL, 0));
	cJSON_AddNumberToObject(root, "itemStar", gs_get_field_int(gs, pcp_rec, "<ItemStar>", NULL, 0));
	cJSON_AddNumberToObject(root, "isUsedHannyZippo", gs_get_field_int(gs, pcp_rec, "<IsUsedHannyZippo>", NULL, 0));

	struct gsave_global *g_pb = gs_find_global(gs, "g_partyBonus", NULL);
	int32_t pb_rec = g_pb ? g_pb->value : -1;
	cJSON_AddNumberToObject(root, "partyBonusMaxPoint", gs_get_field_int(gs, pb_rec, "m_maxPoint", NULL, 0));

	struct gsave_global *g_party = gs_find_global(gs, "g_party", NULL);
	int32_t party_rec = g_party ? g_party->value : -1;
	int32_t ps_rec = gs_get_field_int(gs, party_rec, "m_status", NULL, -1);
	int32_t cs_rec = gs_get_field_int(gs, ps_rec, "m_status", NULL, -1);
	cJSON_AddNumberToObject(root, "partyHp", gs_get_field_int(gs, party_rec, "m_hp", NULL, 0));
	cJSON_AddNumberToObject(root, "partyMaxHp", gs_get_field_int(gs, ps_rec, "m_totalHp", NULL, 0));
	cJSON_AddNumberToObject(root, "partyAvailCount", gs_get_field_int(gs, party_rec, "m_availableCount", NULL, 4));
	cJSON_AddNumberToObject(root, "partyIsFixStatus", gs_get_field_int(gs, party_rec, "<IsFixStatus>", NULL, 0));
	json_read_common_status_gs(root, gs, cs_rec);

	cJSON_AddNumberToObject(root, "humanCount", gs_get_tt(gs, 33, 300000));
	cJSON_AddNumberToObject(root, "humanDead", gs_get_tt(gs, 34, 0));
	cJSON_AddNumberToObject(root, "humanAlivePer", gs_get_tt(gs, 35, 100));
	cJSON_AddNumberToObject(root, "humanDeadPer", gs_get_tt(gs, 36, 0));
	cJSON_AddNumberToObject(root, "curseTurn", gs_get_tt(gs, 41, 0));
	cJSON_AddNumberToObject(root, "rareLuck", gs_get_tt(gs, 42, 0));
	cJSON_AddNumberToObject(root, "hannyZippo", gs_get_tt(gs, 101, 0));
	cJSON_AddNumberToObject(root, "seaPlayerArmy", gs_get_tt(gs, 66, 0));
	cJSON_AddNumberToObject(root, "seaEnemyArmy", gs_get_tt(gs, 68, 0));
	cJSON_AddNumberToObject(root, "leazasState", gs_get_tt(gs, 13, 0));
	cJSON_AddNumberToObject(root, "helmanState", gs_get_tt(gs, 14, 0));
	cJSON_AddNumberToObject(root, "zethState", gs_get_tt(gs, 15, 0));
	cJSON_AddNumberToObject(root, "freeCitiesState", gs_get_tt(gs, 16, 0));
	cJSON_AddNumberToObject(root, "leazasPArmy", gs_get_tt(gs, 25, 300));
	cJSON_AddNumberToObject(root, "helmanPArmy", gs_get_tt(gs, 26, 300));
	cJSON_AddNumberToObject(root, "zethPArmy", gs_get_tt(gs, 27, 300));
	cJSON_AddNumberToObject(root, "freeCitiesPArmy", gs_get_tt(gs, 28, 300));
	cJSON_AddNumberToObject(root, "leazasEArmy", gs_get_tt(gs, 29, 500));
	cJSON_AddNumberToObject(root, "helmanEArmy", gs_get_tt(gs, 30, 500));
	cJSON_AddNumberToObject(root, "zethEArmy", gs_get_tt(gs, 31, 500));
	cJSON_AddNumberToObject(root, "freeCitiesEArmy", gs_get_tt(gs, 32, 500));
	cJSON_AddNumberToObject(root, "subRemnant", gs_get_tt(gs, 37, 0));
	cJSON_AddNumberToObject(root, "subDwarf", gs_get_tt(gs, 38, 0));
	cJSON_AddNumberToObject(root, "subGeneral", gs_get_tt(gs, 39, 0));
	cJSON_AddNumberToObject(root, "subHanny", gs_get_tt(gs, 40, 0));

	cJSON *c_state = cJSON_CreateArray();
	cJSON *c_parmy = cJSON_CreateArray();
	cJSON *c_earmy = cJSON_CreateArray();
	cJSON *c_sub = cJSON_CreateArray();
	for (int i = 0; i < 4; i++) {
		cJSON_AddItemToArray(c_state, cJSON_CreateNumber(gs_get_tt(gs, 13 + i, 3)));
		cJSON_AddItemToArray(c_parmy, cJSON_CreateNumber(gs_get_tt(gs, 25 + i, 300)));
		cJSON_AddItemToArray(c_earmy, cJSON_CreateNumber(gs_get_tt(gs, 29 + i, 500)));
		cJSON_AddItemToArray(c_sub, cJSON_CreateNumber(gs_get_tt(gs, 37 + i, 0)));
	}
	cJSON_AddItemToObject(root, "countryState", c_state);
	cJSON_AddItemToObject(root, "countryPlayerArmy", c_parmy);
	cJSON_AddItemToObject(root, "countryEnemyArmy", c_earmy);
	cJSON_AddItemToObject(root, "subArmy", c_sub);

	int card_count = gs_process_all_cards(gs, 0, 0, 0, 0);
	int char_count = gs_process_all_characters(gs, 0, 0);
	cJSON_AddNumberToObject(root, "cardCount", card_count);
	cJSON_AddNumberToObject(root, "charCount", char_count);

	gsave_free(gs);
	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return out;
}

static bool write_local_save_mod_str(const char *path, const char *mod_json_str)
{
	sys4_set_text_gbk(true);
	cJSON *mod = cJSON_Parse(mod_json_str);
	if (!mod) return false;

	enum savefile_error serr;
	struct gsave *gs = gsave_read(path, &serr);
	if (!gs) {
		cJSON_Delete(mod);
		return false;
	}

	struct gsave_global *g_gc = gs_find_global(gs, "g_gameContext", NULL);
	int32_t gc_rec = g_gc ? g_gc->value : -1;
	json_write_int_if_present(mod, "turn", gs, gc_rec, "m_turn", NULL);
	json_write_int_if_present(mod, "chapter", gs, gc_rec, "m_chapter", NULL);

	cJSON *j_diff = cJSON_GetObjectItem(mod, "difficulty");
	if (j_diff && cJSON_IsNumber(j_diff)) {
		struct gsave_global *g_diff = gs_find_global(gs, GBK_DIFFICULTY, SJIS_DIFFICULTY);
		if (g_diff) g_diff->value = j_diff->valueint;
	}
	cJSON *j_bc = cJSON_GetObjectItem(mod, "battleCount");
	if (j_bc && cJSON_IsNumber(j_bc)) {
		struct gsave_global *g_bc = gs_find_global(gs, GBK_BATTLE_COUNT, SJIS_BATTLE_COUNT);
		if (g_bc) g_bc->value = j_bc->valueint;
	}

	struct gsave_global *g_pcp = gs_find_global(gs, "g_playerCommonParam", NULL);
	int32_t pcp_rec = g_pcp ? g_pcp->value : -1;
	json_write_int_if_present(mod, "foodTicket", gs, pcp_rec, "m_foodTicket", NULL);
	json_write_int_if_present(mod, "honor", gs, pcp_rec, "m_honor", NULL);
	json_write_int_if_present(mod, "friendPoint", gs, pcp_rec, "m_friendPoint", NULL);
	json_write_int_if_present(mod, "totalExp", gs, pcp_rec, "<TotalExp>", NULL);
	json_write_int_if_present(mod, "partyRank", gs, pcp_rec, "<PartyRank>", NULL);
	json_write_int_if_present(mod, "treasureNotGetCount", gs, pcp_rec, "<TreasureNotGetCount>", NULL);
	json_write_int_if_present(mod, "itemStar", gs, pcp_rec, "<ItemStar>", NULL);
	json_write_int_if_present(mod, "isUsedHannyZippo", gs, pcp_rec, "<IsUsedHannyZippo>", NULL);

	struct gsave_global *g_pb = gs_find_global(gs, "g_partyBonus", NULL);
	int32_t pb_rec = g_pb ? g_pb->value : -1;
	json_write_int_if_present(mod, "partyBonusMaxPoint", gs, pb_rec, "m_maxPoint", NULL);

	struct gsave_global *g_party = gs_find_global(gs, "g_party", NULL);
	int32_t party_rec = g_party ? g_party->value : -1;
	int32_t ps_rec = gs_get_field_int(gs, party_rec, "m_status", NULL, -1);
	int32_t cs_rec = gs_get_field_int(gs, ps_rec, "m_status", NULL, -1);
	json_write_int_if_present(mod, "partyHp", gs, party_rec, "m_hp", NULL);
	json_write_int_if_present(mod, "partyMaxHp", gs, ps_rec, "m_totalHp", NULL);
	json_write_int_if_present(mod, "partyAvailCount", gs, party_rec, "m_availableCount", NULL);
	json_write_int_if_present(mod, "partyIsFixStatus", gs, party_rec, "<IsFixStatus>", NULL);
	{
		int32_t cur_hp = gs_get_field_int(gs, party_rec, "m_hp", NULL, 1);
		int32_t max_hp = gs_get_field_int(gs, ps_rec, "m_totalHp", NULL, 1);
		if (max_hp > 0) {
			union { float f; int32_t i; } u;
			u.f = (float)cur_hp / (float)max_hp;
			if (u.f > 1.0f) u.f = 1.0f;
			gs_set_field_int(gs, party_rec, "m_hpRatio", NULL, u.i);
		}
	}
	json_write_int_if_present(mod, "partyAtk", gs, cs_rec, "<Atk>", NULL);
	json_write_int_if_present(mod, "atkPerPhys", gs, cs_rec, "m_AtkPerPhysical", NULL);
	json_write_int_if_present(mod, "atkPerMag", gs, cs_rec, "m_AtkPerMagical", NULL);
	json_write_int_if_present(mod, "defPerPhys", gs, cs_rec, "m_DefPerPhysical", NULL);
	json_write_int_if_present(mod, "defPerMag", gs, cs_rec, "m_DefPerMagical", NULL);
	json_write_int_if_present(mod, "perAvoid", gs, cs_rec, "m_PerAvoidance", NULL);
	json_write_int_if_present(mod, "decBadCond", gs, cs_rec, "m_DecPerBadCondition", NULL);
	json_write_int_if_present(mod, "perAutoAtk", gs, cs_rec, "m_perAutoBattleAtk", NULL);
	json_write_int_if_present(mod, "perCrit", gs, cs_rec, "m_PerCritical", NULL);
	json_write_int_if_present(mod, "perMulti", gs, cs_rec, "m_PerMultiAttack", NULL);
	json_write_int_if_present(mod, "perExpUp", gs, cs_rec, "m_PerExpUp", NULL);
	json_write_int_if_present(mod, "perHpUp", gs, cs_rec, "m_PerHpUp", NULL);
	json_write_int_if_present(mod, "perAutoHeal", gs, cs_rec, "m_PerAutoHeal", NULL);
	json_write_int_if_present(mod, "perFindTreasure", gs, cs_rec, "m_PerFindTreasure", NULL);
	json_write_int_if_present(mod, "perInjection", gs, cs_rec, "m_PerInjection", NULL);
	json_write_int_if_present(mod, "addPerHit", gs, cs_rec, "m_AddPerHit", NULL);
	json_write_int_if_present(mod, "statusMaxPer", gs, cs_rec, "StatusMaxPer", NULL);

	cJSON *it;
	if ((it = cJSON_GetObjectItem(mod, "humanCount")) && cJSON_IsNumber(it)) gs_set_tt(gs, 33, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "humanDead")) && cJSON_IsNumber(it)) {
		gs_set_tt(gs, 34, it->valueint);
		gs_set_field_int(gs, pcp_rec, "m_lastHumanDead", NULL, it->valueint);
	}
	if ((it = cJSON_GetObjectItem(mod, "humanAlivePer")) && cJSON_IsNumber(it)) {
		gs_set_tt(gs, 35, it->valueint);
		gs_set_field_int(gs, pcp_rec, "m_lastHumanAlivePer", NULL, it->valueint);
	}
	if ((it = cJSON_GetObjectItem(mod, "humanDeadPer")) && cJSON_IsNumber(it)) gs_set_tt(gs, 36, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "curseTurn")) && cJSON_IsNumber(it)) gs_set_tt(gs, 41, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "rareLuck")) && cJSON_IsNumber(it)) gs_set_tt(gs, 42, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "hannyZippo")) && cJSON_IsNumber(it)) gs_set_tt(gs, 101, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "seaPlayerArmy")) && cJSON_IsNumber(it)) gs_set_tt(gs, 66, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "seaEnemyArmy")) && cJSON_IsNumber(it)) gs_set_tt(gs, 68, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "leazasState")) && cJSON_IsNumber(it)) gs_set_tt(gs, 13, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "helmanState")) && cJSON_IsNumber(it)) gs_set_tt(gs, 14, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "zethState")) && cJSON_IsNumber(it)) gs_set_tt(gs, 15, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "freeCitiesState")) && cJSON_IsNumber(it)) gs_set_tt(gs, 16, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "leazasPArmy")) && cJSON_IsNumber(it)) gs_set_tt(gs, 25, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "helmanPArmy")) && cJSON_IsNumber(it)) gs_set_tt(gs, 26, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "zethPArmy")) && cJSON_IsNumber(it)) gs_set_tt(gs, 27, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "freeCitiesPArmy")) && cJSON_IsNumber(it)) gs_set_tt(gs, 28, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "leazasEArmy")) && cJSON_IsNumber(it)) gs_set_tt(gs, 29, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "helmanEArmy")) && cJSON_IsNumber(it)) gs_set_tt(gs, 30, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "zethEArmy")) && cJSON_IsNumber(it)) gs_set_tt(gs, 31, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "freeCitiesEArmy")) && cJSON_IsNumber(it)) gs_set_tt(gs, 32, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "subRemnant")) && cJSON_IsNumber(it)) gs_set_tt(gs, 37, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "subDwarf")) && cJSON_IsNumber(it)) gs_set_tt(gs, 38, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "subGeneral")) && cJSON_IsNumber(it)) gs_set_tt(gs, 39, it->valueint);
	if ((it = cJSON_GetObjectItem(mod, "subHanny")) && cJSON_IsNumber(it)) gs_set_tt(gs, 40, it->valueint);

	cJSON *arr;
	if ((arr = cJSON_GetObjectItem(mod, "countryState")) && cJSON_IsArray(arr)) {
		for (int i = 0; i < 4 && i < cJSON_GetArraySize(arr); i++)
			gs_set_tt(gs, 13 + i, cJSON_GetArrayItem(arr, i)->valueint);
	}
	if ((arr = cJSON_GetObjectItem(mod, "countryPlayerArmy")) && cJSON_IsArray(arr)) {
		for (int i = 0; i < 4 && i < cJSON_GetArraySize(arr); i++)
			gs_set_tt(gs, 25 + i, cJSON_GetArrayItem(arr, i)->valueint);
	}
	if ((arr = cJSON_GetObjectItem(mod, "countryEnemyArmy")) && cJSON_IsArray(arr)) {
		for (int i = 0; i < 4 && i < cJSON_GetArraySize(arr); i++)
			gs_set_tt(gs, 29 + i, cJSON_GetArrayItem(arr, i)->valueint);
	}
	if ((arr = cJSON_GetObjectItem(mod, "subArmy")) && cJSON_IsArray(arr)) {
		for (int i = 0; i < 4 && i < cJSON_GetArraySize(arr); i++)
			gs_set_tt(gs, 37 + i, cJSON_GetArrayItem(arr, i)->valueint);
	}

	int set_card_count = (it = cJSON_GetObjectItem(mod, "setCardCount")) && cJSON_IsNumber(it) ? it->valueint : 0;
	int add_card_count = (it = cJSON_GetObjectItem(mod, "addCardCount")) && cJSON_IsNumber(it) ? it->valueint : 0;
	int card_atk_bonus = (it = cJSON_GetObjectItem(mod, "cardAtkBonus")) && cJSON_IsNumber(it) ? it->valueint : 0;
	int card_hp_bonus  = (it = cJSON_GetObjectItem(mod, "cardHpBonus")) && cJSON_IsNumber(it) ? it->valueint : 0;
	if (set_card_count > 0 || add_card_count > 0 || card_atk_bonus > 0 || card_hp_bonus > 0) {
		gs_process_all_cards(gs, set_card_count, add_card_count, card_atk_bonus, card_hp_bonus);
	}

	int set_char_star = (it = cJSON_GetObjectItem(mod, "setCharStar")) && cJSON_IsNumber(it) ? it->valueint : 0;
	int set_char_exp  = (it = cJSON_GetObjectItem(mod, "setCharExp")) && cJSON_IsNumber(it) ? it->valueint : 0;
	if (set_char_star > 0 || set_char_exp > 0) {
		gs_process_all_characters(gs, set_char_star, set_char_exp);
	}

	char *tmp_path = NULL;
	FILE *fp = atomic_save_open(path, "wb", &tmp_path);
	bool ok = false;
	if (fp) {
		if (gsave_write(gs, fp, false, 1) == SAVEFILE_SUCCESS)
			ok = atomic_save_commit(fp, path, tmp_path, false);
		else
			atomic_save_abort(fp, tmp_path);
	}
	gsave_free(gs);
	cJSON_Delete(mod);
	return ok;
}

static char *read_global_save_summary_str(const char *global_path, const char *collection_path)
{
	sys4_set_text_gbk(true);
	cJSON *root = cJSON_CreateObject();
	enum savefile_error serr;

	struct gsave *gs_g = (global_path && global_path[0]) ? gsave_read(global_path, &serr) : NULL;
	if (gs_g) {
		cJSON_AddBoolToObject(root, "hasGlobal", true);
		struct gsave_global *g_gc = gs_find_global(gs_g, "g_globalContext", NULL);
		int32_t gc_rec = g_gc ? g_gc->value : -1;
		cJSON_AddNumberToObject(root, "gamePlayCount", gs_get_field_int(gs_g, gc_rec, "m_gamePlayCount", NULL, 0));
		cJSON_AddNumberToObject(root, "itemStarRank", gs_get_field_int(gs_g, gc_rec, "m_itemStarRank", NULL, 0));
		struct gsave_flat_array *fa_cards = gs_get_flat_array(gs_g, gs_get_field_int(gs_g, gc_rec, "m_cardIds", NULL, -1));
		struct gsave_flat_array *fa_ranks = gs_get_flat_array(gs_g, gs_get_field_int(gs_g, gc_rec, "m_characterRank", NULL, -1));
		struct gsave_flat_array *fa_info  = gs_get_flat_array(gs_g, gs_get_field_int(gs_g, gc_rec, "m_cardInfo", NULL, -1));
		cJSON_AddNumberToObject(root, "unlockedCardCount", fa_cards ? fa_cards->nr_values : 0);
		cJSON_AddNumberToObject(root, "charRankCount", fa_ranks ? fa_ranks->nr_values : 0);
		cJSON_AddNumberToObject(root, "takeOverCardCount", fa_info ? fa_info->nr_values : 0);
		gsave_free(gs_g);
	} else {
		cJSON_AddBoolToObject(root, "hasGlobal", false);
	}

	struct gsave *gs_c = (collection_path && collection_path[0]) ? gsave_read(collection_path, &serr) : NULL;
	if (gs_c) {
		cJSON_AddBoolToObject(root, "hasCollection", true);
		struct gsave_global *g_ct = gs_find_global(gs_c, "g_completedTrophy", NULL);
		int32_t ct_rec = g_ct ? g_ct->value : -1;
		struct gsave_flat_array *fa_tr = gs_get_flat_array(gs_c, gs_get_field_int(gs_c, ct_rec, "m_trophyInfo", NULL, -1));
		int valid_trophies = 0;
		if (fa_tr) {
			for (int i = 0; i < fa_tr->nr_values; i++) {
				int32_t tr_rec = fa_tr->values[i].value;
				int32_t s_idx = gs_get_field_int(gs_c, tr_rec, "Id", "<Id>", GSAVE7_EMPTY_STRING);
				if (s_idx != GSAVE7_EMPTY_STRING && s_idx >= 0 && s_idx < gs_c->nr_strings && gs_c->strings[s_idx]->size > 0)
					valid_trophies++;
			}
		}
		cJSON_AddNumberToObject(root, "trophyCount", valid_trophies);
		union { int32_t i; float f; } u;
		u.i = gs_get_field_int(gs_c, ct_rec, "<LeaderAtkRatio>", NULL, 0);
		cJSON_AddNumberToObject(root, "leaderAtkRatio", u.f);
		int32_t cs_rec = gs_get_field_int(gs_c, ct_rec, "m_status", NULL, -1);
		cJSON_AddNumberToObject(root, "trophyExpBonus", gs_get_field_int(gs_c, cs_rec, "m_PerExpUp", NULL, 0));
		cJSON_AddNumberToObject(root, "trophyStatusMaxPer", gs_get_field_int(gs_c, cs_rec, "StatusMaxPer", NULL, 0));
		gsave_free(gs_c);
	} else {
		cJSON_AddBoolToObject(root, "hasCollection", false);
	}

	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return out;
}

static bool write_global_save_mod_str(const char *global_path, const char *collection_path, const char *mod_json_str)
{
	sys4_set_text_gbk(true);
	cJSON *mod = cJSON_Parse(mod_json_str);
	if (!mod) return false;
	bool any_ok = false;
	enum savefile_error serr;

	if (global_path && global_path[0]) {
		struct gsave *gs_g = gsave_read(global_path, &serr);
		if (gs_g) {
			struct gsave_global *g_gc = gs_find_global(gs_g, "g_globalContext", NULL);
			int32_t gc_rec = g_gc ? g_gc->value : -1;
			json_write_int_if_present(mod, "gamePlayCount", gs_g, gc_rec, "m_gamePlayCount", NULL);
			json_write_int_if_present(mod, "itemStarRank", gs_g, gc_rec, "m_itemStarRank", NULL);

			cJSON *j_rank = cJSON_GetObjectItem(mod, "setAllCharRank");
			if (j_rank && cJSON_IsNumber(j_rank) && j_rank->valueint > 0) {
				struct gsave_flat_array *fa_ranks = gs_get_flat_array(gs_g, gs_get_field_int(gs_g, gc_rec, "m_characterRank", NULL, -1));
				if (fa_ranks) {
					for (int i = 0; i < fa_ranks->nr_values; i++) {
						int32_t r_rec = fa_ranks->values[i].value;
						gs_set_field_int(gs_g, r_rec, "rank", "Rank", j_rank->valueint);
						gs_set_field_int(gs_g, r_rec, "eventStage", "EventStage", j_rank->valueint);
					}
				}
			}
			cJSON *j_to = cJSON_GetObjectItem(mod, "setTakeOverCardCount");
			if (j_to && cJSON_IsNumber(j_to) && j_to->valueint > 0) {
				struct gsave_flat_array *fa_info = gs_get_flat_array(gs_g, gs_get_field_int(gs_g, gc_rec, "m_cardInfo", NULL, -1));
				if (fa_info) {
					for (int i = 0; i < fa_info->nr_values; i++) {
						int32_t i_rec = fa_info->values[i].value;
						gs_set_field_int(gs_g, i_rec, "m_count", NULL, j_to->valueint);
					}
				}
			}
			char *tmp_path = NULL;
			FILE *fp = atomic_save_open(global_path, "wb", &tmp_path);
			if (fp) {
				if (gsave_write(gs_g, fp, false, 1) == SAVEFILE_SUCCESS &&
				    atomic_save_commit(fp, global_path, tmp_path, true))
					any_ok = true;
				else
					atomic_save_abort(fp, tmp_path);
			}
			gsave_free(gs_g);
		}
	}

	if (collection_path && collection_path[0]) {
		struct gsave *gs_c = gsave_read(collection_path, &serr);
		if (gs_c) {
			struct gsave_global *g_ct = gs_find_global(gs_c, "g_completedTrophy", NULL);
			int32_t ct_rec = g_ct ? g_ct->value : -1;
			cJSON *j_lar = cJSON_GetObjectItem(mod, "leaderAtkRatio");
			if (j_lar && cJSON_IsNumber(j_lar)) {
				union { float f; int32_t i; } u;
				u.f = (float)j_lar->valuedouble;
				gs_set_field_int(gs_c, ct_rec, "<LeaderAtkRatio>", NULL, u.i);
			}
			int32_t cs_rec = gs_get_field_int(gs_c, ct_rec, "m_status", NULL, -1);
			json_write_int_if_present(mod, "trophyExpBonus", gs_c, cs_rec, "m_PerExpUp", NULL);
			json_write_int_if_present(mod, "trophyStatusMaxPer", gs_c, cs_rec, "StatusMaxPer", NULL);
			char *tmp_path = NULL;
			FILE *fp = atomic_save_open(collection_path, "wb", &tmp_path);
			if (fp) {
				if (gsave_write(gs_c, fp, false, 1) == SAVEFILE_SUCCESS &&
				    atomic_save_commit(fp, collection_path, tmp_path, true))
					any_ok = true;
				else
					atomic_save_abort(fp, tmp_path);
			}
			gsave_free(gs_c);
		}
	}

	cJSON_Delete(mod);
	return any_ok;
}

static int read_game_config_streamer_mode(const char *config_path)
{
	if (!config_path || !config_path[0] || !file_exists(config_path)) return -1;
	bool prev_gbk = sys4_text_is_gbk;
	if (!ain) sys4_set_text_gbk(true);
	enum savefile_error serr;
	struct gsave *gs = gsave_read(config_path, &serr);
	if (!gs) {
		if (!ain) sys4_set_text_gbk(prev_gbk);
		return -1;
	}
	struct gsave_global *g_cfg = gs_find_global(gs, "g_gameConfig", NULL);
	int32_t cfg_rec = g_cfg ? g_cfg->value : -1;
	int32_t val = gs_get_field_int(gs, cfg_rec, "m_isSummaryMode", NULL, -1);
	gsave_free(gs);
	if (!ain) sys4_set_text_gbk(prev_gbk);
	if (val < 0) return -1;
	return val != 0 ? 1 : 0;
}

#endif /* __ANDROID__ */

/* =========================================================================
 * Live In-Game VM Memory Helpers (Executed on VM Thread in modifier_on_frame)
 * ========================================================================= */

static bool vm_globals_ready(void)
{
	return ain != NULL && heap != NULL && heap_size > 0 &&
	       heap[0].ref > 0 && heap[0].page != NULL &&
	       heap[0].page->type == GLOBAL_PAGE;
}

static bool vm_is_ready(void)
{
	return vm_globals_ready() && call_stack_ptr > 0;
}

static struct page *vm_safe_get_page(int slot)
{
	if (slot <= 0 || !heap || !page_index_valid(slot))
		return NULL;
	return heap[slot].page;
}

static int vm_find_global_idx(const char *name1, const char *name2)
{
	if (!vm_globals_ready() || !ain->globals) return -1;
	for (int i = 0; i < ain->nr_globals; i++) {
		const char *n1 = ain->globals[i].name;
		const char *n2 = ain->globals[i].name2;
		if (name1 && ((n1 && !strcmp(n1, name1)) || (n2 && !strcmp(n2, name1)))) return i;
		if (name2 && ((n1 && !strcmp(n1, name2)) || (n2 && !strcmp(n2, name2)))) return i;
	}
	return -1;
}

static struct page *vm_get_global_page(const char *name1, const char *name2)
{
	int idx = vm_find_global_idx(name1, name2);
	if (idx < 0 || idx >= heap[0].page->nr_vars) return NULL;
	union vm_value v = global_get(idx);
	return vm_safe_get_page(v.i);
}

static union vm_value *vm_get_struct_member(struct page *page, const char *name1, const char *name2)
{
	if (!ain || !page || page->type != STRUCT_PAGE) return NULL;
	if (page->index < 0 || page->index >= ain->nr_structures) return NULL;
	struct ain_struct *st = &ain->structures[page->index];
	for (int i = 0; i < st->nr_members && i < page->nr_vars; i++) {
		const char *n1 = st->members[i].name;
		const char *n2 = st->members[i].name2;
		if (name1 && ((n1 && !strcmp(n1, name1)) || (n2 && n2[0] && !strcmp(n2, name1))))
			return &page->values[i];
		if (name2 && ((n1 && !strcmp(n1, name2)) || (n2 && n2[0] && !strcmp(n2, name2))))
			return &page->values[i];
	}
	return NULL;
}

static int32_t vm_get_field_int(struct page *page, const char *name1, const char *name2, int32_t def_val)
{
	union vm_value *v = vm_get_struct_member(page, name1, name2);
	return v ? v->i : def_val;
}

static void vm_set_field_int(struct page *page, const char *name1, const char *name2, int32_t val)
{
	union vm_value *v = vm_get_struct_member(page, name1, name2);
	if (v) v->i = val;
}

static struct page *vm_get_field_page(struct page *page, const char *name1, const char *name2)
{
	union vm_value *v = vm_get_struct_member(page, name1, name2);
	if (!v) return NULL;
	return vm_safe_get_page(v->i);
}

static void vm_set_string_slot(int slot, const char *cstr)
{
	if (!cstr || slot <= 0 || !string_index_valid(slot)) return;
	struct string *s = make_string(cstr, strlen(cstr));
	heap_string_assign(slot, s);
	free_string(s);
}

static void vm_set_player_info_name(struct page *gc, const char *gbk_str)
{
	if (!gc || !gbk_str) return;
	struct page *c2p = vm_get_field_page(gc, "PlayerInfo", "m_chapter2PlayerInfo");
	if (!c2p) c2p = vm_get_field_page(gc, "<Chapter2PlayerInfo>", NULL);
	if (c2p && c2p->type == STRUCT_PAGE) {
		union vm_value *nv = vm_get_struct_member(c2p, "m_playerName", "<Name>");
		if (!nv) nv = vm_get_struct_member(c2p, "m_name", NULL);
		if (!nv && c2p->nr_vars > 0) nv = &c2p->values[0];
		if (nv) vm_set_string_slot(nv->i, gbk_str);
	}
}

static int vm_clear_array_slot(int slot)
{
	struct page *old = vm_safe_get_page(slot);
	if (!old || old->type != ARRAY_PAGE || old->nr_vars <= 0)
		return 0;
	int cleared = old->nr_vars;
	struct page *empty = NULL;
	if (ain && ain->version >= 11) {
		empty = alloc_page(ARRAY_PAGE, old->a_type, 0);
		empty->array = old->array;
	}
	for (int i = 0; i < old->nr_vars; i++) {
		variable_fini(old->values[i], variable_type(old, i, NULL, NULL), false);
	}
	free_page(old);
	heap_set_page(slot, empty);
	return cleared;
}

static int vm_clear_kvmap_field(struct page *cache_sub, const char *map_field)
{
	if (!cache_sub || cache_sub->type != STRUCT_PAGE)
		return 0;
	struct page *kvmap = vm_get_field_page(cache_sub, map_field, NULL);
	if (!kvmap || kvmap->type != STRUCT_PAGE)
		return 0;
	union vm_value *m_data = vm_get_struct_member(kvmap, "m_data", NULL);
	if (!m_data)
		return 0;
	return vm_clear_array_slot(m_data->i);
}

#define FNO_FLAG_CARD_CACHE     (1u << 0)
#define FNO_FLAG_CARD_UI        (1u << 1)
#define FNO_FLAG_GAMEPLAY_SCENE (1u << 2)

static struct ain *s_inspected_ain = NULL;
static uint8_t *s_fno_flags = NULL;
static int s_fno_flags_len = 0;
static bool s_is_mod_streamer = false;
static bool s_is_summary_gated_by_adv5 = false;

static int s_fno_run_char_setting = -1;
static int s_fno_scene_char_setting_run = -1;
static int s_fno_scene_stack_join = -1;
static int s_fno_run_info_dialog = -1;
static int s_fno_is_summary_get = -1;
static int s_fno_is_summary_set = -1;
static int s_fno_pcfv_load = -1;
static int s_fno_qmmcv_update = -1;
static int s_fno_cm_set_checkbox = -1;

static int s_struc_pcfv = -1;
static int s_struc_qmmcv = -1;
static int s_struc_common_menu = -1;
static int s_struc_scene_common_menu = -1;
static int s_struc_game_config = -1;

/* Cached hot global indices */
static int s_glob_game_config = -1;
static int s_glob_card_cache = -1;
static int s_glob_game_context = -1;
static int s_glob_pcp = -1;
static int s_glob_party_bonus = -1;
static int s_glob_party = -1;
static int s_glob_battle_ctx = -1;
static int s_glob_enemy = -1;
static int s_glob_tt = -1;
static int s_glob_diff = -1;
static int s_glob_battle_count = -1;
static int s_glob_end_busy = -1;
static int s_glob_end_busy_num = -1;

/* Cached GameConfig & CardCache member offsets */
static int s_mem_cfg_adv_shortcut = -1;
static int s_mem_cfg_summary_mode = -1;
static int s_mem_cc_front = -1;
static int s_mem_front_proc_org = -1;
static int s_card_org_mdata_slot = -1;
static int s_card_org_pair0_slot = -1;

static char *s_rcs_streamer_cheat_gbk = NULL;
static bool s_rcs_streamer_has_tt0_req = false;
static int32_t s_rcs_streamer_req_tt0 = 0;
static int s_rcs_invalid_title_idx = -1;
static int s_rcs_invalid_msg_idx = -1;

static bool s_saved_tt0_active = false;
static int32_t s_saved_tt0 = 0;
static bool s_saved_tt6655_active = false;
static int32_t s_saved_tt6655 = 0;
static bool s_rcs_popup_override_active = false;
static struct string *s_orig_invalid_title_str = NULL;
static struct string *s_orig_invalid_msg_str = NULL;
static struct string *s_override_ok_title_str = NULL;
static struct string *s_override_off_msg_str = NULL;

static int ain_find_struct_member_idx(int struc_idx, const char *name1, const char *name2)
{
	if (!ain || !ain->structures || struc_idx < 0 || struc_idx >= ain->nr_structures)
		return -1;
	struct ain_struct *st = &ain->structures[struc_idx];
	for (int i = 0; i < st->nr_members; i++) {
		const char *n1 = st->members[i].name;
		const char *n2 = st->members[i].name2;
		if (name1 && ((n1 && !strcmp(n1, name1)) || (n2 && n2[0] && !strcmp(n2, name1))))
			return i;
		if (name2 && ((n1 && !strcmp(n1, name2)) || (n2 && n2[0] && !strcmp(n2, name2))))
			return i;
	}
	return -1;
}

static inline struct page *vm_get_global_page_idx(int glob_idx)
{
	if (!vm_globals_ready() || glob_idx < 0 || glob_idx >= heap[0].page->nr_vars)
		return NULL;
	return vm_safe_get_page(global_get(glob_idx).i);
}

static inline union vm_value *vm_get_member_at(struct page *page, int mem_idx)
{
	if (!page || page->type != STRUCT_PAGE || mem_idx < 0 || mem_idx >= page->nr_vars)
		return NULL;
	return &page->values[mem_idx];
}

static inline uint16_t mod_le16(const uint8_t *buf, size_t off)
{
	return (uint16_t)buf[off] | ((uint16_t)buf[off + 1] << 8);
}

static inline int32_t mod_le32(const uint8_t *buf, size_t off)
{
	uint32_t v = (uint32_t)buf[off] |
	             ((uint32_t)buf[off + 1] << 8) |
	             ((uint32_t)buf[off + 2] << 16) |
	             ((uint32_t)buf[off + 3] << 24);
	return (int32_t)v;
}

static size_t mod_func_end_addr(int fno)
{
	if (!ain || fno < 0 || fno >= ain->nr_functions)
		return 0;
	size_t start = (size_t)ain->functions[fno].address;
	size_t best = ain->code_size;
	for (int i = 0; i < ain->nr_functions; i++) {
		if (i == fno || !ain->functions[i].name)
			continue;
		if (!strncmp(ain->functions[i].name, "<lambda", 7))
			continue;
		size_t addr = (size_t)ain->functions[i].address;
		if (addr > start && addr < best)
			best = addr;
	}
	return best;
}

static bool mod_is_streamer_cheat_str(const char *s)
{
	if (!s || !s[0])
		return false;
	return strstr(s, "Eins") != NULL ||
	       strstr(s, "eins") != NULL ||
	       strstr(s, GBK_XIEXIEMIAO) != NULL ||
	       strstr(s, GBK_ZHUBO) != NULL;
}

static bool mod_scan_tt0_req_at(size_t start_ip, size_t limit_addr, int tt_glob_idx, int32_t *out_req)
{
	if (!ain || !ain->code || start_ip >= limit_addr || limit_addr > ain->code_size)
		return false;
	size_t ip = start_ip;
	int hist_op[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	int32_t hist_arg[8] = { 0 };
	for (int steps = 0; steps < 32 && ip + 2 <= limit_addr; steps++) {
		enum opcode op = mod_le16(ain->code, ip) & ~OPTYPE_MASK;
		if (op >= NR_OPCODES)
			break;
		int32_t w = instruction_width(op);
		if (w <= 0 || ip + (size_t)w > limit_addr)
			break;
		int32_t arg0 = (instructions[op].nr_args >= 1) ? mod_le32(ain->code, ip + 2) : 0;

		if (op == RETURN || op == ENDFUNC)
			break;
		if (op == CALLFUNC && arg0 == s_fno_run_info_dialog)
			break;

		/* Match: PUSH <tt>; REF; PUSH 0; REF; PUSH <req>; (NOTE + IFNZ | EQUALE + IFZ) */
		if ((op == IFNZ && hist_op[0] == NOTE) || (op == IFZ && hist_op[0] == EQUALE)) {
			if (hist_op[1] == PUSH &&
			    hist_op[2] == REF &&
			    hist_op[3] == PUSH && hist_arg[3] == 0 &&
			    hist_op[4] == REF &&
			    hist_op[5] == PUSH && (tt_glob_idx < 0 || hist_arg[5] == tt_glob_idx)) {
				*out_req = hist_arg[1];
				return true;
			}
		}

		for (int k = 7; k > 0; k--) {
			hist_op[k] = hist_op[k - 1];
			hist_arg[k] = hist_arg[k - 1];
		}
		hist_op[0] = (int)op;
		hist_arg[0] = arg0;
		ip += (size_t)w;
	}
	return false;
}

static void mod_scan_func_for_streamer_cheat(int fno, int tt_glob_idx, int depth)
{
	if (!ain || !ain->code || fno < 0 || fno >= ain->nr_functions || depth > 1)
		return;
	size_t ip = (size_t)ain->functions[fno].address;
	size_t end_addr = mod_func_end_addr(fno);
	if (end_addr > ain->code_size)
		end_addr = ain->code_size;

	while (ip + 2 <= end_addr) {
		enum opcode op = mod_le16(ain->code, ip) & ~OPTYPE_MASK;
		if (op >= NR_OPCODES) {
			ip += 2;
			continue;
		}
		int32_t w = instruction_width(op);
		if (w <= 0 || ip + (size_t)w > end_addr)
			break;

		/* 1. Fallback invalid-cheat dialog string slots in RunCharacterSetting */
		if (depth == 0 && op == S_PUSH && ip + 18 <= end_addr) {
			int32_t s1 = mod_le32(ain->code, ip + 2);
			enum opcode op2 = mod_le16(ain->code, ip + 6) & ~OPTYPE_MASK;
			enum opcode op3 = mod_le16(ain->code, ip + 12) & ~OPTYPE_MASK;
			if (op2 == S_PUSH && op3 == CALLFUNC) {
				int32_t s2 = mod_le32(ain->code, ip + 8);
				int32_t callee = mod_le32(ain->code, ip + 14);
				if (callee == s_fno_run_info_dialog &&
				    s1 >= 0 && s1 < ain->nr_strings && ain->strings[s1] &&
				    s2 >= 0 && s2 < ain->nr_strings && ain->strings[s2] &&
				    !strcmp(ain->strings[s1]->text, GBK_CHEAT_INVALID_TITLE) &&
				    !strcmp(ain->strings[s2]->text, GBK_CHEAT_INVALID_MSG)) {
					s_rcs_invalid_title_idx = s1;
					s_rcs_invalid_msg_idx = s2;
				}
			}
		}

		/* 2. Direct S_PUSH + (S_EQUALE / S_NOTE / String.Contains) cheat check */
		if (!s_rcs_streamer_cheat_gbk && op == S_PUSH && ip + 8 <= end_addr) {
			int32_t sidx = mod_le32(ain->code, ip + 2);
			if (sidx >= 0 && sidx < ain->nr_strings && ain->strings[sidx] &&
			    mod_is_streamer_cheat_str(ain->strings[sidx]->text)) {
				size_t ip2 = ip + 6;
				enum opcode op2 = mod_le16(ain->code, ip2) & ~OPTYPE_MASK;
				if (op2 < NR_OPCODES && (op2 == S_EQUALE || op2 == S_NOTE || op2 == CALLHLL)) {
					size_t ip3 = ip2 + (size_t)instruction_width(op2);
					size_t handler_ip = 0;
					bool matched = false;
					if (ip3 + 6 <= end_addr) {
						enum opcode op3 = mod_le16(ain->code, ip3) & ~OPTYPE_MASK;
						int32_t target = mod_le32(ain->code, ip3 + 2);
						if ((op2 == S_EQUALE || op2 == CALLHLL) && op3 == IFNZ && target > 0) {
							handler_ip = (size_t)target;
							matched = true;
						} else if ((op2 == S_EQUALE || op2 == CALLHLL) && op3 == IFZ) {
							handler_ip = ip3 + 6;
							matched = true;
						} else if (op2 == S_NOTE && op3 == IFNZ) {
							handler_ip = ip3 + 6;
							matched = true;
						} else if (op2 == S_NOTE && op3 == IFZ && target > 0) {
							handler_ip = (size_t)target;
							matched = true;
						}
					}
					if (matched) {
						s_rcs_streamer_cheat_gbk = strdup(ain->strings[sidx]->text);
						int32_t req = 0;
						if (handler_ip > 0 && mod_scan_tt0_req_at(handler_ip, end_addr, tt_glob_idx, &req)) {
							s_rcs_streamer_has_tt0_req = true;
							s_rcs_streamer_req_tt0 = req;
						}
					}
				}
			}
		}

		/* 3. String SWITCH / STRSWITCH cheat dispatch */
		if (!s_rcs_streamer_cheat_gbk && (op == SWITCH || op == STRSWITCH)) {
			int32_t sw_idx = mod_le32(ain->code, ip + 2);
			if (sw_idx >= 0 && sw_idx < ain->nr_switches) {
				struct ain_switch *sw = &ain->switches[sw_idx];
				if (sw->case_type == AIN_SWITCH_STRING && sw->cases) {
					for (int c = 0; c < sw->nr_cases; c++) {
						int32_t sidx = sw->cases[c].value;
						if (sidx >= 0 && sidx < ain->nr_strings && ain->strings[sidx] &&
						    mod_is_streamer_cheat_str(ain->strings[sidx]->text)) {
							s_rcs_streamer_cheat_gbk = strdup(ain->strings[sidx]->text);
							int32_t req = 0;
							if (sw->cases[c].address > 0 &&
							    mod_scan_tt0_req_at((size_t)sw->cases[c].address, end_addr, tt_glob_idx, &req)) {
								s_rcs_streamer_has_tt0_req = true;
								s_rcs_streamer_req_tt0 = req;
							}
							break;
						}
					}
				}
			}
		}

		/* 4. 1-level helper CALLFUNC inside RunCharacterSetting */
		if (!s_rcs_streamer_cheat_gbk && depth == 0 && op == CALLFUNC) {
			int32_t callee = mod_le32(ain->code, ip + 2);
			if (callee >= 0 && callee < ain->nr_functions &&
			    callee != fno && callee != s_fno_run_info_dialog &&
			    ain->functions[callee].name &&
			    strncmp(ain->functions[callee].name, "Scenes::RunIScene", 17) != 0) {
				mod_scan_func_for_streamer_cheat(callee, tt_glob_idx, depth + 1);
			}
		}

		if (op == ENDFUNC && mod_le32(ain->code, ip + 2) == fno)
			break;
		ip += (size_t)w;
	}
}

static bool mod_func_reads_struct_member(int fno, int mem_idx)
{
	if (!ain || !ain->code || fno < 0 || fno >= ain->nr_functions || mem_idx < 0)
		return false;
	size_t ip = (size_t)ain->functions[fno].address;
	size_t end_addr = mod_func_end_addr(fno);
	if (end_addr > ain->code_size)
		end_addr = ain->code_size;
	bool prev_struct_page = false;
	while (ip + 2 <= end_addr) {
		enum opcode op = mod_le16(ain->code, ip) & ~OPTYPE_MASK;
		if (op >= NR_OPCODES) {
			prev_struct_page = false;
			ip += 2;
			continue;
		}
		int32_t w = instruction_width(op);
		if (w <= 0 || ip + (size_t)w > end_addr)
			break;
		if (prev_struct_page && op == PUSH && mod_le32(ain->code, ip + 2) == mem_idx)
			return true;
		if (op == SH_STRUCTREF && mod_le32(ain->code, ip + 2) == mem_idx)
			return true;
		prev_struct_page = (op == PUSHSTRUCTPAGE);
		if (op == ENDFUNC && mod_le32(ain->code, ip + 2) == fno)
			break;
		ip += (size_t)w;
	}
	return false;
}

static void vm_inspect_mod_streamer_ain(void)
{
	if (!ain)
		return;
	if (s_inspected_ain == ain)
		return;

	s_inspected_ain = ain;
	s_game_config_ready = false;
	s_last_observed_summary_mode = -1;
	s_card_org_mdata_slot = -1;
	s_card_org_pair0_slot = -1;

	s_is_mod_streamer = false;
	s_is_summary_gated_by_adv5 = false;
	s_fno_run_char_setting = -1;
	s_fno_scene_char_setting_run = -1;
	s_fno_scene_stack_join = -1;
	s_fno_run_info_dialog = -1;
	s_fno_is_summary_get = -1;
	s_fno_is_summary_set = -1;
	s_fno_pcfv_load = -1;
	s_fno_qmmcv_update = -1;
	s_fno_cm_set_checkbox = -1;

	s_struc_pcfv = -1;
	s_struc_qmmcv = -1;
	s_struc_common_menu = -1;
	s_struc_scene_common_menu = -1;
	s_struc_game_config = -1;

	s_glob_game_config = -1;
	s_glob_card_cache = -1;
	s_glob_game_context = -1;
	s_glob_pcp = -1;
	s_glob_party_bonus = -1;
	s_glob_party = -1;
	s_glob_battle_ctx = -1;
	s_glob_enemy = -1;
	s_glob_tt = -1;
	s_glob_diff = -1;
	s_glob_battle_count = -1;
	s_glob_end_busy = -1;
	s_glob_end_busy_num = -1;

	s_mem_cfg_adv_shortcut = -1;
	s_mem_cfg_summary_mode = -1;
	s_mem_cc_front = -1;
	s_mem_front_proc_org = -1;

	free(s_rcs_streamer_cheat_gbk);
	s_rcs_streamer_cheat_gbk = NULL;
	s_rcs_streamer_has_tt0_req = false;
	s_rcs_streamer_req_tt0 = 0;
	s_rcs_invalid_title_idx = -1;
	s_rcs_invalid_msg_idx = -1;

	free(s_fno_flags);
	s_fno_flags = NULL;
	s_fno_flags_len = 0;

	if (ain->strings) {
		for (int i = 0; i < ain->nr_strings; i++) {
			if (!ain->strings[i])
				continue;
			const char *t = ain->strings[i]->text;
			if (strstr(t, GBK_CARD_STREAMER) ||
			    strstr(t, GBK_STREAMER_MODE) ||
			    strstr(t, "Eins")) {
				s_is_mod_streamer = true;
				break;
			}
		}
	}

	int struc_card_cache = -1;
	int struc_cc_front = -1;
	if (ain->structures) {
		for (int i = 0; i < ain->nr_structures; i++) {
			const char *sn = ain->structures[i].name;
			if (!sn)
				continue;
			if (!strcmp(sn, "PlayerCardFlipView"))
				s_struc_pcfv = i;
			else if (!strcmp(sn, "QuestMapMiniCardView"))
				s_struc_qmmcv = i;
			else if (!strcmp(sn, "CommonMenu"))
				s_struc_common_menu = i;
			else if (!strcmp(sn, "SceneCommonMenu"))
				s_struc_scene_common_menu = i;
			else if (!strcmp(sn, "GameConfig"))
				s_struc_game_config = i;
			else if (!strcmp(sn, "CardCache"))
				struc_card_cache = i;
			else if (!strcmp(sn, "CardConstructProcessCacheFront"))
				struc_cc_front = i;
		}
	}

	if (s_struc_game_config >= 0) {
		s_mem_cfg_adv_shortcut = ain_find_struct_member_idx(s_struc_game_config, "m_isShowAdvShortcut", NULL);
		s_mem_cfg_summary_mode = ain_find_struct_member_idx(s_struc_game_config, "m_isSummaryMode", NULL);
	}
	if (struc_card_cache >= 0)
		s_mem_cc_front = ain_find_struct_member_idx(struc_card_cache, "Front", NULL);
	if (struc_cc_front >= 0)
		s_mem_front_proc_org = ain_find_struct_member_idx(struc_cc_front, "m_procOrg", NULL);

	if (ain->globals) {
		for (int i = 0; i < ain->nr_globals; i++) {
			const char *gn = ain->globals[i].name;
			const char *gn2 = ain->globals[i].name2;
			if (!gn)
				continue;
			if (!strcmp(gn, "g_gameConfig"))
				s_glob_game_config = i;
			else if (!strcmp(gn, "g_cardCache"))
				s_glob_card_cache = i;
			else if (!strcmp(gn, "g_gameContext"))
				s_glob_game_context = i;
			else if (!strcmp(gn, "g_playerCommonParam"))
				s_glob_pcp = i;
			else if (!strcmp(gn, "g_partyBonus"))
				s_glob_party_bonus = i;
			else if (!strcmp(gn, "g_party"))
				s_glob_party = i;
			else if (!strcmp(gn, "g_battleContext"))
				s_glob_battle_ctx = i;
			else if (!strcmp(gn, "g_enemy"))
				s_glob_enemy = i;
			else if (!strcmp(gn, "tt"))
				s_glob_tt = i;
			else if (!strcmp(gn, GBK_DIFFICULTY) || !strcmp(gn, SJIS_DIFFICULTY) ||
			         (gn2 && (!strcmp(gn2, GBK_DIFFICULTY) || !strcmp(gn2, SJIS_DIFFICULTY))))
				s_glob_diff = i;
			else if (!strcmp(gn, GBK_BATTLE_COUNT) || !strcmp(gn, SJIS_BATTLE_COUNT) ||
			         (gn2 && (!strcmp(gn2, GBK_BATTLE_COUNT) || !strcmp(gn2, SJIS_BATTLE_COUNT))))
				s_glob_battle_count = i;
			else if (!strcmp(gn, "parts::detail::g_EndPartsBusyLoop"))
				s_glob_end_busy = i;
			else if (!strcmp(gn, "parts::detail::g_EndPartsBusyLoopNumber"))
				s_glob_end_busy_num = i;
		}
	}

	if (ain->functions && ain->nr_functions > 0) {
		s_fno_flags = xcalloc((size_t)ain->nr_functions, sizeof(uint8_t));
		s_fno_flags_len = ain->nr_functions;
		for (int i = 0; i < ain->nr_functions; i++) {
			const char *fn = ain->functions[i].name;
			if (!fn)
				continue;
			uint8_t fl = 0;
			if (!strncmp(fn, "CardCache@", 10) ||
			    !strncmp(fn, "CardConstruct", 13) ||
			    !strncmp(fn, "KeyValueMap<", 12)) {
				fl |= FNO_FLAG_CARD_CACHE | FNO_FLAG_CARD_UI;
			}
			if (!strncmp(fn, "PlayerCardFlipView", 18) ||
			    !strncmp(fn, "QuestMapMiniCardView", 20) ||
			    !strncmp(fn, "PartyCard@", 10) ||
			    !strncmp(fn, "FormationView@", 14) ||
			    !strncmp(fn, "CommonMenu@", 11) ||
			    !strncmp(fn, "SceneCharacterSetting@", 22)) {
				fl |= FNO_FLAG_CARD_UI;
			}
			if (!strncmp(fn, "SceneTitle@", 11) ||
			    !strncmp(fn, "SceneMap@", 9) ||
			    !strncmp(fn, "SceneQuestMap@", 14) ||
			    !strncmp(fn, "SceneAdv@", 9) ||
			    !strncmp(fn, "SceneParty@", 11) ||
			    !strncmp(fn, "SceneBattle@", 12) ||
			    !strncmp(fn, "SceneCommonMenu@", 16) ||
			    !strncmp(fn, "CommonMenu@", 11)) {
				fl |= FNO_FLAG_GAMEPLAY_SCENE;
			}

			if (!strcmp(fn, "Scenes::RunCharacterSetting")) {
				s_fno_run_char_setting = i;
				fl |= FNO_FLAG_CARD_UI;
			} else if (!strcmp(fn, "SceneCharacterSetting@Run")) {
				s_fno_scene_char_setting_run = i;
			} else if (!strcmp(fn, "SceneStack@Join")) {
				s_fno_scene_stack_join = i;
			} else if (!strcmp(fn, "DialogScenes::RunInformationDialog")) {
				s_fno_run_info_dialog = i;
			} else if (!strcmp(fn, "GameConfig@IsSummaryMode::get") &&
			           s_fno_is_summary_get < 0 &&
			           ain->functions[i].nr_args == 0 &&
			           ain->functions[i].return_type.data != AIN_VOID) {
				s_fno_is_summary_get = i;
			} else if (!strcmp(fn, "GameConfig@IsSummaryMode::set") &&
			           s_fno_is_summary_set < 0 &&
			           ain->functions[i].nr_args == 1 &&
			           ain->functions[i].return_type.data == AIN_VOID) {
				s_fno_is_summary_set = i;
			} else if (!strcmp(fn, "PlayerCardFlipView@Load") &&
			           ain->functions[i].nr_args == 0 &&
			           ain->functions[i].return_type.data == AIN_VOID) {
				s_fno_pcfv_load = i;
			} else if (!strcmp(fn, "QuestMapMiniCardView@UpdateCard") &&
			           ain->functions[i].nr_args == 0 &&
			           ain->functions[i].return_type.data == AIN_VOID) {
				s_fno_qmmcv_update = i;
			} else if (!strcmp(fn, "CommonMenu@SetCheckBoxState") &&
			           ain->functions[i].nr_args == 0 &&
			           ain->functions[i].return_type.data == AIN_VOID) {
				s_fno_cm_set_checkbox = i;
			}
			s_fno_flags[i] = fl;
		}
	}

	if (s_fno_is_summary_get >= 0 && s_mem_cfg_adv_shortcut >= 0) {
		s_is_summary_gated_by_adv5 = mod_func_reads_struct_member(s_fno_is_summary_get, s_mem_cfg_adv_shortcut);
	}

	if (s_fno_run_char_setting >= 0) {
		mod_scan_func_for_streamer_cheat(s_fno_run_char_setting, s_glob_tt, 0);
	}
}

static inline bool vm_has_mod_streamer_mode(void)
{
	if (s_inspected_ain != ain)
		vm_inspect_mod_streamer_ain();
	return s_is_mod_streamer;
}

static void vm_activate_rcs_popup_override(void)
{
	vm_inspect_mod_streamer_ain();
	if (s_rcs_popup_override_active || !ain || !ain->strings)
		return;
	if (s_rcs_invalid_title_idx < 0 || s_rcs_invalid_title_idx >= ain->nr_strings ||
	    s_rcs_invalid_msg_idx < 0 || s_rcs_invalid_msg_idx >= ain->nr_strings)
		return;
	if (!s_override_ok_title_str)
		s_override_ok_title_str = make_string(GBK_CHEAT_OK_TITLE, strlen(GBK_CHEAT_OK_TITLE));
	if (!s_override_off_msg_str)
		s_override_off_msg_str = make_string(GBK_STREAMER_OFF_MSG, strlen(GBK_STREAMER_OFF_MSG));
	s_orig_invalid_title_str = ain->strings[s_rcs_invalid_title_idx];
	s_orig_invalid_msg_str = ain->strings[s_rcs_invalid_msg_idx];
	ain->strings[s_rcs_invalid_title_idx] = s_override_ok_title_str;
	ain->strings[s_rcs_invalid_msg_idx] = s_override_off_msg_str;
	s_rcs_popup_override_active = true;
}

static inline bool vm_callstack_has_fno(int target_fno)
{
	if (target_fno < 0 || call_stack_ptr <= 0)
		return false;
	for (int i = 0; i < call_stack_ptr; i++) {
		if (call_stack[i].fno == target_fno)
			return true;
	}
	return false;
}

static inline bool vm_callstack_any_flag(uint8_t flag_mask)
{
	if (!s_fno_flags || call_stack_ptr <= 0)
		return false;
	for (int i = 0; i < call_stack_ptr; i++) {
		int32_t fno = call_stack[i].fno;
		if (fno >= 0 && fno < s_fno_flags_len && (s_fno_flags[fno] & flag_mask))
			return true;
	}
	return false;
}

static bool vm_is_common_menu_on_callstack(void)
{
	if (call_stack_ptr <= 0 || (s_struc_common_menu < 0 && s_struc_scene_common_menu < 0))
		return false;
	for (int i = call_stack_ptr - 1; i >= 0; i--) {
		struct page *sp = vm_safe_get_page(call_stack[i].struct_page);
		if (!sp || sp->type != STRUCT_PAGE)
			continue;
		if (sp->index == s_struc_common_menu || sp->index == s_struc_scene_common_menu)
			return true;
	}
	return false;
}

static int32_t vm_get_tt(int idx, int32_t def_val)
{
	vm_inspect_mod_streamer_ain();
	struct page *tt = s_glob_tt >= 0 ? vm_get_global_page_idx(s_glob_tt) : vm_get_global_page("tt", NULL);
	if (!tt || tt->type != ARRAY_PAGE || idx < 0 || idx >= tt->nr_vars) return def_val;
	return tt->values[idx].i;
}

static void vm_set_tt(int idx, int32_t val)
{
	vm_inspect_mod_streamer_ain();
	struct page *tt = s_glob_tt >= 0 ? vm_get_global_page_idx(s_glob_tt) : vm_get_global_page("tt", NULL);
	if (!tt || tt->type != ARRAY_PAGE || idx < 0 || idx >= tt->nr_vars) return;
	tt->values[idx].i = val;
}

static void vm_restore_temp_rcs_overrides(bool force)
{
	if (!s_rcs_popup_override_active && !s_saved_tt0_active && !s_saved_tt6655_active)
		return;
	vm_inspect_mod_streamer_ain();
	bool in_rcs = vm_callstack_has_fno(s_fno_run_char_setting) ||
	              vm_callstack_has_fno(s_fno_scene_char_setting_run);
	bool in_info = vm_callstack_has_fno(s_fno_run_info_dialog);

	if (s_rcs_popup_override_active && ain && ain->strings) {
		if (force || in_info || !in_rcs) {
			if (s_rcs_invalid_title_idx >= 0 && s_rcs_invalid_title_idx < ain->nr_strings && s_orig_invalid_title_str)
				ain->strings[s_rcs_invalid_title_idx] = s_orig_invalid_title_str;
			if (s_rcs_invalid_msg_idx >= 0 && s_rcs_invalid_msg_idx < ain->nr_strings && s_orig_invalid_msg_str)
				ain->strings[s_rcs_invalid_msg_idx] = s_orig_invalid_msg_str;
			s_rcs_popup_override_active = false;
		}
	}
	if (s_saved_tt0_active) {
		if (force || in_info || !in_rcs) {
			vm_set_tt(0, s_saved_tt0);
			s_saved_tt0_active = false;
		}
	}
	if (s_saved_tt6655_active) {
		/* MOD 0.49 assigns tt[6655] = -99999 AFTER RunInformationDialog returns */
		if (force || !in_rcs) {
			vm_set_tt(6655, s_saved_tt6655);
			s_saved_tt6655_active = false;
		}
	}
}

static inline bool vm_is_in_card_cache_callstack(void)
{
	vm_inspect_mod_streamer_ain();
	return vm_callstack_any_flag(FNO_FLAG_CARD_CACHE);
}

static inline bool vm_is_in_card_ui_callstack(void)
{
	vm_inspect_mod_streamer_ain();
	return vm_callstack_any_flag(FNO_FLAG_CARD_CACHE | FNO_FLAG_CARD_UI);
}

/*
 * g_cardCache (global[286], group = -1) caches constructed CAS process arrays
 * across the entire process lifetime. Only clear the 5 lazily-populated
 * per-card KeyValueMap<string, array<CASConstructionProcess>> tables that
 * depend on m_isSummaryMode ("卡牌／主播" / "卡牌／物品／主播").
 * NEVER clear Front.m_procOrg: it is populated once in
 * CardConstructProcessCacheFront@0 for org IDs 1..10 and is never recreated!
 */
static int vm_clear_card_cache(void)
{
	vm_inspect_mod_streamer_ain();
	struct page *cc = s_glob_card_cache >= 0
		? vm_get_global_page_idx(s_glob_card_cache)
		: vm_get_global_page("g_cardCache", NULL);
	if (!cc || cc->type != STRUCT_PAGE)
		return 0;
	if (vm_is_in_card_cache_callstack()) {
		s_pending_clear_card_cache = true;
		return 0;
	}
	s_pending_clear_card_cache = false;
	int total = 0;
	struct page *front = vm_get_field_page(cc, "Front", NULL);
	total += vm_clear_kvmap_field(front, "m_procCardBasic");
	total += vm_clear_kvmap_field(vm_get_field_page(cc, "FrontItem", NULL), "m_proc");
	total += vm_clear_kvmap_field(vm_get_field_page(cc, "BackSkill", NULL), "m_proc");
	total += vm_clear_kvmap_field(vm_get_field_page(cc, "BackCard", NULL), "m_proc");
	total += vm_clear_kvmap_field(vm_get_field_page(cc, "BackCardListInfo", NULL), "m_proc");
	return total;
}

static bool vm_is_live_pcfv_page(struct page *p)
{
	if (!p || p->type != STRUCT_PAGE || p->index != s_struc_pcfv)
		return false;
	struct page *pc = vm_get_field_page(p, "m_playerCard", NULL);
	if (!pc || pc->type != STRUCT_PAGE)
		return false;
	struct page *par = vm_get_field_page(p, "m_parent", NULL);
	if (!par || par->type != STRUCT_PAGE || vm_get_field_int(par, "m_partsNumber", NULL, 0) <= 0)
		return false;
	struct page *cv = vm_get_field_page(p, "m_cardView", NULL);
	if (!cv || cv->type != ARRAY_PAGE || cv->nr_vars <= 0)
		return false;
	struct page *cv0 = vm_safe_get_page(cv->values[0].i);
	if (!cv0 || cv0->type != STRUCT_PAGE || vm_get_field_int(cv0, "m_partsNumber", NULL, 0) <= 0)
		return false;
	return true;
}

static bool vm_is_live_qmmcv_page(struct page *p)
{
	if (!p || p->type != STRUCT_PAGE || p->index != s_struc_qmmcv)
		return false;
	struct page *par = vm_get_field_page(p, "m_parent", NULL);
	if (!par || par->type != STRUCT_PAGE || vm_get_field_int(par, "m_partsNumber", NULL, 0) <= 0)
		return false;
	struct page *cids = vm_get_field_page(p, "m_cardId", NULL);
	if (!cids || cids->type != ARRAY_PAGE || cids->nr_vars <= 0)
		return false;
	return true;
}

/*
 * Rebuilds already-instantiated card views (SceneParty's PlayerCardFlipView and
 * QuestMap's QuestMapMiniCardView) and CommonMenu's SummaryCheckBox in-place
 * after Streamer Mode changes, so white-curtain cards reload immediately
 * without requiring the player to leave the current screen.
 */
static void vm_refresh_live_streamer_ui(void)
{
	if (!vm_is_ready())
		return;
	vm_inspect_mod_streamer_ain();
	if (vm_is_in_card_ui_callstack()) {
		s_pending_live_ui_refresh = true;
		return;
	}
	s_pending_live_ui_refresh = false;
	if (s_pending_clear_card_cache)
		vm_clear_card_cache();

	bool saved_dg = dg_callbegin_pending;
	dg_callbegin_pending = false;

	if (s_fno_pcfv_load >= 0 && s_struc_pcfv >= 0) {
		size_t cap = 64, nr = 0;
		int *slots = malloc(cap * sizeof(int));
		if (slots) {
			for (size_t s = 1; s < heap_size; s++) {
				if (heap[s].ref > 0 && vm_is_live_pcfv_page(heap[s].page)) {
					if (nr == cap) {
						cap *= 2;
						int *ns = realloc(slots, cap * sizeof(int));
						if (!ns) break;
						slots = ns;
					}
					slots[nr++] = (int)s;
				}
			}
			for (size_t i = 0; i < nr; i++) {
				int slot = slots[i];
				if (page_index_valid(slot) && heap[slot].ref > 0 &&
				    vm_is_live_pcfv_page(heap[slot].page)) {
					vm_call(s_fno_pcfv_load, slot);
				}
			}
			free(slots);
		}
	}

	if (s_fno_qmmcv_update >= 0 && s_struc_qmmcv >= 0) {
		size_t cap = 16, nr = 0;
		int *slots = malloc(cap * sizeof(int));
		if (slots) {
			for (size_t s = 1; s < heap_size; s++) {
				if (heap[s].ref > 0 && vm_is_live_qmmcv_page(heap[s].page)) {
					if (nr == cap) {
						cap *= 2;
						int *ns = realloc(slots, cap * sizeof(int));
						if (!ns) break;
						slots = ns;
					}
					slots[nr++] = (int)s;
				}
			}
			for (size_t i = 0; i < nr; i++) {
				int slot = slots[i];
				if (page_index_valid(slot) && heap[slot].ref > 0 &&
				    vm_is_live_qmmcv_page(heap[slot].page)) {
					vm_call(s_fno_qmmcv_update, slot);
				}
			}
			free(slots);
		}
	}

	if (s_fno_cm_set_checkbox >= 0 && s_struc_common_menu >= 0) {
		int cm_slot = -1;
		for (int i = call_stack_ptr - 1; i >= 0; i--) {
			struct page *sp = vm_safe_get_page(call_stack[i].struct_page);
			if (!sp || sp->type != STRUCT_PAGE)
				continue;
			if (sp->index == s_struc_common_menu) {
				cm_slot = call_stack[i].struct_page;
				break;
			}
			if (sp->index == s_struc_scene_common_menu) {
				union vm_value *mv = vm_get_struct_member(sp, "m_menu", NULL);
				if (mv && vm_safe_get_page(mv->i)) {
					cm_slot = mv->i;
					break;
				}
			}
		}
		struct page *cm_page = vm_safe_get_page(cm_slot);
		if (cm_page && cm_page->type == STRUCT_PAGE &&
		    cm_page->index == s_struc_common_menu &&
		    vm_get_field_page(cm_page, "m_act", NULL) != NULL) {
			vm_call(s_fno_cm_set_checkbox, cm_slot);
		}
	}

	dg_callbegin_pending = saved_dg;
}

/*
 * MOD 0.49+ gates GameConfig@IsSummaryMode::get and IsSummaryMode::set behind
 * m_isShowAdvShortcut[5] == true (the permanent unlock flag set by
 * SetEnableAdvShortCut(5, true) in "灌注Eins陈谢谢喵"). Without it,
 * IsSummaryMode::get always returns true AND IsSummaryMode::set(false) forces
 * m_isSummaryMode back to true and saves GameConfig.asd!
 */
static bool vm_ensure_streamer_unlock_gate(struct page *cfg)
{
	if (!cfg || cfg->type != STRUCT_PAGE)
		return false;
	union vm_value *adv_mem = (s_mem_cfg_adv_shortcut >= 0)
		? vm_get_member_at(cfg, s_mem_cfg_adv_shortcut)
		: vm_get_struct_member(cfg, "m_isShowAdvShortcut", NULL);
	if (!adv_mem)
		return false;

	bool changed = false;
	int adv_slot = adv_mem->i;
	if (adv_slot <= 0 || !page_index_valid(adv_slot)) {
		adv_slot = heap_alloc_slot(VM_PAGE);
		adv_mem->i = adv_slot;
		union vm_value dims = { .i = 7 };
		struct page *arr = alloc_array(1, &dims, AIN_ARRAY_BOOL, -1, false);
		heap_set_page(adv_slot, arr);
		if (arr && arr->nr_vars > 5)
			arr->values[5].i = 1;
		return true;
	}

	struct page *arr = heap[adv_slot].page;
	if (!arr || arr->type != ARRAY_PAGE || arr->nr_vars < 6) {
		union vm_value dims = { .i = 7 };
		arr = realloc_array(arr, 1, &dims, AIN_ARRAY_BOOL, -1, false);
		heap_set_page(adv_slot, arr);
		changed = true;
	}
	if (arr && arr->type == ARRAY_PAGE && arr->nr_vars > 5 && arr->values[5].i != 1) {
		arr->values[5].i = 1;
		changed = true;
	}
	return changed;
}

static bool vm_persist_game_config_asd(void)
{
	if (!vm_globals_ready() || !config.save_dir || !config.save_dir[0])
		return false;
	vm_inspect_mod_streamer_ain();
	int cfg_idx = s_glob_game_config >= 0 ? s_glob_game_config : vm_find_global_idx("g_gameConfig", NULL);
	if (cfg_idx < 0 || cfg_idx >= ain->nr_globals)
		return false;
	int gidx = ain->globals[cfg_idx].group_index;
	if (gidx >= 0 && gidx < ain->nr_global_groups &&
	    ain->global_group_names && ain->global_group_names[gidx] && ain->global_group_names[gidx][0]) {
		return save_globals("rance10", "GameConfig.asd", ain->global_group_names[gidx], NULL) != 0;
	}
	return false;
}

static int vm_get_live_streamer_mode(bool allow_vm_call)
{
	if (!vm_globals_ready() || !vm_has_mod_streamer_mode())
		return -1;
	int cfg_idx = s_glob_game_config >= 0 ? s_glob_game_config : vm_find_global_idx("g_gameConfig", NULL);
	if (cfg_idx < 0 || cfg_idx >= heap[0].page->nr_vars)
		return -1;
	int cfg_slot = global_get(cfg_idx).i;
	struct page *cfg = vm_safe_get_page(cfg_slot);
	if (!cfg || cfg->type != STRUCT_PAGE)
		return -1;
	if (allow_vm_call && call_stack_ptr > 0 && s_fno_is_summary_get >= 0) {
		bool saved_dg = dg_callbegin_pending;
		dg_callbegin_pending = false;
		vm_call(s_fno_is_summary_get, cfg_slot);
		int res = stack_pop().i;
		dg_callbegin_pending = saved_dg;
		return res != 0 ? 1 : 0;
	}
	if (s_is_summary_gated_by_adv5) {
		union vm_value *adv_mem = s_mem_cfg_adv_shortcut >= 0
			? vm_get_member_at(cfg, s_mem_cfg_adv_shortcut)
			: vm_get_struct_member(cfg, "m_isShowAdvShortcut", NULL);
		struct page *adv_arr = adv_mem ? vm_safe_get_page(adv_mem->i) : NULL;
		if (!adv_arr || adv_arr->type != ARRAY_PAGE || adv_arr->nr_vars < 6 || adv_arr->values[5].i == 0)
			return 1;
	}
	union vm_value *sm_mem = s_mem_cfg_summary_mode >= 0
		? vm_get_member_at(cfg, s_mem_cfg_summary_mode)
		: vm_get_struct_member(cfg, "m_isSummaryMode", NULL);
	if (!sm_mem)
		return -1;
	return sm_mem->i != 0 ? 1 : 0;
}

static void vm_set_streamer_mode(int enabled, bool persist_disk, bool force_clear_cache)
{
	if (!vm_globals_ready() || !vm_has_mod_streamer_mode())
		return;
	int target = enabled ? 1 : 0;
	int cfg_idx = s_glob_game_config >= 0 ? s_glob_game_config : vm_find_global_idx("g_gameConfig", NULL);
	int cfg_slot = (cfg_idx >= 0 && cfg_idx < heap[0].page->nr_vars) ? global_get(cfg_idx).i : -1;
	struct page *cfg = vm_safe_get_page(cfg_slot);
	if (!cfg || cfg->type != STRUCT_PAGE)
		return;

	bool changed = false;
	if (target == 0) {
		if (vm_ensure_streamer_unlock_gate(cfg))
			changed = true;
	}
	union vm_value *sm_mem = s_mem_cfg_summary_mode >= 0
		? vm_get_member_at(cfg, s_mem_cfg_summary_mode)
		: vm_get_struct_member(cfg, "m_isSummaryMode", NULL);
	if (sm_mem && sm_mem->i != target) {
		sm_mem->i = target;
		changed = true;
	}
	if (changed || force_clear_cache) {
		vm_clear_card_cache();
		s_pending_live_ui_refresh = true;
	}
	if (persist_disk) {
		if (!vm_persist_game_config_asd())
			persist_streamer_mode_to_savedir(target != 0);
	}
	s_last_observed_summary_mode = target;
}

/*
 * CardConstructProcessCacheFront@0 populates Front.m_procOrg only for
 * organization IDs 1..10. When a save contains cross-version MOD cards absent
 * from Rance10EX.ex ("卡牌数据"), PlayerCard@OrganizationId::get returns 0,
 * causing m_procOrg.Get(0) to return null (no 208x312 CreateBase canvas and
 * no card frame/star/skill base ops -> 0x0 "空卡牌"). Clone org ID 7 ("其他")
 * into key 0 once so OrganizationId == 0 cards build a full 208x312 base.
 */
static void vm_ensure_card_org_fallback_proc(void)
{
	if (!vm_globals_ready())
		return;
	vm_inspect_mod_streamer_ain();

	struct page *cc = s_glob_card_cache >= 0
		? vm_get_global_page_idx(s_glob_card_cache)
		: vm_get_global_page("g_cardCache", NULL);
	if (!cc || cc->type != STRUCT_PAGE)
		return;
	union vm_value *front_v = s_mem_cc_front >= 0
		? vm_get_member_at(cc, s_mem_cc_front)
		: vm_get_struct_member(cc, "Front", NULL);
	struct page *front = front_v ? vm_safe_get_page(front_v->i) : NULL;
	if (!front || front->type != STRUCT_PAGE)
		return;
	union vm_value *po_v = s_mem_front_proc_org >= 0
		? vm_get_member_at(front, s_mem_front_proc_org)
		: vm_get_struct_member(front, "m_procOrg", NULL);
	struct page *proc_org = po_v ? vm_safe_get_page(po_v->i) : NULL;
	if (!proc_org || proc_org->type != STRUCT_PAGE || proc_org->nr_vars <= 0)
		return;

	/* Fast O(1) verification once key 0 has already been injected */
	if (s_card_org_mdata_slot > 0 && s_card_org_pair0_slot > 0 &&
	    proc_org->values[0].i == s_card_org_mdata_slot) {
		struct page *md_fast = vm_safe_get_page(s_card_org_mdata_slot);
		if (md_fast && md_fast->type == ARRAY_PAGE && md_fast->nr_vars > 0 &&
		    md_fast->values[0].i == s_card_org_pair0_slot) {
			return;
		}
	}

	if (vm_is_in_card_cache_callstack())
		return;

	union vm_value *m_data_mem = vm_get_struct_member(proc_org, "m_data", NULL);
	if (!m_data_mem)
		return;
	int m_data_slot = m_data_mem->i;
	struct page *m_data = vm_safe_get_page(m_data_slot);
	if (!m_data || m_data->type != ARRAY_PAGE || m_data->nr_vars <= 0)
		return;

	int src_pair_slot = -1;
	int fallback_pair_slot = -1;
	int insert_pos = m_data->nr_vars;
	for (int i = 0; i < m_data->nr_vars; i++) {
		int pslot = m_data->values[i].i;
		struct page *pair = vm_safe_get_page(pslot);
		if (!pair || pair->type != STRUCT_PAGE)
			continue;
		union vm_value *kv = vm_get_struct_member(pair, "key", NULL);
		if (!kv)
			continue;
		struct page *val_arr = vm_get_field_page(pair, "val", NULL);
		bool valid_val = (val_arr && val_arr->type == ARRAY_PAGE && val_arr->nr_vars > 0);
		if (kv->i == 0) {
			if (valid_val) {
				s_card_org_mdata_slot = m_data_slot;
				s_card_org_pair0_slot = pslot;
				return;
			}
			insert_pos = -1 - i;
		} else if (kv->i > 0 && insert_pos == m_data->nr_vars) {
			insert_pos = i;
		}
		if (valid_val) {
			if (kv->i == 7)
				src_pair_slot = pslot;
			if (fallback_pair_slot < 0)
				fallback_pair_slot = pslot;
		}
	}

	if (src_pair_slot < 0)
		src_pair_slot = fallback_pair_slot;
	if (src_pair_slot < 0)
		return;

	struct page *src_pair = vm_safe_get_page(src_pair_slot);
	if (!src_pair)
		return;
	struct page *cloned_pair = copy_page(src_pair);
	if (!cloned_pair)
		return;
	vm_set_field_int(cloned_pair, "key", NULL, 0);
	int new_pair_slot = heap_alloc_page(cloned_pair);

	m_data = vm_safe_get_page(m_data_slot);
	if (!m_data || m_data->type != ARRAY_PAGE) {
		heap_unref(new_pair_slot);
		return;
	}
	if (insert_pos < 0) {
		int existing_idx = -1 - insert_pos;
		if (existing_idx >= 0 && existing_idx < m_data->nr_vars) {
			variable_set(m_data, existing_idx, AIN_STRUCT, vm_int(new_pair_slot));
		} else {
			heap_unref(new_pair_slot);
			return;
		}
	} else {
		int old_nr = m_data->nr_vars;
		if (insert_pos > old_nr)
			insert_pos = old_nr;
		struct page *next = alloc_page(ARRAY_PAGE, m_data->a_type, old_nr + 1);
		next->array = m_data->array;
		if (insert_pos > 0)
			memcpy(next->values, m_data->values, (size_t)insert_pos * sizeof(*next->values));
		next->values[insert_pos].i = new_pair_slot;
		if (insert_pos < old_nr)
			memcpy(next->values + insert_pos + 1, m_data->values + insert_pos,
			       (size_t)(old_nr - insert_pos) * sizeof(*next->values));
		free_page(m_data);
		heap_set_page(m_data_slot, next);
	}

	s_card_org_mdata_slot = m_data_slot;
	if (insert_pos == 0 || insert_pos == -1)
		s_card_org_pair0_slot = new_pair_slot;

	vm_clear_card_cache();
	s_pending_live_ui_refresh = true;
}

static void maybe_load_initial_modifier_config(void)
{
	char *cfg_path = NULL;
	pthread_mutex_lock(&g_mod_mutex);
	if (g_has_lock_mask_init) {
		pthread_mutex_unlock(&g_mod_mutex);
		return;
	}
	if (g_modifier_config_path && g_modifier_config_path[0])
		cfg_path = strdup(g_modifier_config_path);
	pthread_mutex_unlock(&g_mod_mutex);

#ifndef __ANDROID__
	if (!cfg_path) {
		const char *env_cfg = getenv("XSYS4_MOD_CONFIG_FILE");
		if (env_cfg && env_cfg[0])
			cfg_path = strdup(env_cfg);
	}
#endif
	if (!cfg_path)
		return;
	if (!file_exists(cfg_path)) {
		free(cfg_path);
		return;
	}
	size_t sz = 0;
	char *raw = file_read(cfg_path, &sz);
	free(cfg_path);
	if (!raw)
		return;
	cJSON *root = cJSON_Parse(raw);
	free(raw);
	if (!root || !cJSON_IsObject(root)) {
		cJSON_Delete(root);
		return;
	}
	cJSON *j_mask = cJSON_GetObjectItem(root, "lock_mask");
	cJSON *j_dis  = cJSON_GetObjectItem(root, "disable_streamer_mode");
	int mask = (j_mask && cJSON_IsNumber(j_mask)) ? j_mask->valueint : 0;
	if (j_dis && cJSON_IsTrue(j_dis))
		mask |= LOCK_DISABLE_STREAMER_MODE;
	cJSON_Delete(root);

	pthread_mutex_lock(&g_mod_mutex);
	if (!g_has_lock_mask_init) {
		g_has_lock_mask_init = true;
		g_live_lock_mask = mask;
		if (mask & LOCK_DISABLE_STREAMER_MODE)
			g_pending_lock_streamer_mode = 0;
	}
	pthread_mutex_unlock(&g_mod_mutex);
}

void modifier_on_config_loaded(void)
{
	if (!vm_globals_ready())
		return;
	maybe_load_initial_modifier_config();
	vm_inspect_mod_streamer_ain();
	s_game_config_ready = true;
	if (!s_is_mod_streamer)
		return;

	pthread_mutex_lock(&g_mod_mutex);
	int mask = g_live_lock_mask;
	int pending_sm = g_pending_lock_streamer_mode;
	g_pending_lock_streamer_mode = -1;
	pthread_mutex_unlock(&g_mod_mutex);

	if ((mask & LOCK_DISABLE_STREAMER_MODE) || pending_sm == 0) {
		pthread_mutex_lock(&g_mod_mutex);
		g_live_lock_mask |= LOCK_DISABLE_STREAMER_MODE;
		mask = g_live_lock_mask;
		pthread_mutex_unlock(&g_mod_mutex);
		vm_set_streamer_mode(0, true, true);
		persist_modifier_config_from_native(mask);
	} else if (pending_sm == 1) {
		vm_set_streamer_mode(1, true, true);
		persist_modifier_config_from_native(mask);
	} else {
		int cur_sm = vm_get_live_streamer_mode(false);
		s_last_observed_summary_mode = cur_sm;
		if (cur_sm == 0) {
			pthread_mutex_lock(&g_mod_mutex);
			g_live_lock_mask |= LOCK_DISABLE_STREAMER_MODE;
			mask = g_live_lock_mask;
			pthread_mutex_unlock(&g_mod_mutex);
			vm_clear_card_cache();
			s_pending_live_ui_refresh = true;
			persist_modifier_config_from_native(mask);
		}
	}
}

void modifier_on_globals_loaded(void)
{
	if (!vm_globals_ready())
		return;
	vm_ensure_card_org_fallback_proc();
	if (!vm_has_mod_streamer_mode() || !s_game_config_ready)
		return;
	pthread_mutex_lock(&g_mod_mutex);
	int mask = g_live_lock_mask;
	pthread_mutex_unlock(&g_mod_mutex);
	if (mask & LOCK_DISABLE_STREAMER_MODE) {
		/* Always purge g_cardCache on save/resume load in case resume.bin had cached white curtains */
		vm_set_streamer_mode(0, true, true);
	} else {
		s_last_observed_summary_mode = vm_get_live_streamer_mode(false);
	}
}

/*
 * In 日不落 MOD, Scenes::RunCharacterSetting gates almost all cheat codes behind
 * tt[1224] == 1 (which challenge modes / base phase A reset to 0, showing
 * "作弊码禁用！当前状态或模式下无法使用！"). Setting tt[1224] = 1 unlocks the gate
 * cleanly without wiping the player's tt[6655] bonus score to -99999.
 * Also ensure tt[6699] (the VK_0 re-entrancy lock) is cleared if RunCharacterSetting
 * is not currently on call_stack.
 */
static void vm_unlock_mod_cheat_gate(void)
{
	vm_inspect_mod_streamer_ain();
	struct page *tt = s_glob_tt >= 0 ? vm_get_global_page_idx(s_glob_tt) : vm_get_global_page("tt", NULL);
	if (!tt || tt->type != ARRAY_PAGE)
		return;
	if (tt->nr_vars > 1224)
		tt->values[1224].i = 1;
	if (tt->nr_vars > 6699 && !vm_callstack_has_fno(s_fno_run_char_setting))
		tt->values[6699].i = 0;
}

static void vm_trigger_streamer_unlock_popup(void)
{
	vm_inspect_mod_streamer_ain();
	pthread_mutex_lock(&g_mod_mutex);
	g_live_lock_mask |= LOCK_DISABLE_STREAMER_MODE;
	int mask = g_live_lock_mask;
	pthread_mutex_unlock(&g_mod_mutex);

	vm_set_streamer_mode(0, true, true);
	persist_modifier_config_from_native(mask);
	if (!s_is_mod_streamer)
		return;

	vm_unlock_mod_cheat_gate();
	const char *cheat_gbk = s_rcs_streamer_cheat_gbk ? s_rcs_streamer_cheat_gbk : GBK_STREAMER_PASSPHRASE;
	free(s_pending_cheat_gbk);
	s_pending_cheat_gbk = strdup(cheat_gbk);
	s_cheat_wait_frames = 180;
	s_pending_is_streamer_cheat = true;
}

static void vm_queue_cheat_code_utf8(const char *utf8_str)
{
	if (!utf8_str || !utf8_str[0]) return;
	if (!strcmp(utf8_str, "关闭主播模式") ||
	    !strcmp(utf8_str, "灌注Eins陈谢谢喵") ||
	    !strcmp(utf8_str, "关注Eins陈谢谢喵")) {
		vm_trigger_streamer_unlock_popup();
		return;
	}
	if (!strcmp(utf8_str, "1123029976")) {
		utf8_str = "界徐盛";
	}
	vm_unlock_mod_cheat_gate();
	if (sys4_text_is_gbk)
		sys4_set_text_gbk(true);
	char *gbk = utf2sjis(utf8_str, 0);
	if (gbk) {
		if (gbk[0]) {
			free(s_pending_cheat_gbk);
			s_pending_cheat_gbk = gbk;
			s_cheat_wait_frames = 180;
			s_pending_is_streamer_cheat = false;
		} else {
			free(gbk);
		}
	}
}

static void vm_step_pending_cheat(void)
{
	if (!s_pending_cheat_gbk)
		return;
	if (!ain || !ain->functions || call_stack_ptr <= 0)
		return;
	vm_inspect_mod_streamer_ain();

	bool in_run_char_setting = false;
	int scs_frame = -1;
	struct page *scs_page = NULL;

	for (int i = 0; i < call_stack_ptr; i++) {
		int32_t fno = call_stack[i].fno;
		if (fno < 0 || fno >= ain->nr_functions)
			continue;
		if (fno == s_fno_run_char_setting) {
			in_run_char_setting = true;
		} else if (fno == s_fno_scene_char_setting_run) {
			in_run_char_setting = true;
			scs_frame = i;
			scs_page = vm_safe_get_page(call_stack[i].struct_page);
		}
	}

	if (scs_page && scs_frame >= 0) {
		struct page *scs_ss = vm_get_field_page(scs_page, "m_sceneStack", NULL);
		bool in_join = false;
		for (int i = scs_frame + 1; i < call_stack_ptr; i++) {
			int32_t fno = call_stack[i].fno;
			if (fno == s_fno_scene_stack_join) {
				struct page *join_ss = vm_safe_get_page(call_stack[i].struct_page);
				if (!scs_ss || join_ss == scs_ss) {
					if (!scs_ss) scs_ss = join_ss;
					in_join = true;
					break;
				}
			}
		}
		if (in_join && scs_ss) {
			key_state[VK_0] = false;
			s_pulse_key0_frames = 0;
			if (ain->globals) {
				struct page *tt = s_glob_tt >= 0 ? vm_get_global_page_idx(s_glob_tt) : vm_get_global_page("tt", NULL);
				if (tt && tt->type == ARRAY_PAGE && tt->nr_vars > 1224)
					tt->values[1224].i = 1;
			}
			union vm_value *cur_v = vm_get_struct_member(scs_page, "m_current", NULL);
			if (cur_v) {
				vm_set_string_slot(cur_v->i, s_pending_cheat_gbk);
			}
			struct page *gc = s_glob_game_context >= 0 ? vm_get_global_page_idx(s_glob_game_context) : vm_get_global_page("g_gameContext", NULL);
			vm_set_player_info_name(gc, s_pending_cheat_gbk);

			if (s_pending_is_streamer_cheat) {
				if (s_rcs_streamer_cheat_gbk != NULL) {
					if (s_rcs_streamer_has_tt0_req) {
						s_saved_tt0 = vm_get_tt(0, 0);
						s_saved_tt0_active = true;
						vm_set_tt(0, s_rcs_streamer_req_tt0);
					}
					s_saved_tt6655 = vm_get_tt(6655, 0);
					s_saved_tt6655_active = true;
				} else {
					vm_activate_rcs_popup_override();
				}
				s_wait_post_rcs_streamer_refresh = true;
				s_pending_is_streamer_cheat = false;
			}

			vm_set_field_int(scs_ss, "<IsFinish>", "m_isFinish", 1);
			int idx_end_busy = s_glob_end_busy >= 0 ? s_glob_end_busy : vm_find_global_idx("parts::detail::g_EndPartsBusyLoop", NULL);
			if (idx_end_busy >= 0)
				global_set(idx_end_busy, vm_int(1), false);
			int idx_end_num = s_glob_end_busy_num >= 0 ? s_glob_end_busy_num : vm_find_global_idx("parts::detail::g_EndPartsBusyLoopNumber", NULL);
			if (idx_end_num >= 0)
				global_set(idx_end_num, vm_int(0), false);
			PE_ReleaseMessage();
			free(s_pending_cheat_gbk);
			s_pending_cheat_gbk = NULL;
			s_cheat_wait_frames = 0;
			return;
		}
	}

	if (in_run_char_setting) {
		key_state[VK_0] = false;
	} else {
		key_state[VK_0] = true;
	}

	if (--s_cheat_wait_frames <= 0) {
		key_state[VK_0] = false;
		free(s_pending_cheat_gbk);
		s_pending_cheat_gbk = NULL;
		s_cheat_wait_frames = 0;
		s_pending_is_streamer_cheat = false;
		vm_restore_temp_rcs_overrides(true);
	}
}

static void vm_boost_card_array(struct page *arr, int set_count, int add_count, int atk_bonus, int hp_bonus, int *total_cards)
{
	if (!arr || arr->type != ARRAY_PAGE) return;
	for (int i = 0; i < arr->nr_vars; i++) {
		struct page *c_page = vm_safe_get_page(arr->values[i].i);
		if (!c_page || c_page->type != STRUCT_PAGE) continue;
		if (total_cards) (*total_cards)++;
		if (set_count > 0) {
			vm_set_field_int(c_page, "m_count", NULL, set_count);
			vm_set_field_int(c_page, "m_lastCount", NULL, set_count);
		} else if (add_count > 0) {
			int32_t cur = vm_get_field_int(c_page, "m_count", NULL, 1);
			vm_set_field_int(c_page, "m_count", NULL, cur + add_count);
			vm_set_field_int(c_page, "m_lastCount", NULL, cur + add_count);
		}
		if (atk_bonus > 0) {
			int32_t cur_atk = vm_get_field_int(c_page, "m_totalAtk", NULL, 100);
			if (cur_atk < atk_bonus)
				vm_set_field_int(c_page, "m_totalAtk", NULL, atk_bonus);
		}
		if (hp_bonus > 0) {
			int32_t cur_hp = vm_get_field_int(c_page, "m_totalHp", NULL, 100);
			if (cur_hp < hp_bonus)
				vm_set_field_int(c_page, "m_totalHp", NULL, hp_bonus);
		}
	}
}

static int vm_process_all_cards(int set_count, int add_count, int atk_bonus, int hp_bonus)
{
	int total = 0;
	struct page *pc = vm_get_global_page("g_playerCard", NULL);
	if (!pc) return 0;
	struct page *org_arr = vm_get_field_page(pc, "m_org", NULL);
	if (org_arr && org_arr->type == ARRAY_PAGE) {
		for (int i = 0; i < org_arr->nr_vars; i++) {
			struct page *org_page = vm_safe_get_page(org_arr->values[i].i);
			struct page *card_arr = vm_get_field_page(org_page, "m_card", NULL);
			vm_boost_card_array(card_arr, set_count, add_count, atk_bonus, hp_bonus, &total);
		}
	}
	vm_boost_card_array(vm_get_field_page(pc, "m_stock", NULL), set_count, add_count, atk_bonus, hp_bonus, &total);
	vm_boost_card_array(vm_get_field_page(pc, "m_extraCard", NULL), set_count, add_count, atk_bonus, hp_bonus, &total);
	vm_boost_card_array(vm_get_field_page(pc, "m_oldCard", "m_card"), set_count, add_count, atk_bonus, hp_bonus, NULL);
	return total;
}

static int vm_process_all_characters(int set_star, int set_exp)
{
	struct page *ch = vm_get_global_page("g_character", NULL);
	if (!ch) return 0;
	struct page *arr = vm_get_field_page(ch, "m_character", NULL);
	if (!arr || arr->type != ARRAY_PAGE) return 0;
	for (int i = 0; i < arr->nr_vars; i++) {
		struct page *c_page = vm_safe_get_page(arr->values[i].i);
		if (!c_page || c_page->type != STRUCT_PAGE) continue;
		if (set_star > 0) {
			int32_t cur = vm_get_field_int(c_page, "m_star", NULL, 0);
			if (cur < set_star) vm_set_field_int(c_page, "m_star", NULL, set_star);
		}
		if (set_exp > 0) {
			vm_set_field_int(c_page, "<Exp>", NULL, set_exp);
		}
	}
	return arr->nr_vars;
}

static void vm_cure_and_boost_leaders(bool boost_crit, bool reset_skills)
{
	struct page *party = vm_get_global_page("g_party", NULL);
	if (!party) return;
	struct page *leaders_col = vm_get_field_page(party, "m_leaders", NULL);
	struct page *leaders_arr = vm_get_field_page(leaders_col, "m_leaders", NULL);
	if (!leaders_arr || leaders_arr->type != ARRAY_PAGE) return;

	for (int i = 0; i < leaders_arr->nr_vars; i++) {
		struct page *lc = vm_safe_get_page(leaders_arr->values[i].i);
		if (!lc || lc->type != STRUCT_PAGE) continue;
		if (boost_crit) {
			struct page *ls = vm_get_field_page(lc, "m_state", NULL);
			if (ls) {
				int32_t st = vm_get_field_int(ls, "m_state", NULL, -1);
				if (st >= 1 || st == -1) vm_set_field_int(ls, "m_state", NULL, 0); /* 0 = Boost & Actionable */
				vm_set_field_int(ls, "IsDamageSacrifice", NULL, 0);
				vm_set_field_int(ls, "IsBadConditionSacrifice", NULL, 0);
				vm_set_field_int(ls, "IsCritical", NULL, 1);
				vm_set_field_int(ls, "IsMultiAttack", NULL, 1);
			}
		}
		if (reset_skills) {
			struct page *sc_col = vm_get_field_page(lc, "m_skillCounter", NULL);
			struct page *sc_arr = vm_get_field_page(sc_col, "m_counter", NULL);
			if (sc_arr && sc_arr->type == ARRAY_PAGE) {
				for (int k = 0; k < sc_arr->nr_vars; k++) {
					struct page *scv = vm_safe_get_page(sc_arr->values[k].i);
					if (scv && scv->type == STRUCT_PAGE) {
						vm_set_field_int(scv, "IsUsed", NULL, 0);
						vm_set_field_int(scv, "IsLastUsed", NULL, 0);
						vm_set_field_int(scv, "UsedCount", NULL, 0);
					}
				}
			}
		}
	}
}

static void vm_json_write_if_present(cJSON *root, const char *key, struct page *page, const char *f1, const char *f2)
{
	cJSON *it = cJSON_GetObjectItem(root, key);
	if (it && cJSON_IsNumber(it)) {
		vm_set_field_int(page, f1, f2, it->valueint);
	}
}

static void vm_execute_pending_action(const char *json_str)
{
	cJSON *act = cJSON_Parse(json_str);
	if (!act) return;

	vm_inspect_mod_streamer_ain();
	struct page *gc    = s_glob_game_context >= 0 ? vm_get_global_page_idx(s_glob_game_context) : vm_get_global_page("g_gameContext", NULL);
	struct page *pcp   = s_glob_pcp >= 0 ? vm_get_global_page_idx(s_glob_pcp) : vm_get_global_page("g_playerCommonParam", NULL);
	struct page *pb    = s_glob_party_bonus >= 0 ? vm_get_global_page_idx(s_glob_party_bonus) : vm_get_global_page("g_partyBonus", NULL);
	struct page *party = s_glob_party >= 0 ? vm_get_global_page_idx(s_glob_party) : vm_get_global_page("g_party", NULL);
	struct page *ps    = vm_get_field_page(party, "m_status", NULL);
	struct page *cs    = vm_get_field_page(ps, "m_status", NULL);
	struct page *bc    = s_glob_battle_ctx >= 0 ? vm_get_global_page_idx(s_glob_battle_ctx) : vm_get_global_page("g_battleContext", NULL);
	struct page *enemy = s_glob_enemy >= 0 ? vm_get_global_page_idx(s_glob_enemy) : vm_get_global_page("g_enemy", NULL);

	cJSON *it;
	/* Instant battle actions */
	if ((it = cJSON_GetObjectItem(act, "killEnemy")) && cJSON_IsTrue(it) && enemy) {
		vm_set_field_int(enemy, "m_hp", NULL, 1);
	}
	if ((it = cJSON_GetObjectItem(act, "weakenEnemy")) && cJSON_IsTrue(it) && enemy) {
		struct page *ecs = vm_get_field_page(enemy, "m_status", NULL);
		if (ecs) {
			vm_set_field_int(ecs, "<Atk>", NULL, 0);
			vm_set_field_int(ecs, "m_AtkPerPhysical", NULL, 0);
			vm_set_field_int(ecs, "m_AtkPerMagical", NULL, 0);
			vm_set_field_int(ecs, "m_DefPerPhysical", NULL, 0);
			vm_set_field_int(ecs, "m_DefPerMagical", NULL, 0);
			vm_set_field_int(ecs, "m_PerAvoidance", NULL, 0);
			vm_set_field_int(ecs, "m_DecPerBadCondition", NULL, 0);
		}
	}
	if ((it = cJSON_GetObjectItem(act, "refillPartyHp")) && cJSON_IsTrue(it) && party) {
		int32_t max_hp = vm_get_field_int(ps, "m_totalHp", NULL, 99999);
		if (max_hp < 1) max_hp = 99999;
		vm_set_field_int(party, "m_hp", NULL, max_hp);
		union vm_value *hr = vm_get_struct_member(party, "m_hpRatio", NULL);
		if (hr) hr->f = 1.0f;
	}
	if ((it = cJSON_GetObjectItem(act, "battleAp")) && cJSON_IsNumber(it) && bc) {
		struct page *ap_p = vm_get_field_page(bc, "<Ap>", NULL);
		vm_set_field_int(ap_p, "m_ap", NULL, it->valueint);
	}
	if ((it = cJSON_GetObjectItem(act, "battleCombo")) && cJSON_IsNumber(it) && bc) {
		struct page *cb_p = vm_get_field_page(bc, "<Combo>", NULL);
		vm_set_field_int(cb_p, "m_count", NULL, it->valueint);
	}
	if ((it = cJSON_GetObjectItem(act, "battleRound")) && cJSON_IsNumber(it) && bc) {
		struct page *rd_p = vm_get_field_page(bc, "<Round>", NULL);
		vm_set_field_int(rd_p, "m_round", NULL, it->valueint);
		int32_t lim = vm_get_field_int(rd_p, "Limit", NULL, 30);
		if (lim < 99) vm_set_field_int(rd_p, "Limit", NULL, 99);
	}
	if ((it = cJSON_GetObjectItem(act, "cureAndBoostLeaders")) && cJSON_IsTrue(it)) {
		vm_cure_and_boost_leaders(true, true);
	}

	/* Resources & Progress */
	vm_json_write_if_present(act, "turn", gc, "m_turn", NULL);
	vm_json_write_if_present(act, "chapter", gc, "m_chapter", NULL);
	if ((it = cJSON_GetObjectItem(act, "difficulty")) && cJSON_IsNumber(it)) {
		int idx = s_glob_diff >= 0 ? s_glob_diff : vm_find_global_idx(GBK_DIFFICULTY, SJIS_DIFFICULTY);
		if (idx >= 0) global_set(idx, vm_int(it->valueint), false);
	}
	if ((it = cJSON_GetObjectItem(act, "battleCount")) && cJSON_IsNumber(it)) {
		int idx = s_glob_battle_count >= 0 ? s_glob_battle_count : vm_find_global_idx(GBK_BATTLE_COUNT, SJIS_BATTLE_COUNT);
		if (idx >= 0) global_set(idx, vm_int(it->valueint), false);
	}
	vm_json_write_if_present(act, "foodTicket", pcp, "m_foodTicket", NULL);
	vm_json_write_if_present(act, "honor", pcp, "m_honor", NULL);
	vm_json_write_if_present(act, "friendPoint", pcp, "m_friendPoint", NULL);
	vm_json_write_if_present(act, "totalExp", pcp, "<TotalExp>", NULL);
	vm_json_write_if_present(act, "partyRank", pcp, "<PartyRank>", NULL);
	vm_json_write_if_present(act, "treasureNotGetCount", pcp, "<TreasureNotGetCount>", NULL);
	vm_json_write_if_present(act, "itemStar", pcp, "<ItemStar>", NULL);
	vm_json_write_if_present(act, "isUsedHannyZippo", pcp, "<IsUsedHannyZippo>", NULL);
	vm_json_write_if_present(act, "partyBonusMaxPoint", pb, "m_maxPoint", NULL);

	/* Party & CommonStatus */
	vm_json_write_if_present(act, "partyHp", party, "m_hp", NULL);
	vm_json_write_if_present(act, "partyMaxHp", ps, "m_totalHp", NULL);
	vm_json_write_if_present(act, "partyAvailCount", party, "m_availableCount", NULL);
	vm_json_write_if_present(act, "partyIsFixStatus", party, "<IsFixStatus>", NULL);
	if (party && ps) {
		int32_t cur_hp = vm_get_field_int(party, "m_hp", NULL, 1);
		int32_t max_hp = vm_get_field_int(ps, "m_totalHp", NULL, 1);
		union vm_value *hr = vm_get_struct_member(party, "m_hpRatio", NULL);
		if (hr && max_hp > 0) {
			hr->f = (float)cur_hp / (float)max_hp;
			if (hr->f > 1.0f) hr->f = 1.0f;
		}
	}
	if ((it = cJSON_GetObjectItem(act, "allStatusPer")) && cJSON_IsNumber(it) && cs) {
		int v = it->valueint;
		vm_set_field_int(cs, "m_AtkPerPhysical", NULL, v);
		vm_set_field_int(cs, "m_AtkPerMagical", NULL, v);
		vm_set_field_int(cs, "m_DefPerPhysical", NULL, v);
		vm_set_field_int(cs, "m_DefPerMagical", NULL, v);
		vm_set_field_int(cs, "m_PerAvoidance", NULL, v);
		vm_set_field_int(cs, "m_DecPerBadCondition", NULL, v);
		vm_set_field_int(cs, "m_perAutoBattleAtk", NULL, v);
		vm_set_field_int(cs, "m_PerCritical", NULL, v);
		vm_set_field_int(cs, "m_PerMultiAttack", NULL, v);
		vm_set_field_int(cs, "m_PerExpUp", NULL, v);
		vm_set_field_int(cs, "m_PerHpUp", NULL, v);
		vm_set_field_int(cs, "m_PerAutoHeal", NULL, v);
		vm_set_field_int(cs, "m_PerFindTreasure", NULL, v);
		vm_set_field_int(cs, "m_PerInjection", NULL, v);
		vm_set_field_int(cs, "m_AddPerHit", NULL, v);
		vm_set_field_int(cs, "StatusMaxPer", NULL, v);
	}
	vm_json_write_if_present(act, "partyAtk", cs, "<Atk>", NULL);
	vm_json_write_if_present(act, "atkPerPhys", cs, "m_AtkPerPhysical", NULL);
	vm_json_write_if_present(act, "atkPerMag", cs, "m_AtkPerMagical", NULL);
	vm_json_write_if_present(act, "defPerPhys", cs, "m_DefPerPhysical", NULL);
	vm_json_write_if_present(act, "defPerMag", cs, "m_DefPerMagical", NULL);
	vm_json_write_if_present(act, "perAvoid", cs, "m_PerAvoidance", NULL);
	vm_json_write_if_present(act, "decBadCond", cs, "m_DecPerBadCondition", NULL);
	vm_json_write_if_present(act, "perAutoAtk", cs, "m_perAutoBattleAtk", NULL);
	vm_json_write_if_present(act, "perCrit", cs, "m_PerCritical", NULL);
	vm_json_write_if_present(act, "perMulti", cs, "m_PerMultiAttack", NULL);
	vm_json_write_if_present(act, "perExpUp", cs, "m_PerExpUp", NULL);
	vm_json_write_if_present(act, "perHpUp", cs, "m_PerHpUp", NULL);
	vm_json_write_if_present(act, "perAutoHeal", cs, "m_PerAutoHeal", NULL);
	vm_json_write_if_present(act, "perFindTreasure", cs, "m_PerFindTreasure", NULL);
	vm_json_write_if_present(act, "perInjection", cs, "m_PerInjection", NULL);
	vm_json_write_if_present(act, "addPerHit", cs, "m_AddPerHit", NULL);
	vm_json_write_if_present(act, "statusMaxPer", cs, "StatusMaxPer", NULL);

	/* World & Country State */
	if ((it = cJSON_GetObjectItem(act, "humanCount")) && cJSON_IsNumber(it)) vm_set_tt(33, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "humanDead")) && cJSON_IsNumber(it)) {
		vm_set_tt(34, it->valueint);
		vm_set_field_int(pcp, "m_lastHumanDead", NULL, it->valueint);
	}
	if ((it = cJSON_GetObjectItem(act, "humanAlivePer")) && cJSON_IsNumber(it)) {
		vm_set_tt(35, it->valueint);
		vm_set_field_int(pcp, "m_lastHumanAlivePer", NULL, it->valueint);
	}
	if ((it = cJSON_GetObjectItem(act, "humanDeadPer")) && cJSON_IsNumber(it)) vm_set_tt(36, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "curseTurn")) && cJSON_IsNumber(it)) vm_set_tt(41, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "rareLuck")) && cJSON_IsNumber(it)) vm_set_tt(42, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "hannyZippo")) && cJSON_IsNumber(it)) {
		vm_set_tt(101, it->valueint);
		if (bc) vm_set_field_int(bc, "m_hannyZippoCount", NULL, it->valueint);
	}
	if ((it = cJSON_GetObjectItem(act, "seaPlayerArmy")) && cJSON_IsNumber(it)) vm_set_tt(66, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "seaEnemyArmy")) && cJSON_IsNumber(it)) vm_set_tt(68, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "leazasState")) && cJSON_IsNumber(it)) vm_set_tt(13, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "helmanState")) && cJSON_IsNumber(it)) vm_set_tt(14, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "zethState")) && cJSON_IsNumber(it)) vm_set_tt(15, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "freeCitiesState")) && cJSON_IsNumber(it)) vm_set_tt(16, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "leazasPArmy")) && cJSON_IsNumber(it)) vm_set_tt(25, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "helmanPArmy")) && cJSON_IsNumber(it)) vm_set_tt(26, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "zethPArmy")) && cJSON_IsNumber(it)) vm_set_tt(27, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "freeCitiesPArmy")) && cJSON_IsNumber(it)) vm_set_tt(28, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "leazasEArmy")) && cJSON_IsNumber(it)) vm_set_tt(29, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "helmanEArmy")) && cJSON_IsNumber(it)) vm_set_tt(30, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "zethEArmy")) && cJSON_IsNumber(it)) vm_set_tt(31, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "freeCitiesEArmy")) && cJSON_IsNumber(it)) vm_set_tt(32, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "subRemnant")) && cJSON_IsNumber(it)) vm_set_tt(37, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "subDwarf")) && cJSON_IsNumber(it)) vm_set_tt(38, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "subGeneral")) && cJSON_IsNumber(it)) vm_set_tt(39, it->valueint);
	if ((it = cJSON_GetObjectItem(act, "subHanny")) && cJSON_IsNumber(it)) vm_set_tt(40, it->valueint);

	cJSON *arr;
	if ((arr = cJSON_GetObjectItem(act, "countryState")) && cJSON_IsArray(arr)) {
		for (int i = 0; i < 4 && i < cJSON_GetArraySize(arr); i++)
			vm_set_tt(13 + i, cJSON_GetArrayItem(arr, i)->valueint);
	}
	if ((arr = cJSON_GetObjectItem(act, "countryPlayerArmy")) && cJSON_IsArray(arr)) {
		for (int i = 0; i < 4 && i < cJSON_GetArraySize(arr); i++)
			vm_set_tt(25 + i, cJSON_GetArrayItem(arr, i)->valueint);
	}
	if ((arr = cJSON_GetObjectItem(act, "countryEnemyArmy")) && cJSON_IsArray(arr)) {
		for (int i = 0; i < 4 && i < cJSON_GetArraySize(arr); i++)
			vm_set_tt(29 + i, cJSON_GetArrayItem(arr, i)->valueint);
	}
	if ((arr = cJSON_GetObjectItem(act, "subArmy")) && cJSON_IsArray(arr)) {
		for (int i = 0; i < 4 && i < cJSON_GetArraySize(arr); i++)
			vm_set_tt(37 + i, cJSON_GetArrayItem(arr, i)->valueint);
	}

	/* Batch Cards & Characters */
	int set_card_count = (it = cJSON_GetObjectItem(act, "setCardCount")) && cJSON_IsNumber(it) ? it->valueint : 0;
	int add_card_count = (it = cJSON_GetObjectItem(act, "addCardCount")) && cJSON_IsNumber(it) ? it->valueint : 0;
	int card_atk_bonus = (it = cJSON_GetObjectItem(act, "cardAtkBonus")) && cJSON_IsNumber(it) ? it->valueint : 0;
	int card_hp_bonus  = (it = cJSON_GetObjectItem(act, "cardHpBonus")) && cJSON_IsNumber(it) ? it->valueint : 0;
	if (set_card_count > 0 || add_card_count > 0 || card_atk_bonus > 0 || card_hp_bonus > 0) {
		vm_process_all_cards(set_card_count, add_card_count, card_atk_bonus, card_hp_bonus);
	}
	int set_char_star = (it = cJSON_GetObjectItem(act, "setCharStar")) && cJSON_IsNumber(it) ? it->valueint : 0;
	int set_char_exp  = (it = cJSON_GetObjectItem(act, "setCharExp")) && cJSON_IsNumber(it) ? it->valueint : 0;
	if (set_char_star > 0 || set_char_exp > 0) {
		vm_process_all_characters(set_char_star, set_char_exp);
	}

	bool want_streamer_popup = false;
	if ((it = cJSON_GetObjectItem(act, "streamerPopup")) &&
	    (cJSON_IsTrue(it) || (cJSON_IsNumber(it) && it->valueint != 0))) {
		want_streamer_popup = true;
	}
	if ((it = cJSON_GetObjectItem(act, "streamerMode")) && cJSON_IsNumber(it)) {
		int en = it->valueint != 0 ? 1 : 0;
		int new_mask;
		pthread_mutex_lock(&g_mod_mutex);
		g_has_lock_mask_init = true;
		if (en == 0)
			g_live_lock_mask |= LOCK_DISABLE_STREAMER_MODE;
		else
			g_live_lock_mask &= ~LOCK_DISABLE_STREAMER_MODE;
		new_mask = g_live_lock_mask;
		pthread_mutex_unlock(&g_mod_mutex);
		s_game_config_ready = true;
		vm_set_streamer_mode(en, true, true);
		persist_modifier_config_from_native(new_mask);
	}
	if (want_streamer_popup) {
		s_game_config_ready = true;
		vm_trigger_streamer_unlock_popup();
	} else if ((it = cJSON_GetObjectItem(act, "cheatCode")) && cJSON_IsString(it) &&
	           it->valuestring && it->valuestring[0]) {
		vm_queue_cheat_code_utf8(it->valuestring);
	} else if ((it = cJSON_GetObjectItem(act, "fillStreamerPassphrase")) &&
	           (cJSON_IsTrue(it) || (cJSON_IsNumber(it) && it->valueint != 0))) {
		s_game_config_ready = true;
		vm_trigger_streamer_unlock_popup();
	}
	if (!s_pending_cheat_gbk &&
	    (it = cJSON_GetObjectItem(act, "triggerKey0")) &&
	    (cJSON_IsTrue(it) || (cJSON_IsNumber(it) && it->valueint != 0))) {
		vm_unlock_mod_cheat_gate();
		s_pulse_key0_frames = 8;
		key_state[VK_0] = true;
	}
	cJSON *tt_idx = cJSON_GetObjectItem(act, "ttFlagIdx");
	cJSON *tt_val = cJSON_GetObjectItem(act, "ttFlagVal");
	if (tt_idx && cJSON_IsNumber(tt_idx) && tt_val && cJSON_IsNumber(tt_val)) {
		vm_set_tt(tt_idx->valueint, tt_val->valueint);
	}

	cJSON_Delete(act);
}

static void vm_enforce_locks(int mask)
{
	const int active_locks = mask & ~LOCK_DISABLE_STREAMER_MODE;
	if (!active_locks || !ain) return;

	vm_inspect_mod_streamer_ain();
	struct page *pcp   = s_glob_pcp >= 0 ? vm_get_global_page_idx(s_glob_pcp) : vm_get_global_page("g_playerCommonParam", NULL);
	struct page *party = s_glob_party >= 0 ? vm_get_global_page_idx(s_glob_party) : vm_get_global_page("g_party", NULL);
	struct page *ps    = party ? vm_get_field_page(party, "m_status", NULL) : NULL;
	struct page *cs    = ps ? vm_get_field_page(ps, "m_status", NULL) : NULL;
	struct page *bc    = s_glob_battle_ctx >= 0 ? vm_get_global_page_idx(s_glob_battle_ctx) : vm_get_global_page("g_battleContext", NULL);
	struct page *enemy = s_glob_enemy >= 0 ? vm_get_global_page_idx(s_glob_enemy) : vm_get_global_page("g_enemy", NULL);

	if ((active_locks & LOCK_PARTY_MAX_HP) && party && ps) {
		int32_t max_hp = vm_get_field_int(ps, "m_totalHp", NULL, 9999);
		if (max_hp < 1) max_hp = 9999;
		if (vm_get_field_int(party, "m_hp", NULL, 0) < max_hp) {
			vm_set_field_int(party, "m_hp", NULL, max_hp);
			union vm_value *hr = vm_get_struct_member(party, "m_hpRatio", NULL);
			if (hr) hr->f = 1.0f;
		}
	}
	if ((active_locks & LOCK_INFINITE_AP) && bc) {
		struct page *ap_p = vm_get_field_page(bc, "<Ap>", NULL);
		if (ap_p && vm_get_field_int(ap_p, "m_ap", NULL, 0) < 99) {
			vm_set_field_int(ap_p, "m_ap", NULL, 99);
		}
	}
	if ((active_locks & LOCK_BATTLE_COMBO) && bc) {
		struct page *cb_p = vm_get_field_page(bc, "<Combo>", NULL);
		if (cb_p && vm_get_field_int(cb_p, "m_count", NULL, 0) < 999) {
			vm_set_field_int(cb_p, "m_count", NULL, 999);
		}
	}
	if ((active_locks & LOCK_BATTLE_ROUND_1) && bc) {
		struct page *rd_p = vm_get_field_page(bc, "<Round>", NULL);
		if (rd_p) {
			if (vm_get_field_int(rd_p, "m_round", NULL, 1) > 1)
				vm_set_field_int(rd_p, "m_round", NULL, 1);
			if (vm_get_field_int(rd_p, "Limit", NULL, 30) < 99)
				vm_set_field_int(rd_p, "Limit", NULL, 99);
		}
	}
	if ((active_locks & LOCK_ONE_HIT_KILL) && enemy) {
		if (vm_get_field_int(enemy, "m_hp", NULL, 0) > 1) {
			vm_set_field_int(enemy, "m_hp", NULL, 1);
		}
	}
	if ((active_locks & LOCK_ENEMY_ZERO_ATK) && enemy) {
		struct page *ecs = vm_get_field_page(enemy, "m_status", NULL);
		if (ecs) {
			vm_set_field_int(ecs, "<Atk>", NULL, 0);
			vm_set_field_int(ecs, "m_AtkPerPhysical", NULL, 0);
			vm_set_field_int(ecs, "m_AtkPerMagical", NULL, 0);
			vm_set_field_int(ecs, "m_DefPerPhysical", NULL, 0);
			vm_set_field_int(ecs, "m_DefPerMagical", NULL, 0);
			vm_set_field_int(ecs, "m_DecPerBadCondition", NULL, 0);
		}
	}
	if ((active_locks & LOCK_LEADER_BOOST_CRIT) || (active_locks & LOCK_INFINITE_SKILL_USE)) {
		vm_cure_and_boost_leaders((active_locks & LOCK_LEADER_BOOST_CRIT) != 0,
		                          (active_locks & LOCK_INFINITE_SKILL_USE) != 0);
	}
	if (active_locks & LOCK_INFINITE_ZIPPO) {
		if (pcp) vm_set_field_int(pcp, "<IsUsedHannyZippo>", NULL, 0);
		if (bc) {
			if (vm_get_field_int(bc, "m_hannyZippoCount", NULL, 0) < 9)
				vm_set_field_int(bc, "m_hannyZippoCount", NULL, 9);
			vm_set_field_int(bc, "m_curseTurn", NULL, 0);
		}
	}
	if ((active_locks & LOCK_MAX_RESOURCES) && pcp) {
		if (vm_get_field_int(pcp, "m_foodTicket", NULL, 0) < 999)
			vm_set_field_int(pcp, "m_foodTicket", NULL, 999);
		if (vm_get_field_int(pcp, "m_honor", NULL, 0) < 999)
			vm_set_field_int(pcp, "m_honor", NULL, 999);
		if (vm_get_field_int(pcp, "m_friendPoint", NULL, 0) < 999)
			vm_set_field_int(pcp, "m_friendPoint", NULL, 999);
	}
	if (active_locks & LOCK_TREASURE_100) {
		if (cs && vm_get_field_int(cs, "m_PerFindTreasure", NULL, 0) < 999)
			vm_set_field_int(cs, "m_PerFindTreasure", NULL, 999);
		if (pcp && vm_get_field_int(pcp, "<TreasureNotGetCount>", NULL, 0) < 99)
			vm_set_field_int(pcp, "<TreasureNotGetCount>", NULL, 99);
		if (vm_get_tt(42, 0) < 999)
			vm_set_tt(42, 999);
	}
}

static char *vm_build_live_state_json(int lock_mask)
{
	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "lockMask", lock_mask);

	vm_inspect_mod_streamer_ain();
	struct page *gc    = s_glob_game_context >= 0 ? vm_get_global_page_idx(s_glob_game_context) : vm_get_global_page("g_gameContext", NULL);
	struct page *cfg   = s_glob_game_config >= 0 ? vm_get_global_page_idx(s_glob_game_config) : vm_get_global_page("g_gameConfig", NULL);
	struct page *pcp   = s_glob_pcp >= 0 ? vm_get_global_page_idx(s_glob_pcp) : vm_get_global_page("g_playerCommonParam", NULL);
	struct page *pb    = s_glob_party_bonus >= 0 ? vm_get_global_page_idx(s_glob_party_bonus) : vm_get_global_page("g_partyBonus", NULL);
	struct page *party = s_glob_party >= 0 ? vm_get_global_page_idx(s_glob_party) : vm_get_global_page("g_party", NULL);
	struct page *ps    = vm_get_field_page(party, "m_status", NULL);
	struct page *cs    = vm_get_field_page(ps, "m_status", NULL);
	struct page *bc    = s_glob_battle_ctx >= 0 ? vm_get_global_page_idx(s_glob_battle_ctx) : vm_get_global_page("g_battleContext", NULL);
	struct page *enemy = s_glob_enemy >= 0 ? vm_get_global_page_idx(s_glob_enemy) : vm_get_global_page("g_enemy", NULL);

	if (vm_has_mod_streamer_mode()) {
		int sm = vm_get_live_streamer_mode(!vm_is_in_card_ui_callstack());
		if (sm >= 0)
			cJSON_AddNumberToObject(root, "streamerMode", sm);
	} else if (cfg) {
		cJSON_AddNumberToObject(root, "streamerMode", vm_get_field_int(cfg, "m_isSummaryMode", NULL, 0));
	}

	if (!ain || !gc || !pcp) {
		cJSON_AddBoolToObject(root, "inGame", false);
		char *out = cJSON_PrintUnformatted(root);
		cJSON_Delete(root);
		return out;
	}
	cJSON_AddBoolToObject(root, "inGame", true);
	cJSON_AddBoolToObject(root, "inBattle", (bc != NULL && enemy != NULL));

	cJSON_AddNumberToObject(root, "turn", vm_get_field_int(gc, "m_turn", NULL, 0));
	cJSON_AddNumberToObject(root, "chapter", vm_get_field_int(gc, "m_chapter", NULL, 0));

	int idx_diff = s_glob_diff >= 0 ? s_glob_diff : vm_find_global_idx(GBK_DIFFICULTY, SJIS_DIFFICULTY);
	int idx_bc   = s_glob_battle_count >= 0 ? s_glob_battle_count : vm_find_global_idx(GBK_BATTLE_COUNT, SJIS_BATTLE_COUNT);
	cJSON_AddNumberToObject(root, "difficulty", idx_diff >= 0 ? global_get(idx_diff).i : 1);
	cJSON_AddNumberToObject(root, "battleCount", idx_bc >= 0 ? global_get(idx_bc).i : 0);

	cJSON_AddNumberToObject(root, "foodTicket", vm_get_field_int(pcp, "m_foodTicket", NULL, 0));
	cJSON_AddNumberToObject(root, "honor", vm_get_field_int(pcp, "m_honor", NULL, 0));
	cJSON_AddNumberToObject(root, "friendPoint", vm_get_field_int(pcp, "m_friendPoint", NULL, 0));
	cJSON_AddNumberToObject(root, "totalExp", vm_get_field_int(pcp, "<TotalExp>", NULL, 0));
	cJSON_AddNumberToObject(root, "partyRank", vm_get_field_int(pcp, "<PartyRank>", NULL, 0));
	cJSON_AddNumberToObject(root, "treasureNotGetCount", vm_get_field_int(pcp, "<TreasureNotGetCount>", NULL, 0));
	cJSON_AddNumberToObject(root, "itemStar", vm_get_field_int(pcp, "<ItemStar>", NULL, 0));
	cJSON_AddNumberToObject(root, "isUsedHannyZippo", vm_get_field_int(pcp, "<IsUsedHannyZippo>", NULL, 0));
	cJSON_AddNumberToObject(root, "partyBonusMaxPoint", vm_get_field_int(pb, "m_maxPoint", NULL, 0));

	cJSON_AddNumberToObject(root, "partyHp", vm_get_field_int(party, "m_hp", NULL, 0));
	cJSON_AddNumberToObject(root, "partyMaxHp", vm_get_field_int(ps, "m_totalHp", NULL, 0));
	cJSON_AddNumberToObject(root, "partyAvailCount", vm_get_field_int(party, "m_availableCount", NULL, 4));
	cJSON_AddNumberToObject(root, "partyIsFixStatus", vm_get_field_int(party, "<IsFixStatus>", NULL, 0));
	cJSON_AddNumberToObject(root, "partyAtk", vm_get_field_int(cs, "<Atk>", NULL, 0));
	cJSON_AddNumberToObject(root, "atkPerPhys", vm_get_field_int(cs, "m_AtkPerPhysical", NULL, 0));
	cJSON_AddNumberToObject(root, "atkPerMag", vm_get_field_int(cs, "m_AtkPerMagical", NULL, 0));
	cJSON_AddNumberToObject(root, "defPerPhys", vm_get_field_int(cs, "m_DefPerPhysical", NULL, 0));
	cJSON_AddNumberToObject(root, "defPerMag", vm_get_field_int(cs, "m_DefPerMagical", NULL, 0));
	cJSON_AddNumberToObject(root, "perAvoid", vm_get_field_int(cs, "m_PerAvoidance", NULL, 0));
	cJSON_AddNumberToObject(root, "decBadCond", vm_get_field_int(cs, "m_DecPerBadCondition", NULL, 0));
	cJSON_AddNumberToObject(root, "perAutoAtk", vm_get_field_int(cs, "m_perAutoBattleAtk", NULL, 0));
	cJSON_AddNumberToObject(root, "perCrit", vm_get_field_int(cs, "m_PerCritical", NULL, 0));
	cJSON_AddNumberToObject(root, "perMulti", vm_get_field_int(cs, "m_PerMultiAttack", NULL, 0));
	cJSON_AddNumberToObject(root, "perExpUp", vm_get_field_int(cs, "m_PerExpUp", NULL, 0));
	cJSON_AddNumberToObject(root, "perHpUp", vm_get_field_int(cs, "m_PerHpUp", NULL, 0));
	cJSON_AddNumberToObject(root, "perAutoHeal", vm_get_field_int(cs, "m_PerAutoHeal", NULL, 0));
	cJSON_AddNumberToObject(root, "perFindTreasure", vm_get_field_int(cs, "m_PerFindTreasure", NULL, 0));
	cJSON_AddNumberToObject(root, "perInjection", vm_get_field_int(cs, "m_PerInjection", NULL, 0));
	cJSON_AddNumberToObject(root, "addPerHit", vm_get_field_int(cs, "m_AddPerHit", NULL, 0));
	cJSON_AddNumberToObject(root, "statusMaxPer", vm_get_field_int(cs, "StatusMaxPer", NULL, 0));

	if (bc) {
		struct page *ap_p = vm_get_field_page(bc, "<Ap>", NULL);
		struct page *cb_p = vm_get_field_page(bc, "<Combo>", NULL);
		struct page *rd_p = vm_get_field_page(bc, "<Round>", NULL);
		cJSON_AddNumberToObject(root, "battleAp", vm_get_field_int(ap_p, "m_ap", NULL, 0));
		cJSON_AddNumberToObject(root, "battleCombo", vm_get_field_int(cb_p, "m_count", NULL, 0));
		cJSON_AddNumberToObject(root, "battleRound", vm_get_field_int(rd_p, "m_round", NULL, 1));
		cJSON_AddNumberToObject(root, "battleRoundLimit", vm_get_field_int(rd_p, "Limit", NULL, 30));
		cJSON_AddNumberToObject(root, "hannyZippoCount", vm_get_field_int(bc, "m_hannyZippoCount", NULL, 0));
	}
	if (enemy) {
		struct page *ecs = vm_get_field_page(enemy, "m_status", NULL);
		cJSON_AddNumberToObject(root, "enemyHp", vm_get_field_int(enemy, "m_hp", NULL, 0));
		cJSON_AddNumberToObject(root, "enemyMaxHp", vm_get_field_int(enemy, "<HpMax>", NULL, 0));
		cJSON_AddNumberToObject(root, "enemyAtk", vm_get_field_int(ecs, "<Atk>", NULL, 0));
	}

	cJSON_AddNumberToObject(root, "humanCount", vm_get_tt(33, 300000));
	cJSON_AddNumberToObject(root, "humanDead", vm_get_tt(34, 0));
	cJSON_AddNumberToObject(root, "humanAlivePer", vm_get_tt(35, 100));
	cJSON_AddNumberToObject(root, "humanDeadPer", vm_get_tt(36, 0));
	cJSON_AddNumberToObject(root, "curseTurn", vm_get_tt(41, 0));
	cJSON_AddNumberToObject(root, "rareLuck", vm_get_tt(42, 0));
	cJSON_AddNumberToObject(root, "hannyZippo", vm_get_tt(101, 0));
	cJSON_AddNumberToObject(root, "seaPlayerArmy", vm_get_tt(66, 0));
	cJSON_AddNumberToObject(root, "seaEnemyArmy", vm_get_tt(68, 0));
	cJSON_AddNumberToObject(root, "leazasState", vm_get_tt(13, 0));
	cJSON_AddNumberToObject(root, "helmanState", vm_get_tt(14, 0));
	cJSON_AddNumberToObject(root, "zethState", vm_get_tt(15, 0));
	cJSON_AddNumberToObject(root, "freeCitiesState", vm_get_tt(16, 0));
	cJSON_AddNumberToObject(root, "leazasPArmy", vm_get_tt(25, 300));
	cJSON_AddNumberToObject(root, "helmanPArmy", vm_get_tt(26, 300));
	cJSON_AddNumberToObject(root, "zethPArmy", vm_get_tt(27, 300));
	cJSON_AddNumberToObject(root, "freeCitiesPArmy", vm_get_tt(28, 300));
	cJSON_AddNumberToObject(root, "leazasEArmy", vm_get_tt(29, 500));
	cJSON_AddNumberToObject(root, "helmanEArmy", vm_get_tt(30, 500));
	cJSON_AddNumberToObject(root, "zethEArmy", vm_get_tt(31, 500));
	cJSON_AddNumberToObject(root, "freeCitiesEArmy", vm_get_tt(32, 500));
	cJSON_AddNumberToObject(root, "subRemnant", vm_get_tt(37, 0));
	cJSON_AddNumberToObject(root, "subDwarf", vm_get_tt(38, 0));
	cJSON_AddNumberToObject(root, "subGeneral", vm_get_tt(39, 0));
	cJSON_AddNumberToObject(root, "subHanny", vm_get_tt(40, 0));

	cJSON *c_state = cJSON_CreateArray();
	cJSON *c_parmy = cJSON_CreateArray();
	cJSON *c_earmy = cJSON_CreateArray();
	cJSON *c_sub   = cJSON_CreateArray();
	for (int i = 0; i < 4; i++) {
		cJSON_AddItemToArray(c_state, cJSON_CreateNumber(vm_get_tt(13 + i, 3)));
		cJSON_AddItemToArray(c_parmy, cJSON_CreateNumber(vm_get_tt(25 + i, 300)));
		cJSON_AddItemToArray(c_earmy, cJSON_CreateNumber(vm_get_tt(29 + i, 500)));
		cJSON_AddItemToArray(c_sub,   cJSON_CreateNumber(vm_get_tt(37 + i, 0)));
	}
	cJSON_AddItemToObject(root, "countryState", c_state);
	cJSON_AddItemToObject(root, "countryPlayerArmy", c_parmy);
	cJSON_AddItemToObject(root, "countryEnemyArmy", c_earmy);
	cJSON_AddItemToObject(root, "subArmy", c_sub);

	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return out;
}

void modifier_on_frame(void)
{
	if (!vm_is_ready() || s_in_modifier_frame)
		return;
	s_in_modifier_frame = true;

	static uint32_t s_frame_tick = 0;
	s_frame_tick++;

	maybe_load_initial_modifier_config();

#ifndef __ANDROID__
	static bool s_env_checked = false;
	static const char *s_action_file = NULL;
	static const char *s_state_file = NULL;
	if (!s_env_checked) {
		s_action_file = getenv("XSYS4_MOD_ACTION_FILE");
		s_state_file  = getenv("XSYS4_MOD_STATE_FILE");
		s_env_checked = true;
	}
	if (s_action_file && s_action_file[0] && (s_frame_tick % 10 == 1)) {
		FILE *afp = fopen(s_action_file, "rb");
		if (afp) {
			if (fseek(afp, 0, SEEK_END) == 0) {
				long sz = ftell(afp);
				if (sz > 0 && sz < 65536 && fseek(afp, 0, SEEK_SET) == 0) {
					char *abuf = malloc((size_t)sz + 1);
					if (abuf) {
						size_t rd = fread(abuf, 1, (size_t)sz, afp);
						abuf[rd] = '\0';
						pthread_mutex_lock(&g_mod_mutex);
						free(g_pending_action_json);
						g_pending_action_json = abuf;
						pthread_mutex_unlock(&g_mod_mutex);
					}
				}
			}
			fclose(afp);
			remove(s_action_file);
		}
	}
#endif

	vm_inspect_mod_streamer_ain();
	if (!s_game_config_ready) {
		if (vm_callstack_any_flag(FNO_FLAG_GAMEPLAY_SCENE) || s_frame_tick >= 60) {
			modifier_on_config_loaded();
		}
	}

	vm_restore_temp_rcs_overrides(false);
	vm_ensure_card_org_fallback_proc();

	pthread_mutex_lock(&g_mod_mutex);
	char *pending = g_pending_action_json;
	g_pending_action_json = NULL;
	int pending_lock_sm = -1;
	if (s_game_config_ready) {
		pending_lock_sm = g_pending_lock_streamer_mode;
		g_pending_lock_streamer_mode = -1;
	}
	pthread_mutex_unlock(&g_mod_mutex);

	bool did_streamer_sync = false;

	if (s_pending_clear_card_cache) {
		vm_clear_card_cache();
	}
	if (pending_lock_sm == 0 || pending_lock_sm == 1) {
		int cur_mask;
		pthread_mutex_lock(&g_mod_mutex);
		cur_mask = g_live_lock_mask;
		pthread_mutex_unlock(&g_mod_mutex);
		vm_set_streamer_mode(pending_lock_sm, true, true);
		persist_modifier_config_from_native(cur_mask);
		did_streamer_sync = true;
	}
	if (pending) {
		vm_execute_pending_action(pending);
		free(pending);
		did_streamer_sync = true;
	}
	if (s_pending_cheat_gbk) {
		vm_step_pending_cheat();
	} else if (s_pulse_key0_frames > 0) {
		key_state[VK_0] = true;
		if (--s_pulse_key0_frames == 0)
			key_state[VK_0] = false;
	}

	/*
	 * In Scenes::RunCharacterSetting, MOD cheat handlers call
	 * DialogScenes::RunInformationDialog(...) FIRST and write the unlock flag /
	 * achievement AFTER the user dismisses the popup. Once RunCharacterSetting
	 * leaves call_stack, enforce m_isShowAdvShortcut[5] == 1 + m_isSummaryMode == 0,
	 * persist GameConfig.asd & modifier_config.json, and rebuild live card views!
	 */
	bool in_rcs = vm_callstack_has_fno(s_fno_run_char_setting) ||
	              vm_callstack_has_fno(s_fno_scene_char_setting_run);
	if (s_wait_post_rcs_streamer_refresh && !s_pending_cheat_gbk && !in_rcs) {
		s_wait_post_rcs_streamer_refresh = false;
		int cur_mask;
		pthread_mutex_lock(&g_mod_mutex);
		g_live_lock_mask |= LOCK_DISABLE_STREAMER_MODE;
		cur_mask = g_live_lock_mask;
		pthread_mutex_unlock(&g_mod_mutex);
		vm_set_streamer_mode(0, true, true);
		persist_modifier_config_from_native(cur_mask);
		did_streamer_sync = true;
	}

	/*
	 * Observe in-game MENU ("SummaryCheckBox") changes to Streamer Mode after
	 * GameConfig.asd has been loaded, while protecting user-disabled Streamer
	 * Mode against non-MENU resets.
	 */
	if (s_game_config_ready &&
	    vm_has_mod_streamer_mode() &&
	    !s_pending_cheat_gbk &&
	    !s_wait_post_rcs_streamer_refresh &&
	    !in_rcs) {
		int cur_sm = vm_get_live_streamer_mode(false);
		if (cur_sm == 0 || cur_sm == 1) {
			int cur_mask;
			pthread_mutex_lock(&g_mod_mutex);
			cur_mask = g_live_lock_mask;
			pthread_mutex_unlock(&g_mod_mutex);

			if (cur_sm == 0) {
				if (s_last_observed_summary_mode != 0 ||
				    !(cur_mask & LOCK_DISABLE_STREAMER_MODE)) {
					int new_mask;
					pthread_mutex_lock(&g_mod_mutex);
					g_live_lock_mask |= LOCK_DISABLE_STREAMER_MODE;
					new_mask = g_live_lock_mask;
					pthread_mutex_unlock(&g_mod_mutex);
					vm_set_streamer_mode(0, true, true);
					persist_modifier_config_from_native(new_mask);
					did_streamer_sync = true;
				}
			} else {
				/* cur_sm == 1 */
				if ((cur_mask & LOCK_DISABLE_STREAMER_MODE) &&
				    !vm_is_common_menu_on_callstack()) {
					vm_set_streamer_mode(0, true, true);
					did_streamer_sync = true;
				} else if (s_last_observed_summary_mode != 1) {
					if (cur_mask & LOCK_DISABLE_STREAMER_MODE) {
						/* User explicitly re-checked SummaryCheckBox in CommonMenu */
						int new_mask;
						pthread_mutex_lock(&g_mod_mutex);
						g_live_lock_mask &= ~LOCK_DISABLE_STREAMER_MODE;
						new_mask = g_live_lock_mask;
						pthread_mutex_unlock(&g_mod_mutex);
						vm_set_streamer_mode(1, true, true);
						persist_modifier_config_from_native(new_mask);
						did_streamer_sync = true;
					} else {
						s_last_observed_summary_mode = 1;
					}
				}
			}
		}
	}

	if (s_pending_live_ui_refresh) {
		vm_refresh_live_streamer_ui();
	}

	pthread_mutex_lock(&g_mod_mutex);
	int lock_mask = g_live_lock_mask;
	uint32_t req_seq = g_live_state_req_seq;
	bool need_snap = (req_seq != g_live_state_built_seq) || did_streamer_sync;
	pthread_mutex_unlock(&g_mod_mutex);

	if (lock_mask & ~LOCK_DISABLE_STREAMER_MODE) {
		vm_enforce_locks(lock_mask);
	}

#ifndef __ANDROID__
	if (s_state_file && s_state_file[0] && (did_streamer_sync || (s_frame_tick % 15 == 1))) {
		need_snap = true;
	}
#endif

	/* Build live state JSON lazily on demand (when UI requests it or state synced) */
	if (need_snap) {
		char *snap = vm_build_live_state_json(lock_mask);
#ifndef __ANDROID__
		if (s_state_file && s_state_file[0] && snap) {
			char *tmp_path = NULL;
			FILE *sfp = atomic_save_open(s_state_file, "wb", &tmp_path);
			if (sfp) {
				size_t slen = strlen(snap);
				if (fwrite(snap, 1, slen, sfp) == slen)
					atomic_save_commit(sfp, s_state_file, tmp_path, false);
				else
					atomic_save_abort(sfp, tmp_path);
			}
		}
#endif
		pthread_mutex_lock(&g_mod_mutex);
		free(g_cached_live_state_json);
		g_cached_live_state_json = snap;
		g_live_state_built_seq = req_seq;
		pthread_cond_broadcast(&g_mod_cond);
		pthread_mutex_unlock(&g_mod_mutex);
	}

	s_in_modifier_frame = false;
}

/* =========================================================================
 * JNI Exports for Android UI (GameModifierBridge)
 * ========================================================================= */
#ifdef __ANDROID__

JNIEXPORT jstring JNICALL
Java_io_github_kichikuou_xsystem4_GameModifierBridge_nativeReadLocalSaveSummary(
	JNIEnv *env, jclass clazz, jstring jpath)
{
	const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
	char *json = read_local_save_summary_str(path);
	(*env)->ReleaseStringUTFChars(env, jpath, path);
	jstring ret = (*env)->NewStringUTF(env, json ? json : "{}");
	free(json);
	return ret;
}

JNIEXPORT jboolean JNICALL
Java_io_github_kichikuou_xsystem4_GameModifierBridge_nativeWriteLocalSaveMod(
	JNIEnv *env, jclass clazz, jstring jpath, jstring jmod)
{
	const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
	const char *mod  = (*env)->GetStringUTFChars(env, jmod, NULL);
	bool ok = write_local_save_mod_str(path, mod);
	(*env)->ReleaseStringUTFChars(env, jpath, path);
	(*env)->ReleaseStringUTFChars(env, jmod, mod);
	return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_io_github_kichikuou_xsystem4_GameModifierBridge_nativeReadGlobalSaveSummary(
	JNIEnv *env, jclass clazz, jstring jglobal_path, jstring jcollection_path)
{
	const char *gp = jglobal_path ? (*env)->GetStringUTFChars(env, jglobal_path, NULL) : "";
	const char *cp = jcollection_path ? (*env)->GetStringUTFChars(env, jcollection_path, NULL) : "";
	char *json = read_global_save_summary_str(gp, cp);
	if (jglobal_path) (*env)->ReleaseStringUTFChars(env, jglobal_path, gp);
	if (jcollection_path) (*env)->ReleaseStringUTFChars(env, jcollection_path, cp);
	jstring ret = (*env)->NewStringUTF(env, json ? json : "{}");
	free(json);
	return ret;
}

JNIEXPORT jboolean JNICALL
Java_io_github_kichikuou_xsystem4_GameModifierBridge_nativeWriteGlobalSaveMod(
	JNIEnv *env, jclass clazz, jstring jglobal_path, jstring jcollection_path, jstring jmod)
{
	const char *gp  = jglobal_path ? (*env)->GetStringUTFChars(env, jglobal_path, NULL) : "";
	const char *cp  = jcollection_path ? (*env)->GetStringUTFChars(env, jcollection_path, NULL) : "";
	const char *mod = (*env)->GetStringUTFChars(env, jmod, NULL);
	bool ok = write_global_save_mod_str(gp, cp, mod);
	if (jglobal_path) (*env)->ReleaseStringUTFChars(env, jglobal_path, gp);
	if (jcollection_path) (*env)->ReleaseStringUTFChars(env, jcollection_path, cp);
	(*env)->ReleaseStringUTFChars(env, jmod, mod);
	return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_io_github_kichikuou_xsystem4_GameModifierBridge_nativeGetLiveStateJson(
	JNIEnv *env, jclass clazz)
{
	(void)clazz;
	pthread_mutex_lock(&g_mod_mutex);
	if (vm_is_ready()) {
		uint32_t target_seq = ++g_live_state_req_seq;
		struct timespec ts;
		if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
			ts.tv_nsec += 25000000L; /* 25ms max wait (~1.5 frames at 60fps) */
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec += 1;
				ts.tv_nsec -= 1000000000L;
			}
			while (g_live_state_built_seq != target_seq) {
				if (pthread_cond_timedwait(&g_mod_cond, &g_mod_mutex, &ts) == ETIMEDOUT)
					break;
			}
		}
	}
	const char *snap = g_cached_live_state_json ? g_cached_live_state_json : "{\"inGame\":false}";
	jstring ret = (*env)->NewStringUTF(env, snap);
	pthread_mutex_unlock(&g_mod_mutex);
	return ret;
}

JNIEXPORT void JNICALL
Java_io_github_kichikuou_xsystem4_GameModifierBridge_nativeSetConfigPath(
	JNIEnv *env, jclass clazz, jstring jpath)
{
	(void)clazz;
	if (!jpath)
		return;
	const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
	if (path && path[0]) {
		pthread_mutex_lock(&g_mod_mutex);
		free(g_modifier_config_path);
		g_modifier_config_path = strdup(path);
		pthread_mutex_unlock(&g_mod_mutex);
	}
	(*env)->ReleaseStringUTFChars(env, jpath, path);
}

JNIEXPORT void JNICALL
Java_io_github_kichikuou_xsystem4_GameModifierBridge_nativeSetLiveLocks(
	JNIEnv *env, jclass clazz, jint mask)
{
	(void)env;
	(void)clazz;
	pthread_mutex_lock(&g_mod_mutex);
	int prev = g_live_lock_mask;
	bool first_init = !g_has_lock_mask_init;
	g_has_lock_mask_init = true;
	g_live_lock_mask = (int)mask;
	bool prev_dis = (prev & LOCK_DISABLE_STREAMER_MODE) != 0;
	bool next_dis = ((int)mask & LOCK_DISABLE_STREAMER_MODE) != 0;
	if (first_init) {
		if (next_dis)
			g_pending_lock_streamer_mode = 0;
	} else if (prev_dis != next_dis) {
		g_pending_lock_streamer_mode = next_dis ? 0 : 1;
	}
	pthread_mutex_unlock(&g_mod_mutex);
}

JNIEXPORT jint JNICALL
Java_io_github_kichikuou_xsystem4_GameModifierBridge_nativeGetLiveLocks(
	JNIEnv *env, jclass clazz)
{
	pthread_mutex_lock(&g_mod_mutex);
	int mask = g_live_lock_mask;
	pthread_mutex_unlock(&g_mod_mutex);
	return (jint)mask;
}

JNIEXPORT void JNICALL
Java_io_github_kichikuou_xsystem4_GameModifierBridge_nativeApplyLiveAction(
	JNIEnv *env, jclass clazz, jstring jaction)
{
	const char *act = (*env)->GetStringUTFChars(env, jaction, NULL);
	pthread_mutex_lock(&g_mod_mutex);
	free(g_pending_action_json);
	g_pending_action_json = strdup(act);
	pthread_mutex_unlock(&g_mod_mutex);
	(*env)->ReleaseStringUTFChars(env, jaction, act);
}

JNIEXPORT jint JNICALL
Java_io_github_kichikuou_xsystem4_GameModifierBridge_nativeReadStreamerMode(
	JNIEnv *env, jclass clazz, jstring jpath)
{
	if (!jpath) return -1;
	const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
	int res = read_game_config_streamer_mode(path);
	(*env)->ReleaseStringUTFChars(env, jpath, path);
	return (jint)res;
}

JNIEXPORT jboolean JNICALL
Java_io_github_kichikuou_xsystem4_GameModifierBridge_nativeWriteStreamerMode(
	JNIEnv *env, jclass clazz, jstring jpath, jboolean jenabled)
{
	if (!jpath) return JNI_FALSE;
	const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
	bool ok = write_game_config_streamer_mode(path, jenabled == JNI_TRUE);
	(*env)->ReleaseStringUTFChars(env, jpath, path);
	return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_io_github_kichikuou_xsystem4_GameModifierBridge_nativeSetCtrlSkip(
	JNIEnv *env, jclass clazz, jboolean jenabled)
{
	(void)env;
	(void)clazz;
	input_set_touch_ctrl_skip(jenabled == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_io_github_kichikuou_xsystem4_GameModifierBridge_nativeGetCtrlSkip(
	JNIEnv *env, jclass clazz)
{
	(void)env;
	(void)clazz;
	return input_get_touch_ctrl_skip() ? JNI_TRUE : JNI_FALSE;
}

#endif /* __ANDROID__ */
