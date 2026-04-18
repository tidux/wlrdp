#include "audio.h"
#include "common.h"

#ifdef WLRDP_HAVE_PIPEWIRE

#include <string.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>

static void on_process(void *userdata)
{
    struct wlrdp_audio *audio = userdata;

    struct pw_buffer *b = pw_stream_dequeue_buffer(audio->stream);
    if (!b) return;

    struct spa_buffer *buf = b->buffer;
    if (!buf->datas[0].data) {
        pw_stream_queue_buffer(audio->stream, b);
        return;
    }

    uint32_t n_bytes = buf->datas[0].chunk->size;
    uint32_t n_frames = n_bytes / WLRDP_AUDIO_FRAME_SIZE;
    const int16_t *samples = buf->datas[0].data;

    if (audio->on_data && n_frames > 0) {
        audio->on_data(audio->cb_data, samples, n_frames);
    }

    pw_stream_queue_buffer(audio->stream, b);
}

static void on_stream_state_changed(void *userdata, enum pw_stream_state old,
                                     enum pw_stream_state state,
                                     const char *error)
{
    (void)userdata;
    WLRDP_LOG_INFO("PipeWire stream state: %s -> %s",
                    pw_stream_state_as_string(old),
                    pw_stream_state_as_string(state));
    if (error) {
        WLRDP_LOG_WARN("PipeWire stream error: %s", error);
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_process,
    .state_changed = on_stream_state_changed,
};

bool audio_init(struct wlrdp_audio *audio, audio_data_cb on_data, void *cb_data)
{
    memset(audio, 0, sizeof(*audio));
    audio->on_data = on_data;
    audio->cb_data = cb_data;

    pw_init(NULL, NULL);

    audio->loop = pw_main_loop_new(NULL);
    if (!audio->loop) {
        WLRDP_LOG_ERROR("failed to create PipeWire main loop");
        return false;
    }

    audio->context = pw_context_new(
        pw_main_loop_get_loop(audio->loop), NULL, 0);
    if (!audio->context) {
        WLRDP_LOG_ERROR("failed to create PipeWire context");
        pw_main_loop_destroy(audio->loop);
        return false;
    }

    audio->core = pw_context_connect(audio->context, NULL, 0);
    if (!audio->core) {
        WLRDP_LOG_ERROR("failed to connect to PipeWire daemon");
        pw_context_destroy(audio->context);
        pw_main_loop_destroy(audio->loop);
        return false;
    }

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        PW_KEY_STREAM_CAPTURE_SINK, "true",  /* capture sink output (monitor) */
        PW_KEY_NODE_NAME, "wlrdp-audio-capture",
        NULL);

    audio->stream = pw_stream_new(audio->core, "wlrdp-audio", props);
    if (!audio->stream) {
        WLRDP_LOG_ERROR("failed to create PipeWire stream");
        audio_destroy(audio);
        return false;
    }

    pw_stream_add_listener(audio->stream, &audio->stream_listener,
                           &stream_events, audio);

    /* Build audio format params */
    uint8_t param_buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(param_buf, sizeof(param_buf));
    const struct spa_pod *params[1];

    struct spa_audio_info_raw info = {
        .format = SPA_AUDIO_FORMAT_S16_LE,
        .rate = WLRDP_AUDIO_RATE,
        .channels = WLRDP_AUDIO_CHANNELS,
    };
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    if (pw_stream_connect(audio->stream,
                          PW_DIRECTION_INPUT,
                          PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT |
                          PW_STREAM_FLAG_MAP_BUFFERS,
                          params, 1) < 0) {
        WLRDP_LOG_ERROR("failed to connect PipeWire stream");
        audio_destroy(audio);
        return false;
    }

    audio->started = true;
    WLRDP_LOG_INFO("audio capture initialized (S16LE, %u Hz, %u ch)",
                    WLRDP_AUDIO_RATE, WLRDP_AUDIO_CHANNELS);
    return true;
}

int audio_get_fd(struct wlrdp_audio *audio)
{
    if (!audio->loop) return -1;
    struct pw_loop *loop = pw_main_loop_get_loop(audio->loop);
    return pw_loop_get_fd(loop);
}

int audio_dispatch(struct wlrdp_audio *audio)
{
    if (!audio->loop) return 0;
    struct pw_loop *loop = pw_main_loop_get_loop(audio->loop);
    return pw_loop_iterate(loop, 0);
}

void audio_destroy(struct wlrdp_audio *audio)
{
    if (audio->stream) {
        pw_stream_destroy(audio->stream);
        audio->stream = NULL;
    }
    if (audio->core) {
        pw_core_disconnect(audio->core);
        audio->core = NULL;
    }
    if (audio->context) {
        pw_context_destroy(audio->context);
        audio->context = NULL;
    }
    if (audio->loop) {
        pw_main_loop_destroy(audio->loop);
        audio->loop = NULL;
    }
    if (audio->started) {
        pw_deinit();
        audio->started = false;
    }
}

#endif /* WLRDP_HAVE_PIPEWIRE */
