#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// G.711 A-law codec used by the NRL audio uplink/downlink path. The encoder
// precomputes a 4 KB magnitude lookup table for 12-bit PCM and the decoder
// precomputes a 256-entry lookup table; both are populated by NRL_G711_Init().
//
// NRL_G711_Init() is idempotent. The encode/decode helpers re-check the
// initialization flag so callers that bypass explicit init still work — but
// the audio bridge calls NRL_G711_Init() at boot to keep the per-sample
// path branchless on the lookup tables themselves.

void NRL_G711_Init(void);

// Encode one 16-bit linear PCM sample to one A-law byte.
uint8_t NRL_G711_EncodeALaw(int16_t pcm);

// Decode one A-law byte to one 16-bit linear PCM sample.
int16_t NRL_G711_DecodeALaw(uint8_t alaw);

#ifdef __cplusplus
}
#endif
