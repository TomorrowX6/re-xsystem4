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
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include "system4/file.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

#include "xsystem4.h"
#include "vm/heap.h"
#include "vm/page.h"
#include "vm/v12.h"
#include "hll.h"

static char *fileop_resolve_path(const char *sjis_path);

static bool FileOperation_ExistFile(struct string *file_name)
{
	char *path = fileop_resolve_path(file_name->text);
	ustat s;
	bool result = stat_utf8(path, &s) == 0 && S_ISREG(s.st_mode);
	free(path);
	return result;
}

static char *fileop_resolve_path(const char *sjis_path)
{
	char *path = unix_path(sjis_path);
	ustat s;
	if (path && stat_utf8(path, &s) == 0)
		return path;
	if (config.game_dir) {
		char *gd_path = gamedir_path_icase(sjis_path);
		if (gd_path && stat_utf8(gd_path, &s) == 0) {
			free(path);
			return gd_path;
		}
		free(gd_path);
	}
	return path;
}

static bool FileOperation_DeleteFile(struct string *file_name)
{
	char *path = fileop_resolve_path(file_name->text);
	bool result = path && remove_utf8(path) == 0;
	free(path);
	return result;
}

static bool FileOperation_CopyFile(struct string *dest_file_name, struct string *src_file_name)
{
	char *dest = unix_path(dest_file_name->text);
	char *src = unix_path(src_file_name->text);
	bool result = file_copy(src, dest);
	free(src);
	free(dest);
	return result;
}

//bool FileOperation_GetFileCreationTime(string FileName, ref int nYear, ref int nMonth, ref int nDay, ref int nWeek, ref int nHour, ref int nMin, ref int nSecond);
//bool FileOperation_GetFileLastAccessTime(string FileName, ref int nYear, ref int nMonth, ref int nDay, ref int nWeek, ref int nHour, ref int nMin, ref int nSecond);
static bool fileop_time_to_fields(time_t t, int *year, int *month, int *day,
		int *week, int *hour, int *min, int *second)
{
	struct tm *tm = localtime(&t);
	if (!tm) {
		*year = *month = *day = *week = *hour = *min = *second = 0;
		return false;
	}
	*year = tm->tm_year + 1900;
	*month = tm->tm_mon + 1;
	*day = tm->tm_mday;
	*week = tm->tm_wday;
	*hour = tm->tm_hour;
	*min = tm->tm_min;
	*second = tm->tm_sec;
	return true;
}

static bool FileOperation_GetFileCreationTime(struct string *file_name,
		int *year, int *month, int *day, int *week, int *hour, int *min, int *second)
{
	char *path = fileop_resolve_path(file_name->text);
	ustat s;
	if (!path || stat_utf8(path, &s) < 0 || !S_ISREG(s.st_mode)) {
		free(path);
		*year = *month = *day = *week = *hour = *min = *second = 0;
		return false;
	}
	free(path);
	return fileop_time_to_fields(s.st_ctime, year, month, day, week, hour, min, second);
}

static bool FileOperation_GetFileLastAccessTime(struct string *file_name,
		int *year, int *month, int *day, int *week, int *hour, int *min, int *second)
{
	char *path = fileop_resolve_path(file_name->text);
	ustat s;
	if (!path || stat_utf8(path, &s) < 0 || !S_ISREG(s.st_mode)) {
		free(path);
		*year = *month = *day = *week = *hour = *min = *second = 0;
		return false;
	}
	free(path);
	return fileop_time_to_fields(s.st_atime, year, month, day, week, hour, min, second);
}

static bool FileOperation_GetFileLastWriteTime(struct string *file_name,
		int *year, int *month, int *day, int *week, int *hour, int *min, int *second)
{
	char *path = fileop_resolve_path(file_name->text);
	ustat s;
	if (!path || stat_utf8(path, &s) < 0 || !S_ISREG(s.st_mode)) {
		free(path);
		*year = *month = *day = *week = *hour = *min = *second = 0;
		return false;
	}
	free(path);
	return fileop_time_to_fields(s.st_mtime, year, month, day, week, hour, min, second);
}

