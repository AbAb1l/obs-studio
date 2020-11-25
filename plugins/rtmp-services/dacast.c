#include <file-updater/file-updater.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <jansson.h>

#include "dacast.h"

#ifndef SEC_TO_NSEC
#define SEC_TO_NSEC 1000000000ULL
#endif

static update_info_t *dacast_update_info = NULL;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static bool ingests_loaded = false;

struct dacast_ingest_info {
	char *key;
	uint64_t last_time;
	struct dacast_ingest ingest;
};

struct dacast_ingest dacast_invalid_ingest = {"", "", "", "", "", "", "", -1,
					      -1, "", "", "", -1, -1, -1};

static DARRAY(struct dacast_ingest_info) cur_ingests;

static void free_ingest(struct dacast_ingest ingest)
{
	bfree((void *)ingest.url);
	bfree((void *)ingest.url2);
	bfree((void *)ingest.username);
	bfree((void *)ingest.password);
	bfree((void *)ingest.streamkey);
	if (ingest.video_codec) {
		bfree((void *)ingest.video_codec);
	}
	bfree((void *)ingest.h264_profile);
	bfree((void *)ingest.scanning);
	bfree((void *)ingest.rate_control);
	bfree((void *)ingest.audio_codec);
}

static void free_ingests(void)
{
	for (size_t i = 0; i < cur_ingests.num; i++) {
		struct dacast_ingest_info *info = &cur_ingests.array[i];
		bfree(info->key);
		free_ingest(info->ingest);
	}
	da_free(cur_ingests);
}

static struct dacast_ingest_info *find_ingest(const char *key)
{
	struct dacast_ingest_info *ret = NULL;
	for (size_t i = 0; i < cur_ingests.num; i++) {
		struct dacast_ingest_info *info = &cur_ingests.array[i];
		if (strcmp(info->key, key) == 0) {
			ret = info;
			break;
		}
	}
	return (struct dacast_ingest_info *)ret;
}

