#include "playout-source.h"
#include "version.h"
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <stdio.h>
#include <util/dstr.h>

struct playout_source_item {
	obs_source_t *source;
	char *section;

	uint64_t start;
	uint64_t end;

	obs_source_t *transition;
	uint32_t transition_duration_ms;
	uint32_t speed;
};

struct playout_source_context {
	obs_source_t *source;
	obs_source_t *current_source;
	obs_source_t *current_transition;
	uint32_t current_transition_duration;
	bool playing;
	bool auto_play;
	bool loop;
	bool next_after_transition;
	bool switch_to_next;
	int playback_mode;
	int current_index;
	DARRAY(struct playout_source_item) items;
};

#define PLAYBACK_MODE_LIST 0
#define PLAYBACK_MODE_SECTION 1
#define PLAYBACK_MODE_SINGLE 2

#define PLAYOUT_ACTION_NONE 0
#define PLAYOUT_ACTION_ADD_ITEM_TOP 1
#define PLAYOUT_ACTION_ADD_ITEM_BOTTOM 2
#define PLAYOUT_ACTION_REMOVE_SELECTED 3
#define PLAYOUT_ACTION_REMOVE_ALL 4
#define PLAYOUT_ACTION_MOVE_SELECTED_UP 5
#define PLAYOUT_ACTION_MOVE_SELECTED_DOWN 6

#define PLUGIN_INFO                                                                                      \
	"<a href=\"https://github.com/exeldro/obs-playout-source\">Playout Source</a> (" PROJECT_VERSION \
	") by <a href=\"https://www.exeldro.com\">Exeldro</a>"

static const char *playout_source_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("Playout");
}

static void playout_source_frontend_event(enum obs_frontend_event event, void *data);

static void *playout_source_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	struct playout_source_context *playout = bzalloc(sizeof(struct playout_source_context));
	playout->source = source;
	playout->current_index = -1;
	obs_source_update(source, settings);
	obs_frontend_add_event_callback(playout_source_frontend_event, playout);
	return playout;
}

static void playout_source_destroy(void *data)
{
	struct playout_source_context *playout = data;
	obs_frontend_remove_event_callback(playout_source_frontend_event, playout);
	if (playout->current_source) {
		obs_source_dec_showing(playout->current_source);
		obs_source_release(playout->current_source);
		playout->current_source = NULL;
	}
	if (playout->current_transition) {
		obs_source_dec_showing(playout->current_transition);
		obs_source_release(playout->current_transition);
		playout->current_transition = NULL;
	}
	for (int i = 0; i < (int)playout->items.num; i++) {
		obs_source_release(playout->items.array[i].source);
		obs_source_release(playout->items.array[i].transition);
	}
	da_free(playout->items);
	bfree(data);
}

void playout_source_update_current_source(struct playout_source_context *playout, bool use_transition)
{
	if (playout->current_index < 0)
		return;
	if (playout->current_index >= (int)playout->items.num)
		return;
	if (playout->current_source == playout->items.array[playout->current_index].source)
		return;
	if (playout->current_transition) {
		if (use_transition) {
			obs_transition_start(playout->current_transition, OBS_TRANSITION_MODE_AUTO,
					     playout->current_transition_duration,
					     playout->items.array[playout->current_index].source);
		} else {
			obs_source_dec_showing(playout->current_transition);
			obs_source_release(playout->current_transition);
			playout->current_transition = NULL;
			playout->current_transition_duration = 0;
		}
	}
	if (playout->current_source) {
		obs_source_dec_showing(playout->current_source);
		obs_source_release(playout->current_source);
	}
	playout->current_source = obs_source_get_ref(playout->items.array[playout->current_index].source);
	if (!playout->current_transition && playout->items.array[playout->current_index].transition) {
		obs_transition_set(playout->items.array[playout->current_index].transition, playout->current_source);
		playout->current_transition = obs_source_get_ref(playout->items.array[playout->current_index].transition);
		obs_source_inc_showing(playout->current_transition);
		playout->current_transition_duration = playout->items.array[playout->current_index].transition_duration_ms;
	}
	if (playout->current_source) {
		obs_source_inc_showing(playout->current_source);
		if (use_transition) {
			enum obs_media_state state = obs_source_media_get_state(playout->current_source);
			if (state == OBS_MEDIA_STATE_ENDED) {
				obs_source_media_restart(playout->current_source);
			}
			obs_source_media_set_time(playout->current_source, playout->items.array[playout->current_index].start);
			obs_source_media_play_pause(playout->current_source, false);
		}
	}
}

