#pragma once
#include <obs-module.h>

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
	obs_source_t *audio_wrapper;
};
