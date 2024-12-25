#pragma once
#include <obs.h>

struct audio_wrapper_info {
	obs_source_t *source;
	struct playout_source_context *playout;
};

extern struct obs_source_info audio_wrapper_source;
