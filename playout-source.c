#include "audio-wrapper.h"
#include "playout-source.h"
#include "version.h"
#include <obs-frontend-api.h>
#include <stdio.h>
#include <util/dstr.h>
#include <util/platform.h>

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
#define PLAYOUT_ACTION_ADD_FOLDER 7
#define PLAYOUT_ACTION_TRANSITION_SELECTED 8
#define PLAYOUT_ACTION_TRANSITION_DURATION_SELECTED 9
#define PLAYOUT_ACTION_SECTION_SELECTED 10
#define PLAYOUT_ACTION_SELECT_ALL 11
#define PLAYOUT_ACTION_SELECT_NONE 12
#define PLAYOUT_ACTION_SELECTION_INVERT 13

#define PLUGIN_INFO                                                                                      \
	"<a href=\"https://github.com/exeldro/obs-playout-source\">Playout Source</a> (" PROJECT_VERSION \
	") by <a href=\"https://www.exeldro.com\">Exeldro</a>"

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
	playout->current_index = -1;
	playout->audio_wrapper = obs_source_create_private(audio_wrapper_source.id, audio_wrapper_source.id, NULL);
	struct audio_wrapper_info *aw = obs_obj_get_data(playout->audio_wrapper);
	aw->playout = playout;
	obs_source_update(source, settings);
	return playout;
}

static void playout_source_destroy(void *data)
{
	struct playout_source_context *playout = data;
	if (playout->audio_wrapper) {
		obs_source_release(playout->audio_wrapper);
		playout->audio_wrapper = NULL;
	}
	if (playout->current_source) {
		obs_source_remove_active_child(playout->source, playout->current_source);
		obs_source_dec_showing(playout->current_source);
		obs_source_release(playout->current_source);
		playout->current_source = NULL;
	}
	if (playout->current_transition) {
		obs_source_remove_active_child(playout->source, playout->current_transition);
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
			obs_source_remove_active_child(playout->source, playout->current_source);
			obs_source_dec_showing(playout->current_source);
			obs_source_release(playout->current_source);
		}
		if (playout->current_transition) {
			obs_source_remove_active_child(playout->source, playout->current_transition);
			obs_source_dec_showing(playout->current_transition);
			obs_source_release(playout->current_transition);
			playout->current_transition = NULL;
		}
		playout->current_source = obs_source_get_ref(playout->items.array[playout->current_index].source);
		if (playout->current_source) {
			obs_source_inc_showing(playout->current_source);
			obs_source_add_active_child(playout->source, playout->current_source);
		}
		if (playout->items.array[playout->current_index].transition) {
			obs_transition_set(playout->items.array[playout->current_index].transition, playout->current_source);
			playout->current_transition = obs_source_get_ref(playout->items.array[playout->current_index].transition);
			obs_source_inc_showing(playout->current_transition);
			obs_source_add_active_child(playout->source, playout->current_transition);
			playout->current_transition_duration = playout->items.array[playout->current_index].transition_duration_ms;
		}
	}
	if (playout->current_source) {
		enum obs_media_state state = obs_source_media_get_state(playout->current_source);
		if (state == OBS_MEDIA_STATE_NONE) {
		} else if (state == OBS_MEDIA_STATE_PAUSED) {
			if (obs_source_media_get_time(playout->current_source) >=
			    (int64_t)playout->items.array[playout->current_index].start) {
				obs_source_media_play_pause(playout->current_source, false);
			}
		} else if (state == OBS_MEDIA_STATE_PLAYING) {
			if (!playout->items.array[playout->current_index].seek_start) {
				if (obs_source_media_get_time(playout->current_source) >=
				    (int64_t)playout->items.array[playout->current_index].start) {
					obs_source_media_set_time(playout->current_source,
								  playout->items.array[playout->current_index].start);
					playout->items.array[playout->current_index].seek_start = true;
				}
			}
		} else {
			obs_source_media_restart(playout->current_source);
		}
	}
	playout->playing = true;
	playout->active = true;
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
			obs_source_remove_active_child(playout->source, playout->current_transition);
			obs_source_dec_showing(playout->current_transition);
			obs_source_release(playout->current_transition);
			playout->current_transition = NULL;
			playout->current_transition_duration = 0;
		}
	}
	if (playout->current_source) {
		obs_source_remove_active_child(playout->source, playout->current_source);
		obs_source_dec_showing(playout->current_source);
		obs_source_release(playout->current_source);
		obs_source_media_play_pause(playout->current_source, true);
		for (size_t i = 0; i < playout->items.num; i++) {
			if (playout->items.array[i].source == playout->current_source) {
				enum obs_media_state state = obs_source_media_get_state(playout->current_source);
				if (state == OBS_MEDIA_STATE_ENDED) {
					obs_source_media_restart(playout->current_source);
				} else {
					obs_source_media_set_time(playout->current_source, playout->items.array[i].start);
					playout->items.array[i].seek_start = true;
					obs_source_media_play_pause(playout->current_source, false);
				}
			}
		}
	}
	playout->current_source = obs_source_get_ref(playout->items.array[playout->current_index].source);
	if (!playout->current_transition && playout->items.array[playout->current_index].transition) {
		obs_transition_set(playout->items.array[playout->current_index].transition, playout->current_source);
		playout->current_transition = obs_source_get_ref(playout->items.array[playout->current_index].transition);
		obs_source_inc_showing(playout->current_transition);
		obs_source_add_active_child(playout->source, playout->current_transition);
		playout->current_transition_duration = playout->items.array[playout->current_index].transition_duration_ms;
	}

	if (playout->current_source) {
		obs_source_inc_showing(playout->current_source);
		obs_source_add_active_child(playout->source, playout->current_source);
	}
	if (playout->active)
		playout_source_activate(playout);
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
		if (playout->loop) {
			obs_source_media_set_time(playout->current_source, playout->items.array[playout->current_index].start);
			playout->items.array[playout->current_index].seek_start = true;
			obs_source_media_play_pause(playout->current_source, false);
		} else if (playout->auto_play && obs_frontend_preview_program_mode_active()) {
			switch_scene = true;
		}
	}
	if (switch_scene && obs_source_active(playout->source)) {
		obs_frontend_preview_program_trigger_transition();
	}
	playout_source_update_current_source(playout, true);
}