void playout_source_switch_to_next_item(struct playout_source_context *playout)
{
	playout->switch_to_next = false;
	if (!playout->items.num)
		return;
	if (playout->current_index >= (int)playout->items.num - 1 && !playout->loop && !playout->auto_play)
		return;

	bool switch_scene = false;

	if (playout->playback_mode == PLAYBACK_MODE_LIST) {
		if (playout->current_index < (int)playout->items.num - 1) {
			playout->current_index++;
		} else if (playout->loop) {
			playout->current_index = 0;
		} else if (playout->auto_play && obs_frontend_preview_program_mode_active()) {
			switch_scene = true;
		}
	} else if (playout->playback_mode == PLAYBACK_MODE_SECTION) {
		if (playout->items.array[playout->current_index].section &&
		    playout->items.array[playout->current_index + 1].section &&
		    strcmp(playout->items.array[playout->current_index].section,
			   playout->items.array[playout->current_index + 1].section) == 0) {

			playout->current_index++;
		} else if (playout->loop) {
			if (playout->items.array[playout->current_index].section) {
				while (playout->current_index > 0 && playout->items.array[playout->current_index - 1].section &&
				       strcmp(playout->items.array[playout->current_index].section,
					      playout->items.array[playout->current_index - 1].section) == 0) {
					playout->current_index--;
				}
			} else {
				while (playout->current_index > 0 && !playout->items.array[playout->current_index - 1].section) {
					playout->current_index--;
				}
			}
		} else if (playout->auto_play && obs_frontend_preview_program_mode_active()) {
			switch_scene = true;
		}
	} else if (playout->playback_mode == PLAYBACK_MODE_SINGLE) {
		if (!playout->loop && playout->auto_play && obs_frontend_preview_program_mode_active()) {
			switch_scene = true;
		}
	}
	if (switch_scene && obs_source_active(playout->source)) {
		obs_frontend_preview_program_trigger_transition();
	}
	playout_source_update_current_source(playout, true);
}

static void playout_source_media_ended(void *data, calldata_t *cd)
{
	struct playout_source_context *playout = data;
	obs_source_t *source = calldata_ptr(cd, "source");
	if (playout->current_source != source)
		return;

	bool last = false;
	if (playout->auto_play && !playout->loop && obs_frontend_preview_program_mode_active()) {
		if (playout->playback_mode == PLAYBACK_MODE_LIST) {
			last = playout->current_index == (int)playout->items.num - 1;
		} else if (playout->playback_mode == PLAYBACK_MODE_SECTION) {
			last = playout->current_index >= (int)playout->items.num - 1 ||
			       (playout->items.array[playout->current_index].section &&
				playout->items.array[playout->current_index + 1].section &&
				strcmp(playout->items.array[playout->current_index].section,
				       playout->items.array[playout->current_index + 1].section) != 0) ||
			       (!playout->items.array[playout->current_index].section &&
				playout->items.array[playout->current_index + 1].section) ||
			       (playout->items.array[playout->current_index].section &&
				!playout->items.array[playout->current_index + 1].section);
		} else if (playout->playback_mode == PLAYBACK_MODE_SINGLE) {
			last = true;
		}
	}
	if (last) {
		if (!playout->next_after_transition && obs_source_active(playout->source)) {
			playout->next_after_transition = true;
			obs_frontend_preview_program_trigger_transition();
		}
	} else {
		playout->switch_to_next = true;
	}
}

