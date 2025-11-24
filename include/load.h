#ifndef LOAD_H
#define LOAD_H

#include <stdint.h>
#include <stddef.h>

#define MAX_PATTERNS 2048

typedef struct {
	int duration_ms;
	uint8_t pattern;
} Pattern;

extern Pattern patterns[MAX_PATTERNS];
extern int pattern_count;

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    size_t frames;

    int16_t *pcm;         // PCM data pointer inside mmap
    void *mapping;        // base of mmap() region
    size_t mapping_size;  // total mapped file size
} WavData;

WavData load_wav_mmap(const char *filename);
void free_wav_mmap(WavData *wav);

void load_patterns(const char *filename);

#endif
