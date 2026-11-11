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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <io.h>
#include "system4/utfsjis.h"
#else
#include <strings.h>
#include <unistd.h>
#endif
#include "cJSON.h"

#include "system4.h"
#include "system4/ain.h"
#include "system4/file.h"
#include "system4/savefile.h"
#include "system4/string.h"

#include "savedata.h"
#include "vm.h"
#include "vm/heap.h"
#include "vm/page.h"
#include "vm/v12.h"
#include "xsystem4.h"

static int current_global;

// Preserve static EX-database caches initialized at startup (such as Rance 10's
// g_questCollection and g_quest) that were placed in the local save group, so
// loading pre-MOD / older saves does not overwrite active MOD quest tables.
static bool should_preserve_static_global(int global_index)
{
	if (global_index < 0 || global_index >= ain->nr_globals)
		return false;
	const char *name = ain->globals[global_index].name;
	if (!name)
		return false;
	if (!strcmp(name, "g_questCollection")) {
		struct page *p = heap_get_page(global_get(global_index).i);
		if (p && p->type == STRUCT_PAGE && p->nr_vars >= 2) {
			struct page *quests = heap_get_page(p->values[0].i);
			if (quests && quests->type == ARRAY_PAGE && quests->nr_vars > 0)
				return true;
		}
	} else if (!strcmp(name, "g_quest")) {
		struct page *arr = heap_get_page(global_get(global_index).i);
		if (arr && arr->type == ARRAY_PAGE && arr->nr_vars > 0)
			return true;
	}
	return false;
}

#define invalid_save_data(msg, data) {					\
		char *str = data ? cJSON_Print(data) : strdup("NULL");	\
		if (!str) str = strdup("PRINTING FAILED");		\
		WARNING("Invalid save data (%d): " msg ": %s", current_global, data); \
		free(str);						\
	}

static char *make_atomic_tmp_path(const char *path)
{
	const char *slash1 = strrchr(path, '/');
	const char *slash2 = strrchr(path, '\\');
	const char *slash = slash1;
	if (slash2 && (!slash || slash2 > slash))
		slash = slash2;
	size_t dir_len = slash ? (size_t)(slash - path + 1) : 0;
	const char *base = slash ? slash + 1 : path;
	size_t base_len = strlen(base);
	char *tmp = xmalloc(dir_len + 1 + base_len + 4 + 1);
	if (dir_len)
		memcpy(tmp, path, dir_len);
	tmp[dir_len] = '.';
	memcpy(tmp + dir_len + 1, base, base_len);
	memcpy(tmp + dir_len + 1 + base_len, ".tmp", 5);
	return tmp;
}

static char *make_backup_path(const char *path)
{
	size_t len = strlen(path);
	char *bak = xmalloc(len + 5);
	memcpy(bak, path, len);
	memcpy(bak + len, ".bak", 5);
	return bak;
}

static bool is_critical_global_save(const char *path)
{
	if (!path)
		return false;
	const char *slash1 = strrchr(path, '/');
	const char *slash2 = strrchr(path, '\\');
	const char *base = slash1;
	if (slash2 && (!base || slash2 > base))
		base = slash2;
	base = base ? base + 1 : path;
#ifdef _WIN32
	return !_stricmp(base, "GlobalSave.asd") || !_stricmp(base, "Collection.asd");
#else
	return !strcasecmp(base, "GlobalSave.asd") || !strcasecmp(base, "Collection.asd");
#endif
}

#define MAX_TRACKED_GLOBAL_SAVES 8
struct tracked_global_save {
	char *keyname;
	char *filename;
	char *group_name;
};
static struct tracked_global_save tracked_global_saves[MAX_TRACKED_GLOBAL_SAVES];
static int nr_tracked_global_saves = 0;
static bool flushing_background_saves = false;

static const char *save_basename(const char *path)
{
	if (!path)
		return "";
	const char *slash1 = strrchr(path, '/');
	const char *slash2 = strrchr(path, '\\');
	const char *base = slash1;
	if (slash2 && (!base || slash2 > base))
		base = slash2;
	return base ? base + 1 : path;
}

