#include "player.h"
#include "gpio.h"
#include "setup_alsa.h"
#include "load.h"
#include "audio.h"
#include "log.h"

#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include <syslog.h>
#include <sys/mman.h>

// AUDIO_PERIOD_FRAMES: calculated at runtime based on sample rate
// Target: 10ms worth of frames (e.g., 441 @ 44100Hz, 480 @ 48000Hz)
#define AUDIO_PERIOD_MS 10
#define AUDIO_THREAD_PERIOD_MS 30
#define LED_THREAD_PERIOD_MS 10
#define MAX_RUNS 60000

#define PREFILL_PERIODS      4
#define MIN_BUFFER_PERIODS   1
#define MAX_BUFFER_PERIODS   5

// Runtime-calculated period size based on sample rate
static size_t audio_period_frames = 441;  // Default for 44100Hz

#define DEFAULT_MUSIC_DIR "/home/linux/music/"
#define MAX_PATH 512

static char music_base_dir[MAX_PATH] = DEFAULT_MUSIC_DIR;

// --------------------------------------------------------------
// Globals for real-time statistics
// --------------------------------------------------------------
static uint32_t gpio_shadow = 0;

// Audio thread stats
static long audio_runtime_us[MAX_RUNS];
static long audio_jitter_us[MAX_RUNS];
static long audio_wake_interval_us[MAX_RUNS];
static long audio_buffer_frames[MAX_RUNS];
static long alsa_delay_frames[MAX_RUNS];
static size_t audio_sample_index = 0;
static int underrun_count = 0;
static int buffer_stall_count = 0;

// GPIO timing stats (nanoseconds)
static long gpio_write_ns[MAX_RUNS];
static long gpio_jitter_ns[MAX_RUNS];
static size_t gpio_timing_index = 0;

// Playback timing
static struct timespec playback_start_time;
static struct timespec playback_end_time;

// Verbose mode flag (set via -v command line arg)
static int verbose_mode = 0;

static AudioStream *audio_stream = NULL;

// --------------------------------------------------------------
// Utility functions
// --------------------------------------------------------------
static long time_diff_us(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000000L +
           (end.tv_nsec - start.tv_nsec) / 1000L;
}

static long time_diff_ns(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000000000L +
           (end.tv_nsec - start.tv_nsec);
}

void reset_runtime_state(void) {
    audio_sample_index = 0;
    underrun_count = 0;
    buffer_stall_count = 0;
    gpio_shadow = 0;
    gpio_timing_index = 0;
    gpio_all_off(led_lines, 8);
}

static void print_stats(int has_audio, double duration_sec) {
    if (!verbose_mode) return;

    printf("\n=== Playback Stats ===\n");
    printf("Duration: %.2f sec\n", duration_sec);

    if (has_audio && audio_sample_index > 0) {
        // Audio jitter stats
        long min_j = audio_jitter_us[0], max_j = audio_jitter_us[0], sum_j = 0;
        long min_buf = audio_buffer_frames[0], max_buf = audio_buffer_frames[0];
        for (size_t i = 0; i < audio_sample_index; i++) {
            sum_j += audio_jitter_us[i];
            if (audio_jitter_us[i] < min_j) min_j = audio_jitter_us[i];
            if (audio_jitter_us[i] > max_j) max_j = audio_jitter_us[i];
            if (audio_buffer_frames[i] < min_buf) min_buf = audio_buffer_frames[i];
            if (audio_buffer_frames[i] > max_buf) max_buf = audio_buffer_frames[i];
        }
        printf("Audio thread:  jitter min=%ld max=%ld avg=%.1f us\n",
               min_j, max_j, (double)sum_j / audio_sample_index);
        printf("Ring buffer:   min=%ld max=%ld frames\n", min_buf, max_buf);
        printf("Underruns: %d, Buffer stalls: %d\n", underrun_count, buffer_stall_count);
    }

    if (gpio_timing_index > 0) {
        long min_write = gpio_write_ns[0], max_write = gpio_write_ns[0], sum_write = 0;
        long min_jitter = gpio_jitter_ns[0], max_jitter = gpio_jitter_ns[0], sum_jitter = 0;

        for (size_t i = 0; i < gpio_timing_index; i++) {
            sum_write += gpio_write_ns[i];
            if (gpio_write_ns[i] < min_write) min_write = gpio_write_ns[i];
            if (gpio_write_ns[i] > max_write) max_write = gpio_write_ns[i];
            sum_jitter += gpio_jitter_ns[i];
            if (gpio_jitter_ns[i] < min_jitter) min_jitter = gpio_jitter_ns[i];
            if (gpio_jitter_ns[i] > max_jitter) max_jitter = gpio_jitter_ns[i];
        }

        printf("LED thread:    jitter min=%.1f max=%.1f avg=%.1f us\n",
               min_jitter / 1000.0, max_jitter / 1000.0, sum_jitter / 1000.0 / (double)gpio_timing_index);
        printf("GPIO write:    min=%.2f max=%.2f avg=%.2f us\n",
               min_write / 1000.0, max_write / 1000.0, sum_write / 1000.0 / (double)gpio_timing_index);
    }
}

