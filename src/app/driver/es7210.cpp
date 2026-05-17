#include "driver/es7210.h"

#include "driver/board_pins.h"
#include "driver/i2c1.h"

#include <Arduino.h>

#if defined(NRL_HAS_ES7210) && NRL_HAS_ES7210

namespace {

// ---- I2C addressing ----------------------------------------------------
// ES7210 7-bit I2C address is selected by the AD0/AD1 strap pins:
//   AD1 AD0 = 00 -> 0x40, 01 -> 0x41, 10 -> 0x42, 11 -> 0x43
// We probe all four and keep whichever acknowledges, so the board strap
// does not have to be known in advance.
constexpr uint8_t kEs7210AddrCandidates[] = {0x40, 0x41, 0x42, 0x43};
uint8_t s_es7210_addr = 0x40;
bool s_es7210_ready = false;

// ---- ES7210 register map (subset, see ES7210 datasheet) ----------------
enum : uint8_t {
    ES7210_RESET_REG00        = 0x00,
    ES7210_CLOCK_OFF_REG01    = 0x01,
    ES7210_MAINCLK_REG02      = 0x02, // ADC clock: adc_div | doubler<<6 | dll_bypass<<7
    ES7210_MASTER_CLK_REG03   = 0x03,
    ES7210_LRCK_DIVH_REG04    = 0x04,
    ES7210_LRCK_DIVL_REG05    = 0x05,
    ES7210_POWER_DOWN_REG06   = 0x06,
    ES7210_OSR_REG07          = 0x07,
    ES7210_MODE_CONFIG_REG08  = 0x08,
    ES7210_TIME_CONTROL0_REG09 = 0x09,
    ES7210_TIME_CONTROL1_REG0A = 0x0A,
    ES7210_SDP_INTERFACE1_REG11 = 0x11, // serial data port format
    ES7210_SDP_INTERFACE2_REG12 = 0x12, // TDM control
    ES7210_ADC34_HPF2_REG20   = 0x20,
    ES7210_ADC34_HPF1_REG21   = 0x21,
    ES7210_ADC12_HPF2_REG22   = 0x22,
    ES7210_ADC12_HPF1_REG23   = 0x23,
    ES7210_ANALOG_REG40       = 0x40,
    ES7210_MIC12_BIAS_REG41   = 0x41,
    ES7210_MIC34_BIAS_REG42   = 0x42,
    ES7210_MIC1_GAIN_REG43    = 0x43,
    ES7210_MIC2_GAIN_REG44    = 0x44,
    ES7210_MIC3_GAIN_REG45    = 0x45,
    ES7210_MIC4_GAIN_REG46    = 0x46,
    ES7210_MIC1_POWER_REG47   = 0x47,
    ES7210_MIC2_POWER_REG48   = 0x48,
    ES7210_MIC3_POWER_REG49   = 0x49,
    ES7210_MIC4_POWER_REG4A   = 0x4A,
    ES7210_MIC12_POWER_REG4B  = 0x4B,
    ES7210_MIC34_POWER_REG4C  = 0x4C,
    ES7210_CHIP_ID1_REGFD     = 0xFD, // expect 0x72
    ES7210_CHIP_ID0_REGFE     = 0xFE, // expect 0x10
};

// ---- Analog mic PGA gain ----------------------------------------------
// REG43..46 low nibble = PGA gain step (0x00 = 0 dB ... 0x0E ~= 42 dB,
// roughly 3 dB/step). Bit4 (0x10) is set per the Espressif reference
// driver. The captured mic level is driven by this PGA gain.
constexpr uint8_t kEs7210MaxGainStep = 0x0E; // highest valid PGA step

// MIC bias for the analog microphones (REG41/REG42).
constexpr uint8_t kEs7210MicBias = 0x70;

// Microphone "volume" shared with the web/AT control (0-255). Default maps
// to PGA step ~0x0A (~30 dB), a sensible level for an electret/MEMS mic.
// ES7210_SetMicVolume() may overwrite this before or after init.
uint8_t s_mic_volume_cfg = 0xB0;

// Map the 0-255 mic volume onto an ES7210 PGA gain register value.
static uint8_t es7210_gain_reg_from_volume(uint8_t volume) {
    const uint8_t step = static_cast<uint8_t>(
        (static_cast<uint16_t>(volume) * kEs7210MaxGainStep + 127u) / 255u);
    return static_cast<uint8_t>((step & 0x0Fu) | 0x10u);
}

static bool es7210_write_reg(uint8_t reg, uint8_t value) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool ok = true;
        I2C_Start();
        if (I2C_Write((s_es7210_addr << 1) | I2C_WRITE) < 0) {
            ok = false;
        } else if (I2C_Write(reg) < 0) {
            ok = false;
        } else if (I2C_Write(value) < 0) {
            ok = false;
        }
        I2C_Stop();
        if (ok) {
            return true;
        }
        Serial.printf("[ES7210] I2C write failed: addr=0x%02X reg=0x%02X value=0x%02X attempt=%d\n",
                      static_cast<unsigned>(s_es7210_addr),
                      static_cast<unsigned>(reg),
                      static_cast<unsigned>(value),
                      attempt + 1);
        delay(1);
    }
    return false;
}

