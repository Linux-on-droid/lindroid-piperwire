/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2024 Lindroid project */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/utils/ringbuffer.h>
#include <spa/debug/types.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>


#include <pipewire/impl.h>
#include <pipewire/i18n.h>

#include <pthread.h>

/** \page page_module_fallback_sink Lindroid Sink
 *
 * Lindroid sink, which passses data to host lindroid app
 *
 * ## Module Name
 *
 * `libpipewire-module-lindroid'
 */

#define NAME "lindroid-sink"

#define AUDIO_SOCKET_PATH "/lindroid/audio_socket"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define AUDIO_OUTPUT_PREFIX 0x01
#define AUDIO_INPUT_PREFIX 0x02

#define BUFFER_SIZE 102400

static uint8_t audio_buffer[BUFFER_SIZE];
static size_t audio_buffer_start = 0;
static size_t audio_buffer_end = 0;
static pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t buffer_cond = PTHREAD_COND_INITIALIZER;

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Luka Panio <lukapanio@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Pushes data to Linsrois app" },
	{ PW_KEY_MODULE_USAGE, "" },
	{ PW_KEY_MODULE_VERSION, "1" },
};

struct bitmap {
	uint8_t *data;
	size_t size;
	size_t items;
};

struct impl {
	struct pw_context *context;

	struct pw_impl_module *module;
	struct spa_hook module_listener;

	struct pw_core *core;
	struct pw_registry *registry;
	struct pw_proxy *sink;

	struct spa_hook core_listener;
	struct spa_hook core_proxy_listener;
	struct spa_hook registry_listener;
	struct spa_hook sink_listener;

	struct pw_properties *properties;
	struct pw_properties *source_properties;

	struct bitmap sink_ids;
	struct bitmap fallback_sink_ids;

	int check_seq;

	unsigned int do_disconnect:1;
	unsigned int scheduled:1;

	int audio_socket_fd;

	struct spa_audio_info_raw info;
	struct spa_audio_info_raw source_info;
	struct pw_properties *stream_props;
	struct pw_properties *source_stream_props;
	struct pw_stream *stream;
	struct pw_stream *source_stream;
	struct spa_hook stream_listener;
	struct spa_hook source_stream_listener;
};

static void* socket_receive_thread(void* arg) {
    struct impl *impl = (struct impl*)arg;
    uint8_t temp_buffer[10241];
    ssize_t bytesRead;

    while (1) {
        bytesRead = recv(impl->audio_socket_fd, temp_buffer, sizeof(temp_buffer), 0);
        if (bytesRead <= 0) {
            pw_log_error("Failed to receive audio data: %m");
            continue;
        }

        // Check if the packet starts with 0x02
        if (temp_buffer[0] != 0x02) {
            pw_log_error("Invalid packet start byte, expected 0x02");
            continue;
        }

        pthread_mutex_lock(&buffer_mutex);

        // Start copying from the second byte
        for (ssize_t i = 1; i < bytesRead; ++i) {
            audio_buffer[audio_buffer_end] = temp_buffer[i];
            audio_buffer_end = (audio_buffer_end + 1) % BUFFER_SIZE;
            if (audio_buffer_end == audio_buffer_start) {
                audio_buffer_start = (audio_buffer_start + 1) % BUFFER_SIZE;
            }
        }

        pthread_cond_signal(&buffer_cond);
        pthread_mutex_unlock(&buffer_mutex);
    }

    return NULL;
}


static void stream_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->stream_listener);
	impl->stream = NULL;
}

static void stream_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = d;
	switch (state) {
	case PW_STREAM_STATE_ERROR:
	case PW_STREAM_STATE_UNCONNECTED:
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_STREAM_STATE_PAUSED:
	case PW_STREAM_STATE_STREAMING:
		break;
	default:
		break;
	}
}

static void playback_stream_process(void *d)
{
	struct impl *impl = d;
	struct pw_buffer *buf;
	struct spa_data *bd;
	void *data;
	uint32_t offs, size;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_debug("out of buffers: %m");
		return;
	}

	bd = &buf->buffer->datas[0];

	offs = SPA_MIN(bd->chunk->offset, bd->maxsize);
	size = SPA_MIN(bd->chunk->size, bd->maxsize - offs);
	data = SPA_PTROFF(bd->data, offs, void);

	if (impl->audio_socket_fd != -1) {
		if(size>=10239) {
			pw_log_error("buffer too big");
			return;
		}

		uint8_t prefixed_data[size + 1];
		prefixed_data[0] = AUDIO_OUTPUT_PREFIX;
		memcpy(prefixed_data + 1, data, size);

		ssize_t sent = send(impl->audio_socket_fd, prefixed_data, size + 1, 0);
		if (sent == -1) {
			pw_log_error("Failed to send audio data: %m");
		} else {
			pw_log_info("Sent %zd bytes of audio data", sent);
		}
	} else {
		pw_log_error("Audio socket is not connected");
	}
	pw_stream_queue_buffer(impl->stream, buf);
}

