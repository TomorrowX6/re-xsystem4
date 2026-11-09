/* AIN v12 sound channels, GPL-2.0-or-later. */
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "system4/archive.h"
#include "system4/ex.h"
#include "system4/file.h"
#include "system4/string.h"
#include "asset_manager.h"
#include "audio.h"
#include "id_pool.h"
#include "kiwi.h"
#include "mixer.h"
#include "xsystem4.h"
#include "hll.h"

static struct id_pool sounds, effects;
static struct ex *metadata;
static struct ex_table *sound_info;

struct effect_voice { struct channel *channel; struct effect_voice *next; };
struct effect {
	int span;
	bool multi, started;
	uint32_t last_play;
	struct effect_voice *voices;
};

static struct effect system_effect = { .multi = true };

static void kiwi_prelink(void)
{
	if (game_rance10 && !config.mixer_nr_channels) {
		const char *names[] = { "Master", "BGM", "SE", "Voice", "Ambient", "BackVoice" };
		config.mixer_nr_channels = 6;
		config.mixer_channels = xcalloc(6, sizeof(char *));
		config.mixer_volumes = xcalloc(6, sizeof(int));
		for (int i = 0; i < 6; i++) {
			config.mixer_channels[i] = strdup(names[i]);
			config.mixer_volumes[i] = 100;
		}
	}
}

static void kiwi_init(void)
{
	id_pool_init(&sounds, 0);
	id_pool_init(&effects, 0);
	SDL_InitSubSystem(SDL_INIT_AUDIO);
	audio_init();
	struct archive_data *data = asset_get_by_name(ASSET_SOUND, "timemap_sound.ex", NULL);
	if (data) {
		metadata = ex_read(data->data, data->size);
		archive_free_data(data);
		if (metadata) sound_info = ex_get_table(metadata, "SoundInfo");
	}
}

static void stop_effect(struct effect *effect)
{
	while (effect->voices) {
		struct effect_voice *voice = effect->voices;
		effect->voices = voice->next;
		channel_close(voice->channel);
		free(voice);
	}
}

static void kiwi_fini(void)
{
	stop_effect(&system_effect);
	for (int i = id_pool_get_first(&sounds); i >= 0; i = id_pool_get_next(&sounds, i))
		channel_close(id_pool_get(&sounds, i));
	for (int i = id_pool_get_first(&effects); i >= 0; i = id_pool_get_next(&effects, i)) {
		struct effect *effect = id_pool_get(&effects, i);
		stop_effect(effect);
		free(effect);
	}
	id_pool_delete(&sounds);
	id_pool_delete(&effects);
	if (metadata) ex_free(metadata);
	metadata = NULL;
	sound_info = NULL;
}

static struct ex_value *sound_row(const char *name)
{
	if (!sound_info) return NULL;
	char *base = archive_basename(name);
	int row = ex_row_at_string_key(sound_info, base);
	free(base);
	return row >= 0 ? sound_info->rows[row] : NULL;
}

static int sound_group(struct string *name)
{
	struct ex_value *row = sound_row(name->text);
	return row && sound_info->nr_columns >= 2 && row[1].type == EX_INT ? row[1].i : 0;
}

static struct channel *open_sound(struct string *name)
{
	struct channel *channel = NULL;
	const enum asset_type types[] = { ASSET_SOUND, ASSET_BGM, ASSET_VOICE };
	for (unsigned i = 0; i < sizeof(types) / sizeof(*types); i++) {
		struct archive_data *data = asset_get_by_name(types[i], name->text, NULL);
		if (!data) continue;
		channel = channel_open_archive_data(data, types[i], AUDIO_NO_METADATA);
		break;
	}
	if (!channel) {
		char *path = gamedir_path_icase(name->text);
		if (path && file_exists(path)) channel = channel_open_file(path);
		free(path);
	}
	if (!channel) return NULL;
	struct ex_value *row = sound_row(name->text);
	if (row && sound_info->nr_columns >= 4) {
		channel_set_group(channel, row[1].i);
		if (row[2].i >= 0 && row[3].i > row[2].i
				&& row[3].i <= channel_get_sample_length(channel)) {
			channel_set_loop_start_pos(channel, row[2].i);
			channel_set_loop_end_pos(channel, row[3].i);
		}
	}
	return channel;
}

