#include "media/media_decoder.h"

#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "MDEC";

namespace {

constexpr size_t kRawBufferBytes = 8 * 1024;
// FLAC blocks can decode to ~64 KB of PCM at hi-res; start smaller and grow
// on ESP_AUDIO_ERR_BUFF_NOT_ENOUGH as the decoder reports its needed size.
constexpr size_t kInitialOutBufferBytes = 32 * 1024;

static bool s_default_registered = false;

} // namespace

struct MediaDecoder {
    FILE *file;
    esp_audio_simple_dec_handle_t handle;
    uint8_t *raw_buffer;
    uint8_t *out_buffer;
    size_t out_capacity;
    size_t raw_fill;      // valid bytes in raw_buffer
    size_t raw_offset;    // consumed bytes in raw_buffer
    bool eof;
    bool info_valid;
    MediaDecoderInfo info;
};

namespace {

static esp_audio_simple_dec_type_t detect_type(FILE *file, const char *path)
{
    uint8_t header[12] = {};
    const size_t got = fread(header, 1, sizeof(header), file);
    (void)fseek(file, 0, SEEK_SET);

    if (got >= 12u && memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVE", 4) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    }
    if (got >= 4u && memcmp(header, "fLaC", 4) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    }
    if (got >= 8u && memcmp(header + 4, "ftyp", 4) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    }
    if (got >= 3u && memcmp(header, "ID3", 3) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (got >= 2u && header[0] == 0xFFu && (header[1] & 0xF6u) == 0xF0u) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC; // ADTS sync
    }
    if (got >= 2u && header[0] == 0xFFu && (header[1] & 0xE0u) == 0xE0u) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3; // bare MPEG frame sync
    }

    // Fall back to the extension (e.g. MP3s that open with junk bytes).
    const char *dot = strrchr(path, '.');
    if (dot != nullptr) {
        if (strcasecmp(dot, ".mp3") == 0) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        }
        if (strcasecmp(dot, ".flac") == 0) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
        }
        if (strcasecmp(dot, ".wav") == 0) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
        }
        if (strcasecmp(dot, ".m4a") == 0) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
        }
        if (strcasecmp(dot, ".aac") == 0) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        }
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

static bool grow_out_buffer(MediaDecoder *d, const size_t needed)
{
    size_t new_capacity = d->out_capacity;
    while (new_capacity < needed) {
        new_capacity *= 2u;
    }
    uint8_t *grown = static_cast<uint8_t *>(heap_caps_realloc(d->out_buffer, new_capacity, MALLOC_CAP_SPIRAM));
    if (grown == nullptr) {
        return false;
    }
    d->out_buffer = grown;
    d->out_capacity = new_capacity;
    return true;
}

} // namespace

extern "C" MediaDecoder *MEDIA_DECODER_Open(const char *path)
{
    if (path == nullptr) {
        return nullptr;
    }

    if (!s_default_registered) {
        if (esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "register default decoders failed");
            return nullptr;
        }
        s_default_registered = true;
    }

    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "open failed: %s", path);
        return nullptr;
    }

    const esp_audio_simple_dec_type_t type = detect_type(file, path);
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        ESP_LOGE(TAG, "unsupported format: %s", path);
        fclose(file);
        return nullptr;
    }

    MediaDecoder *d = static_cast<MediaDecoder *>(heap_caps_calloc(1, sizeof(MediaDecoder), MALLOC_CAP_SPIRAM));
    if (d == nullptr) {
        fclose(file);
        return nullptr;
    }
    d->file = file;
    d->raw_buffer = static_cast<uint8_t *>(heap_caps_malloc(kRawBufferBytes, MALLOC_CAP_SPIRAM));
    d->out_buffer = static_cast<uint8_t *>(heap_caps_malloc(kInitialOutBufferBytes, MALLOC_CAP_SPIRAM));
    d->out_capacity = kInitialOutBufferBytes;

    esp_audio_simple_dec_cfg_t cfg = {};
    cfg.dec_type = type;
    if (d->raw_buffer == nullptr || d->out_buffer == nullptr ||
        esp_audio_simple_dec_open(&cfg, &d->handle) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "decoder open failed: %s (%s)", path, esp_audio_simple_dec_get_name(type));
        MEDIA_DECODER_Close(d);
        return nullptr;
    }

    ESP_LOGI(TAG, "decoding %s as %s", path, esp_audio_simple_dec_get_name(type));
    return d;
}

