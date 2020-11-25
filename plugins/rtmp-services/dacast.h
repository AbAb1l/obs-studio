#pragma once

struct dacast_ingest {
	const char *url;
	const char *url2;
	const char *username;
	const char *password;
	const char *streamkey;

	const char *video_codec;
	const char *h264_profile;
	double frame_rate;
	int keyframe_interval;
	const char *scanning;
	const char *rate_control;
	const char *audio_codec;
	int audio_bitrate;
	int audio_channels;
	int audio_sample_rate;
};

extern void init_dacast_data(void);
extern void unload_dacast_data(void);

extern void dacast_ingests_load_data(const char *server, const char *key);
extern struct dacast_ingest *dacast_ingest(const char *key);
