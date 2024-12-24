#pragma once
#include <obs-module.h>
#include <util/threading.h>
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 1, 0)
#include <util/deque.h>
#define circlebuf_peek_front deque_peek_front
#define circlebuf_peek_back deque_peek_back
#define circlebuf_push_front deque_push_front
#define circlebuf_push_back deque_push_back
#define circlebuf_pop_front deque_pop_front
#define circlebuf_pop_back deque_pop_back
#define circlebuf_init deque_init
#define circlebuf_free deque_free
#define circlebuf_data deque_data
#else
#include <util/circlebuf.h>
#endif

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

	obs_source_t* audio_wrapper;
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 1, 0)
	struct deque audio_data[MAX_AUDIO_CHANNELS];
	struct deque audio_frames;
	struct deque audio_timestamps;
#else
	struct circlebuf audio_data[MAX_AUDIO_CHANNELS];
	struct circlebuf audio_frames;
	struct circlebuf audio_timestamps;
#endif
	uint64_t audio_ts;
	size_t num_channels;
	pthread_mutex_t audio_mutex;
};
