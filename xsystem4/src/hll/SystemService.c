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

#include <string.h>
#include <assert.h>

#include "system4/ain.h"
#include "system4/string.h"
#include "system4/utfsjis.h"

#include "cJSON.h"
#include "input.h"
#include "gfx/gfx.h"
#include "gfx/private.h"
#include "mixer.h"
#include "savedata.h"
#include "vm.h"
#include "xsystem4.h"
#include "hll.h"
#include "system4/file.h"

static int SystemService_GetMixerName(int n, struct string **name)
{
	const char *r = mixer_get_name(n);
	if (!r)
		return 0;
	*name = make_string(r, strlen(r));
	return 1;
}

static bool SystemService_GetMixerDefaultVolume(int n, int *volume)
{
	if (n < 0 || (unsigned)n >= config.mixer_nr_channels)
		return false;
	*volume = config.mixer_volumes[n];
	return true;
}

static bool SystemService_SetMixerName(int n, struct string *name)
{
	return mixer_set_name(n, name->text);
}

static int SystemService_GetGameVersion(void)
{
	return ain->game_version;
}

static struct string *SystemService_GetGameVersionByText(void)
{
	if (game_rance10) {
		// This API returns a compatibility identifier, not a dotted version.
		// Captured from the original Rance X v1.04 runtime.
		return cstr_to_string("lasdf62uihf29fosif83oisdhf15j2l87asdf");
	}
	VM_ERROR("Unknown game version text for this title");
}

static void SystemService_GetGameName(struct string **game_name)
{
	if (*game_name)
		free_string(*game_name);
	*game_name = cstr_to_string(config.game_name);
}

static struct string *SystemService_GetGameName_v12(void)
{
	return cstr_to_string(config.game_name);
}

static void *SystemService_ResolveGameName(const struct ain_hll_function *f)
{
	if (!f->nr_arguments && f->return_type.data == AIN_STRING)
		return SystemService_GetGameName_v12;
	if (f->nr_arguments == 1 && f->return_type.data == AIN_VOID
			&& f->arguments[0].type.data == AIN_REF_STRING)
		return SystemService_GetGameName;
	return NULL;
}

HLL_WARN_UNIMPLEMENTED(false, bool, SystemService, AddURLMenu, struct string *title, struct string *url);

static bool SystemService_IsFullScreen(void)
{
	return gfx_is_fullscreen();
}

static bool SystemService_ChangeNormalScreen(void)
{
	return gfx_set_fullscreen(false);
}

static bool SystemService_ChangeFullScreen(void)
{
	return gfx_set_fullscreen(true);
}

HLL_WARN_UNIMPLEMENTED(false, bool, SystemService, InitMainWindowPosAndSize);

static bool SystemService_UpdateView(void)
{
	gfx_swap();
	return true;
}

static void SystemService_ShowWaitMessage(bool show)
{
#ifdef __ANDROID__
	SDL_AndroidSendMessage(0x8001, show);
#else
	static char *previous_title;
	if (!sdl.window) return;
	if (show && !previous_title) {
		previous_title = strdup(SDL_GetWindowTitle(sdl.window));
		SDL_SetWindowTitle(sdl.window, "Loading...");
	} else if (!show && previous_title) {
		SDL_SetWindowTitle(sdl.window, previous_title);
		free(previous_title);
		previous_title = NULL;
	}
#endif
}

static int SystemService_GetViewWidth(void)
{
	return config.view_width;
}

static int SystemService_GetViewHeight(void)
{
	return config.view_height;
}

static float SystemService_GetGameViewScaleRate(void)
{
	// Rendering and hit testing both use SDL's unscaled logical coordinates.
	return 1.0f;
}

static bool SystemService_MoveMouseCursorPosImmediately(int x, int y)
{
	mouse_set_pos(x, y);
	return true;
}

static bool hide_mouse_cursor_by_game = false;

static bool SystemService_SetHideMouseCursorByGame(bool hide)
{
	hide_mouse_cursor_by_game = hide;
	return mouse_show_cursor(!hide);
}