#ifdef ENABLE_TRACE
static void make_log_filename(char *dst, size_t len,
                              const char *prefix, const char *song) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(dst, len, "%s_%s_%04d%02d%02d_%02d%02d%02d.txt",
             prefix, song,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}
#endif

void set_verbose_mode(int enabled) {
    verbose_mode = enabled;
}

void set_music_dir(const char *dir) {
    strncpy(music_base_dir, dir, MAX_PATH - 1);
    music_base_dir[MAX_PATH - 1] = '\0';
    // Ensure trailing slash
    size_t len = strlen(music_base_dir);
    if (len > 0 && len < MAX_PATH - 1 && music_base_dir[len - 1] != '/') {
        music_base_dir[len] = '/';
        music_base_dir[len + 1] = '\0';
    }
}

/*** Re-prefill after underrun (streaming version) ***/
static void do_reprefill_streaming(int16_t *buffer)
{
    for (int r = 0; r < PREFILL_PERIODS; ++r) {
        int frames_read = audio_read(audio_stream, buffer, audio_period_frames);
        if (frames_read <= 0)
            break;

        snd_pcm_sframes_t w = snd_pcm_writei(pcm, buffer, frames_read);

        if (w < 0) {
            snd_pcm_prepare(pcm);
            r--;    // retry this prefill period
            continue;
        }
    }
}