static int free_id(struct id_pool *pool, int low, int high)
{
	if (low < 0 || high < low) return -1;
	for (int id = low; ; id++) {
		if (!id_pool_get(pool, id)) return id;
		if (id == high) return -1;
	}
}

static int kiwi_free(int low, int high) { return free_id(&sounds, low, high); }
static int kiwi_free_se(int low, int high) { return free_id(&effects, low, high); }
static bool kiwi_exists(int id) { return id_pool_get(&sounds, id); }
static bool kiwi_exists_se(int id) { return id_pool_get(&effects, id); }

static bool kiwi_prepare(int id, struct string *name, struct string *filter, bool streaming)
{
	if (id < 0) return false;
	if (filter->size) VM_ERROR("Unimplemented sound filter: %s", display_sjis0(filter->text));
	// libsndfile streams both archive and filesystem data on demand.
	(void)streaming;
	struct channel *channel = open_sound(name);
	if (!channel) return false;
	struct channel *old = id_pool_set(&sounds, id, channel);
	if (old) channel_close(old);
	return true;
}

static bool kiwi_unprepare(int id)
{
	struct channel *channel = id_pool_release(&sounds, id);
	if (!channel) return false;
	channel_close(channel);
	return true;
}

#define CHANNEL_CALL(name, result, function) \
	static result name(int id) { \
		struct channel *channel = id_pool_get(&sounds, id); \
		return channel ? function(channel) : 0; \
	}
CHANNEL_CALL(kiwi_play, bool, channel_play)
CHANNEL_CALL(kiwi_stop, bool, channel_stop)
CHANNEL_CALL(kiwi_playing, bool, channel_is_playing)
CHANNEL_CALL(kiwi_get_loop, int, channel_get_loop_count)
CHANNEL_CALL(kiwi_pause, bool, channel_pause)
CHANNEL_CALL(kiwi_restart, bool, channel_restart)
CHANNEL_CALL(kiwi_paused, bool, channel_is_paused)
CHANNEL_CALL(kiwi_stop_fade, bool, channel_stop_fade)
CHANNEL_CALL(kiwi_fading, bool, channel_is_fading)
CHANNEL_CALL(kiwi_position, int, channel_get_pos)
CHANNEL_CALL(kiwi_length, int, channel_get_length)
CHANNEL_CALL(kiwi_group, int, channel_get_group)
#undef CHANNEL_CALL

static bool kiwi_loop(int id, int count)
{
	struct channel *channel = id_pool_get(&sounds, id);
	return channel && channel_set_loop_count(channel, count);
}

static bool kiwi_seek(int id, int milliseconds)
{
	struct channel *channel = id_pool_get(&sounds, id);
	return channel && channel_seek(channel, milliseconds);
}

static void memory_archive_free_data(struct archive_data *data)
{
	free(data->data);
	free(data);
}

static struct archive_ops memory_archive_ops = {
	.free_data = memory_archive_free_data,
};

static struct archive memory_archive = {
	.ops = &memory_archive_ops,
};

bool kiwi_prepare_from_memory(int id, uint8_t *buf, size_t size, int group)
{
	if (id < 0 || !buf || !size) {
		free(buf);
		return false;
	}
	SDL_InitSubSystem(SDL_INIT_AUDIO);
	audio_init();
	struct archive_data *dfile = xcalloc(1, sizeof(*dfile));
	dfile->data = buf;
	dfile->size = size;
	dfile->archive = &memory_archive;
	struct channel *channel = channel_open_archive_data(dfile, ASSET_SOUND, AUDIO_NO_METADATA);
	if (!channel) return false;
	if (group >= 0) channel_set_group(channel, group);
	struct channel *old = id_pool_set(&sounds, id, channel);
	if (old) channel_close(old);
	return true;
}

