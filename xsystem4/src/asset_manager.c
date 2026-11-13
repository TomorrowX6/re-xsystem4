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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <assert.h>

#include "system4.h"
#include "system4/ald.h"
#include "system4/afa.h"
#include "system4/cg.h"
#include "system4/file.h"
#include "system4/hashtable.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

#include "xsystem4.h"
#include "asset_manager.h"
#include "gfx/font.h"

enum archive_type {
	AR_TYPE_ALD,
	AR_TYPE_AFA,
	AR_TYPE_MAX
};

const char *asset_strtype(enum asset_type type)
{
	switch (type) {
	case ASSET_BGM:   return "BGM";
	case ASSET_SOUND: return "Sound";
	case ASSET_VOICE: return "Voice";
	case ASSET_CG:    return "CG";
	case ASSET_FLAT:  return "Flat";
	case ASSET_PACT:  return "Pact";
	case ASSET_DATA:  return "Data";
	case ASSET_FLASH: return "Flash";
	}
	return "Invalid";
}

struct asset_manager {
	bool (*load_archive)(struct asset_manager *manager, const char *name);
	bool (*exists_by_id)(struct asset_manager *manager, int id);
	bool (*exists_by_name)(struct asset_manager *manager, const char *name, int *id_out);
	struct archive_data *(*get_by_id)(struct asset_manager *manager, int id);
	struct archive_data *(*get_by_name)(struct asset_manager *manager, const char *name, int *id_out);
};

// XXX: also used for AFAv1 in Daiteikoku, Shaman's Sanctuary
struct asset_manager_ald {
	struct asset_manager manager;
	struct archive *archive;
};

struct asset_manager_afa {
	struct asset_manager manager;
	struct afa_archive **archives;
	size_t nr_archives;
	struct afa_archive **id_archives;
	size_t nr_id_archives;
};

static struct asset_manager *assets[ASSET_TYPE_MAX] = {0};
static struct asset_manager_afa *afa_create_manager(void);

bool asset_manager_load_archive(enum asset_type type, const char *archive_name)
{
	if (type < 0 || type >= ASSET_TYPE_MAX) return false;
	if (!assets[type]) assets[type] = &afa_create_manager()->manager;
	if (!assets[type]->load_archive)
		ERROR("load_archive not supported on this archive type");
	char rel_name[PATH_MAX];
	snprintf(rel_name, sizeof(rel_name), "%s.afa", archive_name);
	char *path = gamedir_path_icase(rel_name);
	if (!path || !file_exists(path)) {
		free(path);
		path = gamedir_path(archive_name);
	}
	bool r = assets[type]->load_archive(assets[type], path);
	free(path);
	if (r && type == ASSET_CG) {
		size_t len = strlen(archive_name);
		if (len >= 3 && !strcasecmp(archive_name + len - 3, "CG5")) {
			char cg6_rel[PATH_MAX];
			snprintf(cg6_rel, sizeof(cg6_rel), "%.*s6.afa", (int)(len - 1), archive_name);
			char *cg6_path = gamedir_path_icase(cg6_rel);
			if (cg6_path && file_exists(cg6_path))
				assets[type]->load_archive(assets[type], cg6_path);
			free(cg6_path);
		}
	}
	return r;
}

bool asset_exists(enum asset_type type, int id)
{
	if (!assets[type])
		return false;
	return assets[type]->exists_by_id(assets[type], id);
}

static bool asset_cg_format_special_org_fallback(const char *name, char *buf, size_t buf_size)
{
	const char *needle = sys4_text_is_gbk
		? "\xcc\xd8\xca\xe2\xcb\xf9\xca\xf4"  /* 特殊所属 (GBK) */
		: "\x93\xc1\x8e\xea\x8f\x8a\x91\xae"; /* 特殊所属 (SJIS) */
	const char *repl = sys4_text_is_gbk
		? "\xc6\xe4\xcb\xfb\xcb\xf9\xca\xf4"          /* 其他所属 (GBK) */
		: "\x82\xbb\x82\xcc\x91\xbc\x8f\x8a\x91\xae"; /* その他所属 (SJIS) */
	size_t needle_len = 8;
	size_t repl_len = sys4_text_is_gbk ? 8 : 10;

	for (const char *p = name; *p; p = sjis_skip_char(p)) {
		if (!memcmp(p, needle, needle_len)) {
			size_t prefix_len = (size_t)(p - name);
			const char *suffix = p + needle_len;
			size_t suffix_len = strlen(suffix);
			if (prefix_len + repl_len + suffix_len + 1 > buf_size)
				return false;
			memcpy(buf, name, prefix_len);
			memcpy(buf + prefix_len, repl, repl_len);
			memcpy(buf + prefix_len + repl_len, suffix, suffix_len + 1);
			return true;
		}
	}
	return false;
}

