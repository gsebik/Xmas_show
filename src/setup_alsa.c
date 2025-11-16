#include "setup_alsa.h"
#include <stdio.h>
#include <stdlib.h>

#define AUDIO_PERIOD_FRAMES 441
#define MAX_AUDIO_FRAMES 120000000

snd_pcm_t *pcm = NULL;
static int16_t audio_buffer[MAX_AUDIO_FRAMES * 2];
int16_t *audio_data = audio_buffer;
size_t audio_frames = 0;

void setup_alsa(unsigned int sample_rate, unsigned int channels) {
    snd_pcm_hw_params_t *params;
    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        perror("snd_pcm_open");
        exit(1);
    }

    snd_pcm_hw_params_malloc(&params);
    snd_pcm_hw_params_any(pcm, params);
    snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, params, channels);
    snd_pcm_hw_params_set_rate(pcm, params, sample_rate, 0);

    snd_pcm_uframes_t buffer_size = AUDIO_PERIOD_FRAMES * 12;
    snd_pcm_uframes_t period_size = AUDIO_PERIOD_FRAMES;
    snd_pcm_hw_params_set_period_size_near(pcm, params, &period_size, 0);
    snd_pcm_hw_params_set_buffer_size_near(pcm, params, &buffer_size);
    
    snd_pcm_hw_params(pcm, params);
    snd_pcm_hw_params_free(params);
    snd_pcm_prepare(pcm);

    // --- PRE-FILL ALSA BUFFER WITH SILENCE ---
    int16_t silence[AUDIO_PERIOD_FRAMES * channels];
    memset(silence, 0, sizeof(silence));

    // Write several silent periods to fully flush old data
    for (int i = 0; i < 4; i++)
    {
	    snd_pcm_writei(pcm, silence, AUDIO_PERIOD_FRAMES);
    }

    // Re-prepare device again to reset buffer pointers
    snd_pcm_drop(pcm);
    snd_pcm_prepare(pcm);
}

void alsa_close(void) {
    if (pcm) {
        snd_pcm_drain(pcm);
        snd_pcm_close(pcm);
        pcm = NULL;
    }
}