static bool load_ingests(const char *json, const char *key)
{
	json_t *root;
	json_t *stream;
	json_t *encoder;
	bool success = false;
	struct dacast_ingest_info *info = find_ingest(key);
	if (!info) {
		info = da_push_back_new(cur_ingests);
		info->key = bstrdup(key);
	} else {
		free_ingest(info->ingest);
	}

	root = json_loads(json, 0, NULL);
	if (!root)
		goto finish;

	stream = json_object_get(root, "stream");
	if (!stream)
		goto finish;

	json_t *item_server = json_object_get(stream, "server");
	json_t *item_backup = json_object_get(stream, "backup");
	json_t *item_username = json_object_get(stream, "username");
	json_t *item_password = json_object_get(stream, "password");
	json_t *item_streamkey = json_object_get(stream, "streamkey");

	if (!item_server || !item_backup || !item_username || !item_password ||
	    !item_streamkey)
		goto finish;

	const char *server = json_string_value(item_server);
	const char *backup = json_string_value(item_backup);
	const char *username = json_string_value(item_username);
	const char *password = json_string_value(item_password);
	const char *streamkey = json_string_value(item_streamkey);

	info->ingest.url = bstrdup(server);
	info->ingest.url2 = bstrdup(backup);
	info->ingest.username = bstrdup(username);
	info->ingest.password = bstrdup(password);
	info->ingest.streamkey = bstrdup(streamkey);

	encoder = json_object_get(root, "encoder");
	if (!encoder)
		goto finish;

	json_t *item_video_codec = json_object_get(encoder, "video_codec");
	json_t *item_h264_profile = json_object_get(encoder, "h264_profile");
	json_t *item_scanning = json_object_get(encoder, "scanning");
	json_t *item_rate_control = json_object_get(encoder, "rate_control");
	json_t *item_audio_codec = json_object_get(encoder, "audio_codec");

	json_t *item_frame_rate = json_object_get(encoder, "frame_rate");
	json_t *item_keyframe_interval =
		json_object_get(encoder, "keyframe_interval");
	json_t *item_audio_bitrate = json_object_get(encoder, "audio_bitrate");
	json_t *item_audio_channels =
		json_object_get(encoder, "audio_channels");
	json_t *item_audio_sample_rate =
		json_object_get(encoder, "audio_sample_rate");

	if (!item_video_codec || !item_h264_profile || !item_scanning ||
	    !item_rate_control || !item_audio_codec || !item_frame_rate ||
	    !item_keyframe_interval || !item_audio_bitrate ||
	    !item_audio_channels || !item_audio_sample_rate)
		goto finish;

	const char *video_codec = json_string_value(item_video_codec);
	const char *h264_profile = json_string_value(item_h264_profile);
	const char *scanning = json_string_value(item_scanning);
	const char *rate_control = json_string_value(item_rate_control);
	const char *audio_codec = json_string_value(item_audio_codec);

	double frame_rate = json_number_value(item_frame_rate);
	int keyframe_interval = json_integer_value(item_keyframe_interval);
	int audio_bitrate = json_integer_value(item_audio_bitrate);
	int audio_channels = json_integer_value(item_audio_channels);
	int audio_sample_rate = json_integer_value(item_audio_sample_rate);

	if (strcmp(video_codec, "H.264") == 0) {
		info->ingest.video_codec = bstrdup("x264");
	}
	info->ingest.h264_profile = bstrdup(h264_profile);
	info->ingest.scanning = bstrdup(scanning);
	info->ingest.rate_control = bstrdup(rate_control);
	info->ingest.audio_codec = bstrdup(audio_codec);

	info->ingest.frame_rate = frame_rate;
	info->ingest.keyframe_interval = keyframe_interval / 60;
	info->ingest.audio_bitrate = audio_bitrate;
	info->ingest.audio_channels = audio_channels;
	info->ingest.audio_sample_rate = audio_sample_rate;

	info->last_time = os_gettime_ns() / SEC_TO_NSEC;

	success = true;

finish:
	if (root)
		json_decref(root);
	return success;
}

static bool dacast_ingest_update(void *param, struct file_download_data *data)
{
	bool success;

	pthread_mutex_lock(&mutex);
	success = load_ingests((const char *)data->buffer.array,
			       (const char *)param);
	pthread_mutex_unlock(&mutex);

	update_info_destroy(dacast_update_info);
	dacast_update_info = NULL;

	if (success) {
		os_atomic_set_bool(&ingests_loaded, true);
	}

	return true;
}

struct dacast_ingest *dacast_ingest(const char *key)
{
	pthread_mutex_lock(&mutex);
	struct dacast_ingest_info *info = find_ingest(key);
	pthread_mutex_unlock(&mutex);
	return info == NULL ? &dacast_invalid_ingest : &info->ingest;
}

void init_dacast_data(void)
{
	da_init(cur_ingests);
	pthread_mutex_init(&mutex, NULL);
}

extern const char *get_module_name(void);

void dacast_ingests_load_data(const char *server, const char *key)
{
	struct dstr uri = {0};

	os_atomic_set_bool(&ingests_loaded, false);

	dstr_copy(&uri, server);
	dstr_cat(&uri, key);

	dacast_update_info = update_info_create_single(
		"[dacast ingest load data] ", get_module_name(), uri.array,
		dacast_ingest_update, (void *)key);

	if (!os_atomic_load_bool(&ingests_loaded)) {
		for (int i = 0; i < 3 * 100; i++) {
			if (os_atomic_load_bool(&ingests_loaded)) {
				break;
			}
			os_sleep_ms(10);
		}
	}

	dstr_free(&uri);
}

void unload_dacast_data(void)
{
	update_info_destroy(dacast_update_info);
	free_ingests();
	pthread_mutex_destroy(&mutex);
}