static bool is_background_flush_save(const char *path)
{
	const char *base = save_basename(path);
	if (!base[0])
		return false;
#ifdef _WIN32
	return !_stricmp(base, "GlobalSave.asd") ||
	       !_stricmp(base, "Collection.asd") ||
	       !_stricmp(base, "GameConfig.asd") ||
	       !_stricmp(base, "GameEnquete.asd") ||
	       !_stricmp(base, "AFCommon.asd") ||
	       !_stricmp(base, "AFConfig.asd") ||
	       !_stricmp(base, "AFInfo.asd");
#else
	return !strcasecmp(base, "GlobalSave.asd") ||
	       !strcasecmp(base, "Collection.asd") ||
	       !strcasecmp(base, "GameConfig.asd") ||
	       !strcasecmp(base, "GameEnquete.asd") ||
	       !strcasecmp(base, "AFCommon.asd") ||
	       !strcasecmp(base, "AFConfig.asd") ||
	       !strcasecmp(base, "AFInfo.asd");
#endif
}

static void track_global_save(const char *keyname, const char *filename, const char *group_name)
{
	if (flushing_background_saves || !keyname || !filename || !group_name || !group_name[0])
		return;
	if (!is_background_flush_save(filename))
		return;
	for (int i = 0; i < nr_tracked_global_saves; i++) {
		if (!strcmp(tracked_global_saves[i].filename, filename)) {
			free(tracked_global_saves[i].keyname);
			free(tracked_global_saves[i].group_name);
			tracked_global_saves[i].keyname = xstrdup(keyname);
			tracked_global_saves[i].group_name = xstrdup(group_name);
			return;
		}
	}
	if (nr_tracked_global_saves >= MAX_TRACKED_GLOBAL_SAVES)
		return;
	tracked_global_saves[nr_tracked_global_saves].keyname = xstrdup(keyname);
	tracked_global_saves[nr_tracked_global_saves].filename = xstrdup(filename);
	tracked_global_saves[nr_tracked_global_saves].group_name = xstrdup(group_name);
	nr_tracked_global_saves++;
}

static void untrack_global_save(const char *filename)
{
	if (!filename)
		return;
	for (int i = 0; i < nr_tracked_global_saves; i++) {
		if (!strcmp(tracked_global_saves[i].filename, filename)) {
			free(tracked_global_saves[i].keyname);
			free(tracked_global_saves[i].filename);
			free(tracked_global_saves[i].group_name);
			for (int j = i; j + 1 < nr_tracked_global_saves; j++)
				tracked_global_saves[j] = tracked_global_saves[j + 1];
			nr_tracked_global_saves--;
			return;
		}
	}
}

void savedata_reset(void)
{
	for (int i = 0; i < nr_tracked_global_saves; i++) {
		free(tracked_global_saves[i].keyname);
		free(tracked_global_saves[i].filename);
		free(tracked_global_saves[i].group_name);
		tracked_global_saves[i].keyname = NULL;
		tracked_global_saves[i].filename = NULL;
		tracked_global_saves[i].group_name = NULL;
	}
	nr_tracked_global_saves = 0;
}

FILE *atomic_save_open(const char *path, const char *mode, char **tmp_path_out)
{
	char *tmp = make_atomic_tmp_path(path);
	FILE *fp = file_open_utf8(tmp, mode);
	if (!fp) {
		free(tmp);
		*tmp_path_out = NULL;
		return NULL;
	}
	*tmp_path_out = tmp;
	return fp;
}

void atomic_save_abort(FILE *fp, char *tmp_path)
{
	if (fp)
		fclose(fp);
	if (tmp_path) {
		remove_utf8(tmp_path);
		free(tmp_path);
	}
}

bool atomic_save_commit(FILE *fp, const char *path, char *tmp_path, bool keep_backup)
{
	bool ok = !ferror(fp) && fflush(fp) == 0;
#ifdef _WIN32
	if (ok && _commit(_fileno(fp)) != 0)
		ok = false;
#else
	if (ok && fsync(fileno(fp)) != 0 && errno != EINVAL && errno != EROFS)
		ok = false;
#endif
	if (fclose(fp) != 0)
		ok = false;
	if (!ok) {
		WARNING("Error flushing temporary save file %s: %s", display_utf0(tmp_path), strerror(errno));
		remove_utf8(tmp_path);
		free(tmp_path);
		return false;
	}

	if (keep_backup && file_exists(path) && file_size(path) > 16) {
		char *bak = make_backup_path(path);
		file_copy(path, bak);
		free(bak);
	}

#ifdef _WIN32
	wchar_t *wtmp = utf8_to_wchar(tmp_path);
	wchar_t *wpath = utf8_to_wchar(path);
	remove_utf8(path);
	int rc = _wrename(wtmp, wpath);
	free(wtmp);
	free(wpath);
#else
	int rc = rename(tmp_path, path);
#endif
	if (rc != 0) {
		WARNING("Failed renaming %s -> %s: %s", display_utf0(tmp_path), display_utf0(path), strerror(errno));
		remove_utf8(tmp_path);
		free(tmp_path);
		return false;
	}
	free(tmp_path);
	return true;
}