bool asset_exists_by_name(enum asset_type type, const char *name, int *id_out)
{
	if (!assets[type] || !name || !name[0])
		return false;
	if (!assets[type]->exists_by_name)
		ERROR("exists_by_name not supported on this archive type");
	if (assets[type]->exists_by_name(assets[type], name, id_out))
		return true;
	char fallback[512];
	if (type == ASSET_CG && asset_cg_format_special_org_fallback(name, fallback, sizeof(fallback)))
		return assets[type]->exists_by_name(assets[type], fallback, id_out);
	return false;
}

struct archive_data *asset_get(enum asset_type type, int id)
{
	if (!assets[type])
		return NULL;
	return assets[type]->get_by_id(assets[type], id);
}

struct archive_data *asset_get_by_name(enum asset_type type, const char *name, int *id_out)
{
	if (!assets[type] || !name || !name[0])
		return NULL;
	if (!assets[type]->get_by_name)
		ERROR("get_by_name not supported on this archive type");
	struct archive_data *data = assets[type]->get_by_name(assets[type], name, id_out);
	if (data)
		return data;
	char fallback[512];
	if (type == ASSET_CG && asset_cg_format_special_org_fallback(name, fallback, sizeof(fallback)))
		return assets[type]->get_by_name(assets[type], fallback, id_out);
	return NULL;
}

struct cg *asset_cg_load(int id)
{
	struct archive_data *data = asset_get(ASSET_CG, id);
	if (!data)
		return NULL;
	struct cg *cg = cg_load_data(data);
	archive_free_data(data);
	return cg;
}

struct cg *asset_cg_load_by_name(const char *name, int *id_out)
{
	struct archive_data *data = asset_get_by_name(ASSET_CG, name, id_out);
	if (!data)
		return NULL;
	struct cg *cg = cg_load_data(data);
	archive_free_data(data);
	return cg;
}

bool asset_cg_get_metrics(int id, struct cg_metrics *metrics)
{
	struct archive_data *data = asset_get(ASSET_CG, id);
	if (!data)
		return false;
	bool r = cg_get_metrics_data(data, metrics);
	archive_free_data(data);
	return r;
}

bool asset_cg_get_metrics_by_name(const char *name, struct cg_metrics *metrics)
{
	struct archive_data *data = asset_get_by_name(ASSET_CG, name, NULL);
	if (!data)
		return false;
	bool r = cg_get_metrics_data(data, metrics);
	archive_free_data(data);
	return r;
}

static bool ald_exists_by_id(struct asset_manager *_manager, int id)
{
	struct asset_manager_ald *manager = (struct asset_manager_ald*)_manager;
	return archive_exists(manager->archive, id - 1);
}

static struct archive_data *ald_get_by_id(struct asset_manager *_manager, int id)
{
	struct asset_manager_ald *manager = (struct asset_manager_ald*)_manager;
	return archive_get(manager->archive, id - 1);
}

static bool afa_is_mod_archive(const char *filepath)
{
	const char *base = strrchr(filepath, '/');
	base = base ? base + 1 : filepath;
	for (const char *p = base; p[0] && p[1] && p[2]; p++) {
		if (tolower((unsigned char)p[0]) == 'm' &&
		    tolower((unsigned char)p[1]) == 'o' &&
		    tolower((unsigned char)p[2]) == 'd')
			return true;
	}
	return false;
}

