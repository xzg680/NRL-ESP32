#include "media/media_decoder.h"

#include <esp_audio_dec_default.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>

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
    FILE *file;                      // local file source, or...
    esp_http_client_handle_t http;   // ...HTTP(S) stream source (net radio)
    esp_audio_simple_dec_handle_t handle;
    uint8_t *raw_buffer;
    uint8_t *out_buffer;
    size_t out_capacity;
    size_t raw_fill;      // valid bytes in raw_buffer
    size_t raw_offset;    // consumed bytes in raw_buffer
    bool eof;
    bool info_valid;
    MediaDecoderInfo info;
    // Sniffed leading bytes for format detection on non-seekable HTTP
    // streams; replayed ahead of further source reads.
    uint8_t sniff[16];
    size_t sniff_len;
    size_t sniff_off;
};

namespace {

// Read from whichever source backs the decoder, serving sniffed bytes first.
static size_t read_source(MediaDecoder *d, uint8_t *dst, const size_t size)
{
    size_t total = 0;
    while (total < size && d->sniff_off < d->sniff_len) {
        dst[total++] = d->sniff[d->sniff_off++];
    }
    if (total == size) {
        return total;
    }
    if (d->file != nullptr) {
        const int64_t t0 = esp_timer_get_time();
        const size_t got = fread(dst + total, 1, size - total, d->file);
        const int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
        if (dt_ms > 2000) {
            ESP_LOGW(TAG, "slow source read: %u bytes in %lld ms",
                     static_cast<unsigned>(got), static_cast<long long>(dt_ms));
        }
        return total + got;
    }
    if (d->http != nullptr) {
        const int got = esp_http_client_read(d->http, reinterpret_cast<char *>(dst) + total,
                                             static_cast<int>(size - total));
        return total + ((got > 0) ? static_cast<size_t>(got) : 0u);
    }
    return total;
}

static esp_audio_simple_dec_type_t detect_type_from_header(const uint8_t *header,
                                                           const size_t got,
                                                           const char *path)
{
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

    // Fall back to the extension / URL suffix (e.g. MP3s that open with
    // junk bytes, or typeless radio streams).
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
        // Two registries: the simple-dec default only adds the container
        // parsers (WAV/M4A/TS/OGG); they dispatch into the elementary-stream
        // decoder registry, which must be populated separately -- without it
        // every MP3/FLAC open fails with "Decoder ... not registered".
        // Register only the codecs the playlist accepts (wav/mp3/flac/m4a/aac,
        // see music_playlist.cpp): esp_audio_dec_register_default() would link
        // every codec lib (Opus/Vorbis/LC3/SBC/...) and overflow the app
        // partition.
        const bool ok = esp_mp3_dec_register() == ESP_AUDIO_ERR_OK &&
                        esp_aac_dec_register() == ESP_AUDIO_ERR_OK &&
                        esp_flac_dec_register() == ESP_AUDIO_ERR_OK &&
                        esp_pcm_dec_register() == ESP_AUDIO_ERR_OK &&
                        esp_adpcm_dec_register() == ESP_AUDIO_ERR_OK &&
                        esp_audio_simple_dec_register_default() == ESP_AUDIO_ERR_OK;
        if (!ok) {
            ESP_LOGE(TAG, "register default decoders failed");
            return nullptr;
        }
        s_default_registered = true;
    }

    const bool is_http = strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0;

    MediaDecoder *d = static_cast<MediaDecoder *>(heap_caps_calloc(1, sizeof(MediaDecoder), MALLOC_CAP_SPIRAM));
    if (d == nullptr) {
        return nullptr;
    }