int save_json(const char *filename, cJSON *json)
{
	char *str = cJSON_Print(json);
	if (!str)
		return 0;

	char *path = savedir_path(filename);
	char *tmp_path = NULL;
	FILE *f = atomic_save_open(path, "wb", &tmp_path);
	if (!f) {
		WARNING("Failed to open save file: %s: %s", display_utf0(filename), strerror(errno));
		free(path);
		free(str);
		return 0;
	}

	size_t len = strlen(str);
	if (len > 0 && fwrite(str, len, 1, f) != 1) {
		WARNING("Failed to write save file: %s", strerror(errno));
		atomic_save_abort(f, tmp_path);
		free(path);
		free(str);
		return 0;
	}
	bool ok = atomic_save_commit(f, path, tmp_path, is_critical_global_save(filename));
	free(path);
	free(str);
	return ok ? 1 : 0;
}

cJSON *load_json(const char *filename)
{
	char *path = savedir_path(filename);
	char *json = file_read(path, NULL);
	free(path);
	if (!json)
		return NULL;
	cJSON *root = cJSON_Parse(json);
	free(json);
	return root;
}

static int get_group_index(const char *name)
{
	for (int i = 0; i < ain->nr_global_groups; i++) {
		if (!strcmp(ain->global_group_names[i], name))
			return i;
	}
	return -1;
}

static int32_t add_value_to_gsave(enum ain_data_type type, union vm_value val, struct gsave *save);

static bool nested_array(struct page *page)
{
	return page->a_type == AIN_ARRAY ? page->array.element_type.data == AIN_ARRAY : page->array.rank > 1;
}

static int page_array_rank(struct page *page)
{
	if (page->a_type != AIN_ARRAY) return page->array.rank;
	int rank = 1;
	for (const struct ain_type *t = &page->array.element_type; t && t->data == AIN_ARRAY; t = t->array_type)
		rank++;
	return rank;
}

static struct gsave_flat_array *collect_flat_arrays(struct page *page, struct gsave_flat_array *fa, struct gsave *save)
{
	if (nested_array(page)) {
		for (int i = 0; i < page->nr_vars; i++)
			fa = collect_flat_arrays(heap_get_page(page->values[i].i), fa, save);
		return fa;
	}
	enum ain_data_type type = variable_type(page, 0, NULL, NULL);
	fa->nr_values = page->nr_vars;
	fa->type = type;
	fa->values = xcalloc(fa->nr_values, sizeof(struct gsave_array_value));
	for (int i = 0; i < page->nr_vars; i++) {
		fa->values[i].type = type;
		fa->values[i].value = add_value_to_gsave(type, page->values[i], save);
	}
	return ++fa;
}

