#include "media/wav_reader.h"

#include <string.h>

namespace {

static bool read_exact(FILE *file, void *dst, const size_t bytes)
{
    return fread(dst, 1, bytes, file) == bytes;
}

static uint32_t le32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

} // namespace

extern "C" bool WAV_ReadHeader(FILE *file, WavInfo *out_info)
{
    if (file == nullptr || out_info == nullptr) {
        return false;
    }
    memset(out_info, 0, sizeof(*out_info));

    if (fseek(file, 0, SEEK_SET) != 0) {
        return false;
    }

    uint8_t riff[12];
    if (!read_exact(file, riff, sizeof(riff)) ||
        memcmp(riff, "RIFF", 4) != 0 ||
        memcmp(riff + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool have_fmt = false;
    // Walk chunks: "fmt " must appear before "data" (guaranteed by the spec).
    while (true) {
        uint8_t header[8];
        if (!read_exact(file, header, sizeof(header))) {
            return false;
        }
        const uint32_t chunk_size = le32(header + 4);

        if (memcmp(header, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (chunk_size < sizeof(fmt) || !read_exact(file, fmt, sizeof(fmt))) {
                return false;
            }
            const uint16_t format_tag = le16(fmt + 0);
            out_info->channels = le16(fmt + 2);
            out_info->sample_rate_hz = le32(fmt + 4);
            out_info->bits_per_sample = le16(fmt + 14);
            if (format_tag != 1u) { // uncompressed PCM only
                return false;
            }
            // Skip any fmt extension bytes (and the pad byte on odd sizes).
            const long skip = static_cast<long>(chunk_size - sizeof(fmt)) + (chunk_size & 1u);
            if (skip > 0 && fseek(file, skip, SEEK_CUR) != 0) {
                return false;
            }
            have_fmt = true;
            continue;
        }

        if (memcmp(header, "data", 4) == 0) {
            if (!have_fmt) {
                return false;
            }
            out_info->data_bytes = chunk_size;
            out_info->data_offset = ftell(file);
            return out_info->data_offset >= 0;
        }

        // Unknown chunk (LIST, fact, id3 ...): skip data + odd-size pad byte.
        const long skip = static_cast<long>(chunk_size) + (chunk_size & 1u);
        if (fseek(file, skip, SEEK_CUR) != 0) {
            return false;
        }
    }
}