static size_t afa_find_insert_pos(struct asset_manager_afa *manager, const char *filepath)
{
	if (afa_is_mod_archive(filepath))
		return 0;
	size_t pos = 0;
	while (pos < manager->nr_archives && afa_is_mod_archive(manager->archives[pos]->filename))
		pos++;
	return pos;
}

static bool afa_load_archive(struct asset_manager *_manager, const char *name)
{
	struct asset_manager_afa *manager = (struct asset_manager_afa*)_manager;
	char path[PATH_MAX];
	size_t name_len = strlen(name);
	if (name_len >= 4 && !strcasecmp(name + name_len - 4, ".afa"))
		snprintf(path, PATH_MAX, "%s", name);
	else
		snprintf(path, PATH_MAX, "%s.afa", name);
	for (size_t i = 0; i < manager->nr_archives; i++) {
		if (!strcasecmp(manager->archives[i]->filename, path))
			return true;
	}

	int error;
	struct afa_archive *ar = afa_open(path, MMAP_IF_64BIT, &error);
	if (!ar) {
		snprintf(path, PATH_MAX, "%s.AFA", name);
		ar = afa_open(path, MMAP_IF_64BIT, &error);
		if (!ar) {
			WARNING("Failed to open archive: %s", display_utf0(path));
			return false;
		}
	}

	size_t pos = afa_find_insert_pos(manager, path);
	manager->archives = xrealloc(manager->archives,
		(manager->nr_archives + 1) * sizeof(*manager->archives));
	memmove(manager->archives + pos + 1, manager->archives + pos,
		(manager->nr_archives - pos) * sizeof(*manager->archives));
	manager->archives[pos] = ar;
	manager->nr_archives++;
	manager->id_archives = xrealloc(manager->id_archives,
		(manager->nr_id_archives + 1) * sizeof(*manager->id_archives));
	manager->id_archives[manager->nr_id_archives++] = ar;
	return true;
}

static size_t afa_register_id_archive(struct asset_manager_afa *manager, struct afa_archive *ar)
{
	for (size_t i = 0; i < manager->nr_id_archives; i++) {
		if (manager->id_archives[i] == ar)
			return i;
	}
	manager->id_archives = xrealloc(manager->id_archives,
		(manager->nr_id_archives + 1) * sizeof(*manager->id_archives));
	manager->id_archives[manager->nr_id_archives] = ar;
	return manager->nr_id_archives++;
}

static int afa_encode_id(struct asset_manager_afa *manager, struct afa_archive *ar, int local_no)
{
	size_t idx = afa_register_id_archive(manager, ar);
	if (idx > 127)
		idx = 0;
	return ((int)idx << 24) | ((local_no + 1) & 0x00ffffff);
}

static bool afa_exists_by_id(struct asset_manager *_manager, int id)
{
	struct asset_manager_afa *manager = (struct asset_manager_afa*)_manager;
	if (id <= 0)
		return false;
	size_t ar_idx = ((unsigned int)id >> 24) & 0x7f;
	int local_id = id & 0x00ffffff;
	if (local_id <= 0)
		return false;
	if (ar_idx < manager->nr_id_archives) {
		if (archive_exists(&manager->id_archives[ar_idx]->ar, local_id - 1))
			return true;
	}
	for (size_t i = 0; i < manager->nr_archives; i++) {
		if (archive_exists(&manager->archives[i]->ar, local_id - 1))
			return true;
	}
	return false;
}

static struct archive_data *afa_get_by_id(struct asset_manager *_manager, int id)
{
	struct asset_manager_afa *manager = (struct asset_manager_afa*)_manager;
	if (id <= 0)
		return NULL;
	size_t ar_idx = ((unsigned int)id >> 24) & 0x7f;
	int local_id = id & 0x00ffffff;
	if (local_id <= 0)
		return NULL;
	if (ar_idx < manager->nr_id_archives) {
		struct archive_data *data = archive_get(&manager->id_archives[ar_idx]->ar, local_id - 1);
		if (data)
			return data;
	}
	for (size_t i = 0; i < manager->nr_archives; i++) {
		struct archive_data *data = archive_get(&manager->archives[i]->ar, local_id - 1);
		if (data)
			return data;
	}
	return NULL;
}