bool kiwi_play_sound(int id) { return kiwi_play(id); }
bool kiwi_stop_sound(int id) { return kiwi_stop(id); }
bool kiwi_unprepare_sound(int id) { return kiwi_unprepare(id); }
bool kiwi_is_playing_sound(int id) { return kiwi_playing(id); }
int kiwi_get_sound_pos(int id) { return kiwi_position(id); }
bool kiwi_seek_sound(int id, int milliseconds) { return kiwi_seek(id, milliseconds); }

static bool kiwi_fade(int id, int milliseconds, float volume, bool stop, int type)
{
	struct channel *channel = id_pool_get(&sounds, id);
	if (!channel) return false;
	// Out-of-range types use linear interpolation in the reference engine.
	if (type < 0 || type >= 8) type = 0;
	return channel_fade_with_curve(channel, max(0, milliseconds), lroundf(volume * 100), stop, type);
}

static bool kiwi_set_se(int id, int span, bool multi)
{
	if (id < 0 || span < 0) return false;
	struct effect *effect = id_pool_get(&effects, id);
	if (!effect) {
		effect = xcalloc(1, sizeof(*effect));
		id_pool_set(&effects, id, effect);
	}
	effect->span = span;
	effect->multi = multi;
	return true;
}

static bool play_effect(struct effect *effect, struct string *name)
{
	uint32_t now = SDL_GetTicks();
	if (effect->started && now - effect->last_play < effect->span) return true;
	for (struct effect_voice **p = &effect->voices; *p;) {
		struct effect_voice *voice = *p;
		if (!channel_is_playing(voice->channel)) {
			*p = voice->next;
			channel_close(voice->channel);
			free(voice);
		} else p = &voice->next;
	}
	struct channel *channel = open_sound(name);
	if (!channel) return false;
	if (!effect->multi) stop_effect(effect);
	struct effect_voice *voice = xcalloc(1, sizeof(*voice));
	voice->channel = channel;
	voice->next = effect->voices;
	effect->voices = voice;
	effect->started = true;
	effect->last_play = now;
	return channel_play(channel);
}

static bool kiwi_play_se(int id, struct string *name)
{
	if (id < 0) return false;
	if (!kiwi_exists_se(id)) kiwi_set_se(id, 0, true);
	return play_effect(id_pool_get(&effects, id), name);
}

bool kiwi_play_system_sound(struct string *name)
{
	return !name->size || play_effect(&system_effect, name);
}

void kiwi_stop_non_system_sound(void)
{
	for (int i = id_pool_get_first(&sounds); i >= 0; i = id_pool_get_next(&sounds, i))
		channel_stop(id_pool_get(&sounds, i));
	for (int i = id_pool_get_first(&effects); i >= 0; i = id_pool_get_next(&effects, i))
		stop_effect(id_pool_get(&effects, i));
}

static bool kiwi_stop_se(int id)
{
	struct effect *effect = id_pool_get(&effects, id);
	if (!effect) return false;
	stop_effect(effect);
	return true;
}

static bool kiwi_playing_se(int id)
{
	struct effect *effect = id_pool_get(&effects, id);
	if (!effect) return false;
	for (struct effect_voice *v = effect->voices; v; v = v->next)
		if (channel_is_playing(v->channel)) return true;
	return false;
}

static bool kiwi_file_exists(struct string *name)
{
	if (asset_exists_by_name(ASSET_SOUND, name->text, NULL)
			|| asset_exists_by_name(ASSET_BGM, name->text, NULL)
			|| asset_exists_by_name(ASSET_VOICE, name->text, NULL)) return true;
	char *path = gamedir_path_icase(name->text);
	bool exists = path && file_exists(path);
	free(path);
	return exists;
}