static int32_t add_value_to_gsave(enum ain_data_type type, union vm_value val, struct gsave *save)
{
	switch (type) {
	case AIN_VOID:
	case AIN_INT:
	case AIN_BOOL:
	case AIN_FUNC_TYPE:
	case AIN_LONG_INT:
	case AIN_FLOAT:
	case AIN_ENUM:
	case AIN_ENUM2:
		return val.i;
	case AIN_DELEGATE:
		// Group saves contain persistent data; the original v12 engine clears events.
		return ain->version >= 11 ? 0 : val.i;
	case AIN_STRING:
		return gsave_add_string(save, heap[val.i].s);
	case AIN_STRUCT:
		{
			struct page *page = heap_get_page(val.i);
			assert(page->type == STRUCT_PAGE);
			struct ain_struct *st = &ain->structures[page->index];
			assert(st->nr_members == page->nr_vars);
			struct gsave_record rec = {
				.type = GSAVE_RECORD_STRUCT,
				.struct_name = strdup(st->name),
				.nr_indices = st->nr_members,
				.indices = xcalloc(st->nr_members, sizeof(int32_t)),
			};
			if (save->version >= 7) {
				rec.struct_index = gsave_get_struct_def(save, st->name);
				if (rec.struct_index < 0)
					rec.struct_index = gsave_add_struct_def(save, st);
			}
			for (int i = 0; i < page->nr_vars; i++) {
				enum ain_data_type type = st->members[i].type.data;
				const char *field_name = st->members[i].name2 ? st->members[i].name2 : st->members[i].name;
				int32_t value = field_name[0] ? add_value_to_gsave(type, page->values[i], save) : 0;
				struct gsave_keyval kv = {
					.name = strdup(field_name),
					.type = type,
					.value = value,
				};
				rec.indices[i] = gsave_add_keyval(save, &kv);
			}
			return gsave_add_record(save, &rec);
		}
	case AIN_ARRAY_TYPE:
	case AIN_ARRAY:
		{
			struct page *page = heap_get_page(val.i);
			if (!page) {
				struct gsave_array array = { .rank = -1 };
				return gsave_add_array(save, &array);
			}
			assert(page->type == ARRAY_PAGE);
			int rank = page_array_rank(page);
			struct gsave_array array = {
				.rank = rank,
				.dimensions = xcalloc(rank, sizeof(int32_t)),
				.nr_flat_arrays = 1
			};
			struct page *p = page;
			for (int dim = rank - 1; dim >= 0; dim--) {
				int count = p ? p->nr_vars : 0;
				array.dimensions[dim] = count;
				if (dim) {
					array.nr_flat_arrays *= count;
					p = count ? heap_get_page(p->values[0].i) : NULL;
				}
			}
			array.flat_arrays = xcalloc(array.nr_flat_arrays, sizeof(struct gsave_flat_array));
			collect_flat_arrays(page, array.flat_arrays, save);
			return gsave_add_array(save, &array);
		}
	case AIN_REF_TYPE:
		return -1;
	default:
		WARNING("Unhandled type: %s", ain_strtype(ain, type, -1));
		return -1;
	}
}

static int get_gsave_version(void)
{
	if (ain->version >= 11) return 8;
	if (AIN_VERSION_GTE(ain, 6, 0)) {
		return config.vm_name ? 5 : 7;
	}
	return AIN_VERSION_GTE(ain, 5, 0) ? 5 : 4;
}

int save_globals(const char *keyname, const char *filename, const char *group_name, int *n_out)
{
	int group = -1;
	int nr_vars = 0;
	if (group_name) {
		if ((group = get_group_index(group_name)) < 0) {
			WARNING("Unregistered global group: %s", display_sjis0(group_name));
		} else {
			for (int i = 0; i < ain->nr_globals; i++) {
				if (ain->globals[i].name2 && !ain->globals[i].name2[0])
					continue;
				if (ain->globals[i].group_index == group)
					nr_vars++;
			}
		}
	} else {
		for (int i = 0; i < ain->nr_globals; i++) {
			if (!(ain->globals[i].name2 && !ain->globals[i].name2[0]))
				nr_vars++;
		}
	}

	struct gsave *save = gsave_create(get_gsave_version(), keyname, ain->nr_globals, group_name);
	gsave_add_globals_record(save, nr_vars);

	if (nr_vars > 0) {
		struct gsave_global *global = save->globals;
		for (int i = 0; i < ain->nr_globals; i++) {
			if (ain->globals[i].name2 && !ain->globals[i].name2[0])
				continue;
			if (group >= 0 && ain->globals[i].group_index != group)
				continue;
			const char *gname = (ain->globals[i].name2 && ain->globals[i].name2[0]) ? ain->globals[i].name2 : ain->globals[i].name;
			global->name = strdup(gname);
			global->type = ain->globals[i].type.data;
			global->value = add_value_to_gsave(global->type, global_get(i), save);
			global++;
		}
	}

	char *path = savedir_path(filename);
	char *tmp_path = NULL;
	FILE *fp = atomic_save_open(path, "wb", &tmp_path);
	if (!fp) {
		WARNING("Failed to open save file %s: %s", display_utf0(path), strerror(errno));
		free(path);
		gsave_free(save);
		return 0;
	}

	bool encrypt = !AIN_VERSION_GTE(ain, 6, 0);
	int compression_level = AIN_VERSION_GTE(ain, 6, 0) ? 1 : 9;
	enum savefile_error error = gsave_write(save, fp, encrypt, compression_level);
	gsave_free(save);
	if (error != SAVEFILE_SUCCESS) {
		WARNING("Failed to write save file: %s", savefile_strerror(error));
		atomic_save_abort(fp, tmp_path);
		free(path);
		return 0;
	}
	bool ok = atomic_save_commit(fp, path, tmp_path, is_critical_global_save(filename));
	free(path);
	if (ok) {
		track_global_save(keyname, filename, group_name);
		if (n_out)
			*n_out = nr_vars;
	}
	return ok ? 1 : 0;
}