static bool afa_is_backup_archive(const char *filepath)
{
	size_t len = strlen(filepath);
	return len >= 12 && !strcasecmp(filepath + len - 12, ".vanilla.bak");
}

static uint64_t afa_entry_timestamp(const struct afa_entry *e)
{
	if (!e->unknown0 && !e->unknown1)
		return 0;
	return ((uint64_t)(e->unknown1 & 0x7fffffffu) << 32) | (uint64_t)e->unknown0;
}

static struct afa_entry *afa_find_best_by_basename(struct asset_manager_afa *manager,
		const char *name, struct afa_archive **ar_out)
{
	struct afa_archive *best_ar = NULL;
	struct afa_entry *best_entry = NULL;
	uint64_t best_ts = 0;

	for (size_t i = 0; i < manager->nr_archives; i++) {
		struct afa_archive *ar = manager->archives[i];
		if (afa_is_backup_archive(ar->filename) && best_entry)
			break;

		struct afa_entry *e = afa_lookup_by_basename(ar, name);
		if (!e)
			continue;

		if (afa_is_mod_archive(ar->filename)) {
			*ar_out = ar;
			return e;
		}

		uint64_t ts = afa_entry_timestamp(e);
		if (ts == 0) {
			if (!best_entry) {
				*ar_out = ar;
				return e;
			}
			continue;
		}

		if (!best_entry || ts > best_ts) {
			best_ar = ar;
			best_entry = e;
			best_ts = ts;
		}
	}

	if (best_entry)
		*ar_out = best_ar;
	return best_entry;
}

static struct archive_data *afa_get_by_name(struct asset_manager *_manager, const char *name,
		int *id_out)
{
	struct asset_manager_afa *manager = (struct asset_manager_afa*)_manager;
	struct afa_archive *ar = NULL;
	struct afa_entry *entry = afa_find_best_by_basename(manager, name, &ar);
	if (!entry)
		return NULL;
	struct archive_data *data = afa_load_entry(ar, entry);
	if (data && id_out)
		*id_out = afa_encode_id(manager, ar, data->no);
	return data;
}

static bool afa_exists_by_name(struct asset_manager *_manager, const char *name, int *id_out)
{
	struct asset_manager_afa *manager = (struct asset_manager_afa*)_manager;
	struct afa_archive *ar = NULL;
	struct afa_entry *entry = afa_find_best_by_basename(manager, name, &ar);
	if (!entry)
		return false;
	if (id_out)
		*id_out = afa_encode_id(manager, ar, entry->no);
	return true;
}

void asset_foreach_file(enum asset_type type,
		void (*callback)(struct archive_data *data, void *user), void *user)
{
	if (type < 0 || type >= ASSET_TYPE_MAX || !assets[type]) return;
	if (assets[type]->get_by_name == afa_get_by_name) {
		struct asset_manager_afa *manager = (struct asset_manager_afa *)assets[type];
		for (size_t i = 0; i < manager->nr_archives; i++)
			archive_for_each(&manager->archives[i]->ar, callback, user);
	} else {
		struct asset_manager_ald *manager = (struct asset_manager_ald *)assets[type];
		archive_for_each(manager->archive, callback, user);
	}
}

static struct asset_manager_afa *afa_create_manager(void)
{
	struct asset_manager_afa *manager = xcalloc(1, sizeof(*manager));
	manager->manager.load_archive = afa_load_archive;
	manager->manager.exists_by_id = afa_exists_by_id;
	manager->manager.get_by_id = afa_get_by_id;
	manager->manager.exists_by_name = afa_exists_by_name;
	manager->manager.get_by_name = afa_get_by_name;
	return manager;
}

static void _ald_init(enum asset_type type, struct archive *ar)
{
	struct asset_manager_ald *manager = xcalloc(1, sizeof(struct asset_manager_ald));
	manager->manager.exists_by_id = ald_exists_by_id;
	manager->manager.get_by_id = ald_get_by_id;
	manager->manager.load_archive = NULL;
	manager->manager.exists_by_name = NULL;
	manager->manager.get_by_name = NULL;
	manager->archive = ar;
	assets[type] = &manager->manager;
}

