#ifndef SRC_MEDIA_MEDIA_DECODER_H
#define SRC_MEDIA_MEDIA_DECODER_H

// File-to-PCM decoder for the media player (docs/architecture.md 3.3),
// built on esp_audio_codec's simple-decoder API: it parses the container /
// frame sync itself, so this wrapper just streams raw file bytes in and
// hands decoded PCM out. Format is detected from the file header (falling
// back to the extension for headerless MP3), covering WAV, MP3, FLAC, M4A
// and AAC today; APE arrives later as a custom decoder plugin.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MediaDecoder MediaDecoder;

typedef struct {
    uint32_t sample_rate_hz;
    uint8_t bits_per_sample;
    uint8_t channels;
} MediaDecoderInfo;

// Open `path` and detect its format. `stop_requested` may be NULL; when
// supplied, blocking prebuffer/read loops return promptly after it becomes
// true. Returns NULL on unsupported/missing.
MediaDecoder *MEDIA_DECODER_Open(const char *path,
                                 const volatile bool *stop_requested);

// Decode the next chunk. On success *pcm_out/*bytes_out reference an
// internal buffer valid until the next call. Returns:
//   1  frame produced (format info valid in MEDIA_DECODER_GetInfo)
//   0  end of stream
//  -1  decode error
int MEDIA_DECODER_Decode(MediaDecoder *decoder, const uint8_t **pcm_out, size_t *bytes_out);

// Format info; valid after the first successful Decode.
bool MEDIA_DECODER_GetInfo(MediaDecoder *decoder, MediaDecoderInfo *out_info);

void MEDIA_DECODER_Close(MediaDecoder *decoder);

#ifdef __cplusplus
}
#endif

#endif // SRC_MEDIA_MEDIA_DECODER_H