static union vm_value json_to_vm_value(enum ain_data_type type, enum ain_data_type struct_type, int array_rank, cJSON *json);

void get_array_dims(cJSON *json, int rank, union vm_value *dims)
{
	cJSON *array = json;
	for (int i = 0; i < rank; i++) {
		dims[i].i = cJSON_GetArraySize(array);
		array = cJSON_GetArrayItem(array, 0);
	}
}

void json_load_page(struct page *page, cJSON *vars, bool call_dtors)
{
	int i = 0;
	cJSON *v;
	cJSON_ArrayForEach(v, vars) {
		int struct_type, array_rank;
		enum ain_data_type data_type = variable_type(page, i, &struct_type, &array_rank);
		union vm_value val = json_to_vm_value(data_type, struct_type, array_rank, v);
		if (call_dtors)
			variable_set(page, i, data_type, val);
		else
			page->values[i] = val;
		i++;
	}
}

static union vm_value json_to_vm_value(enum ain_data_type type, enum ain_data_type struct_type, int array_rank, cJSON *json)
{
	char *str;
	int slot;
	union vm_value *dims;
	struct page *page;
	switch (type) {
	case AIN_INT:
	case AIN_BOOL:
	case AIN_LONG_INT:
		if (!cJSON_IsNumber(json)) {
			invalid_save_data("Not a number", json);
			return vm_int(0);
		}
		return vm_int(json->valueint);
	case AIN_FLOAT:
		if (!cJSON_IsNumber(json)) {
			invalid_save_data("Not a number", json);
			return vm_float(0);
		}
		return vm_float(json->valuedouble);
	case AIN_STRING:
		slot = heap_alloc_slot(VM_STRING);
		if (!cJSON_IsString(json)) {
			invalid_save_data("Not a string", json);
			heap[slot].s = string_ref(&EMPTY_STRING);
			return vm_int(slot);
		}
		str = cJSON_GetStringValue(json);
		if (!str[0]) {
			heap[slot].s = string_ref(&EMPTY_STRING);
		} else {
			heap[slot].s = make_string(str, strlen(str));
		}
		return vm_int(slot);
	case AIN_STRUCT:
		slot = alloc_struct(struct_type);
		if (!cJSON_IsArray(json) || cJSON_GetArraySize(json) != ain->structures[struct_type].nr_members) {
			invalid_save_data("Not an array", json);
		} else {
			json_load_page(heap[slot].page, json, false);
		}
		return vm_int(slot);
	case AIN_ARRAY_TYPE:
		slot = heap_alloc_slot(VM_PAGE);
		if (cJSON_IsNull(json)) {
			heap[slot].page = NULL;
			return vm_int(slot);
		}
		if (!cJSON_IsArray(json)) {
			invalid_save_data("Not an array", json);
			heap[slot].page = NULL;
			return vm_int(slot);
		}
		dims = xmalloc(sizeof(union vm_value) * array_rank);
		get_array_dims(json, array_rank, dims);
		page = alloc_array(array_rank, dims, type, struct_type, false);
		heap[slot].page = page;
		free(dims);
		json_load_page(heap[slot].page, json, false);
		return vm_int(slot);
	case AIN_REF_TYPE:
		return vm_int(-1);
	default:
		WARNING("Unhandled data type: %s", ain_strtype(ain, type, -1));
		return vm_int(-1);
	}
}

static cJSON *read_save_file(const char *path)
{
	FILE *f;
	long len;
	char *buf;

	if (!(f = file_open_utf8(path, "rb"))) {
		WARNING("Failed to open save file: %s: %s", display_utf0(path), strerror(errno));
		return NULL;
	}

	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);

	buf = xmalloc(len+1);
	buf[len] = '\0';
	if (fread(buf, len, 1, f) != 1) {
		WARNING("Failed to read save file: %s", display_utf0(path));
		free(buf);
		return 0;
	}
	fclose(f);

	cJSON *r = cJSON_Parse(buf);
	free(buf);
	return r;
}

