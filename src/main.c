#include "player.h"
#include "gpio.h"
#include "udp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#define MAX_SONG_NAME 64



int main(int argc, char *argv[]) {

    openlog("sequencer", LOG_PID | LOG_CONS, LOG_USER);

    printf("Initializing GPIO...\n");
    gpio_init();
    gpio_set_outputs(led_lines, 8);
    gpio_all_off(led_lines, 8);

    if (argc > 1) {
    // Parameter mode: just play the given song
    	play_song(argv[1]);
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

    gpio_cleanup();
    printf("GPIO cleaned up. Goodbye.\n");

    closelog();

    return 0;
}