//bool SystemService_GetHideMouseCursorByGame(void);
HLL_WARN_UNIMPLEMENTED(false, bool, SystemService, SetUsePower2Texture, bool use);
//bool SystemService_GetUsePower2Texture(void);
HLL_WARN_UNIMPLEMENTED(true, bool, SystemService, SetAntiAliasingMode, int mode);
//int SystemService_GetAntiAliasingMode(void);

enum window_settings_asect_ratio {
	ASPECT_RATIO_NORMAL,
	ASPECT_RATIO_FIXED
};

enum window_settings_scaling_type {
	SCALING_NORMAL,
	SCALING_BICUBIC,
};

struct window_settings {
	enum window_settings_asect_ratio aspect_ratio;
	enum window_settings_scaling_type scaling_type;
	bool wait_vsync;
	bool record_pos_size;
	bool minimize_by_full_screen_inactive;
	bool back_to_title_confirm;
	bool close_game_confirm;
};

static struct window_settings window_settings = {
	.aspect_ratio = ASPECT_RATIO_NORMAL,
	.scaling_type = SCALING_NORMAL,
#ifdef __ANDROID__
	.wait_vsync = true,
#else
	.wait_vsync = false,
#endif
	.record_pos_size = false,
	.minimize_by_full_screen_inactive = true,
	.back_to_title_confirm = true,
	.close_game_confirm = true,
};

enum window_settings_id {
	WINDOW_SETTINGS_ASPECT_RATIO = 0,
	WINDOW_SETTINGS_SCALING_TYPE = 1,
	WINDOW_SETTINGS_WAIT_VSYNC = 2,
	WINDOW_SETTINGS_RECORD_POS_SIZE = 3,
	WINDOW_SETTINGS_MINIMIZE_BY_FULL_SCREEN_INACTIVE = 4,
	WINDOW_SETTINGS_BACK_TO_TITLE_CONFIRM = 5,
	WINDOW_SETTINGS_CLOSE_GAME_CONFIRM = 6,
};

static void save_window_settings(void)
{
	cJSON *root = cJSON_CreateObject();
	cJSON_AddBoolToObject(root, "wait_vsync", window_settings.wait_vsync);
	save_json("WindowSetting.json", root);
	cJSON_Delete(root);
}

static void load_window_settings(void)
{
	cJSON *root = load_json("WindowSetting.json");
	if (!root)
		return;
	cJSON *v;
	if ((v = cJSON_GetObjectItem(root, "wait_vsync"))) {
		window_settings.wait_vsync = cJSON_IsTrue(v);
		gfx_set_wait_vsync(window_settings.wait_vsync);
	}
	cJSON_Delete(root);
}

static bool SystemService_SetWindowSetting(int type, int value)
{
	switch (type) {
	case WINDOW_SETTINGS_ASPECT_RATIO:
		window_settings.aspect_ratio = value;
		break;
	case WINDOW_SETTINGS_SCALING_TYPE:
		window_settings.scaling_type = value;
		break;
	case WINDOW_SETTINGS_WAIT_VSYNC:
		window_settings.wait_vsync = value;
		gfx_set_wait_vsync(value);
		break;
	case WINDOW_SETTINGS_RECORD_POS_SIZE:
		window_settings.record_pos_size = value;
		break;
	case WINDOW_SETTINGS_MINIMIZE_BY_FULL_SCREEN_INACTIVE:
		window_settings.minimize_by_full_screen_inactive = value;
		break;
	case WINDOW_SETTINGS_BACK_TO_TITLE_CONFIRM:
		window_settings.back_to_title_confirm = value;
		break;
	case WINDOW_SETTINGS_CLOSE_GAME_CONFIRM:
		window_settings.close_game_confirm = value;
		break;
	default:
		WARNING("Invalid window setting type: %d", type);
		return false;
	}
	save_window_settings();
	return true;
}