static int load_globals_from_json(const char *path, const char *keyname, const char *group_name, int *n)
{
	int retval = 0;
	cJSON *save = read_save_file(path);
	if (!save)
		return 0;
	if (!cJSON_IsObject(save)) {
		invalid_save_data("Not an object", save);
		goto cleanup;
	}

	cJSON *key = cJSON_GetObjectItem(save, "key");
	if (!key || strcmp(keyname, cJSON_GetStringValue(key)))
		VM_ERROR("Attempted to load save data with wrong key: %s", display_sjis0(keyname));

	if (group_name) {
		// TODO?
	}

	cJSON *globals = cJSON_GetObjectItem(save, "globals");
	if (!globals || !cJSON_IsArray(globals)) {
		invalid_save_data("Not an array", globals);
		goto cleanup;
	}

	cJSON *g;
	cJSON_ArrayForEach(g, globals) {
		cJSON *index, *value;
		if (!(index = cJSON_GetObjectItem(g, "index"))) {
			invalid_save_data("Missing index", g);
			goto cleanup;
		}
		if (!(value = cJSON_GetObjectItem(g, "value"))) {
			invalid_save_data("Missing value", g);
			goto cleanup;
		}
		if (!cJSON_IsNumber(index)) {
			invalid_save_data("Not a number", index);
			goto cleanup;
		}

		int i = index->valueint;
		if (i < 0 || i > ain->nr_globals) {
			invalid_save_data("Invalid global index", index);
			goto cleanup;
		}
		current_global = i;

		if (should_preserve_static_global(i)) {
			if (n)
				(*n)++;
			continue;
		}
		bool call_dtors = false; // Destructors for old objects are not called.
		global_set(i, json_to_vm_value(ain->globals[i].type.data, ain->globals[i].type.struc, ain->globals[i].type.rank, value), call_dtors);
		if (n)
			(*n)++;
	}

	retval = 1;
cleanup:
	cJSON_Delete(save);
	return retval;
}

static int struct_member_index(struct ain_struct *st, const char *name)
{
	if (!name || !name[0])
		return -1;
	for (int i = 0; i < st->nr_members; i++) {
		if (st->members[i].name2 && st->members[i].name2[0] && !strcmp(st->members[i].name2, name))
			return i;
		if (!strcmp(st->members[i].name, name))
			return i;
	}
	return -1;
}

static union vm_value gsave_to_vm_value(struct gsave *save, enum ain_data_type type, int struct_type, int array_rank, int32_t value);
static union vm_value gsave_to_vm_value_typed(struct gsave *save, const struct ain_type *type, int32_t value);

static struct gsave_flat_array *gsave_load_array(struct gsave *save, struct page *page, struct gsave_flat_array *flat_array)
{
	assert(page->type == ARRAY_PAGE);
	if (nested_array(page)) {
		for (int i = 0; i < page->nr_vars; i++)
			flat_array = gsave_load_array(save, heap_get_page(page->values[i].i), flat_array);
		return flat_array;
	}

	if (page->nr_vars != flat_array->nr_values)
		VM_ERROR("Bad save file: unexpected number of array elements");
	for (int i = 0; i < page->nr_vars; i++) {
		int struct_type, array_rank;
		enum ain_data_type data_type = variable_type(page, i, &struct_type, &array_rank);
		if (data_type != flat_array->values[i].type &&
		    !(data_type == AIN_REF_STRUCT && flat_array->values[i].type == AIN_STRUCT))
			VM_ERROR("Bad save file: unexpected array element type");
		const struct ain_type *type = variable_full_type(page, i);
		union vm_value val = type ? gsave_to_vm_value_typed(save, type, flat_array->values[i].value)
			: gsave_to_vm_value(save, data_type, struct_type, array_rank, flat_array->values[i].value);
		variable_fini(page->values[i], data_type, false);
		page->values[i] = val;
	}
	return flat_array + 1;
}

