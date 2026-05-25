#include "nrl_g711.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <stddef.h>
#include <stdint.h>

static const char *TAG = "G711";

namespace {

bool s_tables_ready = false;
int16_t s_decode_table[256];

// 64 KB encoder LUT keyed by the unsigned reinterpretation of the int16_t
// sample (0..0x7FFF = positive, 0x8000..0xFFFF = negative). Each entry is
// the algorithmic encoder's output for that sample, so EncodeALaw is a
// single table read with no branches, no clamping, no sign handling.
//
// Allocated from PSRAM (8 MB external) so it doesn't squeeze the internal
// SRAM heap that the BT controller / NimBLE host / WiFi / lwIP pools draw
// from. An earlier static-BSS placement in internal SRAM triggered
// bt_osi_mem_malloc OOM during NimBLE init.
//
// PSRAM access goes through the cache (~2 cycle hit, ~30 cycle miss). At
// 8 kHz audio, the per-sample cost is well under 0.01 % CPU even with a
// pessimistic miss rate.
uint8_t *s_encode_table = nullptr;

static uint8_t linearToALawSlow(const int16_t pcm)
{
    uint8_t sign = 0u;
    int16_t ix = 0;

    if (pcm < 0) {
        sign = 0x80u;
        ix = static_cast<int16_t>((~pcm) >> 4);
    } else {
        ix = static_cast<int16_t>(pcm >> 4);
    }

    if (ix > 15) {
        uint8_t iexp = 1u;
        while (ix > 31) {
            ix = static_cast<int16_t>(ix >> 1);
            ++iexp;
        }
        ix = static_cast<int16_t>(ix - 16);
        ix = static_cast<int16_t>(ix + static_cast<int16_t>(iexp << 4));
    }

    if (sign == 0u) {
        ix = static_cast<int16_t>(ix | 0x80);
    }

    return static_cast<uint8_t>(ix) ^ 0x55u;
}

static int16_t aLawToLinearSlow(const uint8_t alaw)
{
    const uint8_t code = static_cast<uint8_t>(alaw ^ 0x55u);
    const int16_t iexp = static_cast<int16_t>((code & 0x70u) >> 4);
    int16_t mant = static_cast<int16_t>(code & 0x0Fu);

    if (iexp > 0) {
        mant = static_cast<int16_t>(mant + 16);
    }
    mant = static_cast<int16_t>((mant << 4) + 0x08);

    if (iexp > 1) {
        mant = static_cast<int16_t>(mant << static_cast<uint8_t>(iexp - 1));
    }

    return ((code & 0x80u) != 0u) ? mant : static_cast<int16_t>(-mant);
}

} // namespace

extern "C" void NRL_G711_Init(void)
{
    if (s_tables_ready) {
        return;
    }

    for (size_t i = 0; i < 256u; ++i) {
        s_decode_table[i] = aLawToLinearSlow(static_cast<uint8_t>(i));
    }

    if (s_encode_table == nullptr) {
        s_encode_table = static_cast<uint8_t *>(
            heap_caps_malloc(65536, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (s_encode_table == nullptr) {
            // No PSRAM available -- fall back to algorithmic path at encode
            // time. We deliberately don't try an internal-SRAM fallback: a
            // 64 KB BSS slab there crashes NimBLE init on the gezipai build.
            ESP_LOGW(TAG, "PSRAM encode table alloc failed; using algorithm fallback");
        }
    }

    if (s_encode_table != nullptr) {
        // ~65 K calls to the algorithmic encoder, ~10-15 ops each = ~1 M ops,
        // ~4 ms at 240 MHz. Runs once at boot.
        for (uint32_t i = 0; i < 65536u; ++i) {
            const int16_t pcm = static_cast<int16_t>(i);
            s_encode_table[i] = linearToALawSlow(pcm);
        }
    }

    s_tables_ready = true;
}

extern "C" uint8_t NRL_G711_EncodeALaw(const int16_t pcm)
{
    if (!s_tables_ready) {
        NRL_G711_Init();
    }
    if (s_encode_table != nullptr) {
        return s_encode_table[static_cast<uint16_t>(pcm)];
    }
    return linearToALawSlow(pcm);
}

extern "C" int16_t NRL_G711_DecodeALaw(const uint8_t alaw)
{
    if (!s_tables_ready) {
        NRL_G711_Init();
    }
    return s_decode_table[alaw];
}