static bool SystemService_GetWindowSetting(int type, int *value)
{
	switch (type) {
	case WINDOW_SETTINGS_ASPECT_RATIO:
		*value = window_settings.aspect_ratio;
		break;
	case WINDOW_SETTINGS_SCALING_TYPE:
		*value = window_settings.scaling_type;
		break;
	case WINDOW_SETTINGS_WAIT_VSYNC:
		*value = window_settings.wait_vsync;
		break;
	case WINDOW_SETTINGS_RECORD_POS_SIZE:
		*value = window_settings.record_pos_size;
		break;
	case WINDOW_SETTINGS_MINIMIZE_BY_FULL_SCREEN_INACTIVE:
		*value = window_settings.minimize_by_full_screen_inactive;
		break;
	case WINDOW_SETTINGS_BACK_TO_TITLE_CONFIRM:
		*value = window_settings.back_to_title_confirm;
		break;
	case WINDOW_SETTINGS_CLOSE_GAME_CONFIRM:
		*value = window_settings.close_game_confirm;
		break;
	default:
		WARNING("Invalid window setting type: %d", type);
		return false;
	}
	return true;
}

// XXX: Values for 'type' above 1 are invalid in Haru Urare, may be different in other games
#define NR_MOUSE_CURSOR_CONFIG 3
static int mouse_cursor_config[NR_MOUSE_CURSOR_CONFIG] = {0, 0, 3000};

static bool SystemService_SetMouseCursorConfig(int type, int value)
{
	if (type < 0 || type >= NR_MOUSE_CURSOR_CONFIG) {
		WARNING("Invalid mouse cursor config type: %d", type);
		return false;
	}
	mouse_cursor_config[type] = value;
	return true;
}

static bool SystemService_GetMouseCursorConfig(int type, int *value)
{
	if (type < 0 || type >= NR_MOUSE_CURSOR_CONFIG) {
		WARNING("Invalid mouse cursor config type: %d", type);
		return false;
	}
	*value = type < 2 ? !!mouse_cursor_config[type] : mouse_cursor_config[type];
	return true;
}

//bool SystemService_RunProgram(struct string *program_file_name, struct string *parameter);
//bool SystemService_IsOpenedMutex(struct string *mutex_name);

void SystemService_GetGameFolderPath(struct string **folder_path)
{
	char *sjis = utf2sjis(config.game_dir, 0);
	size_t len = strlen(sjis);
	if (len > 0 && sjis[len - 1] != '/' && sjis[len - 1] != '\\') {
		struct string *s = make_string(sjis, len);
		string_push_back(&s, '/');
		*folder_path = s;
	} else {
		*folder_path = make_string(sjis, len);
	}
	free(sjis);
}

static struct string *SystemService_GetGameFolderPath_v12(void)
{
	struct string *folder = NULL;
	SystemService_GetGameFolderPath(&folder);
	return folder;
}

static void *SystemService_ResolveGameFolder(const struct ain_hll_function *f)
{
	if (!f->nr_arguments && f->return_type.data == AIN_STRING)
		return SystemService_GetGameFolderPath_v12;
	if (f->nr_arguments == 1 && f->return_type.data == AIN_VOID
			&& f->arguments[0].type.data == AIN_REF_STRING)
		return SystemService_GetGameFolderPath;
	return NULL;
}

static void SystemService_GetTime(int *hour, int *min, int *sec)
{
	int ms;
	get_time(hour, min, sec, &ms);
}

static bool SystemService_IsResetOnce(void)
{
	return vm_reset_once;
}

static bool SystemService_IsResetOnce_Drapeko(struct string **text)
{
	*text = cstr_to_string("XXX TTT YYY"); // ???
	return vm_reset_once;
}

static char *get_manual_filename(void) {
	char *manual_path = path_join("Manual", "index.html");
	char *file_path = path_join(config.game_dir, manual_path);
	free(manual_path);
	return file_path;
}

static bool SystemService_IsExistPlayingManual(void) {
	char *filename = get_manual_filename();
	const bool exists = file_exists(filename);
	free(filename);
	return exists;
}