static bool FileOperation_GetFileSize(struct string *file_name, int *size)
{
	char *path = fileop_resolve_path(file_name->text);
	ustat s;
	if (stat_utf8(path, &s) < 0) {
		WARNING("stat(\"%s\"): %s", display_utf0(path), strerror(errno));
		free(path);
		return false;
	}
	if (!S_ISREG(s.st_mode)) {
		WARNING("stat(\"%s\"): not a regular file", display_utf0(path));
		free(path);
		return false;
	}
	*size = s.st_size;
	free(path);
	return true;
}

static bool FileOperation_ExistFolder(struct string *folder_name)
{
	char *path = unix_path(folder_name->text);
	bool result = is_directory(path);
	free(path);
	return result;
}

static bool FileOperation_CreateFolder(struct string *folder_name)
{
	char *path = unix_path(folder_name->text);
	bool result = mkdir_p(path) == 0;
	if (!result)
		WARNING("mkdir_p(%s): %s", path, strerror(errno));
	free(path);
	return result;
}

static bool rmtree(const char *path)
{
	UDIR *d = opendir_utf8(path);
	if (!d) {
		WARNING("opendir(\"%s\"): %s", display_utf0(path), strerror(errno));
		return false;
	}
	bool ok = true;
	char *d_name;
	while (ok && (d_name = readdir_utf8(d)) != NULL) {
		if (d_name[0] == '.' && (d_name[1] == '\0' || (d_name[1] == '.' && d_name[2] == '\0'))) {
			free(d_name);
			continue;
		}

		char *utf8_path = path_join(path, d_name);
		ustat s;
		if (stat_utf8(utf8_path, &s) < 0) {
			WARNING("stat(\"%s\"): %s", display_utf0(utf8_path), strerror(errno));
			ok = false;
		} else {
			if (S_ISDIR(s.st_mode)) {
				if (!rmtree(utf8_path))
					ok = false;
			} else {
				if (remove_utf8(utf8_path) < 0) {
					WARNING("remove(\"%s\"): %s", display_utf0(utf8_path), strerror(errno));
					ok = false;
				}
			}
		}
		free(utf8_path);
		free(d_name);
	}
	closedir_utf8(d);
	if (ok) {
		if (rmdir_utf8(path) < 0) {
			WARNING("rmdir(\"%s\"): %s", display_utf0(path), strerror(errno));
			ok = false;
		}
	}
	return ok;
}

static bool FileOperation_DeleteFolder(struct string *folder_name)
{
	char *path = unix_path(folder_name->text);
	bool result = rmtree(path);
	free(path);
	return result;
}

static bool get_file_list(struct string *folder_name, struct page **out, bool folders)
{
	char *dir_name = ain->version >= 12 ? gamedir_path_icase(folder_name->text)
		: unix_path(folder_name->text);
	if (!dir_name) return false;

	UDIR *d = opendir_utf8(dir_name);
	if (!d) {
		WARNING("opendir(\"%s\"): %s", display_utf0(dir_name), strerror(errno));
		free(dir_name);
		return false;
	}

	struct string **names = NULL;
	int nr_names = 0;

	char *d_name;
	while ((d_name = readdir_utf8(d)) != NULL) {
		if (d_name[0] == '.') {
			free(d_name);
			continue;
		}

		char *utf8_path = path_join(dir_name, d_name);
		ustat s;
		if (stat_utf8(utf8_path, &s) < 0) {
			WARNING("stat(\"%s\"): %s", display_utf0(utf8_path), strerror(errno));
			goto loop_next;
		}
		if (folders) {
			if (!S_ISDIR(s.st_mode))
				goto loop_next;
		} else {
			if (!S_ISREG(s.st_mode))
				goto loop_next;
		}

		char *sjis_name = utf2sjis(d_name, 0);
		names = xrealloc_array(names, nr_names, nr_names+1, sizeof(struct string*));
		names[nr_names++] = cstr_to_string(sjis_name);
		free(sjis_name);
	loop_next:
		free(utf8_path);
		free(d_name);
	}
	closedir_utf8(d);
	free(dir_name);

	union vm_value dim = { .i = nr_names };
	struct page *page = alloc_array(1, &dim, AIN_ARRAY_STRING, 0, false);
	for (int i = 0; i < nr_names; i++) {
		page->values[i].i = heap_alloc_string(names[i]);
	}
	free(names);

	if (*out) {
		delete_page_vars(*out);
		free_page(*out);
	}
	*out = page;
	return true;
}