static bool playout_source_use_global_transition(struct playout_source_context *playout)
{
	if (!playout->auto_play)
		return false;
	if (!obs_frontend_preview_program_mode_active())
		return false;
	return true;
}

static bool playout_source_last(struct playout_source_context *playout)
{
	if (playout->loop)
		return false;
	if (playout->playback_mode == PLAYBACK_MODE_LIST) {
		return playout->current_index == (int)playout->items.num - 1;
	} else if (playout->playback_mode == PLAYBACK_MODE_SECTION) {
		return playout->current_index >= (int)playout->items.num - 1 ||
		       (playout->items.array[playout->current_index].section &&
			playout->items.array[playout->current_index + 1].section &&
			strcmp(playout->items.array[playout->current_index].section,
			       playout->items.array[playout->current_index + 1].section) != 0) ||
		       (!playout->items.array[playout->current_index].section &&
			playout->items.array[playout->current_index + 1].section) ||
		       (playout->items.array[playout->current_index].section &&
			!playout->items.array[playout->current_index + 1].section);
	} else if (playout->playback_mode == PLAYBACK_MODE_SINGLE) {
		return true;
	}
	return false;
}

static void playout_source_media_ended(void *data, calldata_t *cd)
{
	struct playout_source_context *playout = data;
	obs_source_t *source = calldata_ptr(cd, "source");
	if (playout->current_source != source)
		return;

	if (playout_source_last(playout)) {
		if (playout_source_use_global_transition(playout) && !playout->next_after_transition &&
		    obs_source_active(playout->source)) {
			playout->next_after_transition = true;
			obs_frontend_preview_program_trigger_transition();
		}
	} else {
		playout->switch_to_next = true;
	}
}

