#include "load.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#pragma pack(push, 1)
typedef struct {
    char     riff_id[4];   // "RIFF"
    uint32_t riff_size;
    char     wave_id[4];   // "WAVE"
} RiffHeader;

typedef struct {
    char     chunk_id[4];  // e.g. "fmt " or "data"
    uint32_t chunk_size;
} ChunkHeader;

typedef struct {
    uint16_t audio_format;   // 1 = PCM
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} FmtChunk;
#pragma pack(pop)

Pattern patterns[MAX_PATTERNS];
int pattern_count = 0;

WavData load_wav_mmap(const char *filename)
{
	WavData out = {0};

	// --- open file ---
	int fd = open(filename, O_RDONLY);
	if (fd < 0) { perror("open WAV"); exit(1); }

	// --- get file size ---
	struct stat st;
	if (fstat(fd, &st) < 0) { perror("fstat"); exit(1); }
	size_t file_size = st.st_size;

	// --- mmap whole file ---
	void *mapping = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd),
		if (mapping == MAP_FAILED) { perror("mmap"); exit(1); }

	out.mapping = mapping;
	out.mapping_size = file_size;

	uint8_t *p = (uint8_t *)mapping;

	// --- parse RIFF header ---
	RiffHeader *riff = (RiffHeader *)p;
	if (memcmp(riff->riff_id, "RIFF", 4) != 0 ||
	    memcmp(riff->wave_id, "WAVE", 4) != 0) {
	    fprintf(stderr, "Not a RIFF/WAVE file\n");
	    exit(1);
	}

	p += sizeof(RiffHeader);

	FmtChunk fmt = {0};
	uint32_t data_size = 0;
	uint8_t *data_ptr = NULL;

	// --- walk chunks ---
	while (p < (uint8_t *)mapping + file_size) {
	    ChunkHeader *ch = (ChunkHeader *)p;

	    if (memcmp(ch->chunk_id, "fmt ", 4) == 0) {
                memcpy(&fmt, p + sizeof(ChunkHeader), sizeof(FmtChunk));

	    } else if (memcmp(ch->chunk_id, "data", 4) == 0) {
	        data_size = ch->chunk_size;
		data_ptr = p + sizeof(ChunkHeader);
		break;
	    }

	    p += sizeof(ChunkHeader) + ch->chunk_size;
	}

	if (!data_ptr) {
		fprintf(stderr, "No data chunk found\n");
		exit(1);
	}

	if (fmt.audio_format != 1 || fmt.bits_per_sample != 16) {
	    fprintf(stderr, "Unsupported WAV format (need PCM 16-bit)\n");
	    exit(1);
	}

        // --- fill output struct ---
	out.sample_rate = fmt.sample_rate;
	out.channels    = fmt.num_channels;
	out.frames      = data_size / (fmt.num_channels * 2);
	out.pcm         = (int16_t *)data_ptr;

	return out;
}

void free_wav_mmap(WavData *wav)
{
    if (wav->mapping) {
	    munmap(wav->mapping, wav->mapping_size);
    }
    memset(wav, 0, sizeof(*wav));
}

void load_patterns(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) { perror("pattern open"); exit(1); }

    char line[64];
    pattern_count = 0;

    while (fgets(line, sizeof(line), f)) {
        if (pattern_count >= MAX_PATTERNS) {
            fprintf(stderr, "Too many patterns!\n");
            break;
        }
        int dur; char bits[10];
        if (sscanf(line, "%d %9s", &dur, bits) == 2) {
            if (dur < 70) dur = 70;
            dur = ((dur + 5) / 10) * 10;
            uint8_t p = 0;
            for (int i = 0, j = 0; i < 8 && bits[j]; ++j) {
                if (bits[j] == '.') continue;
                p = (p << 1) | (bits[j] == '1' ? 1 : 0);
                ++i;
            }
            patterns[pattern_count++] = (Pattern){dur, p};
        }
    }
    fclose(f);
}