void playout_source_transition_stop(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct playout_source_context *playout = data;
	if (playout->current_index < 0 || playout->current_index >= (int)playout->items.num)
		return;
	if (playout->items.array[playout->current_index].transition) {
		if (playout->current_transition == playout->items.array[playout->current_index].transition)
			return;
		if (playout->current_transition) {
			obs_source_t *newTransition = playout->items.array[playout->current_index].transition;
			obs_source_t *oldTransition = playout->current_transition;
			obs_transition_swap_begin(newTransition, oldTransition);
			playout->current_transition = obs_source_get_ref(newTransition);
			playout->current_transition_duration = playout->items.array[playout->current_index].transition_duration_ms;
			obs_transition_swap_end(newTransition, oldTransition);
			obs_source_release(oldTransition);
		} else {
			obs_source_t *newTransition = playout->items.array[playout->current_index].transition;
			obs_transition_set(newTransition, playout->current_source);
			playout->current_transition = obs_source_get_ref(newTransition);
			obs_source_inc_showing(playout->current_transition);
			playout->current_transition_duration = playout->items.array[playout->current_index].transition_duration_ms;
		}
	} else if (playout->current_transition) {
		obs_source_dec_showing(playout->current_transition);
		obs_source_release(playout->current_transition);
		playout->current_transition = NULL;
		playout->current_transition_duration = 0;
	}
}

static void playout_source_update(void *data, obs_data_t *settings)
{
	struct playout_source_context *playout = data;
	playout->auto_play = obs_data_get_bool(settings, "autoplay");
	playout->loop = obs_data_get_bool(settings, "loop");
	playout->playback_mode = (int)obs_data_get_int(settings, "playback_mode");
	struct dstr setting_name;
	dstr_init(&setting_name);

	for (int i = 0;; i++) {
		dstr_printf(&setting_name, "path%d", i);
		const char *path = obs_data_get_string(settings, setting_name.array);
		if (!strlen(path))
			break;
		if (i >= (int)playout->items.num) {
			da_push_back_new(playout->items);
		}
		if (i == playout->current_index && playout->items.array[i].source) {
			obs_data_t *s = obs_source_get_settings(playout->items.array[i].source);
			if (strcmp(obs_data_get_string(s, "local_file"), path) != 0) {
				obs_source_release(playout->items.array[i].source);
				playout->items.array[i].source = NULL;
			}
			obs_data_release(s);
		}
		if (!playout->items.array[i].source) {
			playout->items.array[i].source = obs_source_create_private("ffmpeg_source", "playout_source_item", NULL);
			signal_handler_t *sh = obs_source_get_signal_handler(playout->items.array[i].source);
			signal_handler_connect(sh, "media_ended", playout_source_media_ended, data);
		}
		obs_data_t *ss = obs_data_create();
		obs_data_set_bool(ss, "is_local_file", true);
		obs_data_set_string(ss, "local_file", path);
		obs_data_set_bool(ss, "looping", playout->loop && playout->playback_mode == PLAYBACK_MODE_SINGLE);
		obs_data_set_bool(ss, "is_stinger", true);
		obs_data_set_bool(ss, "hw_decode", true);
		obs_data_set_bool(ss, "close_when_inactive", false);
		obs_data_set_bool(ss, "clear_on_media_end", false);
		obs_data_set_bool(ss, "restart_on_activate", false);

		dstr_printf(&setting_name, "speed_percent%d", i);
		playout->items.array[i].speed = (uint32_t)obs_data_get_int(settings, setting_name.array);
		if (!playout->items.array[i].speed)
			playout->items.array[i].speed = 100;
		obs_data_set_int(ss, "speed_percent", playout->items.array[i].speed);
		dstr_printf(&setting_name, "start%d", i);
		playout->items.array[i].start = (uint64_t)(obs_data_get_double(settings, setting_name.array) * 1000.0);
		dstr_printf(&setting_name, "end%d", i);
		playout->items.array[i].end = (uint64_t)(obs_data_get_double(settings, setting_name.array) * -1000.0);
		obs_source_update(playout->items.array[i].source, ss);
		obs_data_release(ss);
		dstr_printf(&setting_name, "transition%d", i);
		const char *transition = obs_data_get_string(settings, setting_name.array);
		if (strlen(transition)) {
			if (!playout->items.array[i].transition ||
			    strcmp(obs_source_get_unversioned_id(playout->items.array[i].transition), transition) != 0) {
				obs_source_release(playout->items.array[i].transition);

				dstr_printf(&setting_name, "transition_settings%d", i);
				obs_data_t *transition_settings = obs_data_get_obj(settings, setting_name.array);
				if (!transition_settings) {
					transition_settings = obs_data_create();
					obs_data_set_obj(settings, setting_name.array, transition_settings);
				}
				playout->items.array[i].transition =
					obs_source_create_private(transition, "test", transition_settings);
				signal_handler_t *sh = obs_source_get_signal_handler(playout->items.array[i].transition);
				signal_handler_connect(sh, "transition_stop", playout_source_transition_stop, data);
				signal_handler_connect(sh, "transition_video_stop", playout_source_transition_stop, data);

				obs_data_release(transition_settings);
			}
		} else if (playout->items.array[i].transition) {
			obs_source_release(playout->items.array[i].transition);
			playout->items.array[i].transition = NULL;
		}
		dstr_printf(&setting_name, "transition_duration%d", i);
		playout->items.array[i].transition_duration_ms = (uint32_t)obs_data_get_int(settings, setting_name.array);
	}

	dstr_free(&setting_name);
}