static void source_playback_process(void *data) {
	struct pw_buffer *b;
	struct spa_buffer *buf;
	uint8_t *dst;
	struct impl *impl = (struct impl*)data;

	if ((b = pw_stream_dequeue_buffer(impl->source_stream)) == NULL) {
		pw_log_debug("Out of playback buffers: %m");
		return;
	}

	buf = b->buffer;
	if ((dst = buf->datas[0].data) == NULL)
		return;

	buf->datas[0].chunk->offset = 0;
	buf->datas[0].chunk->stride = 4;
	buf->datas[0].chunk->size = 0;

	size_t requested_size = SPA_MIN(b->requested * 4, buf->datas[0].maxsize);
	size_t available_size = 0;

	pthread_mutex_lock(&buffer_mutex);
	while (audio_buffer_start == audio_buffer_end) {
		pthread_cond_wait(&buffer_cond, &buffer_mutex); // Wait for data to be available
	}

	if (audio_buffer_end > audio_buffer_start) {
		available_size = audio_buffer_end - audio_buffer_start;
	} else {
		available_size = BUFFER_SIZE - audio_buffer_start;
	}

	size_t copy_size = SPA_MIN(requested_size, available_size);
	memcpy(dst, audio_buffer + audio_buffer_start, copy_size);
	audio_buffer_start = (audio_buffer_start + copy_size) % BUFFER_SIZE;

	pthread_mutex_unlock(&buffer_mutex);

	buf->datas[0].chunk->size = copy_size;
	b->size = copy_size / 4;

	pw_stream_queue_buffer(impl->source_stream, b);
}

static const struct pw_stream_events playback_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
	.process = playback_stream_process
};

static const struct pw_stream_events input_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
	.process = source_playback_process
};