// --------------------------------------------------------------
// Audio thread (streaming version)
// --------------------------------------------------------------
static void *audio_thread_fn(void *arg) {
    struct timespec next_time;
    clock_gettime(CLOCK_MONOTONIC, &next_time);
    struct timespec prev_wake_time = {0};

    const snd_pcm_sframes_t max_delay_frames =
        MAX_BUFFER_PERIODS * audio_period_frames;

    // Local buffer for reading from stream
    int16_t *local_buffer = malloc(audio_period_frames * 2 * sizeof(int16_t));
    if (!local_buffer) {
        syslog(LOG_ERR, "Failed to allocate audio buffer");
        return NULL;
    }

    while (!audio_finished(audio_stream) && audio_sample_index < MAX_RUNS && !stop_requested) {

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_time, NULL);

        // Check again after waking - signal may have arrived during sleep
        if (stop_requested) break;

        struct timespec start_time, end_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);

        long wake_us = 0;
        if (prev_wake_time.tv_sec != 0)
            wake_us = time_diff_us(prev_wake_time, start_time);
        prev_wake_time = start_time;

        long total_runtime_us = 0;

        snd_pcm_sframes_t delay = 0;
        if (snd_pcm_delay(pcm, &delay) < 0)
            delay = 0;

        // Record ring buffer fill level
        size_t ring_avail = audio_available(audio_stream);

        for (int i = 0; i < 3; ++i) {

            if (delay > max_delay_frames) {
                break;
            }

            // Check if enough data available
            size_t avail = audio_available(audio_stream);
            if (avail < audio_period_frames) {
                if (audio_finished(audio_stream))
                    break;
                buffer_stall_count++;
                continue;  // Wait for decoder to catch up
            }

            struct timespec call_start, call_end;
            clock_gettime(CLOCK_MONOTONIC, &call_start);

            // Read from stream
            int frames_read = audio_read(audio_stream, local_buffer, audio_period_frames);
            if (frames_read <= 0) {
                break;
            }

            snd_pcm_sframes_t written = snd_pcm_writei(pcm, local_buffer, frames_read);
            if (written < 0) {
                underrun_count++;
                if (underrun_count <= 10 || underrun_count % 50 == 0)
                    syslog(LOG_WARNING, "Underrun #%d: %s",
                            underrun_count, snd_strerror(written));
                snd_pcm_prepare(pcm);

                do_reprefill_streaming(local_buffer);

                break;
            }

            clock_gettime(CLOCK_MONOTONIC, &call_end);
            total_runtime_us += time_diff_us(call_start, call_end);

            if (snd_pcm_delay(pcm, &delay) < 0)
                delay = 0;
        }

        clock_gettime(CLOCK_MONOTONIC, &end_time);
        long jitter = time_diff_us(next_time, start_time);
        if (jitter < 0)
            syslog(LOG_ERR, "Deadline miss at cycle %zu by %ld us\n",
                    audio_sample_index, -jitter);

        // Record all metrics
        audio_runtime_us[audio_sample_index] = total_runtime_us;
        audio_wake_interval_us[audio_sample_index] = wake_us;
        audio_jitter_us[audio_sample_index] = jitter;
        audio_buffer_frames[audio_sample_index] = (long)ring_avail;
        alsa_delay_frames[audio_sample_index] = (long)delay;

        if (verbose_mode && audio_sample_index % 100 == 0) {
            syslog(LOG_INFO, "[Cycle %zu] ALSA=%ld Ring=%zu jitter=%ld us",
                    audio_sample_index, delay, ring_avail, jitter);
        }

        audio_sample_index++;

        // Advance next_time by one audio period
        next_time.tv_nsec += AUDIO_THREAD_PERIOD_MS * 1000000;
        while (next_time.tv_nsec >= 1000000000) {
            next_time.tv_sec++;
            next_time.tv_nsec -= 1000000000;
        }
    }

    free(local_buffer);
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
    while (current_index < pattern_count && !stop_requested) {
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_time, NULL);

        // Check again after waking - signal may have arrived during sleep
        if (stop_requested) break;

        struct timespec tick_start, write_start, write_end;
        clock_gettime(CLOCK_MONOTONIC, &tick_start);

        // Record wake jitter (difference between scheduled and actual wake time)
        long jitter_ns = time_diff_ns(next_time, tick_start);

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

            // Store timing data (nanoseconds)
            if (gpio_timing_index < MAX_RUNS) {
                gpio_write_ns[gpio_timing_index] = time_diff_ns(write_start, write_end);
                gpio_jitter_ns[gpio_timing_index] = jitter_ns;
                gpio_timing_index++;
            }

            int duration = patterns[current_index].duration_ms;
            if (duration < 10) duration = 10;  // minimum 10ms (1 tick)
            duration = ((duration + 5) / 10) * 10;  // round to nearest 10ms
            ticks_for_current = duration / LED_THREAD_PERIOD_MS;
            tick_count = ticks_for_current;
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
// Find audio file (tries .mp3 first, then .wav)
// --------------------------------------------------------------
static int find_audio_file(char *out_path, size_t out_len, const char *base_name) {
    int n;

    // Try MP3 first
    n = snprintf(out_path, out_len, "%s%s.mp3", music_base_dir, base_name);
    if (n > 0 && (size_t)n < out_len && access(out_path, R_OK) == 0) {
        return 0;
    }

    // Fall back to WAV
    n = snprintf(out_path, out_len, "%s%s.wav", music_base_dir, base_name);
    if (n > 0 && (size_t)n < out_len && access(out_path, R_OK) == 0) {
        return 0;
    }

    return -1;  // Not found
}

