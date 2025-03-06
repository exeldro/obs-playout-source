#include "audio-wrapper.h"
#include "playout-source.h"
#include <obs-module.h>

const char *audio_wrapper_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "playout_source_audio_wrapper";
}

void *audio_wrapper_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	struct audio_wrapper_info *audio_wrapper = bzalloc(sizeof(struct audio_wrapper_info));
	audio_wrapper->source = source;
	return audio_wrapper;
}

static void audio_wrapper_audio_output_callback(void *param, size_t mix_idx, struct audio_data *data)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(mix_idx);
	UNUSED_PARAMETER(data);
}

void audio_wrapper_destroy(void *data)
{
	struct audio_wrapper_info *aw = (struct audio_wrapper_info *)data;
	audio_output_disconnect(obs_get_audio(), 1, audio_wrapper_audio_output_callback, aw);
	if (aw->playout)
		aw->playout->audio_wrapper = NULL;
	bfree(data);
}

bool audio_wrapper_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_mix, uint32_t mixers, size_t channels,
			  size_t sample_rate)
{
	UNUSED_PARAMETER(ts_out);
	UNUSED_PARAMETER(audio_mix);
	UNUSED_PARAMETER(sample_rate);
	struct audio_wrapper_info *aw = (struct audio_wrapper_info *)data;

	obs_source_t *source =
		obs_source_get_ref(aw->playout->current_transition ? aw->playout->current_transition : aw->playout->current_source);
	if (!source)
		return false;
	if (obs_source_audio_pending(source)) {
		obs_source_release(source);
		return false;
	}
	if (!mixers) {
		audio_output_disconnect(obs_get_audio(), 1, audio_wrapper_audio_output_callback, aw);
		audio_output_connect(obs_get_audio(), 1, NULL, audio_wrapper_audio_output_callback, aw);
	}

	struct obs_source_audio_mix child_audio;
	obs_source_get_audio_mix(source, &child_audio);
	uint64_t timestamp = obs_source_get_audio_timestamp(source);
	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
		if ((mixers & (1 << mix)) == 0)
			continue;
		const audio_t *a = obs_get_audio();
		const struct audio_output_info *aoi = audio_output_get_info(a);
		struct obs_source_audio audio;
		audio.format = aoi->format;
		audio.samples_per_sec = aoi->samples_per_sec;
		audio.speakers = aoi->speakers;
		audio.frames = AUDIO_OUTPUT_FRAMES;
		audio.timestamp = timestamp;
		for (size_t i = 0; i < channels; i++) {
			audio.data[i] = (uint8_t *)child_audio.output[mix].data[i];
		}
		obs_source_output_audio(aw->playout->source, &audio);
		break;
	}
	obs_source_release(source);

	return false;
}

static void audio_wrapper_enum_sources(void *data, obs_source_enum_proc_t enum_callback, void *param, bool active)
{
	UNUSED_PARAMETER(active);
	struct audio_wrapper_info *aw = (struct audio_wrapper_info *)data;

	obs_source_t *source =
		obs_source_get_ref(aw->playout->current_transition ? aw->playout->current_transition : aw->playout->current_source);
	if (!source)
		return;

	enum_callback(aw->source, source, param);

	obs_source_release(source);
}

void audio_wrapper_enum_active_sources(void *data, obs_source_enum_proc_t enum_callback, void *param)
{
	audio_wrapper_enum_sources(data, enum_callback, param, true);
}

void audio_wrapper_enum_all_sources(void *data, obs_source_enum_proc_t enum_callback, void *param)
{
	audio_wrapper_enum_sources(data, enum_callback, param, false);
}

struct obs_source_info audio_wrapper_source = {
	.id = "playout_source_audio_wrapper_source",
	.type = OBS_SOURCE_TYPE_SCENE,
	.output_flags = OBS_SOURCE_COMPOSITE | OBS_SOURCE_CAP_DISABLED,
	.get_name = audio_wrapper_get_name,
	.create = audio_wrapper_create,
	.destroy = audio_wrapper_destroy,
	.audio_render = audio_wrapper_render,
	.enum_active_sources = audio_wrapper_enum_active_sources,
	.enum_all_sources = audio_wrapper_enum_all_sources,
};
