#ifndef SRC_MEDIA_WAV_READER_H
#define SRC_MEDIA_WAV_READER_H

// Minimal RIFF/WAVE parser for the media player: walks the chunk list of an
// already-opened file, validates the "fmt " chunk and positions the stream at
// the start of "data". Decoding is trivial (PCM passthrough); heavier formats
// (MP3/FLAC/APE) get their own decoder plugins per docs/architecture.md 3.3.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sample_rate_hz;
    uint16_t bits_per_sample;  // 16 or 24 (24 not yet playable, see music_player)
    uint16_t channels;         // 1 or 2
    uint32_t data_bytes;       // length of the PCM payload
    long data_offset;          // file offset of the first PCM byte
} WavInfo;

// Parse the header of `file` (any seek position; the function rewinds).
// On success the file is positioned at data_offset and *out_info is filled.
// Only uncompressed PCM (format tag 1) is accepted.
bool WAV_ReadHeader(FILE *file, WavInfo *out_info);

#ifdef __cplusplus
}
#endif

#endif // SRC_MEDIA_WAV_READER_H