#ifndef _WIN32
static char *percent_encode(const char *str) {
	const char *hex = "0123456789ABCDEF";
	// Worst case all characters are percent-encoded
	char *encoded = xmalloc(strlen(str) * 3 + 1);

	char *p = encoded;
	while (*str) {
		const unsigned char c = *str++;
		if ((c >= 'a' && c <= 'z') ||
			(c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') ||
			c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
			*p++ = (char)c;
		} else {
			*p++ = '%';
			*p++ = hex[c >> 4];
			*p++ = hex[c & 15];
		}
	}

	*p = '\0';
	return encoded;
}
#endif

static void SystemService_OpenPlayingManual(void) {
	if (!SystemService_IsExistPlayingManual()) {
		return;
	}
#ifdef __ANDROID__
	const int COMMAND_OPEN_PLAYING_MANUAL = 0x8000;
	SDL_AndroidSendMessage(COMMAND_OPEN_PLAYING_MANUAL, 0);
#else
	char *filename = get_manual_filename();

	char *real_path = realpath_utf8(filename);
	free(filename);
	if (!real_path) {
		return;
	}

#ifdef _WIN32
	const char *prefix = "file:///";
	char *path_component = real_path;
#else
	const char *prefix = "file://";
	char *path_component = percent_encode(real_path);
	free(real_path);
#endif
	char *url = xmalloc(strlen(prefix) + strlen(path_component) + 1);
	strcpy(url, prefix);
	strcat(url, path_component);

	if (SDL_OpenURL(url) < 0) {
		WARNING("Failed to open manual at '%s': %s", url, SDL_GetError());
	}

	free(url);
	free(path_component);
#endif
}

//static bool SystemService_IsExistSystemMessage(void);
HLL_QUIET_UNIMPLEMENTED(false, bool, SystemService, IsExistSystemMessage);
//static bool SystemService_PopSystemMessage(int *message);

static void SystemService_RestrainScreensaver(void) { }

//static int SystemService_Debug_GetUseVideoMemorySize(void);

// Rance 01
static void SystemService_Rance0123456789(struct string **text)
{
	*text = cstr_to_string("-RANCE010ECNAR-");
}

// Rance 01 trial edition
static void SystemService_XXXXX01XXXXXXXX(struct string **text)
{
	*text = cstr_to_string("RANCE01RANCEKAKKOII");
}

// Drapeko
static void SystemService_Test(struct string **text)
{
	*text = cstr_to_string("DELETE ALL 758490275489207548093");
}

// Drapeko trial edition
static void SystemService_DRPKT(struct string **text)
{
	*text = cstr_to_string("DRPKT QWERTY NUFUAUEO 75849027582754829");
}

// Rance 9
static void SystemService_Rance96161988(struct string **text) {
	*text = cstr_to_string("=Rance99/RANCE99=");
}

// Pascha3 Plus Contents
static void SystemService_XXX(struct string **text) {
	*text = cstr_to_string("FORMAT HDD ERASE 578205024758284076520478254092784789752384758204687293");
}

static void SystemService_PreLink(void);

struct service_variable {
	struct string *key, *value;
	struct service_variable *next;
};

// These stores survive a VM reset, but end with the engine process.
static struct service_variable *system_variables, *game_variables;

static struct service_variable **service_variable_find(struct service_variable **store, const char *key)
{
	while (*store && strcmp((*store)->key->text, key) < 0) store = &(*store)->next;
	return store;
}

static bool service_variable_exists(struct service_variable **store, struct string *key)
{
	struct service_variable *v = *service_variable_find(store, key->text);
	return v && !strcmp(v->key->text, key->text);
}

static void service_variable_set(struct service_variable **store, struct string *key, struct string *value)
{
	store = service_variable_find(store, key->text);
	if (*store && !strcmp((*store)->key->text, key->text)) {
		free_string((*store)->value);
		(*store)->value = string_dup(value);
		return;
	}
	struct service_variable *v = xmalloc(sizeof(*v));
	*v = (struct service_variable) { string_dup(key), string_dup(value), *store };
	*store = v;
}

static struct string *service_variable_get(struct service_variable **store, struct string *key)
{
	struct service_variable *v = *service_variable_find(store, key->text);
	return string_ref(v && !strcmp(v->key->text, key->text) ? v->value : &EMPTY_STRING);
}

static void service_variable_erase(struct service_variable **store, struct string *key)
{
	store = service_variable_find(store, key->text);
	struct service_variable *v = *store;
	if (!v || strcmp(v->key->text, key->text)) return;
	*store = v->next;
	free_string(v->key);
	free_string(v->value);
	free(v);
}

#define SERVICE_VARIABLE_FUNCTIONS(prefix, store) \
	static bool prefix##_IsExist(struct string *key) { return service_variable_exists(&store, key); } \
	static void prefix##_Set(struct string *key, struct string *value) { service_variable_set(&store, key, value); } \
	static struct string *prefix##_Get(struct string *key) { return service_variable_get(&store, key); } \
	static void prefix##_Erase(struct string *key) { service_variable_erase(&store, key); } \
	static int prefix##_NumofKey(void) { \
		int n = 0; for (struct service_variable *v = store; v; v = v->next) n++; return n; \
	} \
	static struct string *prefix##_GetKey(int index) { \
		for (struct service_variable *v = store; v; v = v->next) \
			if (!index--) return string_ref(v->value); \
		return string_ref(&EMPTY_STRING); \
	}