static bool FileOperation_GetFileList(struct string *folder_name, struct page **out)
{
	return get_file_list(folder_name, out, false);
}

static bool FileOperation_GetFolderList(struct string *folder_name, struct page **out)
{
	return get_file_list(folder_name, out, true);
}

static struct page *FileOperation_GetFileList_v12(struct string *folder_name)
{
	struct page *out = NULL;
	if (!get_file_list(folder_name, &out, false))
		out = v12_array_new(&(struct ain_type){ .data = AIN_STRING }, 0, false);
	return out;
}

static struct page *FileOperation_GetFolderList_v12(struct string *folder_name)
{
	struct page *out = NULL;
	if (!get_file_list(folder_name, &out, true))
		out = v12_array_new(&(struct ain_type){ .data = AIN_STRING }, 0, false);
	return out;
}

static void *FileOperation_ResolveList(const struct ain_hll_function *function)
{
	bool folders = !strcmp(function->name, "GetFolderList");
	if (function->nr_arguments == 1 && function->return_type.data == AIN_ARRAY)
		return folders ? FileOperation_GetFolderList_v12 : FileOperation_GetFileList_v12;
	if (function->nr_arguments == 2)
		return folders ? FileOperation_GetFolderList : FileOperation_GetFileList;
	return NULL;
}

static bool FileOperation_OpenFolder(struct string *folder_name)
{
	return FileOperation_ExistFolder(folder_name);
}

HLL_LIBRARY(FileOperation,
	    HLL_EXPORT(ExistFile, FileOperation_ExistFile),
	    HLL_EXPORT(DeleteFile, FileOperation_DeleteFile),
	    HLL_EXPORT(CopyFile, FileOperation_CopyFile),
	    HLL_EXPORT(GetFileCreationTime, FileOperation_GetFileCreationTime),
	    HLL_EXPORT(GetFileLastAccessTime, FileOperation_GetFileLastAccessTime),
	    HLL_EXPORT(GetFileLastWriteTime, FileOperation_GetFileLastWriteTime),
	    HLL_EXPORT(GetFileSize, FileOperation_GetFileSize),
	    HLL_EXPORT(ExistFolder, FileOperation_ExistFolder),
	    HLL_EXPORT(CreateFolder, FileOperation_CreateFolder),
	    HLL_EXPORT(DeleteFolder, FileOperation_DeleteFolder),
	    HLL_EXPORT(OpenFolder, FileOperation_OpenFolder),
	    HLL_OVERLOAD(GetFileList, FileOperation_ResolveList),
	    HLL_OVERLOAD(GetFileListWithSubFolder, FileOperation_ResolveList),
	    HLL_OVERLOAD(GetFolderList, FileOperation_ResolveList));

static char *clipboard_buf = NULL;

static void Clipboard_SetText(struct string *text)
{
	free(clipboard_buf);
	clipboard_buf = xstrdup(text ? text->text : "");
}

static struct string *Clipboard_GetText(void)
{
	return cstr_to_string(clipboard_buf ? clipboard_buf : "");
}

HLL_LIBRARY(Clipboard,
	    HLL_EXPORT(SetText, Clipboard_SetText),
	    HLL_EXPORT(GetText, Clipboard_GetText));