static void playout_source_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct playout_source_context *playout = data;
	if (playout->switch_to_next) {
		playout_source_switch_to_next_item(playout);
		return;
	}

	if (!playout->playing)
		return;

	if (!playout->current_source)
		return;

	int64_t duration = obs_source_media_get_duration(playout->current_source);
	if (!duration)
		return;

	int64_t time = obs_source_media_get_time(playout->current_source);
	bool last = false;
	if (playout->auto_play && !playout->loop && obs_frontend_preview_program_mode_active()) {
		if (playout->playback_mode == PLAYBACK_MODE_LIST) {
			last = playout->current_index == (int)playout->items.num - 1;
		} else if (playout->playback_mode == PLAYBACK_MODE_SECTION) {
			last = playout->current_index >= (int)playout->items.num - 1 ||
			       (playout->items.array[playout->current_index].section &&
				playout->items.array[playout->current_index + 1].section &&
				strcmp(playout->items.array[playout->current_index].section,
				       playout->items.array[playout->current_index + 1].section) != 0) ||
			       (!playout->items.array[playout->current_index].section &&
				playout->items.array[playout->current_index + 1].section) ||
			       (playout->items.array[playout->current_index].section &&
				!playout->items.array[playout->current_index + 1].section);
		} else if (playout->playback_mode == PLAYBACK_MODE_SINGLE) {
			last = true;
		}
	}

	int64_t transition_duration = last ? (int64_t)obs_frontend_get_transition_duration()
					   : (playout->items.array[playout->current_index].transition
						      ? (int64_t)playout->items.array[playout->current_index].transition_duration_ms
						      : 0);
	if (time >= duration - transition_duration - (int64_t)playout->items.array[playout->current_index].end) {
		if (last) {
			if (!playout->next_after_transition && obs_source_active(playout->source)) {
				playout->next_after_transition = true;
				obs_frontend_preview_program_trigger_transition();
			}
		} else {
			playout_source_switch_to_next_item(playout);
		}
	}
}

bool edit_transition_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	struct playout_source_context *playout = data;
	const char *name = obs_property_name(property);
	int i;
	if (sscanf(name, "transition_edit%d", &i) != 1)
		return false;
	if (i < 0 || i >= (int)playout->items.num)
		return false;
	if (playout->items.array[i].transition)
		obs_frontend_open_source_properties(playout->items.array[i].transition);
	return false;
}