SERVICE_VARIABLE_FUNCTIONS(SystemVariable, system_variables)
SERVICE_VARIABLE_FUNCTIONS(GameVariable, game_variables)
// Despite its name, v12 GetKey returns the value at the sorted key index.

#define SERVICE_VARIABLE_EXPORTS(prefix) \
	HLL_EXPORT(prefix##_IsExist, prefix##_IsExist), \
	HLL_EXPORT(prefix##_Set, prefix##_Set), \
	HLL_EXPORT(prefix##_Get, prefix##_Get), \
	HLL_EXPORT(prefix##_NumofKey, prefix##_NumofKey), \
	HLL_EXPORT(prefix##_GetKey, prefix##_GetKey), \
	HLL_EXPORT(prefix##_Erase, prefix##_Erase)

static void SystemService_ModuleInit(void)
{
	load_window_settings();
}

static void SystemService_SetAndroidViewKeepScreen(bool keep)
{
	if (keep) SDL_DisableScreenSaver();
	else SDL_EnableScreenSaver();
}

static bool view_resizable_mode = false;
static bool hook_close_app = false;
static bool show_mipmap_level = false;

static bool SystemService_GetHideMouseCursorByGame(void)
{
	return hide_mouse_cursor_by_game;
}

static bool SystemService_GetUsePower2Texture(void)
{
	return false;
}

static int SystemService_GetAntiAliasingMode(void)
{
	return 0;
}

static void SystemService_SetAndroidViewOrientation(int mode)
{
	(void)mode;
}

static void SystemService_SetGameViewScaleRate(float rate)
{
	(void)rate;
}

static void SystemService_SetViewSize(int width, int height)
{
	(void)width;
	(void)height;
}

static void SystemService_SetWindowPos(int x, int y)
{
	(void)x;
	(void)y;
}

static int SystemService_GetWindowPosX(void)
{
	return 0;
}

static int SystemService_GetWindowPosY(void)
{
	return 0;
}

static void SystemService_SetViewResizableMode(bool mode)
{
	view_resizable_mode = mode;
}

static bool SystemService_IsViewResizableMode(void)
{
	return view_resizable_mode;
}

static bool SystemService_SetWaitVSyncMode(int mode)
{
	window_settings.wait_vsync = !!mode;
	gfx_set_wait_vsync(window_settings.wait_vsync);
	return true;
}

static int SystemService_GetWaitVSyncMode(void)
{
	return window_settings.wait_vsync ? 1 : 0;
}

static bool SystemService_RunProgram(struct string *program, struct string *param)
{
	(void)program;
	(void)param;
	return false;
}

static bool SystemService_IsOpenedMutex(struct string *mutex_name)
{
	(void)mutex_name;
	return false;
}

static bool SystemService_PopSystemMessage(int *message)
{
	(void)message;
	return false;
}

static bool SystemService_PopSystemMessageString(struct string **message)
{
	(void)message;
	return false;
}

static int SystemService_GetPlatformType(void)
{
	return 0;
}

static void SystemService_SetHookCloseApp(bool flag)
{
	hook_close_app = flag;
}

static bool SystemService_IsHookCloseApp(void)
{
	return hook_close_app;
}

static bool SystemService_SaveScreenshot(struct string *file_name, int x, int y, int width, int height)
{
	(void)file_name;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	return false;
}

static bool SystemService_RenderToSurfaceImage(struct page **surface)
{
	(void)surface;
	return false;
}

static bool SystemService_RenderToSurfaceImageByScale(struct page **surface, int filter)
{
	(void)surface;
	(void)filter;
	return false;
}

static struct string *SystemService_EmptyInfo(void)
{
	return string_ref(&EMPTY_STRING);
}

static void SystemService_GetCPUInfo(struct string **vendor, int *sig, int *flag, struct string **brand)
{
	if (vendor) {
		if (*vendor) free_string(*vendor);
		*vendor = string_ref(&EMPTY_STRING);
	}
	if (sig) *sig = 0;
	if (flag) *flag = 0;
	if (brand) {
		if (*brand) free_string(*brand);
		*brand = string_ref(&EMPTY_STRING);
	}
}

static void SystemService_GetMemoryInfo(int *max_mem, int *use_mem)
{
	if (max_mem) *max_mem = 0;
	if (use_mem) *use_mem = 0;
}

static int SystemService_Debug_GetUseMemorySize(void)
{
	return 0;
}

static int SystemService_Debug_GetUseVideoMemorySize(void)
{
	return 0;
}

static void SystemService_Debug_SetShowMipmapLevel(bool flag)
{
	show_mipmap_level = flag;
}

static bool SystemService_Debug_IsShowMipmapLevel(void)
{
	return show_mipmap_level;
}

HLL_LIBRARY(SystemService,
	    HLL_EXPORT(_PreLink, SystemService_PreLink),
	    SERVICE_VARIABLE_EXPORTS(SystemVariable),
	    SERVICE_VARIABLE_EXPORTS(GameVariable),
	    HLL_EXPORT(_ModuleInit, SystemService_ModuleInit),
	    HLL_EXPORT(GetMixerNumof, mixer_get_numof),
	    HLL_EXPORT(GetMixerName, SystemService_GetMixerName),
	    HLL_EXPORT(GetMixerVolume, mixer_get_volume),
	    HLL_EXPORT(GetMixerDefaultVolume, SystemService_GetMixerDefaultVolume),
	    HLL_EXPORT(GetMixerMute, mixer_get_mute),
	    HLL_EXPORT(SetMixerName, SystemService_SetMixerName),
	    HLL_EXPORT(SetMixerVolume, mixer_set_volume),
	    HLL_EXPORT(SetMixerMute, mixer_set_mute),
	    HLL_EXPORT(GetGameVersion, SystemService_GetGameVersion),
	    HLL_EXPORT(GetGameVersionByText, SystemService_GetGameVersionByText),
	    HLL_OVERLOAD(GetGameName, SystemService_ResolveGameName),
	    HLL_EXPORT(AddURLMenu, SystemService_AddURLMenu),
	    HLL_EXPORT(IsFullScreen, SystemService_IsFullScreen),
	    HLL_EXPORT(ChangeNormalScreen, SystemService_ChangeNormalScreen),
	    HLL_EXPORT(ChangeFullScreen, SystemService_ChangeFullScreen),
	    HLL_EXPORT(InitMainWindowPosAndSize, SystemService_InitMainWindowPosAndSize),
	    HLL_EXPORT(UpdateView, SystemService_UpdateView),
	    HLL_EXPORT(ShowWaitMessage, SystemService_ShowWaitMessage),
	    HLL_EXPORT(SetAndroidViewKeepScreen, SystemService_SetAndroidViewKeepScreen),
	    HLL_EXPORT(SetAndroidViewOrientation, SystemService_SetAndroidViewOrientation),
	    HLL_EXPORT(GetPlatformType, SystemService_GetPlatformType),
	    HLL_EXPORT(SetHookCloseApp, SystemService_SetHookCloseApp),
	    HLL_EXPORT(IsHookCloseApp, SystemService_IsHookCloseApp),
	    HLL_EXPORT(SetGameViewScaleRate, SystemService_SetGameViewScaleRate),
	    HLL_EXPORT(SetViewSize, SystemService_SetViewSize),
	    HLL_EXPORT(SetWindowPos, SystemService_SetWindowPos),
	    HLL_EXPORT(GetWindowPosX, SystemService_GetWindowPosX),
	    HLL_EXPORT(GetWindowPosY, SystemService_GetWindowPosY),
	    HLL_EXPORT(SetViewResizableMode, SystemService_SetViewResizableMode),
	    HLL_EXPORT(IsViewResizableMode, SystemService_IsViewResizableMode),
	    HLL_EXPORT(SetWaitVSyncMode, SystemService_SetWaitVSyncMode),
	    HLL_EXPORT(GetWaitVSyncMode, SystemService_GetWaitVSyncMode),
	    HLL_EXPORT(GetHideMouseCursorByGame, SystemService_GetHideMouseCursorByGame),
	    HLL_EXPORT(GetUsePower2Texture, SystemService_GetUsePower2Texture),
	    HLL_EXPORT(GetAntiAliasingMode, SystemService_GetAntiAliasingMode),
	    HLL_EXPORT(RunProgram, SystemService_RunProgram),
	    HLL_EXPORT(IsOpenedMutex, SystemService_IsOpenedMutex),
	    HLL_EXPORT(PopSystemMessage, SystemService_PopSystemMessage),
	    HLL_EXPORT(PopSystemMessageString, SystemService_PopSystemMessageString),
	    HLL_EXPORT(SaveScreenshot, SystemService_SaveScreenshot),
	    HLL_EXPORT(RenderToSurfaceImage, SystemService_RenderToSurfaceImage),
	    HLL_EXPORT(RenderToSurfaceImageByScale, SystemService_RenderToSurfaceImageByScale),
	    HLL_EXPORT(GetCPUInfo, SystemService_GetCPUInfo),
	    HLL_EXPORT(GetMemoryInfo, SystemService_GetMemoryInfo),
	    HLL_EXPORT(GetOSInfo, SystemService_EmptyInfo),
	    HLL_EXPORT(GetScreenInfo, SystemService_EmptyInfo),
	    HLL_EXPORT(Debug_GetUseMemorySize, SystemService_Debug_GetUseMemorySize),
	    HLL_EXPORT(Debug_GetUseVideoMemorySize, SystemService_Debug_GetUseVideoMemorySize),
	    HLL_EXPORT(Debug_SetShowMipmapLevel, SystemService_Debug_SetShowMipmapLevel),
	    HLL_EXPORT(Debug_IsShowMipmapLevel, SystemService_Debug_IsShowMipmapLevel),
	    HLL_EXPORT(GetViewWidth, SystemService_GetViewWidth),
	    HLL_EXPORT(GetViewHeight, SystemService_GetViewHeight),
	    HLL_EXPORT(GetDefaultViewWidth, SystemService_GetViewWidth),
	    HLL_EXPORT(GetDefaultViewHeight, SystemService_GetViewHeight),
	    HLL_EXPORT(GetGameViewScaleRate, SystemService_GetGameViewScaleRate),
	    HLL_EXPORT(MoveMouseCursorPosImmediately, SystemService_MoveMouseCursorPosImmediately),
	    HLL_EXPORT(SetHideMouseCursorByGame, SystemService_SetHideMouseCursorByGame),
	    HLL_TODO_EXPORT(GetHideMouseCursorByGame, SystemService_GetHideMouseCursorByGame),
	    HLL_EXPORT(SetUsePower2Texture, SystemService_SetUsePower2Texture),
	    HLL_TODO_EXPORT(GetUsePower2Texture, SystemService_GetUsePower2Texture),
	    HLL_EXPORT(SetAntiAliasingMode, SystemService_SetAntiAliasingMode),
	    HLL_TODO_EXPORT(GetAntiAliasingMode, SystemService_GetAntiAliasingMode),
	    HLL_EXPORT(SetWindowSetting, SystemService_SetWindowSetting),
	    HLL_EXPORT(GetWindowSetting, SystemService_GetWindowSetting),
	    HLL_EXPORT(SetMouseCursorConfig, SystemService_SetMouseCursorConfig),
	    HLL_EXPORT(GetMouseCursorConfig, SystemService_GetMouseCursorConfig),
	    HLL_TODO_EXPORT(RunProgram, SystemService_RunProgram),
	    HLL_TODO_EXPORT(IsOpenedMutex, SystemService_IsOpenedMutex),
	    HLL_OVERLOAD(GetGameFolderPath, SystemService_ResolveGameFolder),
	    HLL_EXPORT(GetDate, get_date),
	    HLL_EXPORT(GetTime, SystemService_GetTime),
	    HLL_EXPORT(IsResetOnce, SystemService_IsResetOnce),
	    HLL_EXPORT(OpenPlayingManual, SystemService_OpenPlayingManual),
	    HLL_EXPORT(IsExistPlayingManual, SystemService_IsExistPlayingManual),
	    HLL_EXPORT(IsExistSystemMessage, SystemService_IsExistSystemMessage),
	    HLL_TODO_EXPORT(PopSystemMessage, SystemService_PopSystemMessage),
	    HLL_EXPORT(RestrainScreensaver, SystemService_RestrainScreensaver),
	    HLL_TODO_EXPORT(Debug_GetUseVideoMemorySize, SystemService_Debug_GetUseVideoMemorySize),
	    HLL_EXPORT(Rance0123456789, SystemService_Rance0123456789),
	    HLL_EXPORT(XXXXX01XXXXXXXX, SystemService_XXXXX01XXXXXXXX),
	    HLL_EXPORT(XXX, SystemService_XXX),
	    HLL_EXPORT(Test, SystemService_Test),
	    HLL_EXPORT(DRPKT, SystemService_DRPKT),
	    HLL_EXPORT(Rance96161988, SystemService_Rance96161988)
	);

static struct ain_hll_function *get_fun(int libno, const char *name)
{
	int fno = ain_get_library_function(ain, libno, name);
	return fno >= 0 ? &ain->libraries[libno].functions[fno] : NULL;
}

static bool InstallInfo_IsInstalledGame(struct string *game_name)
{
	(void)game_name;
	return false;
}

static int InstallInfo_GetInstallMediaType(void)
{
	return 1;
}

HLL_LIBRARY(InstallInfo,
	    HLL_EXPORT(IsInstalledGame, InstallInfo_IsInstalledGame),
	    HLL_EXPORT(GetInstallMediaType, InstallInfo_GetInstallMediaType));

static bool PCInfo_GetEmpty(struct string **info)
{
	if (*info)
		free_string(*info);
	*info = string_ref(&EMPTY_STRING);
	return true;
}

HLL_LIBRARY(PCInfo,
	    HLL_EXPORT(GetOSInfo, PCInfo_GetEmpty),
	    HLL_EXPORT(GetCPUInfo, PCInfo_GetEmpty),
	    HLL_EXPORT(GetMemoryInfo, PCInfo_GetEmpty),
	    HLL_EXPORT(GetScreenInfo, PCInfo_GetEmpty),
	    HLL_EXPORT(GetSoundDeviceInfo, PCInfo_GetEmpty),
	    HLL_EXPORT(GetVideoCardInfo, PCInfo_GetEmpty),
	    HLL_EXPORT(GetMonitorInfo, PCInfo_GetEmpty),
	    HLL_EXPORT(GetDirectXInfo, PCInfo_GetEmpty));

static void SystemService_PreLink(void)
{
	struct ain_hll_function *fun;
	int libno = ain_get_library(ain, "SystemService");
	assert(libno >= 0);

	fun = get_fun(libno, "IsResetOnce");
	if (fun && fun->nr_arguments == 1) {
		static_library_replace(&lib_SystemService, "IsResetOnce",
				SystemService_IsResetOnce_Drapeko);
	}
}