static void ald_init(enum asset_type type, char **files, int count)
{
	if (count < 1)
		return;
	if (assets[type])
		WARNING("Multiple asset archives for type %s", asset_strtype(type));

	int error;
	struct archive *ar = ald_open(files, count, MMAP_IF_64BIT, &error);
	if (!ar)
		ERROR("Failed to open ALD file: %s", archive_strerror(error));

	_ald_init(type, ar);

	for (int i = 0; i < count; i++) {
		free(files[i]);
	}
}

static void afa_init(enum asset_type type, char *file)
{
	if (!file)
		return;
	if (assets[type])
		WARNING("Multiple asset archives for type %s", asset_strtype(type));

	int error;
	struct afa_archive *ar = afa_open(file, MMAP_IF_64BIT, &error);
	if (!ar)
		ERROR("Failed to open AFA file: %s", archive_strerror(error));

	struct asset_manager_afa *manager = afa_create_manager();
	manager->archives = xmalloc(sizeof(*manager->archives));
	manager->archives[0] = ar;
	manager->nr_archives = 1;
	afa_register_id_archive(manager, ar);
	assets[type] = &manager->manager;

	char bak_path[PATH_MAX];
	snprintf(bak_path, sizeof(bak_path), "%s.vanilla.bak", file);
	if (file_exists(bak_path)) {
		struct afa_archive *bak_ar = afa_open(bak_path, MMAP_IF_64BIT, &error);
		if (bak_ar) {
			manager->archives = xrealloc(manager->archives, 2 * sizeof(*manager->archives));
			manager->archives[1] = bak_ar;
			manager->nr_archives = 2;
			afa_register_id_archive(manager, bak_ar);
		}
	}

	free(file);
}

static char *get_base_name(const char *ain_filename)
{
	char *path = sjis2utf(ain_filename, 0);
	char *dot = strrchr(path, '.');
	if (dot)
		*dot = '\0';
	return path;
}

