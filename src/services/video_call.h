#ifndef SRC_SERVICES_VIDEO_CALL_H
#define SRC_SERVICES_VIDEO_CALL_H

// Video call over the NRL protocol (docs/architecture.md 功能3): the DVP
// camera's native JPEG frames are fragmented into NRL packet type 13 and
// relayed by the server; received fragments reassemble into JPEG frames the
// LCD's video page decodes (reusing the album-art JPEG decoder). Audio rides
// the existing voice path unchanged.
//
// Payload layout (after the 48-byte NRL header):
//   [0..1] frame_seq (big-endian), [2] frag_idx, [3] frag_cnt, [4..] JPEG.
//
// S31-only (camera + LCD); stubs elsewhere.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Register the RX handler with the bridge. Call once at startup; reception
// is passive from then on (frames buffer whenever a peer transmits).
void VIDEO_Init(void);

// Camera transmission on/off (~5 fps VGA JPEG). Returns false when the
// camera fails to start (no sensor / wrong board).
bool VIDEO_SetTxEnabled(bool enabled);
bool VIDEO_TxEnabled(void);

// Latest completely received JPEG frame. Returns true when `*seq` differs
// from the caller's previous value; the data stays valid until the next
// VIDEO_AcquireFrame call. Pattern: acquire -> decode -> render.
bool VIDEO_AcquireFrame(const uint8_t **jpeg, size_t *jpeg_size, uint32_t *seq);

// True while frames arrived within the last couple of seconds.
bool VIDEO_Receiving(void);

#ifdef __cplusplus
}
#endif

#endif // SRC_SERVICES_VIDEO_CALL_H