// --------------------------------------------------------------
// Playback
// --------------------------------------------------------------
void play_song(const char *base_name) {
    char audio_file[MAX_PATH], pattern_file[MAX_PATH];
    int has_audio = 0;

    // Check for audio file (optional)
    if (find_audio_file(audio_file, sizeof(audio_file), base_name) == 0) {
        has_audio = 1;
    }

    int n = snprintf(pattern_file, sizeof(pattern_file), "%s%s.txt", music_base_dir, base_name);
    if (n < 0 || (size_t)n >= sizeof(pattern_file)) {
        fprintf(stderr, "Pattern file path too long\n");
        return;
    }

    // Pattern file is required
    if (access(pattern_file, R_OK) != 0) {
        fprintf(stderr, "Pattern file not found: %s\n", pattern_file);
        return;
    }

#ifdef ENABLE_TRACE
    char report_file[128];
    make_log_filename(report_file, sizeof(report_file), "playback_report", base_name);
#endif

    printf("\n=== Starting playback of '%s' ===\n", base_name);
    printf("Pattern file: %s\n", pattern_file);

    reset_runtime_state();
    load_patterns(pattern_file);

    printf("Loaded %d patterns\n", pattern_count);

    // Record start time (always needed for stats)
    clock_gettime(CLOCK_MONOTONIC, &playback_start_time);

    if (has_audio) {
        printf("Audio file: %s\n", audio_file);

        // Open audio stream (auto-detects format)
        audio_stream = audio_open(audio_file);
        if (!audio_stream) {
            fprintf(stderr, "Failed to open audio file, continuing with LED only\n");
            has_audio = 0;
        } else {
            printf("Format: %s, %u Hz, %u channels\n",
                   audio_stream->format == AUDIO_FORMAT_MP3 ? "MP3" : "WAV",
                   audio_stream->sample_rate,
                   audio_stream->channels);

            // Calculate period size based on sample rate (10ms worth of frames)
            audio_period_frames = (audio_stream->sample_rate * AUDIO_PERIOD_MS) / 1000;
            printf("Audio period: %zu frames (%d ms)\n", audio_period_frames, AUDIO_PERIOD_MS);

            setup_alsa(audio_stream->sample_rate, audio_stream->channels);

	    // initialize mixer on default card, "PCM" control
	    if (init_mixer("default", "PCM") == 0) {
		set_hw_volume(100);    // 100% system volume
	    }

            // Start decoder thread (for MP3) or prepare stream
            if (audio_start(audio_stream) < 0) {
                fprintf(stderr, "Failed to start audio stream, continuing with LED only\n");
                audio_close(audio_stream);
                audio_stream = NULL;
                has_audio = 0;
            }
        }
    } else {
        printf("No audio file found, playing LED pattern only\n");
    }

    pthread_t audio_thread, led_thread;

    struct sched_param audio_param = {.sched_priority = 75};
    struct sched_param led_param   = {.sched_priority = 80};

    pthread_attr_t led_attr;
    pthread_attr_init(&led_attr);
    pthread_attr_setinheritsched(&led_attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&led_attr, SCHED_FIFO);
    pthread_attr_setschedparam(&led_attr, &led_param);

    int rc = pthread_create(&led_thread, &led_attr, led_thread_fn, NULL);
    if (rc != 0) {
        fprintf(stderr, "Warning: Failed to create LED thread with SCHED_FIFO (rc=%d), trying default\n", rc);
        pthread_attr_init(&led_attr);
        pthread_create(&led_thread, &led_attr, led_thread_fn, NULL);
    }

    if (has_audio) {
        pthread_attr_t audio_attr;
        pthread_attr_init(&audio_attr);
        pthread_attr_setinheritsched(&audio_attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&audio_attr, SCHED_FIFO);
        pthread_attr_setschedparam(&audio_attr, &audio_param);

        rc = pthread_create(&audio_thread, &audio_attr, audio_thread_fn, NULL);
        if (rc != 0) {
            fprintf(stderr, "Warning: Failed to create audio thread with SCHED_FIFO (rc=%d), trying default\n", rc);
            pthread_attr_init(&audio_attr);
            pthread_create(&audio_thread, &audio_attr, audio_thread_fn, NULL);
        }
        pthread_join(audio_thread, NULL);
    }

    pthread_join(led_thread, NULL);

    gpio_all_off(led_lines, 8);

    if (has_audio) {
        alsa_close();
    }

    // Record end time
    clock_gettime(CLOCK_MONOTONIC, &playback_end_time);

    // Calculate playback duration
    double duration_sec = (playback_end_time.tv_sec - playback_start_time.tv_sec) +
                          (playback_end_time.tv_nsec - playback_start_time.tv_nsec) / 1e9;

    // Print stats summary if verbose mode (-v flag)
    print_stats(has_audio, duration_sec);

#ifdef ENABLE_TRACE
    // Save full CSV report when compiled with ENABLE_TRACE=1
    {
        PlaybackStats stats = {0};

        // Audio stats
        if (has_audio) {
            stats.audio_runtime_us = audio_runtime_us;
            stats.audio_jitter_us = audio_jitter_us;
            stats.audio_wake_interval_us = audio_wake_interval_us;
            stats.audio_buffer_frames = audio_buffer_frames;
            stats.alsa_delay_frames = alsa_delay_frames;
            stats.audio_samples = audio_sample_index;
            stats.underrun_count = underrun_count;
            stats.buffer_stall_count = buffer_stall_count;
            stats.audio_format = audio_stream->format == AUDIO_FORMAT_MP3 ? "MP3" : "WAV";
            stats.sample_rate = audio_stream->sample_rate;
            stats.channels = audio_stream->channels;
        } else {
            stats.audio_format = "NONE";
            stats.sample_rate = 0;
            stats.channels = 0;
        }

        // GPIO stats
        stats.gpio_write_ns = gpio_write_ns;
        stats.gpio_jitter_ns = gpio_jitter_ns;
        stats.gpio_samples = gpio_timing_index;

        // General info
        stats.pattern_count = pattern_count;
        stats.playback_duration_sec = duration_sec;

        save_playback_report(report_file, &stats);
    }
#endif

    if (has_audio) {
        audio_close(audio_stream);
        audio_stream = NULL;
    }

    printf("Playback finished for '%s'.\n", base_name);
}
