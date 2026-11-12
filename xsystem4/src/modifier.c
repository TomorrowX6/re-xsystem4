#define VM_PRIVATE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __ANDROID__
#include <jni.h>
#endif

#include "cJSON.h"
#include "system4.h"
#include "system4/ain.h"
#include "system4/file.h"
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
static bool s_pending_clear_card_cache = false;

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
/* GBK for "灌注Eins陈谢谢喵" (MOD streamer-mode unlock passphrase) */
#define GBK_STREAMER_PASSPHRASE "\xb9\xe0\xd7\xa2\x45\x69\x6e\x73\xb3\xc2\xd0\xbb\xd0\xbb\xdf\xf7"

static pthread_mutex_t g_mod_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_live_lock_mask = LOCK_DISABLE_STREAMER_MODE;
static bool g_force_refresh_streamer_off = false;
static char *g_pending_action_json = NULL;
static char *g_cached_live_state_json = NULL;

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
	if (kv->value == target) {
		gsave_free(gs);
		if (!ain)
			sys4_set_text_gbk(prev_gbk);
		return true;
	}
	kv->value = target;

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

static bool vm_is_in_card_cache_callstack(void)
{
	if (!ain || !ain->functions || call_stack_ptr <= 0)
		return false;
	for (int i = 0; i < call_stack_ptr; i++) {
		int32_t fno = call_stack[i].fno;
		if (fno < 0 || fno >= ain->nr_functions)
			continue;
		const char *fn = ain->functions[fno].name;
		if (!fn)
			continue;
		if (!strncmp(fn, "CardCache@", 10) ||
		    !strncmp(fn, "CardConstruct", 13) ||
		    !strncmp(fn, "KeyValueMap<", 12)) {
			return true;
		}
	}
	return false;
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
	struct page *cc = vm_get_global_page("g_cardCache", NULL);
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

static void vm_set_streamer_mode(int enabled, bool persist_disk, bool force_clear_cache)
{
	int target = enabled ? 1 : 0;
	bool changed = false;
	struct page *cfg = vm_get_global_page("g_gameConfig", NULL);
	if (cfg) {
		int32_t prev = vm_get_field_int(cfg, "m_isSummaryMode", NULL, -1);
		if (prev != target) {
			vm_set_field_int(cfg, "m_isSummaryMode", NULL, target);
			changed = true;
		}
	}
	if (changed || force_clear_cache) {
		vm_clear_card_cache();
	}
	if (persist_disk) {
		persist_streamer_mode_to_savedir(target != 0);
	}
}

void modifier_on_globals_loaded(void)
{
	if (!vm_globals_ready())
		return;
	pthread_mutex_lock(&g_mod_mutex);
	int mask = g_live_lock_mask;
	pthread_mutex_unlock(&g_mod_mutex);
	if (mask & LOCK_DISABLE_STREAMER_MODE) {
		vm_set_streamer_mode(0, true, false);
	}
}

static int32_t vm_get_tt(int idx, int32_t def_val)
{
	struct page *tt = vm_get_global_page("tt", NULL);
	if (!tt || tt->type != ARRAY_PAGE || idx < 0 || idx >= tt->nr_vars) return def_val;
	return tt->values[idx].i;
}

static void vm_set_tt(int idx, int32_t val)
{
	struct page *tt = vm_get_global_page("tt", NULL);
	if (!tt || tt->type != ARRAY_PAGE || idx < 0 || idx >= tt->nr_vars) return;
	tt->values[idx].i = val;
}

/*
 * In 日不落 MOD, Scenes::RunCharacterSetting gates almost all cheat codes behind
 * tt[1224] == 1 (which challenge modes / base phase A reset to 0, showing
 * "作弊码禁用！当前状态或模式下无法使用！"). Setting tt[1224] = 1 unlocks the gate
 * cleanly without wiping the player's tt[6655] bonus score to -99999.
 */
static void vm_unlock_mod_cheat_gate(void)
{
	struct page *tt = vm_get_global_page("tt", NULL);
	if (tt && tt->type == ARRAY_PAGE && tt->nr_vars > 1224) {
		tt->values[1224].i = 1;
	}
}

static void vm_queue_cheat_code_utf8(const char *utf8_str)
{
	if (!utf8_str || !utf8_str[0]) return;
	if (!strcmp(utf8_str, "关闭主播模式") ||
	    !strcmp(utf8_str, "灌注Eins陈谢谢喵") ||
	    !strcmp(utf8_str, "关注Eins陈谢谢喵")) {
		pthread_mutex_lock(&g_mod_mutex);
		g_live_lock_mask |= LOCK_DISABLE_STREAMER_MODE;
		pthread_mutex_unlock(&g_mod_mutex);
		vm_set_streamer_mode(0, true, true);
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

	bool in_run_char_setting = false;
	int scs_frame = -1;
	struct page *scs_page = NULL;

	for (int i = 0; i < call_stack_ptr; i++) {
		int32_t fno = call_stack[i].fno;
		if (fno < 0 || fno >= ain->nr_functions)
			continue;
		const char *fname = ain->functions[fno].name;
		if (!fname)
			continue;
		if (!strcmp(fname, "Scenes::RunCharacterSetting")) {
			in_run_char_setting = true;
		} else if (!strcmp(fname, "SceneCharacterSetting@Run")) {
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
			if (fno < 0 || fno >= ain->nr_functions)
				continue;
			const char *fname = ain->functions[fno].name;
			if (fname && !strcmp(fname, "SceneStack@Join")) {
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
			vm_unlock_mod_cheat_gate();
			union vm_value *cur_v = vm_get_struct_member(scs_page, "m_current", NULL);
			if (cur_v) {
				vm_set_string_slot(cur_v->i, s_pending_cheat_gbk);
			}
			struct page *gc = vm_get_global_page("g_gameContext", NULL);
			vm_set_player_info_name(gc, s_pending_cheat_gbk);
			vm_set_field_int(scs_ss, "<IsFinish>", "m_isFinish", 1);
			int idx_end_busy = vm_find_global_idx("parts::detail::g_EndPartsBusyLoop", NULL);
			if (idx_end_busy >= 0)
				global_set(idx_end_busy, vm_int(1), false);
			int idx_end_num = vm_find_global_idx("parts::detail::g_EndPartsBusyLoopNumber", NULL);
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

	struct page *gc    = vm_get_global_page("g_gameContext", NULL);
	struct page *pcp   = vm_get_global_page("g_playerCommonParam", NULL);
	struct page *pb    = vm_get_global_page("g_partyBonus", NULL);
	struct page *party = vm_get_global_page("g_party", NULL);
	struct page *ps    = vm_get_field_page(party, "m_status", NULL);
	struct page *cs    = vm_get_field_page(ps, "m_status", NULL);
	struct page *bc    = vm_get_global_page("g_battleContext", NULL);
	struct page *enemy = vm_get_global_page("g_enemy", NULL);

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
		int idx = vm_find_global_idx(GBK_DIFFICULTY, SJIS_DIFFICULTY);
		if (idx >= 0) global_set(idx, vm_int(it->valueint), false);
	}
	if ((it = cJSON_GetObjectItem(act, "battleCount")) && cJSON_IsNumber(it)) {
		int idx = vm_find_global_idx(GBK_BATTLE_COUNT, SJIS_BATTLE_COUNT);
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

	if ((it = cJSON_GetObjectItem(act, "streamerMode")) && cJSON_IsNumber(it)) {
		int en = it->valueint != 0 ? 1 : 0;
		pthread_mutex_lock(&g_mod_mutex);
		if (en == 0)
			g_live_lock_mask |= LOCK_DISABLE_STREAMER_MODE;
		else
			g_live_lock_mask &= ~LOCK_DISABLE_STREAMER_MODE;
		pthread_mutex_unlock(&g_mod_mutex);
		vm_set_streamer_mode(en, true, true);
	}
	if ((it = cJSON_GetObjectItem(act, "cheatCode")) && cJSON_IsString(it) &&
	    it->valuestring && it->valuestring[0]) {
		vm_queue_cheat_code_utf8(it->valuestring);
	} else if ((it = cJSON_GetObjectItem(act, "fillStreamerPassphrase")) &&
	           (cJSON_IsTrue(it) || (cJSON_IsNumber(it) && it->valueint != 0))) {
		pthread_mutex_lock(&g_mod_mutex);
		g_live_lock_mask |= LOCK_DISABLE_STREAMER_MODE;
		pthread_mutex_unlock(&g_mod_mutex);
		vm_set_streamer_mode(0, true, true);
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
	if (!mask || !ain) return;

	if (mask & LOCK_DISABLE_STREAMER_MODE) {
		struct page *cfg = vm_get_global_page("g_gameConfig", NULL);
		if (cfg && vm_get_field_int(cfg, "m_isSummaryMode", NULL, 0) != 0) {
			vm_set_streamer_mode(0, true, true);
		}
	}

	struct page *pcp   = vm_get_global_page("g_playerCommonParam", NULL);
	struct page *party = vm_get_global_page("g_party", NULL);
	struct page *ps    = vm_get_field_page(party, "m_status", NULL);
	struct page *cs    = vm_get_field_page(ps, "m_status", NULL);
	struct page *bc    = vm_get_global_page("g_battleContext", NULL);
	struct page *enemy = vm_get_global_page("g_enemy", NULL);

	if ((mask & LOCK_PARTY_MAX_HP) && party && ps) {
		int32_t max_hp = vm_get_field_int(ps, "m_totalHp", NULL, 9999);
		if (max_hp < 1) max_hp = 9999;
		if (vm_get_field_int(party, "m_hp", NULL, 0) < max_hp) {
			vm_set_field_int(party, "m_hp", NULL, max_hp);
			union vm_value *hr = vm_get_struct_member(party, "m_hpRatio", NULL);
			if (hr) hr->f = 1.0f;
		}
	}
	if ((mask & LOCK_INFINITE_AP) && bc) {
		struct page *ap_p = vm_get_field_page(bc, "<Ap>", NULL);
		if (ap_p && vm_get_field_int(ap_p, "m_ap", NULL, 0) < 99) {
			vm_set_field_int(ap_p, "m_ap", NULL, 99);
		}
	}
	if ((mask & LOCK_BATTLE_COMBO) && bc) {
		struct page *cb_p = vm_get_field_page(bc, "<Combo>", NULL);
		if (cb_p && vm_get_field_int(cb_p, "m_count", NULL, 0) < 999) {
			vm_set_field_int(cb_p, "m_count", NULL, 999);
		}
	}
	if ((mask & LOCK_BATTLE_ROUND_1) && bc) {
		struct page *rd_p = vm_get_field_page(bc, "<Round>", NULL);
		if (rd_p) {
			if (vm_get_field_int(rd_p, "m_round", NULL, 1) > 1)
				vm_set_field_int(rd_p, "m_round", NULL, 1);
			if (vm_get_field_int(rd_p, "Limit", NULL, 30) < 99)
				vm_set_field_int(rd_p, "Limit", NULL, 99);
		}
	}
	if ((mask & LOCK_ONE_HIT_KILL) && enemy) {
		if (vm_get_field_int(enemy, "m_hp", NULL, 0) > 1) {
			vm_set_field_int(enemy, "m_hp", NULL, 1);
		}
	}
	if ((mask & LOCK_ENEMY_ZERO_ATK) && enemy) {
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
	if ((mask & LOCK_LEADER_BOOST_CRIT) || (mask & LOCK_INFINITE_SKILL_USE)) {
		vm_cure_and_boost_leaders((mask & LOCK_LEADER_BOOST_CRIT) != 0,
		                          (mask & LOCK_INFINITE_SKILL_USE) != 0);
	}
	if (mask & LOCK_INFINITE_ZIPPO) {
		if (pcp) vm_set_field_int(pcp, "<IsUsedHannyZippo>", NULL, 0);
		if (bc) {
			if (vm_get_field_int(bc, "m_hannyZippoCount", NULL, 0) < 9)
				vm_set_field_int(bc, "m_hannyZippoCount", NULL, 9);
			vm_set_field_int(bc, "m_curseTurn", NULL, 0);
		}
	}
	if ((mask & LOCK_MAX_RESOURCES) && pcp) {
		if (vm_get_field_int(pcp, "m_foodTicket", NULL, 0) < 999)
			vm_set_field_int(pcp, "m_foodTicket", NULL, 999);
		if (vm_get_field_int(pcp, "m_honor", NULL, 0) < 999)
			vm_set_field_int(pcp, "m_honor", NULL, 999);
		if (vm_get_field_int(pcp, "m_friendPoint", NULL, 0) < 999)
			vm_set_field_int(pcp, "m_friendPoint", NULL, 999);
	}
	if (mask & LOCK_TREASURE_100) {
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

	struct page *gc    = vm_get_global_page("g_gameContext", NULL);
	struct page *cfg   = vm_get_global_page("g_gameConfig", NULL);
	struct page *pcp   = vm_get_global_page("g_playerCommonParam", NULL);
	struct page *pb    = vm_get_global_page("g_partyBonus", NULL);
	struct page *party = vm_get_global_page("g_party", NULL);
	struct page *ps    = vm_get_field_page(party, "m_status", NULL);
	struct page *cs    = vm_get_field_page(ps, "m_status", NULL);
	struct page *bc    = vm_get_global_page("g_battleContext", NULL);
	struct page *enemy = vm_get_global_page("g_enemy", NULL);

	if (cfg) {
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

	int idx_diff = vm_find_global_idx(GBK_DIFFICULTY, SJIS_DIFFICULTY);
	int idx_bc   = vm_find_global_idx(GBK_BATTLE_COUNT, SJIS_BATTLE_COUNT);
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
	if (!vm_is_ready())
		return;

	static uint32_t s_frame_tick = 0;
	s_frame_tick++;

#ifndef __ANDROID__
	static bool s_env_checked = false;
	static const char *s_action_file = NULL;
	if (!s_env_checked) {
		s_action_file = getenv("XSYS4_MOD_ACTION_FILE");
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

	pthread_mutex_lock(&g_mod_mutex);
	char *pending = g_pending_action_json;
	g_pending_action_json = NULL;
	bool force_streamer_off = g_force_refresh_streamer_off;
	g_force_refresh_streamer_off = false;
	pthread_mutex_unlock(&g_mod_mutex);

	if (s_pending_clear_card_cache) {
		vm_clear_card_cache();
	}
	if (force_streamer_off) {
		vm_set_streamer_mode(0, true, true);
	}
	if (pending) {
		vm_execute_pending_action(pending);
		free(pending);
	}
	if (s_pending_cheat_gbk) {
		vm_step_pending_cheat();
	} else if (s_pulse_key0_frames > 0) {
		key_state[VK_0] = true;
		if (--s_pulse_key0_frames == 0)
			key_state[VK_0] = false;
	}

	pthread_mutex_lock(&g_mod_mutex);
	int lock_mask = g_live_lock_mask;
	pthread_mutex_unlock(&g_mod_mutex);

	if (lock_mask) {
		vm_enforce_locks(lock_mask);
	}

	/* Refresh live snapshot when action executed or every 30 frames */
	if (pending || force_streamer_off || (s_frame_tick % 30 == 1)) {
		char *snap = vm_build_live_state_json(lock_mask);
		pthread_mutex_lock(&g_mod_mutex);
		free(g_cached_live_state_json);
		g_cached_live_state_json = snap;
		pthread_mutex_unlock(&g_mod_mutex);
	}
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
	pthread_mutex_lock(&g_mod_mutex);
	const char *snap = g_cached_live_state_json ? g_cached_live_state_json : "{\"inGame\":false}";
	jstring ret = (*env)->NewStringUTF(env, snap);
	pthread_mutex_unlock(&g_mod_mutex);
	return ret;
}

JNIEXPORT void JNICALL
Java_io_github_kichikuou_xsystem4_GameModifierBridge_nativeSetLiveLocks(
	JNIEnv *env, jclass clazz, jint mask)
{
	(void)env;
	(void)clazz;
	pthread_mutex_lock(&g_mod_mutex);
	int prev = g_live_lock_mask;
	g_live_lock_mask = (int)mask;
	if (!(prev & LOCK_DISABLE_STREAMER_MODE) && (mask & LOCK_DISABLE_STREAMER_MODE)) {
		g_force_refresh_streamer_off = true;
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