static union vm_value gsave_to_vm_value(struct gsave *save, enum ain_data_type type, int struct_type, int array_rank, int32_t value)
{
	switch (type) {
	case AIN_VOID:
	case AIN_INT:
	case AIN_BOOL:
	case AIN_LONG_INT:
	case AIN_FLOAT:
	case AIN_ENUM:
	case AIN_ENUM2:
	case AIN_FUNC_TYPE:
		return vm_int(value);
	case AIN_DELEGATE:
		return variable_initval(AIN_DELEGATE);
	case AIN_STRING:
		{
			int slot = heap_alloc_slot(VM_STRING);
			heap[slot].s = string_ref(value == GSAVE7_EMPTY_STRING ? &EMPTY_STRING : save->strings[value]);
			return vm_int(slot);
		}
	case AIN_STRUCT:
		{
			struct gsave_record *rec = &save->records[value];
			struct ain_struct *st = &ain->structures[struct_type];
			struct gsave_struct_def *sd =
				save->version >= 7 ? &save->struct_defs[rec->struct_index] : NULL;
			const char *struct_name = sd ? sd->name : rec->struct_name;
			if (strcmp(struct_name, st->name))
				VM_ERROR("Bad save file: structure name mismatch");
			int slot = alloc_struct(struct_type);
			struct page *page = heap[slot].page;
			for (int i = 0; i < rec->nr_indices; i++) {
				struct gsave_keyval *kv = &save->keyvals[rec->indices[i]];
				const char *field_name = sd ? sd->fields[i].name : kv->name;
				enum ain_data_type field_type = sd ? sd->fields[i].type : kv->type;
				if (!field_name || !field_name[0])
					continue;
				int index = struct_member_index(st, field_name);
				if (index < 0) {
					WARNING("structure %s has no member named %s", display_sjis0(struct_name), display_sjis1(field_name));
					continue;
				}
				int struct_type, array_rank;
				enum ain_data_type data_type = variable_type(page, index, &struct_type, &array_rank);
				if (data_type != field_type)
					VM_ERROR("Bad save file: structure member type mismatch");
				union vm_value val = gsave_to_vm_value_typed(save, &st->members[index].type, kv->value);
				variable_fini(page->values[index], data_type, false);
				page->values[index] = val;
			}
			return vm_int(slot);
		}
	case AIN_ARRAY_TYPE:
		{
			struct gsave_array *array = &save->arrays[value];
			int slot = heap_alloc_slot(VM_PAGE);
			if (array->rank == -1) {
				heap[slot].page = NULL;
				return vm_int(slot);
			}
			if (array_rank != array->rank)
				VM_ERROR("Bad save file: array rank mismatch");
			union vm_value *dims = xmalloc(sizeof(union vm_value) * array_rank);
			for (int i = 0; i < array_rank; i++)
				dims[i].i = array->dimensions[array_rank - 1 - i];
			struct page *page = alloc_array(array_rank, dims, type, struct_type, false);
			heap[slot].page = page;
			free(dims);
			gsave_load_array(save, page, array->flat_arrays);
			return vm_int(slot);
		}
	case AIN_REF_TYPE:
		return vm_int(-1);
	default:
		WARNING("Unhandled data type: %s", ain_strtype(ain, type, -1));
		return vm_int(-1);
	}
}

static struct page *v12_saved_array_new(const struct ain_type *type, int rank, int32_t *dimensions)
{
	if (type->data != AIN_ARRAY || rank < 1 || !type->array_type)
		VM_ERROR("Bad save file: nested array type mismatch");
	struct page *page = v12_array_new(type->array_type, dimensions[rank - 1], false);
	if (rank > 1) {
		for (int i = 0; i < page->nr_vars; i++) {
			int slot = page->values[i].i;
			struct page *child = v12_saved_array_new(type->array_type, rank - 1, dimensions);
			delete_page(slot);
			heap_set_page(slot, child);
		}
	}
	return page;
}

static union vm_value gsave_to_vm_value_typed(struct gsave *save, const struct ain_type *type, int32_t value)
{
	if (type->data != AIN_ARRAY)
		return gsave_to_vm_value(save, type->data, type->struc, type->rank, value);
	if (value < 0 || value >= save->nr_arrays)
		VM_ERROR("Bad save file: invalid generic array index %d", value);
	struct gsave_array *array = &save->arrays[value];
	if (array->rank < 1) return variable_initval_typed(type);
	struct page *page = v12_saved_array_new(type, array->rank, array->dimensions);
	gsave_load_array(save, page, array->flat_arrays);
	return vm_int(heap_alloc_page(page));
}

