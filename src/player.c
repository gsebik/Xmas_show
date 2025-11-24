#include "player.h"
#include "gpio.h"
#include "setup_alsa.h"
#include "load.h"
#include "log.h"

#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#define AUDIO_PERIOD_FRAMES 441
#define AUDIO_THREAD_PERIOD_MS 30
#define LED_THREAD_PERIOD_MS 10
#define MAX_RUNS 60000

#define PREFILL_PERIODS      4
#define MIN_BUFFER_PERIODS   1
#define MAX_BUFFER_PERIODS   5

#define MUSIC_BASE_DIR "/home/pi/music/"

// --------------------------------------------------------------
// Globals for real-time statistics
// --------------------------------------------------------------
static uint32_t gpio_shadow = 0;
static long runtimes_us[MAX_RUNS];
static long jitter_us[MAX_RUNS];
static long wake_intervals_us[MAX_RUNS];
static size_t runtime_index = 0;
static int underrun_count = 0;

static WavData wav;

// --------------------------------------------------------------
// Utility functions
// --------------------------------------------------------------
static long time_diff_us(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000000L +
           (end.tv_nsec - start.tv_nsec) / 1000L;
}

void reset_runtime_state(void) {
    runtime_index = 0;
    underrun_count = 0;
    gpio_shadow = 0;
    gpio_all_off(led_lines, 8);
}

static void make_log_filename(char *dst, size_t len,
                              const char *prefix, const char *song) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(dst, len, "%s_%s_%04d%02d%02d_%02d%02d%02d.csv",
             prefix, song,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/*** Re-prefill after underrun ***/
static void do_reprefill(size_t *frame_idx_ptr)
{
    size_t fi = *frame_idx_ptr;

    for (int r = 0; r < PREFILL_PERIODS; ++r) {
        if (fi + AUDIO_PERIOD_FRAMES > wav.frames)
            break;

        snd_pcm_sframes_t w =
            snd_pcm_writei(pcm,
                           &wav.pcm[fi * wav.channels],
                           AUDIO_PERIOD_FRAMES);

        if (w < 0) {
            snd_pcm_prepare(pcm);
            r--;    // retry this prefill period
            continue;
        }

        fi += AUDIO_PERIOD_FRAMES;
    }

    *frame_idx_ptr = fi;
}


// --------------------------------------------------------------
// Audio thread
// --------------------------------------------------------------
static void *audio_thread_fn(void *arg) {
    size_t frame_idx = 0;
    struct timespec next_time;
    clock_gettime(CLOCK_MONOTONIC, &next_time);
    struct timespec prev_wake_time = {0};

    const snd_pcm_sframes_t max_delay_frames =
        MAX_BUFFER_PERIODS * AUDIO_PERIOD_FRAMES;

    while (frame_idx + AUDIO_PERIOD_FRAMES * 3 <= wav.frames &&
           runtime_index < MAX_RUNS) {

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_time, NULL);

        struct timespec start_time, end_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        long wake_us = 0;
        if (prev_wake_time.tv_sec != 0)
            wake_us = time_diff_us(prev_wake_time, start_time);
        prev_wake_time = start_time;

        long total_runtime_us = 0;

        snd_pcm_sframes_t delay_frames = 0;
        if (snd_pcm_delay(pcm, &delay_frames) < 0)
            delay_frames = 0;

        for (int i = 0; i < 3; ++i) {
            
            if (delay_frames > max_delay_frames) {
                break;
            }

            struct timespec call_start, call_end;
            clock_gettime(CLOCK_MONOTONIC, &call_start);

            snd_pcm_sframes_t written =
                snd_pcm_writei(pcm, &wav.pcm[frame_idx * wav.channels],
                               AUDIO_PERIOD_FRAMES);
            if (written < 0) {
                underrun_count++;
                if (underrun_count <= 10 || underrun_count % 50 == 0)
                    syslog(LOG_WARNING, "Underrun #%d: %s",
                            underrun_count, snd_strerror(written));
                snd_pcm_prepare(pcm);

                do_reprefill(&frame_idx);

                break;
            }

            clock_gettime(CLOCK_MONOTONIC, &call_end);
            total_runtime_us += time_diff_us(call_start, call_end);
            frame_idx += AUDIO_PERIOD_FRAMES;

            if (snd_pcm_delay(pcm, &delay_frames) < 0)
                delay_frames = 0;

        }

        clock_gettime(CLOCK_MONOTONIC, &end_time);
        long jitter = time_diff_us(next_time, start_time);
        if (jitter < 0)
            syslog(LOG_ERR, "Deadline miss at cycle %zu by %ld us\n",
                    runtime_index, -jitter);

        runtimes_us[runtime_index] = total_runtime_us;
        wake_intervals_us[runtime_index] = wake_us;
        jitter_us[runtime_index] = jitter;

        if (runtime_index % 100 == 0) {
            snd_pcm_sframes_t delay;
            if (snd_pcm_delay(pcm, &delay) == 0) {
                syslog(LOG_INFO, "[Cycle %zu] ALSA delay: %ld frames (%.2f ms)\n",
                        runtime_index, delay,
                        (delay * 1000.0) / 44100.0);
            }
        }

        runtime_index++;

        // Advance next_time by one audio period
        next_time.tv_nsec += AUDIO_THREAD_PERIOD_MS * 1000000;
        while (next_time.tv_nsec >= 1000000000) {
            next_time.tv_sec++;
            next_time.tv_nsec -= 1000000000;
        }
    }
    return NULL;
}