static bool es7210_read_reg(uint8_t reg, uint8_t *value) {
    if (value == nullptr) {
        return false;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool ok = true;
        I2C_Start();
        if (I2C_Write((s_es7210_addr << 1) | I2C_WRITE) < 0) {
            ok = false;
            goto stop_once;
        }
        if (I2C_Write(reg) < 0) {
            ok = false;
            goto stop_once;
        }
        I2C_Start();
        if (I2C_Write((s_es7210_addr << 1) | I2C_READ) < 0) {
            ok = false;
            goto stop_once;
        }
        *value = I2C_Read(true);
    stop_once:
        I2C_Stop();
        if (ok) {
            return true;
        }
        delay(1);
    }
    return false;
}

// Write the MIC1/MIC2 PGA gain registers from the current mic-volume setting.
static bool es7210_apply_mic_gain(void) {
    const uint8_t reg = es7210_gain_reg_from_volume(s_mic_volume_cfg);
    const bool ok = es7210_write_reg(ES7210_MIC1_GAIN_REG43, reg) &&
                    es7210_write_reg(ES7210_MIC2_GAIN_REG44, reg);
    Serial.printf("[ES7210] mic gain: volume=%u -> REG43/44=0x%02X%s\n",
                  static_cast<unsigned>(s_mic_volume_cfg),
                  static_cast<unsigned>(reg),
                  ok ? "" : " (WRITE FAILED)");
    return ok;
}

// Probe the four possible ES7210 addresses; return true and latch
// s_es7210_addr when one acknowledges an I2C write.
static bool es7210_detect_address(void) {
    for (uint8_t addr : kEs7210AddrCandidates) {
        I2C_Start();
        const bool ack = (I2C_Write((addr << 1) | I2C_WRITE) >= 0);
        I2C_Stop();
        if (ack) {
            s_es7210_addr = addr;
            Serial.printf("[ES7210] found device at I2C addr 0x%02X\n",
                          static_cast<unsigned>(addr));
            return true;
        }
    }
    Serial.println("[ES7210] no device acknowledged on I2C addresses 0x40-0x43");
    return false;
}

// One (reg,value) pair of the init sequence.
struct Es7210RegInit {
    uint8_t reg;
    uint8_t value;
};

