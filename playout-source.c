#include "playout-source.h"
#include <obs-module.h>
#include <util/dstr.h>

struct playout_source_item {
	obs_source_t *source;
	//int64_t duration;
	char *section;

	float start;
	float end;

	obs_source_t *transition;
	uint32_t transition_duration_ms;
};

struct playout_source_context {
	obs_source_t *source;
	obs_source_t *current_source;
	DARRAY(struct playout_source_item) items;
};

#define PLAYBACK_MODE_LIST 0
#define PLAYBACK_MODE_SECTION 1
#define PLAYBACK_MODE_SINGLE 2

static const char *playout_source_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("Playout");
}

static void *playout_source_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	struct playout_source_context *playout = bzalloc(sizeof(struct playout_source_context));
	playout->source = source;
	obs_source_update(source, settings);
	return playout;
}

static void playout_source_destroy(void *data)
{
	struct playout_source_context *playout = data;
	da_free(playout->items);
	bfree(data);
}

static void playout_source_update(void *data, obs_data_t *settings)
{
	struct playout_source_context *playout = data;
	struct dstr setting_name;
	dstr_init(&setting_name);

	for (int i = 0;; i++) {
		dstr_printf(&setting_name, "path%d", i);
		const char *path = obs_data_get_string(settings, setting_name.array);
		if (!strlen(path))
			break;
		if (i >= (int)playout->items.num) {
			struct playout_source_item item = {0};
			da_push_back(playout->items, &item);
		}
	}

	dstr_free(&setting_name);

	//obs_transition_enable_fixed
	//obs_source_media_get_duration()
}

static void playout_source_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(seconds);
}