// --------------------------------------------------------------
// LED thread
// --------------------------------------------------------------
static void *led_thread_fn(void *arg) {  

    int current_index = 0, tick_count = 0, ticks_for_current = 0;
    struct timespec start, next_time;
    clock_gettime(CLOCK_MONOTONIC, &start);
    next_time = start;

    int tick = 0;
    while (current_index < pattern_count) {
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_time, NULL);

        struct timespec tick_start, write_start, write_end;
        clock_gettime(CLOCK_MONOTONIC, &tick_start);

        if (tick_count == 0) {
            int values[8];
            for (int j = 0; j < 8; ++j)
                values[j] = (patterns[current_index].pattern >> (7 - j)) & 1;

            uint32_t set_mask = 0, clr_mask = 0;
            for (int j = 0; j < 8; ++j) {
                int pin = led_lines[j];
                if (values[j]) set_mask |= (1u << pin);
                else clr_mask |= (1u << pin);
            }

            clock_gettime(CLOCK_MONOTONIC, &write_start);

            uint32_t desired_state = gpio_shadow;
            desired_state &= ~clr_mask;
            desired_state |= set_mask;

            uint32_t led_mask = 0;
            for (int j = 0; j < 8; ++j)
                led_mask |= (1u << led_lines[j]);

            uint32_t bits_to_clear =
                (gpio_shadow & ~desired_state) & led_mask;
            uint32_t bits_to_set =
                (~gpio_shadow & desired_state) & led_mask;

            volatile uint32_t *GPSET0 = gpio + 0x1C / 4;
            volatile uint32_t *GPCLR0 = gpio + 0x28 / 4;

            *GPSET0 = bits_to_set;
            __sync_synchronize();
            *GPCLR0 = bits_to_clear;

            gpio_shadow = desired_state;

            clock_gettime(CLOCK_MONOTONIC, &write_end);

            int duration = patterns[current_index].duration_ms;
            if (duration < 70) duration = 70;
            duration = ((duration + 5) / 10) * 10;
            ticks_for_current = duration / LED_THREAD_PERIOD_MS;
            tick_count = ticks_for_current;

            syslog(LOG_DEBUG, "%d,%ld,%ld\n",
                    tick,
                    time_diff_us(start, tick_start),
                    time_diff_us(write_start, write_end));
        }

        tick_count--;
        if (tick_count == 0) current_index++;
        tick++;

        next_time.tv_nsec += LED_THREAD_PERIOD_MS * 1000000;
        while (next_time.tv_nsec >= 1000000000) {
            next_time.tv_sec++;
            next_time.tv_nsec -= 1000000000;
        }
    }

    return NULL;
}

// --------------------------------------------------------------
// Playback
// --------------------------------------------------------------
void play_song(const char *base_name) {
    char wav_file[128], pattern_file[128];
    snprintf(wav_file, sizeof(wav_file), "%s%s.wav", MUSIC_BASE_DIR, base_name);
    snprintf(pattern_file, sizeof(pattern_file), "%s%s.txt", MUSIC_BASE_DIR, base_name);

    char led_log[128], audio_log[128];
    make_log_filename(led_log, sizeof(led_log), "led_log", base_name);
    make_log_filename(audio_log, sizeof(audio_log), "audio_log", base_name);

    printf("\n=== Starting playback of '%s' ===\n", base_name);

    reset_runtime_state();

    wav = load_wav_mmap(wav_file);
    if (mlock(wav.mapping, wav.mapping_size) != 0) {
        perror("mlock failed");
	// avoids Linux demand paging (a.k.a. lazy loading), 4 kB/s disk -> RAM.
	// locks all the song into RAM, negating the effects of flash page faults
	// in case of error program may continue, but audio is now soft-RT instead of hard-RT
    }


    uint32_t sample_rate = wav.sample_rate;
    uint16_t channels    = wav.channels;
    load_patterns(pattern_file);
    setup_alsa(sample_rate, channels);

// Hard lock. Uncomment only for full lock for 'harder' RT behaviour.
//    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
//        perror("mlockall failed");
//	// locks everything into RAM (including code, data, libraries, stacks of RT threads)
//	// may continue, but becomes soft-rt
    
//    }

    pthread_t audio_thread, led_thread;

    struct sched_param audio_param = {.sched_priority = 75};
    struct sched_param led_param   = {.sched_priority = 80};

    pthread_attr_t audio_attr, led_attr;
    pthread_attr_init(&audio_attr);
    pthread_attr_setinheritsched(&audio_attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&audio_attr, SCHED_FIFO);
    pthread_attr_setschedparam(&audio_attr, &audio_param);

    pthread_attr_init(&led_attr);
    pthread_attr_setinheritsched(&led_attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&led_attr, SCHED_FIFO);
    pthread_attr_setschedparam(&led_attr, &led_param);

    pthread_create(&led_thread, &led_attr, led_thread_fn, led_log);
    pthread_create(&audio_thread, &audio_attr, audio_thread_fn, NULL);

    pthread_join(audio_thread, NULL);
    pthread_join(led_thread, NULL);

    gpio_all_off(led_lines, 8);
    alsa_close();

    save_runtime_log(audio_log,
                     runtimes_us, wake_intervals_us,
                     jitter_us, runtime_index, underrun_count);

    free_wav_mmap(&wav);

    printf("Playback finished for '%s'. Logs saved.\n", base_name);
}