    if (is_http) {
        // Net-radio stream: open the connection, then sniff the first bytes
        // for format detection (the stream is not seekable, so the sniffed
        // bytes are replayed ahead of the decoder's reads).
        esp_http_client_config_t http_cfg = {};
        http_cfg.url = path;
        http_cfg.timeout_ms = 10000;
        http_cfg.buffer_size = 4096;
        http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
        d->http = esp_http_client_init(&http_cfg);
        if (d->http == nullptr ||
            esp_http_client_open(d->http, 0) != ESP_OK) {
            ESP_LOGE(TAG, "stream open failed: %s", path);
            MEDIA_DECODER_Close(d);
            return nullptr;
        }
        (void)esp_http_client_fetch_headers(d->http);
        const int status = esp_http_client_get_status_code(d->http);
        if (status != 200) {
            ESP_LOGE(TAG, "stream HTTP %d: %s", status, path);
            MEDIA_DECODER_Close(d);
            return nullptr;
        }
        int sniffed = 0;
        while (sniffed < static_cast<int>(sizeof(d->sniff))) {
            const int got = esp_http_client_read(d->http,
                                                 reinterpret_cast<char *>(d->sniff) + sniffed,
                                                 static_cast<int>(sizeof(d->sniff)) - sniffed);
            if (got <= 0) {
                break;
            }
            sniffed += got;
        }
        d->sniff_len = static_cast<size_t>(sniffed);
    } else {
        d->file = fopen(path, "rb");
        if (d->file == nullptr) {
            ESP_LOGE(TAG, "open failed: %s", path);
            MEDIA_DECODER_Close(d);
            return nullptr;
        }
        d->sniff_len = fread(d->sniff, 1, sizeof(d->sniff), d->file);
        (void)fseek(d->file, 0, SEEK_SET);
        d->sniff_off = d->sniff_len; // file rewound: nothing to replay
    }

    esp_audio_simple_dec_type_t type = detect_type_from_header(d->sniff, d->sniff_len, path);
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE && is_http) {
        // Typeless radio URL: MP3 is the de-facto webradio default.
        type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        ESP_LOGE(TAG, "unsupported format: %s", path);
        MEDIA_DECODER_Close(d);
        return nullptr;
    }

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

    ESP_LOGI(TAG, "decoding %s as %s%s", path, esp_audio_simple_dec_get_name(type),
             is_http ? " (stream)" : "");
    return d;
}

extern "C" int MEDIA_DECODER_Decode(MediaDecoder *d, const uint8_t **pcm_out, size_t *bytes_out)
{
    if (d == nullptr || d->handle == nullptr || pcm_out == nullptr || bytes_out == nullptr) {
        return -1;
    }

    // Progress diagnostics: a healthy call returns within a few reads. If we
    // are still in here after seconds, report where the time is going (spin
    // in the parser vs. blocked in the source read) instead of hanging mute.
    const int64_t enter_us = esp_timer_get_time();
    int64_t last_report_us = enter_us;
    unsigned iterations = 0;

    while (true) {
        ++iterations;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_report_us > 5000000LL) {
            last_report_us = now_us;
            ESP_LOGW(TAG, "decode stalled %llds: %u loops, raw fill=%u off=%u eof=%d",
                     static_cast<long long>((now_us - enter_us) / 1000000LL),
                     iterations,
                     static_cast<unsigned>(d->raw_fill),
                     static_cast<unsigned>(d->raw_offset),
                     d->eof ? 1 : 0);
        }

        // Refill the raw buffer when the parser consumed everything.
        if (d->raw_offset >= d->raw_fill) {
            if (d->eof) {
                return 0;
            }
            d->raw_fill = read_source(d, d->raw_buffer, kRawBufferBytes);
            d->raw_offset = 0;
            if (d->raw_fill == 0u) {
                d->eof = true;
                return 0;
            }
            // Live HTTP streams legitimately return short reads; only local
            // files treat one as end-of-stream (FLAC needs the eos flag to
            // flush its tail).
            if (d->file != nullptr && d->raw_fill < kRawBufferBytes) {
                d->eof = true;
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
            const size_t before = d->out_capacity;
            if (!grow_out_buffer(d, frame.needed_size)) {
                ESP_LOGE(TAG, "out buffer grow to %lu failed",
                         static_cast<unsigned long>(frame.needed_size));
                return -1;
            }
            if (d->out_capacity == before) {
                // Decoder demands a buffer we already have: bail rather than
                // spin forever on the same error.
                ESP_LOGE(TAG, "decoder wants %lu bytes but %lu available",
                         static_cast<unsigned long>(frame.needed_size),
                         static_cast<unsigned long>(d->out_capacity));
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
            const size_t extra = d->eof ? 0u : read_source(d, d->raw_buffer + remain, kRawBufferBytes - remain);
            if (extra == 0u) {
                if (d->eof) {
                    return 0;
                }
                d->eof = true;
            } else if (d->file != nullptr && remain + extra < kRawBufferBytes) {
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
    if (d->http != nullptr) {
        (void)esp_http_client_close(d->http);
        (void)esp_http_client_cleanup(d->http);
    }
    if (d->raw_buffer != nullptr) {
        heap_caps_free(d->raw_buffer);
    }
    if (d->out_buffer != nullptr) {
        heap_caps_free(d->out_buffer);
    }
    heap_caps_free(d);
}