static FILE *text_file_fp = NULL;

static bool TextFile_Close(void)
{
	if (!text_file_fp)
		return false;
	fclose(text_file_fp);
	text_file_fp = NULL;
	return true;
}

static bool TextFile_Create(struct string *file_name)
{
	if (!file_name)
		return false;
	TextFile_Close();
	char *path = unix_path(file_name->text);
	if (!path)
		return false;
	text_file_fp = file_open_utf8(path, "wb");
	free(path);
	return text_file_fp != NULL;
}

static bool TextFile_Write(struct string *text)
{
	if (!text_file_fp || !text)
		return false;
	if (text->size <= 0)
		return true;
	return fwrite(text->text, 1, text->size, text_file_fp) == (size_t)text->size;
}

static bool TextFile_ReadAll(struct string *file_name, struct string **text)
{
	if (!file_name || !text)
		return false;
	char *path = fileop_resolve_path(file_name->text);
	if (!path)
		return false;
	size_t len = 0;
	char *buf = file_read(path, &len);
	free(path);
	if (!buf)
		return false;
	if (*text)
		free_string(*text);
	*text = make_string(buf, len);
	free(buf);
	return true;
}

HLL_LIBRARY(TextFile,
	    HLL_EXPORT(Create, TextFile_Create),
	    HLL_EXPORT(Close, TextFile_Close),
	    HLL_EXPORT(Write, TextFile_Write),
	    HLL_EXPORT(ReadAll, TextFile_ReadAll));

static int EXWriter_CreateHandle(void) { return 0; }
static void EXWriter_ReleaseHandle(possibly_unused int id) {}
static bool EXWriter_Load(possibly_unused int id, possibly_unused struct string *file_name) { return false; }
static bool EXWriter_Save(possibly_unused int id, possibly_unused struct string *file_name) { return false; }
static bool EXWriter_LoadFromString(possibly_unused int id, possibly_unused struct string *text) { return false; }
static bool EXWriter_SaveToString(possibly_unused int id, struct string **text)
{
	if (text) {
		if (*text)
			free_string(*text);
		*text = make_string("", 0);
	}
	return false;
}
static bool EXWriter_EraseDefine(possibly_unused int id, possibly_unused struct string *name) { return false; }
static bool EXWriter_Rename(possibly_unused int id, possibly_unused struct string *old_name, possibly_unused struct string *new_name) { return false; }
static bool EXWriter_SetPartial(possibly_unused int id, possibly_unused struct string *name, possibly_unused bool partial) { return false; }
static bool EXWriter_SetInt(possibly_unused int id, possibly_unused struct string *name, possibly_unused int data) { return false; }
static bool EXWriter_SetFloat(possibly_unused int id, possibly_unused struct string *name, possibly_unused float data) { return false; }
static bool EXWriter_SetString(possibly_unused int id, possibly_unused struct string *name, possibly_unused struct string *data) { return false; }
static bool EXWriter_AddArrayInt(possibly_unused int id, possibly_unused struct string *name, possibly_unused int data) { return false; }
static bool EXWriter_AddArrayFloat(possibly_unused int id, possibly_unused struct string *name, possibly_unused float data) { return false; }
static bool EXWriter_AddArrayString(possibly_unused int id, possibly_unused struct string *name, possibly_unused struct string *data) { return false; }
static bool EXWriter_AddTableFormatInt(possibly_unused int id, possibly_unused struct string *name, possibly_unused struct string *format_name, possibly_unused bool is_key) { return false; }
static bool EXWriter_AddTableFormatFloat(possibly_unused int id, possibly_unused struct string *name, possibly_unused struct string *format_name) { return false; }
static bool EXWriter_AddTableFormatString(possibly_unused int id, possibly_unused struct string *name, possibly_unused struct string *format_name, possibly_unused bool is_key) { return false; }
static bool EXWriter_AddTableInt(possibly_unused int id, possibly_unused struct string *name, possibly_unused int data) { return false; }
static bool EXWriter_AddTableFloat(possibly_unused int id, possibly_unused struct string *name, possibly_unused float data) { return false; }
static bool EXWriter_AddTableString(possibly_unused int id, possibly_unused struct string *name, possibly_unused struct string *data) { return false; }