void add_item_properties(obs_properties_t *props, struct dstr *setting_name, int i)
{

	dstr_printf(setting_name, "item%d", i);
	obs_properties_t *item_group = obs_properties_create();
	dstr_printf(setting_name, "section%d", i);
	obs_properties_add_text(item_group, setting_name->array, obs_module_text("Section"), OBS_TEXT_DEFAULT);
	dstr_printf(setting_name, "path%d", i);
	obs_properties_add_path(item_group, setting_name->array, obs_module_text("Path"), OBS_PATH_FILE, NULL, NULL);
	dstr_printf(setting_name, "start%d", i);
	obs_property_t *p =
		obs_properties_add_float_slider(item_group, setting_name->array, obs_module_text("Start"), 0.0, 10.0, 1.0);
	obs_property_float_set_suffix(p, " s");
	dstr_printf(setting_name, "end%d", i);
	p = obs_properties_add_float_slider(item_group, setting_name->array, obs_module_text("Stop"), 0.0, 10.0, 1.0);
	obs_property_float_set_suffix(p, " s");
	dstr_printf(setting_name, "speed_percent%d", i);
	p = obs_properties_add_int_slider(item_group, setting_name->array, obs_module_text("Speed"), 1, 200, 1);
	obs_property_int_set_suffix(p, "%");
	dstr_printf(setting_name, "transition%d", i);
	p = obs_properties_add_list(item_group, setting_name->array, obs_module_text("Transition"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("None"), "");

	dstr_printf(setting_name, "transition_duration%d", i);
	p = obs_properties_add_int(item_group, setting_name->array, obs_module_text("TransitionDuration"), 1, 200, 1);
	obs_property_int_set_suffix(p, " ms");

	//dstr_printf(setting_name, "remove%d", i);
	//obs_properties_add_button(item_group, setting_name->array, obs_module_text("Remove"), button_func);
	//obs_properties_add_button(item_group, setting_name->array, obs_module_text("MoveUp"), button_func);
	//obs_properties_add_button(item_group, setting_name->array, obs_module_text("MoveDown"), button_func);

	obs_properties_add_group(props, setting_name->array, "group", OBS_GROUP_NORMAL, item_group);
}

bool playout_source_add_item(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(property);
	if (!data)
		return false;
	struct playout_source_context *playout = data;
	struct dstr setting_name;
	dstr_init(&setting_name);
	obs_properties_remove_by_name(props, "plugin_info");
	add_item_properties(props, &setting_name, (int)playout->items.num);
	obs_properties_add_text(props, "plugin_info",
				"<a href=\"https://github.com/exeldro/obs-playout-source\">Playout Source</a> (" PROJECT_VERSION
				") by <a href=\"https://www.exeldro.com\">Exeldro</a>",
				OBS_TEXT_INFO);
	dstr_free(&setting_name);
	struct playout_source_item item = {0};

	da_push_back(playout->items, &item);
	return true;
}

static obs_properties_t *playout_source_properties(void *data)
{
	struct playout_source_context *playout = data;
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_bool(props, "autoplay", obs_module_text("Autoplay"));
	obs_property_t *p = obs_properties_add_list(props, "playback_mode", obs_module_text("PlaybackMode"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Single"), PLAYBACK_MODE_SINGLE);
	obs_property_list_add_int(p, obs_module_text("Section"), PLAYBACK_MODE_SECTION);
	obs_property_list_add_int(p, obs_module_text("List"), PLAYBACK_MODE_LIST);
	obs_properties_add_bool(props, "loop", obs_module_text("Loop"));

	obs_properties_add_button2(props, "add_item", obs_module_text("AddItem"), playout_source_add_item, data);

	if (playout) {
		struct dstr setting_name;
		dstr_init(&setting_name);
		for (int i = 0; i < (int)playout->items.num; i++) {
			add_item_properties(props, &setting_name, i);
		}
		dstr_free(&setting_name);
	}
	obs_properties_add_text(props, "plugin_info",
				"<a href=\"https://github.com/exeldro/obs-playout-source\">Playout Source</a> (" PROJECT_VERSION
				") by <a href=\"https://www.exeldro.com\">Exeldro</a>",
				OBS_TEXT_INFO);
	return props;
}

void playout_source_defaults(obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
}

uint32_t playout_source_get_width(void *data)
{
	struct playout_source_context *playout = data;
	if (!playout->current_source)
		return 0;
	return obs_source_get_width(playout->current_source);
}

uint32_t playout_source_get_height(void *data)
{
	struct playout_source_context *playout = data;
	if (!playout->current_source)
		return 0;
	return obs_source_get_height(playout->current_source);
}

static void playout_source_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct playout_source_context *playout = data;
	if (playout->current_source)
		obs_source_video_render(playout->current_source);
}

static bool playout_source_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_output, uint32_t mixers,
					size_t channels, size_t sample_rate)
{
	UNUSED_PARAMETER(sample_rate);
	struct playout_source_context *playout = data;

	obs_source_t *source = obs_source_get_ref(playout->current_source);
	if (!source)
		return false;

	uint64_t timestamp = 0;

	if (obs_source_audio_pending(source)) {
		obs_source_release(source);
		return false;
	}
	timestamp = obs_source_get_audio_timestamp(source);
	if (!timestamp) {
		obs_source_release(source);
		return false;
	}

	struct obs_source_audio_mix child_audio;
	obs_source_get_audio_mix(source, &child_audio);
	obs_source_release(source);
	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
		if ((mixers & (1 << mix)) == 0)
			continue;

		for (size_t ch = 0; ch < channels; ch++) {
			float *out = audio_output->output[mix].data[ch];
			float *in = child_audio.output[mix].data[ch];

			memcpy(out, in, AUDIO_OUTPUT_FRAMES * sizeof(float));
		}
	}
	*ts_out = timestamp;
	return true;
}

struct obs_source_info playout_source = {
	.id = "playout_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_OUTPUT_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_COMPOSITE | OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_CONTROLLABLE_MEDIA,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.get_name = playout_source_get_name,
	.create = playout_source_create,
	.destroy = playout_source_destroy,
	.update = playout_source_update,
	.video_tick = playout_source_video_tick,
	.get_properties = playout_source_properties,
	.get_defaults = playout_source_defaults,
	.get_width = playout_source_get_width,
	.get_height = playout_source_get_height,
	.video_render = playout_source_video_render,
	.audio_render = playout_source_audio_render,
};

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Exeldro");
OBS_MODULE_USE_DEFAULT_LOCALE("playout-source", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("PlayoutSource");
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[Playout Source] loaded version %s", PROJECT_VERSION);
	obs_register_source(&playout_source);
	return true;
}

void obs_module_post_load() {}

void obs_module_unload() {}
