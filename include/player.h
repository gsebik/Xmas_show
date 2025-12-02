#ifndef PLAYER_H
#define PLAYER_H

#include <stdint.h>
#include <signal.h>

// Global stop flag - set by signal handler in main.c
extern volatile sig_atomic_t stop_requested;

void play_song(const char *base_name);
void reset_runtime_state(void);
void set_verbose_mode(int enabled);
void set_music_dir(const char *dir);

#endif
