#pragma once

#include <stddef.h>
#include <stdint.h>

typedef void (*DtmfDigitCallback)(char digit, void *context);

class DtmfDecoder {
public:
    DtmfDecoder();
    void reset();
    void feed(const int16_t *samples, size_t count, DtmfDigitCallback callback, void *context);

private:
    int16_t frame_[320]; // 20 ms at 16 kHz
    size_t fill_;
    char candidate_;
    char emitted_;
    uint8_t stable_frames_;
    char decodeFrame() const;
};

bool DTMF_IsValid(const char *digits);
bool DTMF_Frequencies(char digit, uint16_t *low_hz, uint16_t *high_hz);