extern "C" int MEDIA_DECODER_Decode(MediaDecoder *d, const uint8_t **pcm_out, size_t *bytes_out)
{
    if (d == nullptr || d->handle == nullptr || pcm_out == nullptr || bytes_out == nullptr) {
        return -1;
    }

    while (true) {
        // Refill the raw buffer when the parser consumed everything.
        if (d->raw_offset >= d->raw_fill) {
            if (d->eof) {
                return 0;
            }
            d->raw_fill = fread(d->raw_buffer, 1, kRawBufferBytes, d->file);
            d->raw_offset = 0;
            if (d->raw_fill == 0u) {
                d->eof = true;
                return 0;
            }
            if (d->raw_fill < kRawBufferBytes) {
                d->eof = true; // last chunk; flag eos so FLAC flushes its tail
            }
        }

        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = d->raw_buffer + d->raw_offset;
        raw.len = static_cast<uint32_t>(d->raw_fill - d->raw_offset);
        raw.eos = d->eof;

        esp_audio_simple_dec_out_t frame = {};
        frame.buffer = d->out_buffer;
        frame.len = static_cast<uint32_t>(d->out_capacity);

        const esp_audio_err_t err = esp_audio_simple_dec_process(d->handle, &raw, &frame);
        if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (!grow_out_buffer(d, frame.needed_size)) {
                ESP_LOGE(TAG, "out buffer grow to %lu failed",
                         static_cast<unsigned long>(frame.needed_size));
                return -1;
            }
            continue;
        }
        if (err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "decode error %d", static_cast<int>(err));
            return -1;
        }

        d->raw_offset += raw.consumed;

        if (frame.decoded_size > 0u) {
            if (!d->info_valid) {
                esp_audio_simple_dec_info_t info = {};
                if (esp_audio_simple_dec_get_info(d->handle, &info) == ESP_AUDIO_ERR_OK) {
                    d->info.sample_rate_hz = info.sample_rate;
                    d->info.bits_per_sample = info.bits_per_sample;
                    d->info.channels = info.channel;
                    d->info_valid = true;
                }
            }
            *pcm_out = d->out_buffer;
            *bytes_out = frame.decoded_size;
            return 1;
        }

        // No output and nothing consumed: parser wants a bigger contiguous
        // chunk than remains; compact by falling through to refill.
        if (raw.consumed == 0u && d->raw_offset < d->raw_fill) {
            const size_t remain = d->raw_fill - d->raw_offset;
            memmove(d->raw_buffer, d->raw_buffer + d->raw_offset, remain);
            const size_t extra = d->eof ? 0u : fread(d->raw_buffer + remain, 1, kRawBufferBytes - remain, d->file);
            if (extra == 0u) {
                if (d->eof) {
                    return 0;
                }
                d->eof = true;
            } else if (remain + extra < kRawBufferBytes) {
                d->eof = true;
            }
            d->raw_fill = remain + extra;
            d->raw_offset = 0;
        }
    }
}

extern "C" bool MEDIA_DECODER_GetInfo(MediaDecoder *d, MediaDecoderInfo *out_info)
{
    if (d == nullptr || !d->info_valid || out_info == nullptr) {
        return false;
    }
    *out_info = d->info;
    return true;
}

extern "C" void MEDIA_DECODER_Close(MediaDecoder *d)
{
    if (d == nullptr) {
        return;
    }
    if (d->handle != nullptr) {
        esp_audio_simple_dec_close(d->handle);
    }
    if (d->file != nullptr) {
        fclose(d->file);
    }
    if (d->raw_buffer != nullptr) {
        heap_caps_free(d->raw_buffer);
    }
    if (d->out_buffer != nullptr) {
        heap_caps_free(d->out_buffer);
    }
    heap_caps_free(d);
}
