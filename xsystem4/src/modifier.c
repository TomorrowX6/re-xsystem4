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
#include "modifier.h"

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

/* GBK / SJIS names for special Rance 10 globals */
#define GBK_BATTLE_COUNT  "\xa1\xf4\xd5\xbd\xb6\xb7\xbb\xd8\xca\xfd"
#define SJIS_BATTLE_COUNT "\x81\x9f\x90\xed\x93\xac\x89\xf1\x90\x94"
#define GBK_DIFFICULTY    "\xa1\xf4\xc4\xd1\xb6\xc8"
#define SJIS_DIFFICULTY   "\x81\x9f\x93\xef\x88\xd5\x93\x78"

static pthread_mutex_t g_mod_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_live_lock_mask = 0;
static char *g_pending_action_json = NULL;
static char *g_cached_live_state_json = NULL;

/* =========================================================================
 * Out-of-Game gsave (.asd) Helpers
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

	FILE *fp = file_open_utf8(path, "wb");
	bool ok = false;
	if (fp) {
		ok = (gsave_write(gs, fp, false, 1) == SAVEFILE_SUCCESS);
		fclose(fp);
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
			FILE *fp = file_open_utf8(global_path, "wb");
			if (fp) {
				if (gsave_write(gs_g, fp, false, 1) == SAVEFILE_SUCCESS) any_ok = true;
				fclose(fp);
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
			FILE *fp = file_open_utf8(collection_path, "wb");
			if (fp) {
				if (gsave_write(gs_c, fp, false, 1) == SAVEFILE_SUCCESS) any_ok = true;
				fclose(fp);
			}
			gsave_free(gs_c);
		}
	}

	cJSON_Delete(mod);
	return any_ok;
}

/* =========================================================================
 * Live In-Game VM Memory Helpers (Executed on VM Thread in modifier_on_frame)
 * ========================================================================= */

static int vm_find_global_idx(const char *name1, const char *name2)
{
	if (!ain || !ain->globals) return -1;
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
	if (idx < 0) return NULL;
	union vm_value v = global_get(idx);
	if (v.i <= 0) return NULL;
	return heap_get_page(v.i);
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
	if (!v || v->i <= 0) return NULL;
	return heap_get_page(v->i);
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

static void vm_boost_card_array(struct page *arr, int set_count, int add_count, int atk_bonus, int hp_bonus, int *total_cards)
{
	if (!arr || arr->type != ARRAY_PAGE) return;
	for (int i = 0; i < arr->nr_vars; i++) {
		struct page *c_page = heap_get_page(arr->values[i].i);
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
			struct page *org_page = heap_get_page(org_arr->values[i].i);
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
		struct page *c_page = heap_get_page(arr->values[i].i);
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
		struct page *lc = heap_get_page(leaders_arr->values[i].i);
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
					struct page *scv = heap_get_page(sc_arr->values[k].i);
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

	cJSON_Delete(act);
}

static void vm_enforce_locks(int mask)
{
	if (!mask || !ain) return;

	struct page *pcp   = vm_get_global_page("g_playerCommonParam", NULL);
	struct page *party = vm_get_global_page("g_party", NULL);
	struct page *ps    = vm_get_field_page(party, "m_status", NULL);
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
}

static char *vm_build_live_state_json(int lock_mask)
{
	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "lockMask", lock_mask);

	struct page *gc    = vm_get_global_page("g_gameContext", NULL);
	struct page *pcp   = vm_get_global_page("g_playerCommonParam", NULL);
	struct page *pb    = vm_get_global_page("g_partyBonus", NULL);
	struct page *party = vm_get_global_page("g_party", NULL);
	struct page *ps    = vm_get_field_page(party, "m_status", NULL);
	struct page *cs    = vm_get_field_page(ps, "m_status", NULL);
	struct page *bc    = vm_get_global_page("g_battleContext", NULL);
	struct page *enemy = vm_get_global_page("g_enemy", NULL);

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

	cJSON_AddNumberToObject(root, "cardCount", vm_process_all_cards(0, 0, 0, 0));
	cJSON_AddNumberToObject(root, "charCount", vm_process_all_characters(0, 0));

	char *out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return out;
}

void modifier_on_frame(void)
{
	static uint32_t s_frame_tick = 0;
	s_frame_tick++;

	pthread_mutex_lock(&g_mod_mutex);
	char *pending = g_pending_action_json;
	g_pending_action_json = NULL;
	int lock_mask = g_live_lock_mask;
	pthread_mutex_unlock(&g_mod_mutex);

	if (pending) {
		vm_execute_pending_action(pending);
		free(pending);
	}
	if (lock_mask) {
		vm_enforce_locks(lock_mask);
	}

	/* Refresh live snapshot when action executed or every 15 frames */
	if (pending || (s_frame_tick % 15 == 1)) {
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
	pthread_mutex_lock(&g_mod_mutex);
	g_live_lock_mask = (int)mask;
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

#endif /* __ANDROID__ */