void add_item_properties(struct playout_source_context *playout, obs_properties_t *props, struct dstr *setting_name, int i)
{
	obs_properties_t *item_group = obs_properties_create();
	dstr_printf(setting_name, "section%d", i);
	obs_properties_add_text(item_group, setting_name->array, obs_module_text("Section"), OBS_TEXT_DEFAULT);
	dstr_printf(setting_name, "path%d", i);
	obs_properties_add_path(item_group, setting_name->array, obs_module_text("Path"), OBS_PATH_FILE, NULL, NULL);
	dstr_printf(setting_name, "start%d", i);

	int64_t duration = playout && i < (int)playout->items.num && playout->items.array[i].source
				   ? obs_source_media_get_duration(playout->items.array[i].source)
				   : 0;
	if (!duration)
		duration = 10000;
	obs_property_t *p = obs_properties_add_float_slider(item_group, setting_name->array, obs_module_text("Start"), 0.0,
							    (double)duration / 1000.0, 0.01);
	obs_property_float_set_suffix(p, " s");
	dstr_printf(setting_name, "end%d", i);
	p = obs_properties_add_float_slider(item_group, setting_name->array, obs_module_text("End"), (double)duration / -1000.0,
					    0.0, 0.01);
	obs_property_float_set_suffix(p, " s");
	dstr_printf(setting_name, "speed_percent%d", i);
	p = obs_properties_add_int_slider(item_group, setting_name->array, obs_module_text("Speed"), 1, 200, 1);
	obs_property_int_set_suffix(p, "%");
	dstr_printf(setting_name, "transition%d", i);
	p = obs_properties_add_list(item_group, setting_name->array, obs_module_text("Transition"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("None"), "");
	size_t idx = 0;
	const char *id;
	while (obs_enum_transition_types(idx++, &id)) {
		const char *name = obs_source_get_display_name(id);
		obs_property_list_add_string(p, name, id);
	}

	dstr_printf(setting_name, "transition_edit%d", i);
	obs_properties_add_button(item_group, setting_name->array, obs_module_text("EditTransition"), edit_transition_clicked);

	dstr_printf(setting_name, "transition_duration%d", i);
	p = obs_properties_add_int(item_group, setting_name->array, obs_module_text("TransitionDuration"), 50, 20000, 1000);
	obs_property_int_set_suffix(p, " ms");

	dstr_printf(setting_name, "selected%d", i);
	obs_properties_add_bool(item_group, setting_name->array, obs_module_text("Selected"));

	dstr_printf(setting_name, "item%d", i);
	struct dstr group_name;
	dstr_init(&group_name);
	dstr_printf(&group_name, "%s %d", obs_module_text("Item"), i + 1);
	obs_properties_add_group(props, setting_name->array, group_name.array, OBS_GROUP_NORMAL, item_group);
	dstr_free(&group_name);
}

static void playout_source_switch_text(obs_data_t *settings, size_t i, size_t j, struct dstr *setting_name, const char *format)
{
	dstr_printf(setting_name, format, i);
	char *vi = bstrdup(obs_data_get_string(settings, setting_name->array));
	dstr_printf(setting_name, format, j);
	char *vj = bstrdup(obs_data_get_string(settings, setting_name->array));
	obs_data_set_string(settings, setting_name->array, vi);
	dstr_printf(setting_name, format, i);
	obs_data_set_string(settings, setting_name->array, vj);
	bfree(vi);
	bfree(vj);
}
static void playout_source_switch_float(obs_data_t *settings, size_t i, size_t j, struct dstr *setting_name, const char *format)
{
	dstr_printf(setting_name, format, i);
	double vi = obs_data_get_double(settings, setting_name->array);
	dstr_printf(setting_name, format, j);
	double vj = obs_data_get_double(settings, setting_name->array);
	obs_data_set_double(settings, setting_name->array, vi);
	dstr_printf(setting_name, format, i);
	obs_data_set_double(settings, setting_name->array, vj);
}

static void playout_source_switch_int(obs_data_t *settings, size_t i, size_t j, struct dstr *setting_name, const char *format)
{
	dstr_printf(setting_name, format, i);
	long long vi = obs_data_get_int(settings, setting_name->array);
	dstr_printf(setting_name, format, j);
	long long vj = obs_data_get_int(settings, setting_name->array);
	obs_data_set_int(settings, setting_name->array, vi);
	dstr_printf(setting_name, format, i);
	obs_data_set_int(settings, setting_name->array, vj);
}

static void playout_source_switch_obj(obs_data_t *settings, size_t i, size_t j, struct dstr *setting_name, const char *format)
{
	dstr_printf(setting_name, format, i);
	obs_data_t *si = obs_data_get_obj(settings, setting_name->array);
	if (!si)
		si = obs_data_create();
	dstr_printf(setting_name, format, j);
	obs_data_t *sj = obs_data_get_obj(settings, setting_name->array);
	if (!sj)
		sj = obs_data_create();
	obs_data_set_obj(settings, setting_name->array, si);
	dstr_printf(setting_name, format, i);
	obs_data_set_obj(settings, setting_name->array, sj);
	obs_data_release(si);
	obs_data_release(sj);
}

static void playout_source_switch_item_settings(obs_data_t *settings, size_t i, size_t j, struct dstr *setting_name)
{
	playout_source_switch_text(settings, i, j, setting_name, "section%d");
	playout_source_switch_text(settings, i, j, setting_name, "path%d");
	playout_source_switch_float(settings, i, j, setting_name, "start%d");
	playout_source_switch_float(settings, i, j, setting_name, "end%d");
	playout_source_switch_int(settings, i, j, setting_name, "speed_percent%d");
	playout_source_switch_text(settings, i, j, setting_name, "transition%d");
	playout_source_switch_obj(settings, i, j, setting_name, "transition_settings%d");
	playout_source_switch_int(settings, i, j, setting_name, "transition_duration%d");
}

static bool playout_source_action(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(property);
	struct playout_source_context *playout = data;
	if (!playout)
		return false;
	obs_data_t *settings = obs_source_get_settings(playout->source);
	if (!settings)
		return false;
	struct dstr setting_name;
	dstr_init(&setting_name);
	long long action = obs_data_get_int(settings, "action");
	if (action == PLAYOUT_ACTION_ADD_ITEM_TOP) {
		for (size_t i = playout->items.num; i > 0; i--) {
			playout_source_switch_item_settings(settings, i, i - 1, &setting_name);
		}
		obs_properties_remove_by_name(props, "plugin_info");
		add_item_properties(playout, props, &setting_name, (int)playout->items.num);
		obs_properties_add_text(props, "plugin_info", PLUGIN_INFO, OBS_TEXT_INFO);
		da_insert_new(playout->items, 0);
	} else if (action == PLAYOUT_ACTION_ADD_ITEM_BOTTOM) {

		obs_properties_remove_by_name(props, "plugin_info");
		add_item_properties(playout, props, &setting_name, (int)playout->items.num);
		obs_properties_add_text(props, "plugin_info", PLUGIN_INFO, OBS_TEXT_INFO);
		dstr_free(&setting_name);
		da_push_back_new(playout->items);
	} else if (action == PLAYOUT_ACTION_REMOVE_SELECTED) {
		int selected = 0;
		for (int i = 0; i < (int)playout->items.num; i++) {
			dstr_printf(&setting_name, "selected%d", i);
			if (obs_data_get_bool(settings, setting_name.array)) {
				selected++;
			} else if (selected) {
				playout_source_switch_item_settings(settings, i, i - selected, &setting_name);
			}
		}
		for (int i = (int)playout->items.num - selected; i < (int)playout->items.num; i++) {
			dstr_printf(&setting_name, "path%d", i);
			obs_data_unset_user_value(settings, setting_name.array);
			dstr_printf(&setting_name, "transition%d", i);
			obs_data_unset_user_value(settings, setting_name.array);
		}
		selected = 0;
		for (int i = 0; i < (int)playout->items.num; i++) {
			dstr_printf(&setting_name, "selected%d", i);
			if (obs_data_get_bool(settings, setting_name.array)) {
				obs_data_unset_user_value(settings, setting_name.array);
				obs_source_release(playout->items.array[i - selected].source);
				obs_source_release(playout->items.array[i - selected].transition);
				da_erase(playout->items, i - selected);
				selected++;
			}
		}
	} else if (action == PLAYOUT_ACTION_REMOVE_ALL) {
		for (int i = 0; i < (int)playout->items.num; i++) {
			dstr_printf(&setting_name, "path%d", i);
			obs_data_unset_user_value(settings, setting_name.array);
			dstr_printf(&setting_name, "transition%d", i);
			obs_data_unset_user_value(settings, setting_name.array);
			obs_source_release(playout->items.array[i].source);
			obs_source_release(playout->items.array[i].transition);
		}
		playout->items.num = 0;
	} else if (action == PLAYOUT_ACTION_MOVE_SELECTED_UP) {
		for (int i = 1; i < (int)playout->items.num; i++) {
			dstr_printf(&setting_name, "selected%d", i);
			if (obs_data_get_bool(settings, setting_name.array)) {
				obs_data_unset_user_value(settings, setting_name.array);
				playout_source_switch_item_settings(settings, i, i - 1, &setting_name);
			}
		}
	} else if (action == PLAYOUT_ACTION_MOVE_SELECTED_DOWN) {
		for (int i = (int)playout->items.num - 1; i >= 0; i--) {
			dstr_printf(&setting_name, "selected%d", i);
			if (obs_data_get_bool(settings, setting_name.array)) {
				obs_data_unset_user_value(settings, setting_name.array);
				playout_source_switch_item_settings(settings, i, i + 1, &setting_name);
			}
		}
	}
	dstr_free(&setting_name);
	obs_data_unset_user_value(settings, "action");
	obs_data_release(settings);
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

	p = obs_properties_add_list(props, "action", obs_module_text("Action"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("None"), PLAYOUT_ACTION_NONE);
	obs_property_list_add_int(p, obs_module_text("AddItemTop"), PLAYOUT_ACTION_ADD_ITEM_TOP);
	obs_property_list_add_int(p, obs_module_text("AddItemBottom"), PLAYOUT_ACTION_ADD_ITEM_BOTTOM);
	obs_property_list_add_int(p, obs_module_text("RemoveSelected"), PLAYOUT_ACTION_REMOVE_SELECTED);
	obs_property_list_add_int(p, obs_module_text("RemoveAll"), PLAYOUT_ACTION_REMOVE_ALL);
	obs_property_list_add_int(p, obs_module_text("MoveSelectedUp"), PLAYOUT_ACTION_MOVE_SELECTED_UP);
	obs_property_list_add_int(p, obs_module_text("MoveSelectedDown"), PLAYOUT_ACTION_MOVE_SELECTED_DOWN);
	obs_properties_add_button2(props, "action_go", obs_module_text("ExecuteAction"), playout_source_action, data);

	if (playout) {
		struct dstr setting_name;
		dstr_init(&setting_name);
		for (int i = 0; i < (int)playout->items.num; i++) {
			add_item_properties(playout, props, &setting_name, i);
		}
		dstr_free(&setting_name);
	}
	obs_properties_add_text(props, "plugin_info", PLUGIN_INFO, OBS_TEXT_INFO);
	return props;
}

void playout_source_defaults(obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
}

uint32_t playout_source_get_width(void *data)
{
	struct playout_source_context *playout = data;
	if (playout->current_transition)
		return obs_source_get_width(playout->current_transition);
	if (playout->current_source)
		return obs_source_get_width(playout->current_source);
	return 0;
}

uint32_t playout_source_get_height(void *data)
{
	struct playout_source_context *playout = data;
	if (playout->current_transition)
		return obs_source_get_height(playout->current_transition);
	if (playout->current_source)
		return obs_source_get_height(playout->current_source);
	return 0;
}

static void playout_source_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct playout_source_context *playout = data;
	if (playout->current_transition)
		obs_source_video_render(playout->current_transition);
	else if (playout->current_source)
		obs_source_video_render(playout->current_source);
}

static bool playout_source_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_output, uint32_t mixers,
					size_t channels, size_t sample_rate)
{
	UNUSED_PARAMETER(sample_rate);
	struct playout_source_context *playout = data;

	obs_source_t *source = obs_source_get_ref(playout->current_transition);
	if (!source)
		source = obs_source_get_ref(playout->current_source);
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

static void playout_source_activate(void *data)
{
	struct playout_source_context *playout = data;
	if (!playout->auto_play)
		return;
	if (!playout->items.num)
		return;
	if (playout->current_index < 0 || playout->current_index >= (int)playout->items.num) {
		playout->current_index = 0;
		if (playout->current_source) {
			obs_source_release(playout->current_source);
			obs_source_dec_showing(playout->current_source);
		}
		playout->current_source = obs_source_get_ref(playout->items.array[playout->current_index].source);
		if (playout->current_source)
			obs_source_inc_showing(playout->current_source);
		if (playout->items.array[playout->current_index].transition) {
			obs_transition_set(playout->items.array[playout->current_index].transition, playout->current_source);
			playout->current_transition = obs_source_get_ref(playout->items.array[playout->current_index].transition);
			playout->current_transition_duration = playout->items.array[playout->current_index].transition_duration_ms;
		}
	}
	if (playout->current_source) {
		enum obs_media_state state = obs_source_media_get_state(playout->current_source);
		if (state == OBS_MEDIA_STATE_ENDED) {
			obs_source_media_restart(playout->current_source);
		}
		obs_source_media_set_time(playout->current_source, playout->items.array[playout->current_index].start);
		obs_source_media_play_pause(playout->current_source, false);
	}
	playout->playing = true;
}

int64_t playout_source_get_duration(void *data)
{
	struct playout_source_context *playout = data;

	if (!playout->current_source)
		return 0;
	int64_t duration = obs_source_media_get_duration(playout->current_source);
	if (!duration)
		return 0;
	if (playout->current_index >= 0 && playout->current_index < (int)playout->items.num) {
		duration -= playout->items.array[playout->current_index].start;
		duration -= playout->items.array[playout->current_index].end;
	}
	duration -= playout->current_transition_duration;
	return duration;
}

int64_t playout_source_get_time(void *data)
{
	struct playout_source_context *playout = data;
	if (!playout->current_source)
		return 0;
	int64_t time = obs_source_media_get_time(playout->current_source);
	if (playout->current_index >= 0 && playout->current_index < (int)playout->items.num)
		time -= playout->items.array[playout->current_index].start;
	return time;
}

enum obs_media_state playout_source_get_state(void *data)
{
	struct playout_source_context *playout = data;
	if (playout->current_source)
		return obs_source_media_get_state(playout->current_source);
	return OBS_MEDIA_STATE_NONE;
}

void playout_source_set_time(void *data, int64_t miliseconds)
{
	struct playout_source_context *playout = data;
	if (!playout->current_source)
		return;
	if (playout->current_index >= 0 && playout->current_index < (int)playout->items.num)
		miliseconds += playout->items.array[playout->current_index].start;
	obs_source_media_set_time(playout->current_source, miliseconds);
}

void playout_source_stop(void *data)
{
	struct playout_source_context *playout = data;
	if (playout->current_source)
		obs_source_media_stop(playout->current_source);
	playout->playing = false;
}

void playout_source_restart(void *data)
{
	struct playout_source_context *playout = data;
	if (playout->current_source) {
		obs_source_media_restart(playout->current_source);
		if (playout->current_index >= 0 && playout->current_index < (int)playout->items.num &&
		    playout->items.array[playout->current_index].start)
			obs_source_media_set_time(playout->current_source, playout->items.array[playout->current_index].start);
		else
			obs_source_media_set_time(playout->current_source, 0);
	}
	playout->playing = true;
}

void playout_source_play_pause(void *data, bool pause)
{
	struct playout_source_context *playout = data;
	if (playout->current_source)
		obs_source_media_play_pause(playout->current_source, pause);
	playout->playing = !pause;
}

void playout_source_next(void *data)
{
	struct playout_source_context *playout = data;
	playout_source_switch_to_next_item(playout);
}

void playout_source_previous(void *data)
{
	struct playout_source_context *playout = data;
	if (playout->current_index <= 0)
		return;
	playout->current_index--;
	playout_source_update_current_source(playout, true);
}

static void playout_source_frontend_event(enum obs_frontend_event event, void *data)
{
	struct playout_source_context *playout = data;
	if (event == OBS_FRONTEND_EVENT_TRANSITION_STOPPED && playout->next_after_transition) {
		if (playout->current_index >= (int)playout->items.num - 1) {
			playout->current_index = 0;
		} else {
			playout->current_index++;
		}
		playout_source_update_current_source(playout, false);
		playout->next_after_transition = false;
	}
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
	.media_play_pause = playout_source_play_pause,
	.media_restart = playout_source_restart,
	.media_stop = playout_source_stop,
	.media_next = playout_source_next,
	.media_previous = playout_source_previous,
	.media_get_state = playout_source_get_state,
	.media_get_duration = playout_source_get_duration,
	.media_get_time = playout_source_get_time,
	.media_set_time = playout_source_set_time,
	.activate = playout_source_activate,
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