void asset_manager_init(void)
{
	char *ald_filenames[ASSET_TYPE_MAX][ALD_FILEMAX] = {0};
	int ald_count[ASSET_TYPE_MAX] = {0};
	char *afa_filenames[ASSET_TYPE_MAX] = {0};

	UDIR *dir;
	char *d_name;

	if (!(dir = opendir_utf8(config.game_dir))) {
		ERROR("Failed to open directory: %s", display_utf0(config.game_dir));
	}

	char *base = get_base_name(config.ain_filename);
	size_t base_len = strlen(base);

	bool ex_is_canonical = false;

	// get ALD filenames
	while ((d_name = readdir_utf8(dir))) {
		// archive filename must begin with ain filename
		if (strncasecmp(base, d_name, base_len))
			goto loop_next;
		const char *ext = file_extension(d_name);
		if (!ext)
			goto loop_next;
		if (!strcasecmp(ext, "ald")) {
			int dno = toupper(d_name[base_len+1]) - 'A';
			if (dno < 0 || dno >= ALD_FILEMAX) {
				WARNING("Invalid ALD index: %s", display_utf0(d_name));
				goto loop_next;
			}

			switch (d_name[base_len]) {
			case 'b':
			case 'B':
				ald_filenames[ASSET_BGM][dno] = path_join(config.game_dir, d_name);
				ald_count[ASSET_BGM] = max(ald_count[ASSET_BGM], dno+1);
				break;
			case 'g':
			case 'G':
				ald_filenames[ASSET_CG][dno] = path_join(config.game_dir, d_name);
				ald_count[ASSET_CG] = max(ald_count[ASSET_CG], dno+1);
				break;
			case 'w':
			case 'W':
				ald_filenames[ASSET_SOUND][dno] = path_join(config.game_dir, d_name);
				ald_count[ASSET_SOUND] = max(ald_count[ASSET_SOUND], dno+1);
				break;
			case 'd':
			case 'D':
				ald_filenames[ASSET_DATA][dno] = path_join(config.game_dir, d_name);
				ald_count[ASSET_DATA] = max(ald_count[ASSET_DATA], dno+1);
				break;
			default:
				WARNING("Unhandled ALD file: %s", display_utf0(d_name));
				break;
			}
		} else if (!strcasecmp(ext, "bgi")) {
			if (config.bgi_path) {
				WARNING("Multiple bgi files");
				goto loop_next;
			}
			config.bgi_path = path_join(config.game_dir, d_name);
		} else if (!strcasecmp(ext, "wai")) {
			if (config.wai_path) {
				WARNING("Multiple wai files");
				goto loop_next;
			}
			config.wai_path = path_join(config.game_dir, d_name);
		} else if (!strcasecmp(ext, "ex")) {
			const char *suffix = d_name + base_len;
			bool canonical = !strcasecmp(suffix, "EX.ex") || !strcasecmp(suffix, ".ex");
			if (config.ex_path) {
				WARNING("Multiple ex files");
				if (!canonical || ex_is_canonical)
					goto loop_next;
				free(config.ex_path);
			}
			config.ex_path = path_join(config.game_dir, d_name);
			ex_is_canonical = canonical;
		} else if (!strcasecmp(ext, "afa")) {
			const char *type = d_name + base_len;
			if (!strcasecmp(type, "BA.afa")) {
				afa_filenames[ASSET_BGM] = path_join(config.game_dir, d_name);
			} else if (!strcasecmp(type, "WA.afa") || !strcasecmp(type, "Sound.afa")) {
				afa_filenames[ASSET_SOUND] = path_join(config.game_dir, d_name);
			} else if (!strcasecmp(type, "Voice.afa")) {
				afa_filenames[ASSET_VOICE] = path_join(config.game_dir, d_name);
			} else if (!strcasecmp(type, "CG.afa")) {
				afa_filenames[ASSET_CG] = path_join(config.game_dir, d_name);
			} else if (!strcasecmp(type, "Flat.afa")) {
				afa_filenames[ASSET_FLAT] = path_join(config.game_dir, d_name);
			} else if (!strcasecmp(type, "Pact.afa")) {
				afa_filenames[ASSET_PACT] = path_join(config.game_dir, d_name);
			} else if (!strcasecmp(type, "AFF.afa")) {
				afa_filenames[ASSET_FLASH] = path_join(config.game_dir, d_name);
			}
		} else if (!strcasecmp(ext, "fnl")) {
			if (!config.fnl_path)
				config.fnl_path = path_join(config.game_dir, d_name);
		}
	loop_next:
		free(d_name);
	}
	closedir_utf8(dir);
	free(base);

	// open ALD archives
	ald_init(ASSET_BGM, ald_filenames[ASSET_BGM], ald_count[ASSET_BGM]);
	ald_init(ASSET_SOUND, ald_filenames[ASSET_SOUND], ald_count[ASSET_SOUND]);
	ald_init(ASSET_VOICE, ald_filenames[ASSET_VOICE], ald_count[ASSET_VOICE]);
	ald_init(ASSET_CG, ald_filenames[ASSET_CG], ald_count[ASSET_CG]);
	ald_init(ASSET_FLAT, ald_filenames[ASSET_FLAT], ald_count[ASSET_FLAT]);
	ald_init(ASSET_PACT, ald_filenames[ASSET_PACT], ald_count[ASSET_PACT]);
	ald_init(ASSET_DATA, ald_filenames[ASSET_DATA], ald_count[ASSET_DATA]);
	ald_init(ASSET_FLASH, ald_filenames[ASSET_FLASH], ald_count[ASSET_FLASH]);

	// open AFA archives
	afa_init(ASSET_BGM, afa_filenames[ASSET_BGM]);
	afa_init(ASSET_SOUND, afa_filenames[ASSET_SOUND]);
	afa_init(ASSET_VOICE, afa_filenames[ASSET_VOICE]);
	afa_init(ASSET_CG, afa_filenames[ASSET_CG]);
	afa_init(ASSET_FLAT, afa_filenames[ASSET_FLAT]);
	afa_init(ASSET_PACT, afa_filenames[ASSET_PACT]);
	afa_init(ASSET_DATA, afa_filenames[ASSET_DATA]);
	afa_init(ASSET_FLASH, afa_filenames[ASSET_FLASH]);
}