static int create_stream(struct impl *impl)
{
	int res;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b;

	impl->stream = pw_stream_new(impl->core, "Lindroid sink", impl->stream_props);
	impl->stream_props = NULL;

	impl->source_stream = pw_stream_new(impl->core, "Lindroid source", impl->source_stream_props);
	impl->source_stream_props = NULL;

	if (impl->stream == NULL || impl->source_stream == NULL)
		return -errno;

	pw_stream_add_listener(impl->stream,
			&impl->stream_listener,
			&playback_stream_events, impl);

	pw_stream_add_listener(impl->source_stream,
			&impl->source_stream_listener,
			&input_stream_events, impl);

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_EnumFormat, &impl->info);

	if ((res = pw_stream_connect(impl->stream,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_EnumFormat, &impl->source_info);

	if ((res = pw_stream_connect(impl->source_stream,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	return 0;
}


static int connect_audio_socket(struct impl *impl) {
	struct sockaddr_un addr;

	impl->audio_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (impl->audio_socket_fd == -1) {
		pw_log_error("Failed to create audio socket: %m");
		return -errno;
	}

	memset(&addr, 0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, AUDIO_SOCKET_PATH, sizeof(addr.sun_path) - 1);

	if (connect(impl->audio_socket_fd, (struct sockaddr *)&addr, sizeof(struct sockaddr_un)) == -1) {
		pw_log_error("Failed to connect to audio socket: %m");
		close(impl->audio_socket_fd);
		return -errno;
	}

	return 0;
}

static int bitmap_add(struct bitmap *map, uint32_t i)
{
	const uint32_t pos = (i >> 3);
	const uint8_t mask = 1 << (i & 0x7);

	if (pos >= map->size) {
		size_t new_size = map->size + pos + 16;
		void *p;

		p = realloc(map->data, new_size);
		if (!p)
			return -errno;

		memset((uint8_t*)p + map->size, 0, new_size - map->size);
		map->data = p;
		map->size = new_size;
	}

	if (map->data[pos] & mask)
		return 1;

	map->data[pos] |= mask;
	++map->items;

	return 0;
}

static bool bitmap_remove(struct bitmap *map, uint32_t i)
{
	const uint32_t pos = (i >> 3);
	const uint8_t mask = 1 << (i & 0x7);

	if (pos >= map->size)
		return false;

	if (!(map->data[pos] & mask))
		return false;

	map->data[pos] &= ~mask;
	--map->items;

	return true;
}

static void bitmap_free(struct bitmap *map)
{
	free(map->data);
	spa_zero(*map);
}

static int add_id(struct bitmap *map, uint32_t id)
{
	int res;

	if (id == SPA_ID_INVALID)
		return -EINVAL;

	if ((res = bitmap_add(map, id)) < 0)
	       pw_log_error("%s", spa_strerror(res));

	return res;
}

static void reschedule_check(struct impl *impl)
{
	if (!impl->scheduled)
		return;

	impl->check_seq = pw_core_sync(impl->core, 0, impl->check_seq);
}

static void schedule_check(struct impl *impl)
{
	if (impl->scheduled)
		return;

	impl->scheduled = true;
	impl->check_seq = pw_core_sync(impl->core, 0, impl->check_seq);
}

static void sink_proxy_removed(void *data)
{
	struct impl *impl = data;

	pw_proxy_destroy(impl->sink);
}

static void sink_proxy_bound_props(void *data, uint32_t id, const struct spa_dict *props)
{
	struct impl *impl = data;

	add_id(&impl->sink_ids, id);
	add_id(&impl->fallback_sink_ids, id);

	reschedule_check(impl);
	schedule_check(impl);
}

static void sink_proxy_destroy(void *data)
{
	struct impl *impl = data;

	pw_log_debug("fallback dummy sink destroyed");

	spa_hook_remove(&impl->sink_listener);
	impl->sink = NULL;
}

static const struct pw_proxy_events sink_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = sink_proxy_removed,
	.bound_props = sink_proxy_bound_props,
	.destroy = sink_proxy_destroy,
};

static int sink_create(struct impl *impl)
{
	if (impl->sink)
		return 0;

	pw_log_info("creating fallback dummy sink");

	impl->sink = pw_core_create_object(impl->core,
			"adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
			impl->properties ? &impl->properties->dict : NULL, 0);
	if (impl->sink == NULL)
		return -errno;

	pw_proxy_add_listener(impl->sink, &impl->sink_listener, &sink_proxy_events, impl);

	return 0;
}

static void sink_destroy(struct impl *impl)
{
	if (!impl->sink)
		return;

	pw_log_info("removing fallback dummy sink");
	pw_proxy_destroy(impl->sink);
}

static void check_sinks(struct impl *impl)
{
	int res;

	pw_log_debug("seeing %zu sink(s), %zu fallback sink(s)",
			impl->sink_ids.items, impl->fallback_sink_ids.items);

	if (impl->sink_ids.items > impl->fallback_sink_ids.items) {
		sink_destroy(impl);
	} else {
		if ((res = sink_create(impl)) < 0)
			pw_log_error("error creating sink: %s", spa_strerror(res));
	}
}

static void registry_event_global(void *data, uint32_t id,
		uint32_t permissions, const char *type, uint32_t version,
		const struct spa_dict *props)
{
	struct impl *impl = data;
	const char *str;

	reschedule_check(impl);

	if (!props)
		return;

	if (!spa_streq(type, PW_TYPE_INTERFACE_Node))
		return;

	str = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
	if (!(spa_streq(str, "Audio/Sink") || spa_streq(str, "Audio/Sink/Virtual")))
		return;

	add_id(&impl->sink_ids, id);
	schedule_check(impl);
}

static void registry_event_global_remove(void *data, uint32_t id)
{
	struct impl *impl = data;

	reschedule_check(impl);

	bitmap_remove(&impl->fallback_sink_ids, id);
	if (bitmap_remove(&impl->sink_ids, id))
		schedule_check(impl);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static void core_done(void *data, uint32_t id, int seq)
{
	struct impl *impl = data;

	if (seq == impl->check_seq) {
		impl->scheduled = false;
		check_sinks(impl);
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = core_done,
};

static void core_proxy_removed(void *data)
{
	struct impl *impl = data;

	if (impl->registry) {
		spa_hook_remove(&impl->registry_listener);
		pw_proxy_destroy((struct pw_proxy*)impl->registry);
		impl->registry = NULL;
	}
}

static void core_proxy_destroy(void *data)
{
	struct impl *impl = data;

	spa_hook_remove(&impl->core_listener);
	spa_hook_remove(&impl->core_proxy_listener);
	impl->core = NULL;
}

static const struct pw_proxy_events core_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = core_proxy_destroy,
	.removed = core_proxy_removed,
};

static void impl_destroy(struct impl *impl)
{
	sink_destroy(impl);

	if (impl->stream)
		pw_stream_destroy(impl->stream);

	pw_properties_free(impl->stream_props);

	if (impl->registry) {
		spa_hook_remove(&impl->registry_listener);
		pw_proxy_destroy((struct pw_proxy*)impl->registry);
		impl->registry = NULL;
	}

	if (impl->core) {
		spa_hook_remove(&impl->core_listener);
		spa_hook_remove(&impl->core_proxy_listener);
		if (impl->do_disconnect)
			pw_core_disconnect(impl->core);
		impl->core = NULL;
	}

	if (impl->properties) {
		pw_properties_free(impl->properties);
		impl->properties = NULL;
	}

	bitmap_free(&impl->sink_ids);
	bitmap_free(&impl->fallback_sink_ids);

	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->module_listener);
	impl_destroy(impl);
}

static void copy_props(struct impl *impl, struct pw_properties *props, const char *key)
{
	const char *str;
	if ((str = pw_properties_get(props, key)) != NULL) {
		if (pw_properties_get(impl->stream_props, key) == NULL)
			pw_properties_set(impl->stream_props, key, str);
	}
}


static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static inline uint32_t format_from_name(const char *name, size_t len)
{
	int i;
	for (i = 0; spa_type_audio_format[i].name; i++) {
		if (strncmp(name, spa_debug_type_short_name(spa_type_audio_format[i].name), len) == 0)
			return spa_type_audio_format[i].type;
	}
	return SPA_AUDIO_FORMAT_UNKNOWN;
}

static uint32_t channel_from_name(const char *name)
{
	int i;
	for (i = 0; spa_type_audio_channel[i].name; i++) {
		if (spa_streq(name, spa_debug_type_short_name(spa_type_audio_channel[i].name)))
			return spa_type_audio_channel[i].type;
	}
	return SPA_AUDIO_CHANNEL_UNKNOWN;
}

static void parse_position(struct spa_audio_info_raw *info, const char *val, size_t len)
{
	struct spa_json it[2];
	char v[256];

	spa_json_init(&it[0], val, len);
        if (spa_json_enter_array(&it[0], &it[1]) <= 0)
                spa_json_init(&it[1], val, len);

	info->channels = 0;
	while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
	    info->channels < SPA_AUDIO_MAX_CHANNELS) {
		info->position[info->channels++] = channel_from_name(v);
	}
}


static void parse_audio_info(const struct pw_properties *props, struct spa_audio_info_raw *info)
{
	const char *str;

	spa_zero(*info);
	info->format = format_from_name("S16", strlen("S16"));
	info->rate = 48000;

	info->channels = 2;
	info->channels = 2;
	parse_position(info, "FL,FR", strlen("FL,FR"));
}

static void parse_audio_info_mic(const struct pw_properties *props, struct spa_audio_info_raw *info)
{
        const char *str;

        spa_zero(*info);
        info->format = format_from_name("S16", strlen("S16"));
        info->rate = 48000;

        info->channels = 1;
        info->channels = 1;
        parse_position(info, "MONO", strlen("MONO"));
}


SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props = NULL;
	struct pw_properties *source_props = NULL;
	struct impl *impl = NULL;
	const char *str;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto error_errno;

	pw_log_debug("module %p: new %s", impl, args);

	impl->module = module;
	impl->context = context;

	props = pw_properties_new(NULL, NULL);
	if (props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}
	impl->properties = props;

	source_props = pw_properties_new(NULL, NULL);
	if (source_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}
	impl->source_properties = source_props;

	impl->stream_props = pw_properties_new(NULL, NULL);
	if (impl->stream_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->source_stream_props = pw_properties_new(NULL, NULL);
	if (impl->source_stream_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	pw_properties_set(props, PW_KEY_NODE_NAME, "Lindroid Sink");
	pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, "Lindroid audio output");

    // TBD: Do not assume channel count/location
	pw_properties_setf(props, SPA_KEY_AUDIO_RATE, "%u", 48000);
	pw_properties_setf(props, SPA_KEY_AUDIO_CHANNELS, "%u", 2);
	pw_properties_set(props, SPA_KEY_AUDIO_POSITION, "FL,FR");

	pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
	pw_properties_set(props, PW_KEY_FACTORY_NAME, "support.null-audio-sink");
	pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "false");
	pw_properties_set(props, "monitor.channel-volumes", "true");

	pw_properties_set(impl->stream_props, PW_KEY_NODE_NAME, "Lindroid Sink");
	pw_properties_set(impl->stream_props, PW_KEY_NODE_DESCRIPTION, "Lindroid audio output");

    // TBD: Do not assume channel count/location
	pw_properties_setf(impl->stream_props, SPA_KEY_AUDIO_RATE, "%u", 48000);
	pw_properties_setf(impl->stream_props, SPA_KEY_AUDIO_CHANNELS, "%u", 2);
	pw_properties_set(impl->stream_props, SPA_KEY_AUDIO_POSITION, "FL,FR");

	pw_properties_set(impl->stream_props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
	pw_properties_set(impl->stream_props, PW_KEY_FACTORY_NAME, "support.null-audio-sink");
	pw_properties_set(impl->stream_props, PW_KEY_NODE_VIRTUAL, "false");
	pw_properties_set(impl->stream_props, "monitor.channel-volumes", "true");

	parse_audio_info(impl->stream_props, &impl->info);


	// TBD: Do not assume channel count/location
	pw_properties_setf(source_props, SPA_KEY_AUDIO_RATE, "%u", 48000);
	pw_properties_setf(source_props, SPA_KEY_AUDIO_CHANNELS, "%u", 1);
	pw_properties_set(source_props, SPA_KEY_AUDIO_POSITION, "MONO");

	pw_properties_set(source_props, PW_KEY_MEDIA_CLASS, "Audio/Source");
	pw_properties_set(source_props, PW_KEY_FACTORY_NAME, "support.null-audio-source");
	pw_properties_set(source_props, PW_KEY_NODE_VIRTUAL, "false");
	pw_properties_set(source_props, "monitor.channel-volumes", "true");

	pw_properties_set(impl->source_stream_props, PW_KEY_NODE_NAME, "Lindroid Source");
	pw_properties_set(impl->source_stream_props, PW_KEY_NODE_DESCRIPTION, "Lindroid audio input");

    // TBD: Do not assume channel count/location
	pw_properties_setf(impl->source_stream_props, SPA_KEY_AUDIO_RATE, "%u", 48000);
	pw_properties_setf(impl->source_stream_props, SPA_KEY_AUDIO_CHANNELS, "%u", 1);
	pw_properties_set(impl->source_stream_props, SPA_KEY_AUDIO_POSITION, "MONO");

	pw_properties_set(impl->source_stream_props, PW_KEY_MEDIA_CLASS, "Audio/Source");
	pw_properties_set(impl->source_stream_props, PW_KEY_FACTORY_NAME, "support.null-audio-source");
	pw_properties_set(impl->source_stream_props, PW_KEY_NODE_VIRTUAL, "false");
	pw_properties_set(impl->source_stream_props, "monitor.channel-volumes", "true");

	parse_audio_info_mic(impl->source_stream_props, &impl->source_info);

	impl->core = pw_context_get_object(impl->context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		impl->core = pw_context_connect(impl->context,
				pw_properties_new(
					PW_KEY_REMOTE_NAME, "",
					NULL),
				0);
		impl->do_disconnect = true;
	}

	if (impl->core == NULL) {
		res = -errno;
		pw_log_error("can't connect: %m");
		goto error;
	}

	pw_proxy_add_listener((struct pw_proxy*)impl->core,
			&impl->core_proxy_listener, &core_proxy_events,
			impl);

	pw_core_add_listener(impl->core, &impl->core_listener, &core_events, impl);

	impl->registry = pw_core_get_registry(impl->core,
			PW_VERSION_REGISTRY, 0);
	if (impl->registry == NULL)
		goto error_errno;

	pw_registry_add_listener(impl->registry,
			&impl->registry_listener,
			&registry_events, impl);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	schedule_check(impl);

	if ((res = connect_audio_socket(impl)) < 0)
		goto error;

	if ((res = create_stream(impl)) < 0)
		goto error;

	// Start socket recieve thread
	pthread_t thread_id;
	if (pthread_create(&thread_id, NULL, socket_receive_thread, impl) != 0) {
		pw_log_error("Failed to create socket receive thread");
		goto error;
	}

	return 0;

error_errno:
	res = -errno;
error:
	if (impl)
		impl_destroy(impl);
	return 0;
}
