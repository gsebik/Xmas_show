#ifndef UDP_H
#define UDP_H

#include <stddef.h>

#define MAX_SONG_NAME 64
#define UDP_PORT 5005

int receive_udp_song(char *song_out, size_t len);
void emulate_udp_from_file(const char *filename);

#endif
