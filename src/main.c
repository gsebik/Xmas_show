/**
 * V43 Christmas Lights Sequencer
 *
 * Real-time audio playback with synchronized LED control.
 * Supports MP3 (streaming) and WAV (mmap) formats.
 * Dynamic sample rate support (32kHz, 44.1kHz, 48kHz).
 *
 * Architecture:
 * =============
 *
 *     [Main Thread]
 *          |
 *          | spawns
 *          v
 *                     +-------------------+
 *                     |  Decoder Thread   | (MP3 only, normal priority)
 *                     |  - mpg123_read()  |
 *                     |  - fills ring buf |
 *                     +---------+---------+
 *                               | writes
 *                               v
 *                     +-------------------+
 *                     |    Ring Buffer    | (~3 sec at 48kHz stereo)
 *                     +---------+---------+
 *                               | reads
 *                               v
 * +-------------------+   +-------------------+
 * |    LED Thread     |   |   Audio Thread    | (SCHED_FIFO, prio 75)
 * | SCHED_FIFO prio80 |   |  - audio_read()   |
 * | - 10ms tick rate  |   |  - ALSA writei()  |
 * | - GPIO mmap write |   |  - 30ms period    |
 * | - checks stop_req |   |  - checks stop_req|
 * +-------------------+   +-------------------+
 *          |                       |
 *          v                       v
 *     [GPIO pins]            [ALSA/audio]
 *
 * Threading Model:
 * - LED thread:     SCHED_FIFO priority 80 (highest), 10ms period
 * - Audio thread:   SCHED_FIFO priority 75, 30ms period
 * - Decoder thread: Normal priority (MP3 only), runs ahead filling buffer
 *
 * Signal Handling:
 * - SIGTERM/SIGINT: Sets stop_requested flag, immediately turns off all LEDs
 * - Threads check stop_requested after each sleep and exit gracefully
 * - Main thread waits for threads to join, then cleans up GPIO
 *
 * For WAV files: mmap + mlock for hard real-time (no disk I/O during playback)
 * For MP3 files: Ring buffer with ~3 sec pre-buffer for soft real-time
 *
 * Capabilities required (non-root execution):
 * - cap_sys_rawio:  GPIO memory mapping (/dev/gpiomem access)
 * - cap_sys_nice:   SCHED_FIFO real-time scheduling
 */

#include "player.h"
#include "gpio.h"
#include "udp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <getopt.h>
#include <sys/mman.h>

#define MAX_SONG_NAME 64


volatile sig_atomic_t stop_requested = 0;

void signal_handler(int sig)
{
    switch(sig)
    {
        case SIGINT:
        case SIGTERM:
            // Set flag for threads to check - they will exit their loops
            stop_requested = 1;
            // Turn off LEDs immediately (signal-safe GPIO write)
            gpio_all_off(led_lines, 8);
            break;

        //case SIGCHLD:
        //case SIGTSTP:
        case SIGTTOU:
        case SIGTTIN:
        // ignore it
        break;
        default:
            break;
    }
}


void print_usage(const char *prog) {
    printf("Usage: %s [-v] [-m musicdir] [-s on|off] [songname]\n", prog);
    printf("  -v              Verbose mode (print GPIO timing stats)\n");
    printf("  -m musicdir     Music directory (default: /home/linux/music/)\n");
    printf("  -s on|off       Turn all LEDs on or off and exit\n");
    printf("  songname        Play song directly (without .wav/.txt extension)\n");
    printf("  No args         Interactive menu mode\n");
}

// Turn all LEDs on
static void gpio_all_on(const unsigned int *lines, int count) {
    if (!gpio || gpio == MAP_FAILED)
        return;
    volatile uint32_t *GPSET0 = gpio + 0x1C / 4;
    uint32_t mask = 0;
    for (int i = 0; i < count; ++i)
        mask |= (1u << lines[i]);
    *GPSET0 = mask;
    __sync_synchronize();
}