// Init sequence based on the Espressif esp_codec_dev / esp-bsp ES7210
// driver (es7210_config_codec), adapted for this board:
//   * serial port = standard Philips I2S, 16-bit, 2 channels (NOT TDM):
//     ADC1->left slot, ADC2->right slot of I2S DIN. The ESP32 I2S RX is
//     already configured for stereo I2S by the ES8311 driver, and reads
//     the LEFT slot, so MIC1 lands in the captured frame.
//   * clock divider computed for sample_rate=8 kHz, MCLK=2.048 MHz
//     (= 256 x fs, the rate the ES8311 driver programs):
//       REG02 = adc_div(1) | doubler(1)<<6 | dll_bypass(1)<<7 = 0xC1
//         -> the doubler brings the 2.048 MHz MCLK to a 4.096 MHz
//            internal ADC clock, matching the reference {4.096 MHz, 8 kHz}
//            coefficient row (adc_div=1, osr=0x20).
//       REG07 = OSR = 0x20
//       REG04:REG05 = LRCK divider = MCLK/LRCK = 2048000/8000 = 256 = 0x0100
//     If the ES7210 fails to lock at 2.048 MHz MCLK, the fallback is to
//     raise the I2S MCLK multiple to 512x (4.096 MHz) and use REG02=0x81.
constexpr Es7210RegInit kEs7210InitSeq[] = {
    {ES7210_RESET_REG00,         0xFF}, // full reset
    {ES7210_RESET_REG00,         0x32}, // release reset
    {ES7210_TIME_CONTROL0_REG09, 0x30},
    {ES7210_TIME_CONTROL1_REG0A, 0x30},
    {ES7210_ADC12_HPF1_REG23,    0x2A}, // ADC1/2 high-pass filter
    {ES7210_ADC12_HPF2_REG22,    0x0A},
    {ES7210_ADC34_HPF1_REG21,    0x2A}, // ADC3/4 high-pass filter
    {ES7210_ADC34_HPF2_REG20,    0x0A},
    {ES7210_SDP_INTERFACE1_REG11, 0x60}, // I2S, 16-bit
    {ES7210_SDP_INTERFACE2_REG12, 0x00}, // non-TDM, normal 2-channel I2S
    {ES7210_OSR_REG07,           0x20}, // ADC oversample ratio
    {ES7210_MAINCLK_REG02,       0xC1}, // adc_div=1 + doubler + dll bypass
    {ES7210_LRCK_DIVH_REG04,     0x01}, // LRCK divider high = 0x100 = 256
    {ES7210_LRCK_DIVL_REG05,     0x00},
    {ES7210_ANALOG_REG40,        0xC3}, // analog power/config
    {ES7210_MIC12_BIAS_REG41,    kEs7210MicBias},
    {ES7210_MIC34_BIAS_REG42,    kEs7210MicBias},
    // MIC1/MIC2 gain (REG43/REG44) are written separately after this
    // sequence from the runtime mic-volume setting; see es7210_apply_mic_gain.
    {ES7210_MIC3_GAIN_REG45,     0x1A}, // unused mics, fixed gain
    {ES7210_MIC4_GAIN_REG46,     0x1A},
    {ES7210_MIC1_POWER_REG47,    0x08}, // mic 1..4 PGA power up
    {ES7210_MIC2_POWER_REG48,    0x08},
    {ES7210_MIC3_POWER_REG49,    0x08},
    {ES7210_MIC4_POWER_REG4A,    0x08},
    {ES7210_POWER_DOWN_REG06,    0x04}, // DLL power down (DLL is bypassed)
    {ES7210_MIC12_POWER_REG4B,   0x0F}, // MIC1/2 ADC + PGA power up
    {ES7210_MIC34_POWER_REG4C,   0x0F}, // MIC3/4 ADC + PGA power up
    {ES7210_RESET_REG00,         0x71}, // device enable
    {ES7210_RESET_REG00,         0x41},
};

} // namespace

