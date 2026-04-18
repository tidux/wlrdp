#ifndef WLRDP_AUDIO_H
#define WLRDP_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef WLRDP_HAVE_PIPEWIRE

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

/* Audio format: 16-bit signed LE, stereo, 44100 Hz */
#define WLRDP_AUDIO_RATE      44100
#define WLRDP_AUDIO_CHANNELS  2
#define WLRDP_AUDIO_FRAME_SIZE (sizeof(int16_t) * WLRDP_AUDIO_CHANNELS)

/* Callback when audio data is available */
typedef void (*audio_data_cb)(void *data, const int16_t *samples,
                              uint32_t n_frames);

struct wlrdp_audio {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_stream *stream;
    struct spa_hook stream_listener;

    audio_data_cb on_data;
    void *cb_data;

    bool started;
};

bool audio_init(struct wlrdp_audio *audio, audio_data_cb on_data, void *cb_data);
void audio_destroy(struct wlrdp_audio *audio);
int audio_get_fd(struct wlrdp_audio *audio);
int audio_dispatch(struct wlrdp_audio *audio);

#else /* !WLRDP_HAVE_PIPEWIRE */

struct wlrdp_audio {
    bool started;
};

static inline bool audio_init(struct wlrdp_audio *audio, void *on_data, void *cb_data)
{
    (void)audio; (void)on_data; (void)cb_data;
    return false;
}
static inline void audio_destroy(struct wlrdp_audio *audio) { (void)audio; }
static inline int audio_get_fd(struct wlrdp_audio *audio) { (void)audio; return -1; }
static inline int audio_dispatch(struct wlrdp_audio *audio) { (void)audio; return 0; }

#endif /* WLRDP_HAVE_PIPEWIRE */

#endif /* WLRDP_AUDIO_H */