int load_globals_from_gsave(struct gsave *save, const char *keyname, const char *group_name, int *n)
{
	if (strcmp(keyname, save->key))
		VM_ERROR("Attempted to load save data with wrong key: %s", display_sjis0(keyname));
	if (!group_name)
		group_name = "";
	if (!save->group)
		save->group = strdup("");
	if (strcmp(group_name, save->group))
		VM_ERROR("Attempted to load save data with wrong group name: '%s'", display_sjis0(group_name));

	for (struct gsave_global *g = save->globals; g < save->globals + save->nr_globals; g++) {
		if (g->type == AIN_VOID && !g->name[0]) continue;
		int global_index = ain_get_global(ain, g->name);
		if (global_index < 0) {
			for (int i = 0; i < ain->nr_globals; i++) {
				if (ain->globals[i].name2 && ain->globals[i].name2[0] && !strcmp(ain->globals[i].name2, g->name)) {
					global_index = i;
					break;
				}
			}
		}
		if (global_index < 0) {
			WARNING("invalid global name %s", display_sjis0(g->name));
			return 0;
		}
		struct ain_type *type = &ain->globals[global_index].type;
		if (type->data != g->type) {
			WARNING("%s: type mismatch", display_sjis0(g->name));
			return 0;
		}
		if (should_preserve_static_global(global_index)) {
			if (n)
				(*n)++;
			continue;
		}
		bool call_dtors = false; // Destructors for old objects should not be called.
		global_set(global_index, gsave_to_vm_value_typed(save, type, g->value), call_dtors);
		if (n)
			(*n)++;
	}

	return 1;
}

int load_globals(const char *keyname, const char *filename, const char *group_name, int *n)
{
	if (n)
		*n = 0;
	char *path = savedir_path(filename);
	int retval;

	// First, try reading as a gsave.
	enum savefile_error error;
	struct gsave *save = gsave_read(path, &error);
	if (error == SAVEFILE_SUCCESS) {
		free(path);
		retval = load_globals_from_gsave(save, keyname, group_name, n);
		gsave_free(save);
		if (retval)
			track_global_save(keyname, filename, group_name);
		return retval;
	}
	if (error == SAVEFILE_INVALID_SIGNATURE) {
		// If not a System4 save file, try reading as a json.
		retval = load_globals_from_json(path, keyname, group_name, n);
		if (retval) {
			free(path);
			track_global_save(keyname, filename, group_name);
			return retval;
		}
	} else {
		WARNING("%s: %s", display_utf0(path), savefile_strerror(error));
	}

	// Fall back to .bak if a backup exists (e.g. GlobalSave.asd.bak / Collection.asd.bak).
	char *bak_path = make_backup_path(path);
	bool had_any_file = file_exists(path) || file_exists(bak_path);
	if (file_exists(bak_path)) {
		enum savefile_error bak_err;
		struct gsave *bak_save = gsave_read(bak_path, &bak_err);
		if (bak_err == SAVEFILE_SUCCESS) {
			WARNING("Recovered corrupted save %s from backup %s", display_utf0(path), display_utf0(bak_path));
			file_copy(bak_path, path);
			free(bak_path);
			free(path);
			retval = load_globals_from_gsave(bak_save, keyname, group_name, n);
			gsave_free(bak_save);
			if (retval)
				track_global_save(keyname, filename, group_name);
			return retval;
		}
	}
	free(bak_path);
	free(path);
	if (!had_any_file)
		track_global_save(keyname, filename, group_name);
	return 0;
}

int delete_save_file(const char *filename)
{
	untrack_global_save(filename);
	char *path = savedir_path(filename);
	char *bak_path = make_backup_path(path);
	if (file_exists(bak_path))
		remove_utf8(bak_path);
	free(bak_path);
	if (!file_exists(path)) {
		free(path);
		return 0;
	}
	if (remove_utf8(path)) {
		WARNING("remove(\"%s\"): %s", display_utf0(path), strerror(errno));
		free(path);
		return 0;
	}
	free(path);
	return 1;
}

void savedata_flush_on_background(void)
{
	if (flushing_background_saves || !ain || !heap || heap_size <= 0)
		return;
	flushing_background_saves = true;
	for (int i = 0; i < nr_tracked_global_saves; i++) {
		const struct tracked_global_save *entry = &tracked_global_saves[i];
		if (!entry->keyname || !entry->filename || !entry->group_name)
			continue;
		if (get_group_index(entry->group_name) < 0)
			continue;
		save_globals(entry->keyname, entry->filename, entry->group_name, NULL);
	}
	flushing_background_saves = false;
}