static void playout_source_media_started(void *data, calldata_t *cd)
{
	struct playout_source_context *playout = data;
	obs_source_t *source = calldata_ptr(cd, "source");

	for (size_t i = 0; i < playout->items.num; i++) {
		if (playout->items.array[i].source != source)
			continue;
		if (obs_source_media_get_time(source) < (int64_t)playout->items.array[i].start) {
			obs_source_media_set_time(source, playout->items.array[i].start);
		}
		playout->items.array[i].seek_start = true;
		break;
	}
}

void playout_source_transition_stop(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct playout_source_context *playout = data;
	if (playout->current_index < 0 || playout->current_index >= (int)playout->items.num)
		return;
	if (playout_source_last(playout)) {
		if (playout->current_transition) {
			obs_source_remove_active_child(playout->source, playout->current_transition);
			obs_source_dec_showing(playout->current_transition);
			obs_source_release(playout->current_transition);
			playout->current_transition = NULL;
			playout->current_transition_duration = 0;
		}
	} else if (playout->items.array[playout->current_index].transition) {
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
			obs_source_add_active_child(playout->source, playout->current_transition);
			playout->current_transition_duration = playout->items.array[playout->current_index].transition_duration_ms;
		}
	} else if (playout->current_transition) {
		obs_source_remove_active_child(playout->source, playout->current_transition);
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

		dstr_printf(&setting_name, "start%d", i);
		playout->items.array[i].start = (uint64_t)(obs_data_get_double(settings, setting_name.array) * 1000.0);
		dstr_printf(&setting_name, "end%d", i);
		playout->items.array[i].end = (uint64_t)(obs_data_get_double(settings, setting_name.array) * -1000.0);

		if (!playout->items.array[i].source) {
			dstr_printf(&setting_name, "%s (%d)", obs_source_get_name(playout->source), i + 1);
			playout->items.array[i].source = obs_source_create_private("ffmpeg_source", setting_name.array, NULL);
			signal_handler_t *sh = obs_source_get_signal_handler(playout->items.array[i].source);
			signal_handler_connect(sh, "media_ended", playout_source_media_ended, data);
			signal_handler_connect(sh, "media_started", playout_source_media_started, data);
		}
		obs_data_t *ss = obs_data_create();
		obs_data_set_bool(ss, "is_local_file", true);
		obs_data_set_string(ss, "local_file", path);
		obs_data_set_bool(ss, "looping", false);
		obs_data_set_bool(ss, "is_stinger", false);
		obs_data_set_bool(ss, "hw_decode", true);
		obs_data_set_bool(ss, "close_when_inactive", false);
		obs_data_set_bool(ss, "clear_on_media_end", false);
		obs_data_set_bool(ss, "restart_on_activate", false);

		dstr_printf(&setting_name, "speed_percent%d", i);
		obs_data_set_default_int(settings, setting_name.array, 100);
		playout->items.array[i].speed = (uint32_t)obs_data_get_int(settings, setting_name.array);
		if (!playout->items.array[i].speed)
			playout->items.array[i].speed = 100;
		obs_data_set_int(ss, "speed_percent", playout->items.array[i].speed);
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
				//signal_handler_connect(sh, "transition_video_stop", playout_source_transition_video_stop, data);

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

static void playout_source_in_active_tree(obs_source_t *parent, obs_source_t *child, void *data)
{
	UNUSED_PARAMETER(parent);
	struct playout_source_context *playout = data;
	if (playout->source == child) {
		playout->active = true;
	}
}

static void playout_source_deactivate(void *data)
{
	struct playout_source_context *playout = data;
	if (playout->next_after_transition) {
		if (playout->current_index >= (int)playout->items.num - 1) {
			playout->current_index = 0;
		} else {
			playout->current_index++;
		}
		playout_source_update_current_source(playout, false);
		playout->next_after_transition = false;
	}
	playout->active = false;
}

static void playout_source_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct playout_source_context *playout = data;
	for (size_t i = 0; i < playout->items.num; i++) {
		if (!playout->items.array[i].seek_start)
			continue;
		enum obs_media_state state = obs_source_media_get_state(playout->items.array[i].source);
		if (state == OBS_MEDIA_STATE_NONE)
			continue;
		if (state != OBS_MEDIA_STATE_PLAYING) {
			playout->items.array[i].seek_start = false;
			continue;
		}
		if (obs_source_media_get_time(playout->items.array[i].source) <= (int64_t)playout->items.array[i].start)
			continue;
		if (!obs_source_get_width(playout->items.array[i].source))
			continue;
		playout->items.array[i].seek_start = false;
		if (playout->current_source != playout->items.array[i].source) {
			obs_source_media_play_pause(playout->items.array[i].source, true);
		} else if (playout->auto_play && !playout->active) {
			obs_source_media_play_pause(playout->items.array[i].source, true);
		}
	}

	if (playout->switch_to_next) {
		playout_source_switch_to_next_item(playout);
		return;
	}

	if (playout->auto_play) {
		bool old = playout->active;
		playout->active = false;
		obs_source_t *current = obs_get_output_source(0);
		if (current) {
			obs_source_enum_active_tree(current, playout_source_in_active_tree, playout);
			obs_source_release(current);
		}
		if (old != playout->active) {
			if (playout->active) {
				playout_source_activate(playout);
			} else {
				playout_source_deactivate(playout);
			}
		}
	}

	if (!playout->playing)
		return;

	if (!playout->current_source)
		return;

	int64_t duration = obs_source_media_get_duration(playout->current_source);
	if (duration <= 0)
		return;

	int64_t time = obs_source_media_get_time(playout->current_source);
	bool use_global_transition = playout_source_use_global_transition(playout);
	bool last = playout_source_last(playout);

	int64_t transition_duration = playout->items.array[playout->current_index].transition
					      ? (int64_t)playout->items.array[playout->current_index].transition_duration_ms
					      : 0;
	if (last) {
		if (playout->active && playout->auto_play && !playout->next_after_transition)
			playout->next_after_transition = true;
		if (use_global_transition) {
			transition_duration = (int64_t)obs_frontend_get_transition_duration();
		} else {
			transition_duration = 0;
		}
	}

	if (time >= duration - transition_duration - (int64_t)playout->items.array[playout->current_index].end) {
		if (use_global_transition && last) {
			if (!playout->next_after_transition && obs_source_active(playout->source)) {
				playout->next_after_transition = true;
				obs_frontend_preview_program_trigger_transition();
			}
		} else if (!last) {
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

static bool playout_source_action_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	bool changed = false;
	long long action = obs_data_get_int(settings, "action");
	obs_property_t *path = obs_properties_get(props, "action_path");
	if (obs_property_visible(path) != (action == PLAYOUT_ACTION_ADD_FOLDER)) {
		obs_property_set_visible(path, !obs_property_visible(path));
		changed = true;
	}
	obs_property_t *section = obs_properties_get(props, "action_section");
	if (obs_property_visible(section) != (action == PLAYOUT_ACTION_SECTION_SELECTED)) {
		obs_property_set_visible(section, !obs_property_visible(section));
		changed = true;
	}
	obs_property_t *transition = obs_properties_get(props, "action_transition");
	if (obs_property_visible(transition) != (action == PLAYOUT_ACTION_TRANSITION_SELECTED)) {
		obs_property_set_visible(transition, !obs_property_visible(transition));
		changed = true;
	}
	obs_property_t *transition_duration = obs_properties_get(props, "action_transition_duration");
	if (obs_property_visible(transition_duration) != (action == PLAYOUT_ACTION_TRANSITION_DURATION_SELECTED)) {
		obs_property_set_visible(transition_duration, !obs_property_visible(transition_duration));
		changed = true;
	}
	return changed;
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
		dstr_printf(&setting_name, "speed_percent%d", (int)playout->items.num);
		obs_data_set_default_int(settings, setting_name.array, 100);
		for (size_t i = playout->items.num; i > 0; i--) {
			playout_source_switch_item_settings(settings, i, i - 1, &setting_name);
		}
		obs_properties_remove_by_name(props, "plugin_info");
		add_item_properties(playout, props, &setting_name, (int)playout->items.num);
		obs_properties_add_text(props, "plugin_info", PLUGIN_INFO, OBS_TEXT_INFO);
		da_insert_new(playout->items, 0);
	} else if (action == PLAYOUT_ACTION_ADD_ITEM_BOTTOM) {
		dstr_printf(&setting_name, "speed_percent%d", (int)playout->items.num);
		obs_data_set_default_int(settings, setting_name.array, 100);
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
			dstr_printf(&setting_name, "item%d", i);
			obs_properties_remove_by_name(props, setting_name.array);
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
			dstr_printf(&setting_name, "item%d", i);
			obs_properties_remove_by_name(props, setting_name.array);
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
	} else if (action == PLAYOUT_ACTION_ADD_FOLDER) {
		const char *action_path = obs_data_get_string(settings, "action_path");
		os_dir_t *dir = os_opendir(action_path);
		obs_properties_remove_by_name(props, "plugin_info");
		for (struct os_dirent *ent = os_readdir(dir); ent != NULL; ent = os_readdir(dir)) {
			if (ent->directory)
				continue;
			dstr_printf(&setting_name, "path%d", (int)playout->items.num);
			struct dstr dir_path;
			dstr_init_copy(&dir_path, obs_data_get_string(settings, "action_path"));
			dstr_copy(&dir_path, action_path);
			dstr_cat_ch(&dir_path, '/');
			dstr_cat(&dir_path, ent->d_name);
			obs_data_set_string(settings, setting_name.array, dir_path.array);
			dstr_free(&dir_path);
			add_item_properties(playout, props, &setting_name, (int)playout->items.num);
			da_push_back_new(playout->items);
		}
		obs_properties_add_text(props, "plugin_info", PLUGIN_INFO, OBS_TEXT_INFO);
	} else if (action == PLAYOUT_ACTION_TRANSITION_SELECTED) {
		for (int i = 0; i < (int)playout->items.num; i++) {
			dstr_printf(&setting_name, "selected%d", i);
			if (obs_data_get_bool(settings, setting_name.array)) {
				dstr_printf(&setting_name, "transition%d", i);
				obs_data_set_string(settings, setting_name.array,
						    obs_data_get_string(settings, "action_transition"));
			}
		}
	} else if (action == PLAYOUT_ACTION_TRANSITION_DURATION_SELECTED) {
		for (int i = 0; i < (int)playout->items.num; i++) {
			dstr_printf(&setting_name, "selected%d", i);
			if (obs_data_get_bool(settings, setting_name.array)) {
				dstr_printf(&setting_name, "transition_duration%d", i);
				obs_data_set_int(settings, setting_name.array,
						 obs_data_get_int(settings, "action_transition_duration"));
			}
		}
	} else if (action == PLAYOUT_ACTION_SECTION_SELECTED) {
		for (int i = 0; i < (int)playout->items.num; i++) {
			dstr_printf(&setting_name, "selected%d", i);
			if (obs_data_get_bool(settings, setting_name.array)) {
				dstr_printf(&setting_name, "section%d", i);
				obs_data_set_string(settings, setting_name.array, obs_data_get_string(settings, "action_section"));
			}
		}
	} else if (action == PLAYOUT_ACTION_SELECT_ALL) {
		for (int i = 0; i < (int)playout->items.num; i++) {
			dstr_printf(&setting_name, "selected%d", i);
			obs_data_set_bool(settings, setting_name.array, true);
		}
	} else if (action == PLAYOUT_ACTION_SELECT_NONE) {
		for (int i = 0; i < (int)playout->items.num; i++) {
			dstr_printf(&setting_name, "selected%d", i);
			obs_data_set_bool(settings, setting_name.array, false);
		}
	} else if (action == PLAYOUT_ACTION_SELECTION_INVERT) {
		for (int i = 0; i < (int)playout->items.num; i++) {
			dstr_printf(&setting_name, "selected%d", i);
			obs_data_set_bool(settings, setting_name.array, !obs_data_get_bool(settings, setting_name.array));
		}
	}
	dstr_free(&setting_name);
	obs_data_unset_user_value(settings, "action");
	playout_source_action_changed(props, property, settings);
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
	obs_property_list_add_int(p, obs_module_text("AddFolder"), PLAYOUT_ACTION_ADD_FOLDER);
	obs_property_list_add_int(p, obs_module_text("SetTransitionSelected"), PLAYOUT_ACTION_TRANSITION_SELECTED);
	obs_property_list_add_int(p, obs_module_text("SetTransitionDurationSelected"), PLAYOUT_ACTION_TRANSITION_DURATION_SELECTED);
	obs_property_list_add_int(p, obs_module_text("SetSectionSelected"), PLAYOUT_ACTION_SECTION_SELECTED);
	obs_property_list_add_int(p, obs_module_text("SelectAll"), PLAYOUT_ACTION_SELECT_ALL);
	obs_property_list_add_int(p, obs_module_text("SelectNone"), PLAYOUT_ACTION_SELECT_NONE);
	obs_property_list_add_int(p, obs_module_text("InvertSelection"), PLAYOUT_ACTION_SELECTION_INVERT);
	obs_property_set_modified_callback(p, playout_source_action_changed);
	obs_properties_add_path(props, "action_path", obs_module_text("Directory"), OBS_PATH_DIRECTORY, NULL, NULL);
	obs_properties_add_text(props, "action_section", obs_module_text("Section"), OBS_TEXT_DEFAULT);
	p = obs_properties_add_list(props, "action_transition", obs_module_text("Transition"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("None"), "");
	size_t idx = 0;
	const char *id;
	while (obs_enum_transition_types(idx++, &id)) {
		const char *name = obs_source_get_display_name(id);
		obs_property_list_add_string(p, name, id);
	}
	p = obs_properties_add_int(props, "action_transition_duration", obs_module_text("TransitionDuration"), 50, 20000, 1000);
	obs_property_int_set_suffix(p, " ms");

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

int64_t playout_source_get_duration(void *data)
{
	struct playout_source_context *playout = data;

	if (!playout->current_source)
		return 0;
	int64_t duration = obs_source_media_get_duration(playout->current_source);
	if (duration <= 0)
		return 0;
	if (playout->current_index >= 0 && playout->current_index < (int)playout->items.num) {
		duration -= playout->items.array[playout->current_index].start;
		duration -= playout->items.array[playout->current_index].end;
	}
	int64_t transition_duration = playout->current_transition_duration;
	if (playout_source_last(playout)) {
		if (playout_source_use_global_transition(playout))
			transition_duration = (int64_t)obs_frontend_get_transition_duration();
		else
			transition_duration = 0;
	}
	duration -= transition_duration;
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
		enum obs_media_state state = obs_source_media_get_state(playout->current_source);
		if (state == OBS_MEDIA_STATE_ENDED || state == OBS_MEDIA_STATE_STOPPED || state == OBS_MEDIA_STATE_NONE) {
			obs_source_media_restart(playout->current_source);
		} else {
			if (playout->current_index >= 0 && playout->current_index < (int)playout->items.num &&
			    playout->items.array[playout->current_index].start) {
				obs_source_media_set_time(playout->current_source,
							  playout->items.array[playout->current_index].start);
				playout->items.array[playout->current_index].seek_start = true;
			} else {
				obs_source_media_set_time(playout->current_source, 0);
			}
			obs_source_media_play_pause(playout->current_source, false);
		}
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

static void playout_source_enum_active_sources(void *data, obs_source_enum_proc_t enum_callback, void *param)
{
	struct playout_source_context *playout = data;
	if (playout->current_source)
		enum_callback(playout->source, playout->current_source, param);
	if (playout->current_transition)
		enum_callback(playout->source, playout->current_transition, param);
	enum_callback(playout->source, playout->audio_wrapper, param);
}

struct obs_source_info playout_source = {
	.id = "playout_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_OUTPUT_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
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
	.deactivate = playout_source_deactivate,
	.enum_active_sources = playout_source_enum_active_sources,
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
	obs_register_source(&audio_wrapper_source);
	return true;
}

void obs_module_post_load() {}

void obs_module_unload() {}