HLL_LIBRARY(EXWriter,
	    HLL_EXPORT(CreateHandle, EXWriter_CreateHandle),
	    HLL_EXPORT(ReleaseHandle, EXWriter_ReleaseHandle),
	    HLL_EXPORT(Load, EXWriter_Load),
	    HLL_EXPORT(Save, EXWriter_Save),
	    HLL_EXPORT(LoadFromString, EXWriter_LoadFromString),
	    HLL_EXPORT(SaveToString, EXWriter_SaveToString),
	    HLL_EXPORT(EraseDefine, EXWriter_EraseDefine),
	    HLL_EXPORT(Rename, EXWriter_Rename),
	    HLL_EXPORT(SetPartial, EXWriter_SetPartial),
	    HLL_EXPORT(SetInt, EXWriter_SetInt),
	    HLL_EXPORT(SetFloat, EXWriter_SetFloat),
	    HLL_EXPORT(SetString, EXWriter_SetString),
	    HLL_EXPORT(AddArrayInt, EXWriter_AddArrayInt),
	    HLL_EXPORT(AddArrayFloat, EXWriter_AddArrayFloat),
	    HLL_EXPORT(AddArrayString, EXWriter_AddArrayString),
	    HLL_EXPORT(AddTableFormatInt, EXWriter_AddTableFormatInt),
	    HLL_EXPORT(AddTableFormatFloat, EXWriter_AddTableFormatFloat),
	    HLL_EXPORT(AddTableFormatString, EXWriter_AddTableFormatString),
	    HLL_EXPORT(AddTableInt, EXWriter_AddTableInt),
	    HLL_EXPORT(AddTableFloat, EXWriter_AddTableFloat),
	    HLL_EXPORT(AddTableString, EXWriter_AddTableString));


static bool FileDialog_OpenLoadDialog(struct string **load_file_name,
		possibly_unused struct string *title_name,
		possibly_unused struct string *folder_name,
		possibly_unused struct page *filter_title_list,
		possibly_unused struct page *filter_extend_list)
{
	(void)load_file_name;
	return false;
}

static bool FileDialog_OpenSaveDialog(struct string **save_file_name,
		possibly_unused struct string *title_name,
		possibly_unused struct string *folder_name,
		possibly_unused struct page *filter_title_list,
		possibly_unused struct page *filter_extend_list)
{
	(void)save_file_name;
	return false;
}

static bool FileDialog_OpenSelectFolderDialog(possibly_unused struct string *display_text,
		possibly_unused struct string *title_text,
		struct string **folder_name)
{
	(void)folder_name;
	return false;
}

HLL_LIBRARY(FileDialog,
	    HLL_EXPORT(OpenLoadDialog, FileDialog_OpenLoadDialog),
	    HLL_EXPORT(OpenSaveDialog, FileDialog_OpenSaveDialog),
	    HLL_EXPORT(OpenSelectFolderDialog, FileDialog_OpenSelectFolderDialog));

static void ADVEngine_GetFunctionList(possibly_unused struct string *file_path,
		possibly_unused struct page **p_array)
{
}
static bool ADVEngine_Load(possibly_unused struct string *function_name) { return false; }
static bool ADVEngine_LoadByFile(possibly_unused struct string *file_path,
		possibly_unused struct string *function_name) { return false; }