int main(int argc, char *argv[]) {

    openlog("sequencer", LOG_PID | LOG_CONS, LOG_USER);

    // Parse command line options
    int opt;
    char *switch_mode = NULL;  // "on" or "off"
    while ((opt = getopt(argc, argv, "vm:s:h")) != -1) {
        switch (opt) {
            case 'v':
                set_verbose_mode(1);
                break;
            case 'm':
                set_music_dir(optarg);
                break;
            case 's':
                switch_mode = optarg;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    printf("Initializing GPIO...\n");
    gpio_init();
    gpio_set_outputs(led_lines, 8);

    // Handle -s on/off switch mode
    if (switch_mode != NULL) {
        if (strcmp(switch_mode, "on") == 0) {
            printf("Turning all LEDs ON\n");
            gpio_all_on(led_lines, 8);
        } else if (strcmp(switch_mode, "off") == 0) {
            printf("Turning all LEDs OFF\n");
            gpio_all_off(led_lines, 8);
        } else {
            fprintf(stderr, "Invalid switch mode: %s (use 'on' or 'off')\n", switch_mode);
            gpio_cleanup();
            return 1;
        }
        gpio_cleanup();
        closelog();
        return 0;
    }

    gpio_all_off(led_lines, 8);

    // add signal handlers
    if (signal(SIGTTOU, signal_handler) == SIG_ERR) { exit(EXIT_FAILURE); }
    if (signal(SIGTTIN, signal_handler) == SIG_ERR) { exit(EXIT_FAILURE); }
    if (signal(SIGHUP,  signal_handler) == SIG_ERR) { exit(EXIT_FAILURE); }
    if (signal(SIGTERM, signal_handler) == SIG_ERR) { exit(EXIT_FAILURE); }
    if (signal(SIGINT,  signal_handler) == SIG_ERR) { exit(EXIT_FAILURE); }

    if (optind < argc) {
    // Parameter mode: just play the given song
    	play_song(argv[optind]);
    }
    else {
    // No parameter -> full menu mode

	    char choice[8];

	    while (1) {
		printf("\n=== LED + Music Sequencer ===\n");
		printf("1) Play song manually\n");
		printf("2) Receive song name via UDP JSON\n");
		printf("3) Exit\n> ");
		  printf("4) Emulate UDP from file\n");
		fflush(stdout);

		if (!fgets(choice, sizeof(choice), stdin))
		    break;
		int ch = atoi(choice);

		if (ch == 1) {
		    char base[MAX_SONG_NAME];
		    printf("Enter song base name (without .wav/.txt): ");
		    fflush(stdout);
		    if (!fgets(base, sizeof(base), stdin))
			continue;
		    base[strcspn(base, "\n")] = 0;
		    if (base[0] == '\0') {
			printf("Empty name, returning to menu.\n");
			continue;
		    }
		    play_song(base);

		} else if (ch == 2) {
		    char base[MAX_SONG_NAME];
		    if (receive_udp_song(base, sizeof(base)) == 0) {
			printf("UDP provided song: '%s'\n", base);
			printf("Play this song? (y/n): ");
			fflush(stdout);
			char ans[8];
			if (fgets(ans, sizeof(ans), stdin)) {
			    if (ans[0] == 'y' || ans[0] == 'Y')
				play_song(base);
			    else
				printf("Canceled, returning to menu.\n");
			}
		    } else {
			printf("No valid UDP song received (timeout or error).\n");
		    }

		} else if (ch == 3) {
		    printf("Exiting program.\n");
		    break;
		  } else if (ch == 4) {
			emulate_udp_from_file("udp_emulation.json");
		} else {
		    printf("Invalid choice. Try again.\n");
		}
	    }

    }

    // Ensure all LEDs are off before cleanup
    gpio_all_off(led_lines, 8);

    gpio_cleanup();
    printf("GPIO cleaned up. Goodbye.\n");

    closelog();

    return 0;
}