bool ES7210_Init(void) {
    if (s_es7210_ready) {
        return true;
    }

    I2C_Init();

    if (!es7210_detect_address()) {
        return false;
    }

    // Optional chip-ID probe. Not all ES7210 parts expose an ID at
    // 0xFD/0xFE (reads then return 0xFF); that is harmless -- the functional
    // register read-back below is the real proof the device is an ES7210.
    {
        uint8_t id1 = 0;
        uint8_t id0 = 0;
        es7210_read_reg(ES7210_CHIP_ID1_REGFD, &id1);
        es7210_read_reg(ES7210_CHIP_ID0_REGFE, &id0);
        if (id1 == 0x72 && id0 == 0x10) {
            Serial.println("[ES7210] chip id ok (ES7210)");
        } else {
            Serial.printf("[ES7210] chip id regs fd=0x%02X fe=0x%02X (no id at 0xFD/0xFE; "
                          "check the register dump below instead)\n",
                          static_cast<unsigned>(id1), static_cast<unsigned>(id0));
        }
    }

    for (const Es7210RegInit &step : kEs7210InitSeq) {
        if (!es7210_write_reg(step.reg, step.value)) {
            Serial.printf("[ES7210] init aborted at reg=0x%02X\n",
                          static_cast<unsigned>(step.reg));
            return false;
        }
        // The reset/release writes need a short settle before the next step.
        if (step.reg == ES7210_RESET_REG00) {
            delay(1);
        }
    }

    s_es7210_ready = true;

    // Apply the mic gain from the shared web/AT mic-volume setting.
    es7210_apply_mic_gain();

    Serial.printf("[ES7210] ready: i2c=0x%02X mode=I2S-16bit-2ch "
                  "(MIC1->left MIC2->right)\n",
                  static_cast<unsigned>(s_es7210_addr));
    ES7210_DumpRegisters();
    return true;
}

void ES7210_SetMicVolume(uint8_t volume) {
    s_mic_volume_cfg = volume;
    if (s_es7210_ready) {
        es7210_apply_mic_gain();
    } else {
        Serial.printf("[ES7210] mic volume pending: volume=%u\n",
                      static_cast<unsigned>(volume));
    }
}

bool ES7210_IsReady(void) {
    return s_es7210_ready;
}

void ES7210_DumpRegisters(void) {
    struct RegName { uint8_t reg; const char *name; };
    static const RegName kDumpRegs[] = {
        {ES7210_RESET_REG00,        "00 RESET"},
        {ES7210_CLOCK_OFF_REG01,    "01 CLOCK_OFF"},
        {ES7210_MAINCLK_REG02,      "02 MAINCLK"},
        {ES7210_LRCK_DIVH_REG04,    "04 LRCK_DIVH"},
        {ES7210_LRCK_DIVL_REG05,    "05 LRCK_DIVL"},
        {ES7210_POWER_DOWN_REG06,   "06 POWER_DOWN"},
        {ES7210_OSR_REG07,          "07 OSR"},
        {ES7210_MODE_CONFIG_REG08,  "08 MODE_CONFIG"},
        {ES7210_SDP_INTERFACE1_REG11, "11 SDP_FORMAT"},
        {ES7210_SDP_INTERFACE2_REG12, "12 SDP_TDM"},
        {ES7210_ANALOG_REG40,       "40 ANALOG"},
        {ES7210_MIC12_BIAS_REG41,   "41 MIC12_BIAS"},
        {ES7210_MIC1_GAIN_REG43,    "43 MIC1_GAIN"},
        {ES7210_MIC2_GAIN_REG44,    "44 MIC2_GAIN"},
        {ES7210_MIC1_POWER_REG47,   "47 MIC1_POWER"},
        {ES7210_MIC2_POWER_REG48,   "48 MIC2_POWER"},
        {ES7210_MIC12_POWER_REG4B,  "4B MIC12_POWER"},
        {ES7210_MIC34_POWER_REG4C,  "4C MIC34_POWER"},
    };
    for (const RegName &r : kDumpRegs) {
        uint8_t v = 0;
        const bool ok = es7210_read_reg(r.reg, &v);
        Serial.printf("[ES7210] REG%-16s = %s0x%02X\n",
                      r.name,
                      ok ? "" : "READ-ERR ",
                      static_cast<unsigned>(v));
    }
}

#else // !NRL_HAS_ES7210

// Board has no ES7210 (e.g. BH4TDV uses the ES8311 ADC directly).
bool ES7210_Init(void) { return false; }
bool ES7210_IsReady(void) { return false; }
void ES7210_SetMicVolume(uint8_t) {}
void ES7210_DumpRegisters(void) {}

#endif // NRL_HAS_ES7210