static int kiwi_file_length(struct string *name)
{
	struct channel *channel = open_sound(name);
	if (!channel) return 0;
	int length = channel_get_length(channel);
	channel_close(channel);
	return length;
}

static float kiwi_group_volume(int group)
{
	int volume = 0;
	mixer_get_volume(group, &volume);
	return volume / 100.0f;
}

static int kiwi_samples(int time, int rate) { return (int64_t)time * rate / 1000; }

static bool kiwi_mixer_name(int index, struct string **out)
{
	const char *name = mixer_get_name(index);
	if (!name) return false;
	free_string(*out);
	*out = cstr_to_string(name);
	return true;
}

static bool kiwi_mixer_set_name(int index, struct string *name) { return mixer_set_name(index, name->text); }
static bool kiwi_mixer_default(int index, int *volume)
{
	if (index < 0 || index >= config.mixer_nr_channels) return false;
	*volume = config.mixer_volumes[index];
	return true;
}

struct static_library lib_KiwiSoundEngine_v12 = {
	.name = "KiwiSoundEngine",
	.functions = {
		HLL_EXPORT(_PreLink, kiwi_prelink), HLL_EXPORT(_ModuleInit, kiwi_init), HLL_EXPORT(_ModuleFini, kiwi_fini),
		HLL_EXPORT(SetGlobalFocus, mixer_set_global_focus),
		HLL_EXPORT(GetFreeSeID, kiwi_free_se), HLL_EXPORT(IsExistSeID, kiwi_exists_se),
		HLL_EXPORT(SetSeParam, kiwi_set_se), HLL_EXPORT(PlaySe, kiwi_play_se),
		HLL_EXPORT(StopSe, kiwi_stop_se), HLL_EXPORT(IsPlaySe, kiwi_playing_se),
		HLL_EXPORT(GetFreeID, kiwi_free), HLL_EXPORT(IsExistID, kiwi_exists),
		HLL_EXPORT(Prepare, kiwi_prepare), HLL_EXPORT(Unprepare, kiwi_unprepare),
		HLL_EXPORT(Play, kiwi_play), HLL_EXPORT(Stop, kiwi_stop), HLL_EXPORT(IsPlay, kiwi_playing),
		HLL_EXPORT(SetLoopCount, kiwi_loop), HLL_EXPORT(GetLoopCount, kiwi_get_loop),
		HLL_EXPORT(Pause, kiwi_pause), HLL_EXPORT(Restart, kiwi_restart), HLL_EXPORT(IsPause, kiwi_paused),
		HLL_EXPORT(Fade, kiwi_fade), HLL_EXPORT(StopFade, kiwi_stop_fade), HLL_EXPORT(IsFade, kiwi_fading),
		HLL_EXPORT(Seek, kiwi_seek), HLL_EXPORT(GetPos, kiwi_position), HLL_EXPORT(GetLength, kiwi_length),
		HLL_EXPORT(GetGroupNum, kiwi_group), HLL_EXPORT(IsExistFile, kiwi_file_exists),
		HLL_EXPORT(GetLengthFromFile, kiwi_file_length), HLL_EXPORT(GetGroupNumFromFile, sound_group),
		HLL_EXPORT(GetGroupVolume, kiwi_group_volume), HLL_EXPORT(MillisecondsToSamples, kiwi_samples),
		HLL_EXPORT(GetMixerNumof, mixer_get_numof), HLL_EXPORT(GetMixerName, kiwi_mixer_name),
		HLL_EXPORT(GetMixerVolume, mixer_get_volume), HLL_EXPORT(GetMixerDefaultVolume, kiwi_mixer_default),
		HLL_EXPORT(GetMixerMute, mixer_get_mute), HLL_EXPORT(SetMixerName, kiwi_mixer_set_name),
		HLL_EXPORT(SetMixerVolume, mixer_set_volume), HLL_EXPORT(SetMixerMute, mixer_set_mute),
		{0}
	}
};