static void ADVEngine_Release(void) {}
static bool ADVEngine_IsExistLowerIf(void) { return false; }
static int ADVEngine_NumofStep(void) { return 0; }
static struct string *ADVEngine_GetStepText(possibly_unused int step) { return string_ref(&EMPTY_STRING); }
static void ADVEngine_OpenFunctionFile(possibly_unused int step) {}
static int ADVEngine_GetCodeType(possibly_unused int step) { return 0; }
static struct string *ADVEngine_GetMessage(possibly_unused int step) { return string_ref(&EMPTY_STRING); }
static bool ADVEngine_BindFunction(possibly_unused int step) { return false; }
static struct string *ADVEngine_GetFuncName(possibly_unused int step) { return string_ref(&EMPTY_STRING); }
static int ADVEngine_CreateDelegateObjectID(possibly_unused int step) { return -1; }
static bool ADVEngine_GetArgumentValue_int(int *value, possibly_unused int step, possibly_unused int index)
{
	if (value) *value = 0;
	return false;
}
static bool ADVEngine_GetArgumentValue_float(float *value, possibly_unused int step, possibly_unused int index)
{
	if (value) *value = 0.0f;
	return false;
}
static bool ADVEngine_GetArgumentValue_bool(int *value, possibly_unused int step, possibly_unused int index)
{
	if (value) *value = 0;
	return false;
}
static bool ADVEngine_GetArgumentValue_string(struct string **value, possibly_unused int step, possibly_unused int index)
{
	if (value) {
		if (*value)
			free_string(*value);
		*value = string_ref(&EMPTY_STRING);
	}
	return false;
}
static struct string *ADVEngine_GetIfFuncName(possibly_unused int step) { return string_ref(&EMPTY_STRING); }
static struct string *ADVEngine_GetIfFuncArg(possibly_unused int step) { return string_ref(&EMPTY_STRING); }
static int ADVEngine_GetIfValue(possibly_unused int step) { return 0; }
static bool ADVEngine_IsLowerIf(possibly_unused int step) { return false; }
static struct string *ADVEngine_GetAttribute(possibly_unused struct string *key) { return string_ref(&EMPTY_STRING); }

HLL_LIBRARY(ADVEngine,
	    HLL_EXPORT(GetFunctionList, ADVEngine_GetFunctionList),
	    HLL_EXPORT(Load, ADVEngine_Load),
	    HLL_EXPORT(LoadByFile, ADVEngine_LoadByFile),
	    HLL_EXPORT(Release, ADVEngine_Release),
	    HLL_EXPORT(IsExistLowerIf, ADVEngine_IsExistLowerIf),
	    HLL_EXPORT(NumofStep, ADVEngine_NumofStep),
	    HLL_EXPORT(GetStepText, ADVEngine_GetStepText),
	    HLL_EXPORT(OpenFunctionFile, ADVEngine_OpenFunctionFile),
	    HLL_EXPORT(GetCodeType, ADVEngine_GetCodeType),
	    HLL_EXPORT(GetMessage, ADVEngine_GetMessage),
	    HLL_EXPORT(BindFunction, ADVEngine_BindFunction),
	    HLL_EXPORT(GetFuncName, ADVEngine_GetFuncName),
	    HLL_EXPORT(CreateDelegateObjectID, ADVEngine_CreateDelegateObjectID),
	    HLL_EXPORT(GetArgumentValue_int, ADVEngine_GetArgumentValue_int),
	    HLL_EXPORT(GetArgumentValue_float, ADVEngine_GetArgumentValue_float),
	    HLL_EXPORT(GetArgumentValue_bool, ADVEngine_GetArgumentValue_bool),
	    HLL_EXPORT(GetArgumentValue_string, ADVEngine_GetArgumentValue_string),
	    HLL_EXPORT(GetIfFuncName, ADVEngine_GetIfFuncName),
	    HLL_EXPORT(GetIfFuncArg, ADVEngine_GetIfFuncArg),
	    HLL_EXPORT(GetIfValue, ADVEngine_GetIfValue),
	    HLL_EXPORT(IsLowerIf, ADVEngine_IsLowerIf),
	    HLL_EXPORT(GetAttribute, ADVEngine_GetAttribute));
