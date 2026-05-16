#include "driver/es8311.h"

#include "driver/board_pins.h"
#include "driver/i2c1.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef ES8311_FORCE_DAC_SILENCE
#define ES8311_FORCE_DAC_SILENCE 0
#endif

namespace {

constexpr uint8_t kEs8311Addr = 0x18; // 7-bit I2C address

constexpr int kPinEspDout = NRL_PIN_I2S_DOUT; // Board-tested ESP -> ES8311 audio data
constexpr int kPinEspDin = NRL_PIN_I2S_DIN;   // Board-tested ES8311 -> ESP audio data
constexpr int kPinBclk = NRL_PIN_I2S_BCLK;
constexpr int kPinLrclk = NRL_PIN_I2S_LRCLK;
constexpr int kPinMclk = NRL_PIN_I2S_MCLK;
constexpr int kPinPaEn = NRL_PIN_PA_EN;

constexpr i2s_port_t kI2sPort = I2S_NUM_0;
constexpr int kSampleRate = 8000;
constexpr int kMclkRate = kSampleRate * 256;
constexpr i2s_bits_per_sample_t kBitsPerSample = I2S_BITS_PER_SAMPLE_16BIT;
constexpr size_t kFrameSamples = 80;
// I2S bus stays stereo (2 slots/frame) so BCLK timing matches ES8311's clock
// divider expectations. ES8311 is mono internally �?it reads/writes the LEFT
// slot only. We duplicate mono content into both slots in i2s_write_frame.
constexpr size_t kI2sSlotCount = 2;
constexpr size_t kFrameBytes = kFrameSamples * sizeof(int16_t);
constexpr size_t kI2sFrameBytes = kFrameSamples * kI2sSlotCount * sizeof(int16_t);
const TickType_t kI2sWaitTicks = pdMS_TO_TICKS(20);

constexpr float kToneFrequency = 440.0f;
constexpr float kToneAmplitude = 0.40f;
constexpr float kTwoPi = 6.283185307179586f;


// ES8311 registers (subset)
enum : uint8_t {
    ES8311_REG00_RESET = 0x00,
    ES8311_REG01_CLK_MANAGER = 0x01,
    ES8311_REG02_CLK_MANAGER = 0x02,
    ES8311_REG03_CLK_MANAGER = 0x03,
    ES8311_REG04_CLK_MANAGER = 0x04,
    ES8311_REG05_CLK_MANAGER = 0x05,
    ES8311_REG06_CLK_MANAGER = 0x06,
    ES8311_REG07_CLK_MANAGER = 0x07,
    ES8311_REG08_CLK_MANAGER = 0x08,
    ES8311_REG09_SDPIN = 0x09,
    ES8311_REG0A_SDPOUT = 0x0A,
    ES8311_REG0B_SYSTEM = 0x0B,
    ES8311_REG0C_SYSTEM = 0x0C,
    ES8311_REG0D_SYSTEM = 0x0D,
    ES8311_REG0E_SYSTEM = 0x0E,
    ES8311_REG10_SYSTEM = 0x10,
    ES8311_REG11_SYSTEM = 0x11,
    ES8311_REG12_SYSTEM = 0x12,
    ES8311_REG13_SYSTEM = 0x13,
    ES8311_REG14_SYSTEM = 0x14,
    ES8311_REG15_ADC = 0x15,
    ES8311_REG16_ADC = 0x16,
    ES8311_REG17_ADC = 0x17,
    ES8311_REG1B_ADC = 0x1B,
    ES8311_REG1C_ADC = 0x1C,
    ES8311_REG31_DAC = 0x31,
    ES8311_REG32_DAC = 0x32,
    ES8311_REG33_DAC = 0x33,
    ES8311_REG34_DAC = 0x34,
    ES8311_REG35_DAC = 0x35,
    ES8311_REG36_DAC = 0x36,
    ES8311_REG37_DAC = 0x37,
    ES8311_REG38_DACEQ = 0x38,
    ES8311_REG39_DACEQ = 0x39,
    ES8311_REG3A_DACEQ = 0x3A,
    ES8311_REG3B_DACEQ = 0x3B,
    ES8311_REG3C_DACEQ = 0x3C,
    ES8311_REG3D_DACEQ = 0x3D,
    ES8311_REG3E_DACEQ = 0x3E,
    ES8311_REG3F_DACEQ = 0x3F,
    ES8311_REG40_DACEQ = 0x40,
    ES8311_REG41_DACEQ = 0x41,
    ES8311_REG42_DACEQ = 0x42,
    ES8311_REG43_DACEQ = 0x43,
    ES8311_REG44_GPIO = 0x44,
    ES8311_REG45_GP = 0x45,
};

constexpr uint8_t kEs8311ClockEnableAll = 0x3F;
constexpr uint8_t kEs8311AnalogMicPgaEnable = 0x1A; // mic gain from test.cpp
constexpr uint8_t kEs8311AdcGainScaleUp = 0x24;
constexpr uint8_t kEs8311AdcVolumeDefault = 0xBF;  // reference es8311_start: REG17 = 0xBF
constexpr uint8_t kEs8311DacVolumeDefault = 0xFF; // max DAC volume for the attenuated radio MIC path
// REG0D bits: PDN_ANA(7) PDN_IBIASGEN(6) PDN_ADCBIASGEN(5) PDN_ADCVERFGEN(4)
//             PDN_DACVREFGEN(3) PDN_VREF(2) VMIDSEL(1:0)
// NOTE: PDN_VREF (bit 2) has REVERSE polarity vs other PDN bits!
//   0 = disable internal reference (BAD), 1 = enable reference (default)
// 0x06 = enable everything analog + enable VREF + VMIDSEL=normal operation.
// Old value 0x01 had bit2=0 which DISABLED the internal reference, leaving
// DAC modulator without proper analog reference (root cause of DC offsets,
// random startup polarity, dependence on external load to settle).
constexpr uint8_t kEs8311PowerUpAnalog = 0x01;
constexpr uint8_t kEs8311PowerUpPgaAdc = 0x02;
constexpr uint8_t kEs8311PowerUpDac = 0x00;
constexpr uint8_t kEs8311OutputDriveLine = 0x00; // REG13: HPSW=0 bit6=0
constexpr uint8_t kEs8311OutputDriveHp = 0x10;   // REG13: HPSW=1 bit6=0
constexpr uint8_t kEs8311AdcEqBypass = 0x6A;
constexpr uint8_t kDefaultDrcWinsize = 0x00;
constexpr uint8_t kDefaultDrcLevel = 0x00;
constexpr uint8_t kDefaultDacRamprate = 0x00;
constexpr uint32_t kDacEqCoefficientMask = 0x3FFFFFFFUL;
constexpr uint8_t kEs8311DacUnmute = 0x00;

struct Es8311ClockConfig {
    uint8_t pre_div;
    uint8_t pre_mult;
    uint8_t adc_div;
    uint8_t dac_div;
    uint8_t fs_mode;
    uint8_t lrck_h;
    uint8_t lrck_l;
    uint8_t bclk_div;
    uint8_t adc_osr;
    uint8_t dac_osr;
};

static bool s_es8311_ready = false;
static bool s_i2s_ready = false;
static bool s_i2s_driver_installed = false;
static TaskHandle_t s_passthrough_task = nullptr;
static volatile bool s_passthrough_running = false;
static ES8311_AudioMode_t s_audio_mode = ES8311_AUDIO_MODE_RECEIVE;
static ES8311_FrameHook_t s_frame_hook = nullptr;
static void *s_frame_hook_user_data = nullptr;

constexpr size_t kOutputQueueSamples = kFrameSamples * 64u;
static int16_t s_output_queue[kOutputQueueSamples];
static size_t s_output_queue_head = 0;
static size_t s_output_queue_tail = 0;
static size_t s_output_queue_count = 0;
static SemaphoreHandle_t s_output_queue_mutex = nullptr;
static uint32_t s_last_output_queue_log_ms = 0;
static uint8_t s_mic_volume = kEs8311AdcVolumeDefault;
static uint8_t s_line_out_volume = 0xFF;
static bool s_hp_drive_enabled = false;
static bool s_drc_enabled = false;
static uint8_t s_drc_winsize = kDefaultDrcWinsize;
static uint8_t s_drc_maxlevel = kDefaultDrcLevel;
static uint8_t s_drc_minlevel = kDefaultDrcLevel;
static uint8_t s_dac_ramprate = kDefaultDacRamprate;
static bool s_dac_eq_bypass = true;
static uint32_t s_daceq_b0 = 0U;
static uint32_t s_daceq_b1 = 0U;
static uint32_t s_daceq_a1 = 0U;

static bool es8311_write_reg(uint8_t reg, uint8_t value) {
    bool ok = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        I2C_Start();
        ok = true;
        if (I2C_Write((kEs8311Addr << 1) | I2C_WRITE) < 0) {
            ok = false;
            goto stop_once;
        }
        if (I2C_Write(reg) < 0) {
            ok = false;
            goto stop_once;
        }
        if (I2C_Write(value) < 0) {
            ok = false;
            goto stop_once;
        }

    stop_once:
        I2C_Stop();
        if (ok) {
            return true;
        }
        Serial.printf("[ES8311] I2C write failed: reg=0x%02X value=0x%02X attempt=%d\n",
                      static_cast<unsigned>(reg),
                      static_cast<unsigned>(value),
                      attempt + 1);
        delay(1);
    }
    return false;
}

static bool es8311_read_reg(const uint8_t reg, uint8_t *value) {
    if (value == nullptr) {
        return false;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        bool ok = true;
        I2C_Start();
        if (I2C_Write((kEs8311Addr << 1) | I2C_WRITE) < 0) {
            ok = false;
            goto stop_read_once;
        }
        if (I2C_Write(reg) < 0) {
            ok = false;
            goto stop_read_once;
        }

        I2C_Start();
        if (I2C_Write((kEs8311Addr << 1) | I2C_READ) < 0) {
            ok = false;
            goto stop_read_once;
        }
        *value = I2C_Read(true);

    stop_read_once:
        I2C_Stop();
        if (ok) {
            return true;
        }
        delay(1);
    }

    return false;
}

static uint8_t es8311_resolution_value(const int bits) {
    switch (bits) {
        case 16:
            return (3u << 2);
        case 18:
            return (2u << 2);
        case 20:
            return (1u << 2);
        case 24:
            return (0u << 2);
        case 32:
            return (4u << 2);
        default:
            return (3u << 2);
    }
}

static uint8_t es8311_pre_mult_code(const uint8_t pre_mult) {
    switch (pre_mult) {
        case 1:
            return 0;
        case 2:
            return 1;
        case 4:
            return 2;
        case 8:
            return 3;
        default:
            return 0;
    }
}

static bool es8311_config_clock(const Es8311ClockConfig &cfg) {
    if (!es8311_write_reg(ES8311_REG01_CLK_MANAGER, kEs8311ClockEnableAll)) {
        return false;
    }

    uint8_t reg02 = 0x00;
    reg02 |= static_cast<uint8_t>((cfg.pre_div - 1u) << 5);
    reg02 |= static_cast<uint8_t>(es8311_pre_mult_code(cfg.pre_mult) << 3);
    if (!es8311_write_reg(ES8311_REG02_CLK_MANAGER, reg02)) {
        return false;
    }

    if (!es8311_write_reg(ES8311_REG03_CLK_MANAGER, static_cast<uint8_t>((cfg.fs_mode << 6) | cfg.adc_osr))) {
        return false;
    }
    if (!es8311_write_reg(ES8311_REG04_CLK_MANAGER, cfg.dac_osr)) {
        return false;
    }
    if (!es8311_write_reg(ES8311_REG05_CLK_MANAGER,
                          static_cast<uint8_t>(((cfg.adc_div - 1u) << 4) | (cfg.dac_div - 1u)))) {
        return false;
    }

    uint8_t reg06 = 0x00;
    if (cfg.bclk_div < 19u) {
        reg06 |= static_cast<uint8_t>(cfg.bclk_div - 1u);
    } else {
        reg06 |= cfg.bclk_div;
    }

    if (!es8311_write_reg(ES8311_REG06_CLK_MANAGER, reg06)) {
        return false;
    }
    if (!es8311_write_reg(ES8311_REG07_CLK_MANAGER, cfg.lrck_h)) {
        return false;
    }
    if (!es8311_write_reg(ES8311_REG08_CLK_MANAGER, cfg.lrck_l)) {
        return false;
    }
    return true;
}

static bool es8311_config_i2s_format(const int bits) {
    const uint8_t reg_value = es8311_resolution_value(bits);
    if (!es8311_write_reg(ES8311_REG09_SDPIN, reg_value)) {
        return false;
    }
    if (!es8311_write_reg(ES8311_REG0A_SDPOUT, reg_value)) {
        return false;
    }
    return true;
}

static uint8_t es8311_output_drive_reg(void) {
    // HP mode (0x10, HPSW=1): OUTN virtual ground, OUTP single-ended.
    //   Good for boards with PA/HP amp.
    // LINE mode (0x00): pure differential OUTP/OUTN, symmetric DC bias.
    //   For direct connection to radio MIC input, use LINE mode
    //   with a differential-to-single-ended circuit (e.g. AC coupling both
    //   OUTP/OUTN through matching RC loads).
    if (s_hp_drive_enabled) {
        return 0x10;
    }
    return 0x00;
}

static uint8_t es8311_drc_reg34(void)
{
    return static_cast<uint8_t>((s_drc_enabled ? 0x80u : 0x00u) |
                                (s_drc_winsize & 0x0Fu));
}

static uint8_t es8311_drc_reg35(void)
{
    return static_cast<uint8_t>(((s_drc_maxlevel & 0x0Fu) << 4) |
                                (s_drc_minlevel & 0x0Fu));
}

static uint8_t es8311_dac_reg37(void)
{
    return static_cast<uint8_t>(((s_dac_ramprate & 0x0Fu) << 4) |
                                (s_dac_eq_bypass ? 0x08u : 0x00u));
}

static bool es8311_apply_drc_config(void)
{
    return es8311_write_reg(ES8311_REG34_DAC, es8311_drc_reg34()) &&
           es8311_write_reg(ES8311_REG35_DAC, es8311_drc_reg35()) &&
           es8311_write_reg(ES8311_REG37_DAC, es8311_dac_reg37());
}

static bool es8311_write_daceq_coeff(const uint8_t first_reg, const uint32_t value)
{
    const uint32_t coeff = value & kDacEqCoefficientMask;
    return es8311_write_reg(first_reg, static_cast<uint8_t>((coeff >> 24) & 0x3Fu)) &&
           es8311_write_reg(static_cast<uint8_t>(first_reg + 1U), static_cast<uint8_t>((coeff >> 16) & 0xFFu)) &&
           es8311_write_reg(static_cast<uint8_t>(first_reg + 2U), static_cast<uint8_t>((coeff >> 8) & 0xFFu)) &&
           es8311_write_reg(static_cast<uint8_t>(first_reg + 3U), static_cast<uint8_t>(coeff & 0xFFu));
}

static bool es8311_apply_daceq_coefficients(void)
{
    return es8311_write_daceq_coeff(ES8311_REG38_DACEQ, s_daceq_b0) &&
           es8311_write_daceq_coeff(ES8311_REG3C_DACEQ, s_daceq_b1) &&
           es8311_write_daceq_coeff(ES8311_REG40_DACEQ, s_daceq_a1);
}

static bool es8311_config_dac_output_path(const uint8_t reg12,
                                          const uint8_t reg13,
                                          const uint8_t reg31,
                                          const uint8_t reg32) {
    if (!es8311_write_reg(ES8311_REG12_SYSTEM, reg12)) {
        return false;
    }
    if (!es8311_write_reg(ES8311_REG13_SYSTEM, reg13)) {
        return false;
    }
    if (!es8311_write_reg(ES8311_REG31_DAC, reg31)) {
        return false;
    }
    if (!es8311_write_reg(ES8311_REG32_DAC, reg32)) {
        return false;
    }
    return true;
}

// Init sequence aligned to the ESP-ADF esp_codec_dev ES8311 implementation:
// es8311_open(), es8311_set_fs(), then es8311_enable(true). Board IO remains
// local to board_pins.h and the I2S setup above.
static bool es8311_configure_codec(void) {
    uint8_t regv = 0u;

    if (es8311_read_reg(ES8311_REG0D_SYSTEM, &regv) && regv != 0xFA) {
        if (!es8311_write_reg(ES8311_REG0D_SYSTEM, 0xFA)) {
            Serial.println("[ES8311] REG0D pre-power failed");
            return false;
        }
    }

    if (!es8311_write_reg(ES8311_REG44_GPIO, 0x08)) {
        Serial.println("[ES8311] REG44 (1) failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG44_GPIO, 0x08)) {
        Serial.println("[ES8311] REG44 (2) failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG01_CLK_MANAGER, 0x30)) {
        Serial.println("[ES8311] REG01 init failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG02_CLK_MANAGER, 0x00)) {
        Serial.println("[ES8311] REG02 init failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG03_CLK_MANAGER, 0x10)) {
        Serial.println("[ES8311] REG03 init failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG16_ADC, kEs8311AdcGainScaleUp)) {
        Serial.println("[ES8311] REG16 init failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG04_CLK_MANAGER, 0x10)) {
        Serial.println("[ES8311] REG04 init failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG05_CLK_MANAGER, 0x00)) {
        Serial.println("[ES8311] REG05 init failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG0B_SYSTEM, 0x00)) {
        Serial.println("[ES8311] REG0B failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG0C_SYSTEM, 0x00)) {
        Serial.println("[ES8311] REG0C failed");
        return false;
    }
    // Analog bias (Espressif known-good values)
    if (!es8311_write_reg(ES8311_REG10_SYSTEM, 0x1F)) {
        Serial.println("[ES8311] REG10 bias config failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG11_SYSTEM, 0x7F)) {
        Serial.println("[ES8311] REG11 bias config failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG00_RESET, 0x80)) {
        Serial.println("[ES8311] REG00 reset/slave failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG01_CLK_MANAGER, 0x3F)) {
        Serial.println("[ES8311] REG01 use_mclk failed");
        return false;
    }

    if (es8311_read_reg(ES8311_REG06_CLK_MANAGER, &regv)) {
        regv = static_cast<uint8_t>(regv & ~0x20u);
        if (!es8311_write_reg(ES8311_REG06_CLK_MANAGER, regv)) {
            Serial.println("[ES8311] REG06 sclk polarity failed");
            return false;
        }
    }

    const Es8311ClockConfig clock_cfg = {
        .pre_div = 0x01,
        .pre_mult = 0x01,
        .adc_div = 0x01,
        .dac_div = 0x01,
        .fs_mode = 0x00,
        .lrck_h = 0x00,
        .lrck_l = 0xFF,
        .bclk_div = 0x04,
        .adc_osr = 0x10,
        .dac_osr = 0x20,
    };

    if (!es8311_config_clock(clock_cfg)) {
        Serial.println("[ES8311] clock configuration failed");
        return false;
    }

    if (!es8311_config_i2s_format(16)) {
        Serial.println("[ES8311] I2S format configuration failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG13_SYSTEM, es8311_output_drive_reg())) {
        Serial.println("[ES8311] REG13 failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG1B_ADC, 0x0A)) {
        Serial.println("[ES8311] REG1B (ADC HPF) failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG1C_ADC, kEs8311AdcEqBypass)) {
        Serial.println("[ES8311] REG1C (ADC EQ) failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG44_GPIO, 0x58)) {
        Serial.println("[ES8311] REG44 final failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG00_RESET, 0x80)) {
        Serial.println("[ES8311] REG00 start failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG01_CLK_MANAGER, 0x3F)) {
        Serial.println("[ES8311] REG01 start clock failed");
        return false;
    }

    uint8_t dac_iface = 0u;
    uint8_t adc_iface = 0u;
    if (!es8311_read_reg(ES8311_REG09_SDPIN, &dac_iface) ||
        !es8311_read_reg(ES8311_REG0A_SDPOUT, &adc_iface)) {
        Serial.println("[ES8311] interface readback failed");
        return false;
    }
    dac_iface = static_cast<uint8_t>(dac_iface & ~0x40u);
    adc_iface = static_cast<uint8_t>(adc_iface & ~0x40u);
    if (!es8311_write_reg(ES8311_REG09_SDPIN, dac_iface) ||
        !es8311_write_reg(ES8311_REG0A_SDPOUT, adc_iface)) {
        Serial.println("[ES8311] interface enable failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG17_ADC, s_mic_volume)) {
        Serial.println("[ES8311] REG17 ADC volume failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG0E_SYSTEM, kEs8311PowerUpPgaAdc)) {
        Serial.println("[ES8311] REG0E power-up PGA/ADC failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG12_SYSTEM, kEs8311PowerUpDac)) {
        Serial.println("[ES8311] REG12 enable DAC failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG14_SYSTEM, kEs8311AnalogMicPgaEnable)) {
        Serial.println("[ES8311] REG14 analog mic PGA failed");
        return false;
    }
    if (es8311_read_reg(ES8311_REG14_SYSTEM, &regv)) {
        regv = static_cast<uint8_t>(regv & ~0x40u);
        if (!es8311_write_reg(ES8311_REG14_SYSTEM, regv)) {
            Serial.println("[ES8311] REG14 analog mic select failed");
            return false;
        }
    }
    if (!es8311_write_reg(ES8311_REG0D_SYSTEM, kEs8311PowerUpAnalog)) {
        Serial.println("[ES8311] REG0D power-up analog failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG15_ADC, 0x40)) {
        Serial.println("[ES8311] REG15 ADC ramp rate failed");
        return false;
    }
    if (!es8311_apply_drc_config() || !es8311_apply_daceq_coefficients()) {
        Serial.println("[ES8311] DAC DRC/EQ config failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG45_GP, 0x00)) {
        Serial.println("[ES8311] REG45 GP control failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG31_DAC, kEs8311DacUnmute)) {
        Serial.println("[ES8311] REG31 unmute failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG32_DAC, s_line_out_volume)) {
        Serial.println("[ES8311] REG32 DAC volume failed");
        return false;
    }

    return true;
}

static bool i2s_setup(void) {
    if (s_i2s_ready) {
        return true;
    }

    if (s_i2s_driver_installed) {
        i2s_driver_uninstall(kI2sPort);
        s_i2s_driver_installed = false;
    }

    i2s_config_t config = {};
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
    config.sample_rate = kSampleRate;
    config.bits_per_sample = kBitsPerSample;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = 0;
    config.dma_buf_count = 8;
    config.dma_buf_len = kFrameSamples;
    config.use_apll = true;
    config.tx_desc_auto_clear = true;
    config.fixed_mclk = kMclkRate;

    const esp_err_t install_err = i2s_driver_install(kI2sPort, &config, 0, nullptr);
    if (install_err != ESP_OK) {
        Serial.printf("[ES8311] i2s_driver_install failed: err=%d\n", static_cast<int>(install_err));
        return false;
    }
    s_i2s_driver_installed = true;

    i2s_pin_config_t pin_config = {};
    pin_config.mck_io_num = kPinMclk;
    pin_config.bck_io_num = kPinBclk;
    pin_config.ws_io_num = kPinLrclk;
    pin_config.data_out_num = kPinEspDout;
    pin_config.data_in_num = kPinEspDin;

    const esp_err_t pin_err = i2s_set_pin(kI2sPort, &pin_config);
    if (pin_err != ESP_OK) {
        Serial.printf("[ES8311] i2s_set_pin failed: err=%d mclk=%d bclk=%d ws=%d dout=%d din=%d\n",
                      static_cast<int>(pin_err),
                      kPinMclk,
                      kPinBclk,
                      kPinLrclk,
                      kPinEspDout,
                      kPinEspDin);
        i2s_driver_uninstall(kI2sPort);
        s_i2s_driver_installed = false;
        return false;
    }

    const esp_err_t clk_err = i2s_set_clk(kI2sPort, kSampleRate, kBitsPerSample, I2S_CHANNEL_STEREO);
    if (clk_err != ESP_OK) {
        Serial.printf("[ES8311] i2s_set_clk failed: err=%d rate=%d bits=%d stereo_slots=2\n",
                      static_cast<int>(clk_err),
                      kSampleRate,
                      16);
        i2s_driver_uninstall(kI2sPort);
        s_i2s_driver_installed = false;
        return false;
    }

    const esp_err_t start_err = i2s_start(kI2sPort);
    if (start_err != ESP_OK) {
        Serial.printf("[ES8311] i2s_start failed: err=%d\n", static_cast<int>(start_err));
        i2s_driver_uninstall(kI2sPort);
        s_i2s_driver_installed = false;
        return false;
    }

    i2s_zero_dma_buffer(kI2sPort);
    s_i2s_ready = true;
    Serial.printf("[ES8311] i2s clocks: rate=%dHz bits=%d stereo mclk=%dHz\n",
                  kSampleRate,
                  16,
                  kMclkRate);
    return true;
}

static bool i2s_read_frame(int16_t *dst) {
    // Stereo I2S: 2 int16 per LRCK frame (L,R). ES8311 outputs ADC data on
    // LEFT only �?extract LEFT samples into dst.
    static_assert(kI2sSlotCount == 2, "i2s_read_frame assumes stereo I2S frame");
    int16_t raw[kFrameSamples * kI2sSlotCount];  // size = 80*2 = 160
    size_t bytes_in_frame = 0;
    while (bytes_in_frame < kI2sFrameBytes) {
        size_t bytes_read = 0;
        if (i2s_read(kI2sPort,
                     reinterpret_cast<uint8_t *>(raw) + bytes_in_frame,
                     kI2sFrameBytes - bytes_in_frame,
                     &bytes_read,
                     kI2sWaitTicks) != ESP_OK) {
            return false;
        }

        if (bytes_read == 0) {
            continue;
        }

        bytes_in_frame += bytes_read;
    }

    // Take LEFT slot (index 2*i) of each frame.
    for (size_t i = 0; i < kFrameSamples; ++i) {
        dst[i] = raw[i * 2];
    }
    return true;
}

static bool i2s_write_frame(const int16_t *src) {
    // Stereo I2S: 2 int16 per LRCK frame. Duplicate mono src into both slots.
    static_assert(kI2sSlotCount == 2, "i2s_write_frame assumes stereo I2S frame");
    int16_t raw[kFrameSamples * kI2sSlotCount];  // size = 80*2 = 160
#if ES8311_FORCE_DAC_SILENCE
    (void)src;
    memset(raw, 0, sizeof(raw));
#else
    for (size_t i = 0; i < kFrameSamples; ++i) {
        raw[i * 2]     = src[i];  // LEFT
        raw[i * 2 + 1] = src[i];  // RIGHT (same data, ES8311 ignores but keeps timing correct)
    }
#endif

    size_t bytes_out_frame = 0;
    while (bytes_out_frame < kI2sFrameBytes) {
        size_t bytes_written = 0;
        if (i2s_write(kI2sPort,
                      reinterpret_cast<const uint8_t *>(raw) + bytes_out_frame,
                      kI2sFrameBytes - bytes_out_frame,
                      &bytes_written,
                      kI2sWaitTicks) != ESP_OK) {
            return false;
        }

        if (bytes_written == 0) {
            continue;
        }

        bytes_out_frame += bytes_written;
    }
    return true;
}

static void output_queue_init(void) {
    if (s_output_queue_mutex == nullptr) {
        s_output_queue_mutex = xSemaphoreCreateMutex();
    }
}

static void output_queue_clear_locked(void) {
    s_output_queue_head = 0;
    s_output_queue_tail = 0;
    s_output_queue_count = 0;
}

static size_t output_queue_push(const int16_t *samples, size_t sample_count) {
    if (samples == nullptr || sample_count == 0) {
        return 0;
    }

    output_queue_init();
    if (s_output_queue_mutex == nullptr) {
        return 0;
    }

    if (xSemaphoreTake(s_output_queue_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return 0;
    }

    size_t written = 0;
    while (written < sample_count && s_output_queue_count < kOutputQueueSamples) {
        s_output_queue[s_output_queue_tail] = samples[written++];
        s_output_queue_tail = (s_output_queue_tail + 1u) % kOutputQueueSamples;
        ++s_output_queue_count;
    }

    xSemaphoreGive(s_output_queue_mutex);
    return written;
}

static size_t output_queue_pop_frame(int16_t *dst, const size_t sample_count) {
    if (dst == nullptr || sample_count == 0) {
        return 0;
    }

    output_queue_init();
    if (s_output_queue_mutex == nullptr) {
        memset(dst, 0, sample_count * sizeof(int16_t));
        return 0;
    }

    if (xSemaphoreTake(s_output_queue_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        memset(dst, 0, sample_count * sizeof(int16_t));
        return 0;
    }

    size_t read = 0;
    while (read < sample_count && s_output_queue_count > 0) {
        dst[read++] = s_output_queue[s_output_queue_head];
        s_output_queue_head = (s_output_queue_head + 1u) % kOutputQueueSamples;
        --s_output_queue_count;
    }

    xSemaphoreGive(s_output_queue_mutex);

    if (read < sample_count) {
        memset(dst + read, 0, (sample_count - read) * sizeof(int16_t));
    }

    return read;
}

static void output_queue_clear(void) {
    output_queue_init();
    if (s_output_queue_mutex == nullptr) {
        return;
    }

    if (xSemaphoreTake(s_output_queue_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return;
    }
    output_queue_clear_locked();
    xSemaphoreGive(s_output_queue_mutex);
}

static void es8311_passthrough_task(void *) {
    static int16_t frame[kFrameSamples];

    while (s_passthrough_running) {
        const ES8311_AudioMode_t mode = s_audio_mode;

        if (!i2s_read_frame(frame)) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (s_frame_hook != nullptr) {
            s_frame_hook(frame, kFrameSamples, mode, s_frame_hook_user_data);
        }

        // ADC and DAC are physically independent paths in this hardware.
        // Do NOT echo captured ADC audio back to DAC �?that would re-transmit
        // received radio audio out the TO_MIC line.
        // In RX mode, DAC plays whatever is in the output queue (NRL downlink).
        // If the queue is empty, write silence so the DAC stays at VMID.
        const size_t popped = output_queue_pop_frame(frame, kFrameSamples);
        (void)popped;
        if (!i2s_write_frame(frame)) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
    }

    i2s_zero_dma_buffer(kI2sPort);
    s_passthrough_task = nullptr;
    vTaskDelete(nullptr);
}

static bool es8311_start_passthrough(void) {
    if (!s_i2s_ready && !i2s_setup()) {
        return false;
    }

    if (s_passthrough_task != nullptr) {
        return true;
    }

    s_passthrough_running = true;
    if (xTaskCreate(es8311_passthrough_task,
                    "es8311_passthrough",
                    4096,
                    nullptr,
                    3,
                    &s_passthrough_task) != pdPASS) {
        s_passthrough_running = false;
        s_passthrough_task = nullptr;
        return false;
    }

    return true;
}

static void es8311_stop_passthrough(void) {
    if (s_passthrough_task == nullptr) {
        return;
    }

    s_passthrough_running = false;
    for (int wait = 0; wait < 50 && s_passthrough_task != nullptr; ++wait) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static inline void es8311_apply_switch_level(const ES8311_AudioMode_t mode) {
    s_audio_mode = mode;
}

} // namespace

bool ES8311_Init(void) {
    if (s_es8311_ready) {
        return es8311_start_passthrough();
    }

    es8311_apply_switch_level(ES8311_AUDIO_MODE_RECEIVE);

    // Enable power amplifier
    if (kPinPaEn >= 0) {
        pinMode(kPinPaEn, OUTPUT);
        digitalWrite(kPinPaEn, HIGH);
        Serial.printf("[ES8311] PA enable pin %d set HIGH\n", kPinPaEn);
    }

    I2C_Init();

    // I2S/MCLK must be running BEFORE codec register config.
    // ES8311 internal DAC bias circuits require MCLK to establish
    // proper VMID and output common-mode voltage.
    if (!i2s_setup()) {
        s_es8311_ready = false;
        Serial.println("[ES8311] initialization failed during I2S setup");
        return false;
    }

    if (!es8311_configure_codec()) {
        s_es8311_ready = false;
        Serial.println("[ES8311] initialization failed during codec configuration");
        return false;
    }

    // Read chip ID. Standard ES8311 returns FD=0x83, FE=0x11. Anything else
    // suggests this is a clone or different codec with mismatched register map.
    {
        uint8_t reg_fd = 0;
        uint8_t reg_fe = 0;
        const bool ok_fd = es8311_read_reg(0xFDU, &reg_fd);
        const bool ok_fe = es8311_read_reg(0xFEU, &reg_fe);
        if (!ok_fd || !ok_fe || reg_fd != 0x83 || reg_fe != 0x11) {
            Serial.printf("[ES8311] WARNING chip id mismatch: fd=%s0x%02X (expect 0x83) fe=%s0x%02X (expect 0x11)\n",
                          ok_fd ? "" : "ERR:", static_cast<unsigned>(reg_fd),
                          ok_fe ? "" : "ERR:", static_cast<unsigned>(reg_fe));
        }
    }

    if (!es8311_start_passthrough()) {
        s_es8311_ready = false;
        Serial.println("[ES8311] initialization failed starting passthrough task");
        return false;
    }

    s_es8311_ready = true;
#if ES8311_FORCE_DAC_SILENCE
    Serial.println("[ES8311] *** FORCE_DAC_SILENCE=1 (DAC always writes zeros) ***");
#else
    Serial.println("[ES8311] FORCE_DAC_SILENCE=0 (normal DAC writes)");
#endif
    Serial.printf("[ES8311] ready: i2c=0x%02X rate=%dHz bits=%d stereo_slots=2 mclk=%d bclk=%d esp_dout=%d lrclk=%d esp_din=%d\n",
                  static_cast<unsigned>(kEs8311Addr),
                  kSampleRate,
                  16,
                  kPinMclk,
                  kPinBclk,
                  kPinEspDout,
                  kPinLrclk,
                  kPinEspDin);
    return true;
}

bool ES8311_IsReady(void) {
    return s_es8311_ready;
}

bool ES8311_SetAudioMode(const ES8311_AudioMode_t mode) {
    if (!ES8311_Init()) {
        return false;
    }

    if (!es8311_config_dac_output_path(kEs8311PowerUpDac,
                                       es8311_output_drive_reg(),
                                       kEs8311DacUnmute,
                                       s_line_out_volume)) {
        Serial.println("[ES8311] failed to configure DAC output path");
        return false;
    }

    s_audio_mode = mode;
    es8311_apply_switch_level(mode);
    return true;
}

bool ES8311_SetReceiveMode(void) {
    return ES8311_SetAudioMode(ES8311_AUDIO_MODE_RECEIVE);
}


void ES8311_SetFrameHook(ES8311_FrameHook_t hook, void *user_data) {
    s_frame_hook = hook;
    s_frame_hook_user_data = user_data;
}

size_t ES8311_QueueOutputSamples(const int16_t *samples, size_t sample_count) {
    if (!ES8311_Init()) {
        return 0;
    }

    const size_t written = output_queue_push(samples, sample_count);
    const uint32_t now = millis();
    if (written != sample_count && (now - s_last_output_queue_log_ms) >= 1000u) {
        s_last_output_queue_log_ms = now;
        Serial.printf("[ES8311] queue short write samples=%u written=%u\n",
                      static_cast<unsigned>(sample_count),
                      static_cast<unsigned>(written));
    }
    return written;
}

void ES8311_ClearOutputQueue(void) {
    output_queue_clear();
}

bool ES8311_ApplyAudioConfig(const uint8_t mic_volume,
                             const uint8_t line_out_volume,
                             const bool hp_drive_enabled,
                             const bool drc_enabled,
                             const uint8_t drc_winsize,
                             const uint8_t drc_maxlevel,
                             const uint8_t drc_minlevel,
                             const uint8_t dac_ramprate,
                             const bool dac_eq_bypass,
                             const uint32_t daceq_b0,
                             const uint32_t daceq_b1,
                             const uint32_t daceq_a1) {
    s_mic_volume = mic_volume;
    s_line_out_volume = line_out_volume;
    s_hp_drive_enabled = hp_drive_enabled;
    s_drc_enabled = drc_enabled;
    s_drc_winsize = drc_winsize & 0x0Fu;
    s_drc_maxlevel = drc_maxlevel & 0x0Fu;
    s_drc_minlevel = drc_minlevel & 0x0Fu;
    s_dac_ramprate = dac_ramprate & 0x0Fu;
    s_dac_eq_bypass = dac_eq_bypass;
    s_daceq_b0 = daceq_b0 & kDacEqCoefficientMask;
    s_daceq_b1 = daceq_b1 & kDacEqCoefficientMask;
    s_daceq_a1 = daceq_a1 & kDacEqCoefficientMask;

    if (!s_es8311_ready) {
        Serial.printf("[ES8311] audio config pending: mic=0x%02X line_out=0x%02X hp_drive=%u drc=%u,%u,%u,%u ramp=%u eq_bypass=%u eq=%lu,%lu,%lu\n",
                      static_cast<unsigned>(s_mic_volume),
                      static_cast<unsigned>(s_line_out_volume),
                      s_hp_drive_enabled ? 1u : 0u,
                      s_drc_enabled ? 1u : 0u,
                      static_cast<unsigned>(s_drc_winsize),
                      static_cast<unsigned>(s_drc_maxlevel),
                      static_cast<unsigned>(s_drc_minlevel),
                      static_cast<unsigned>(s_dac_ramprate),
                      s_dac_eq_bypass ? 1u : 0u,
                      static_cast<unsigned long>(s_daceq_b0),
                      static_cast<unsigned long>(s_daceq_b1),
                      static_cast<unsigned long>(s_daceq_a1));
        return true;
    }

    const bool ok =
        es8311_write_reg(ES8311_REG17_ADC, s_mic_volume) &&
        es8311_config_dac_output_path(kEs8311PowerUpDac,
                                      es8311_output_drive_reg(),
                                      kEs8311DacUnmute,
                                      s_line_out_volume) &&
        es8311_apply_drc_config() &&
        es8311_apply_daceq_coefficients();
    if (!ok) {
        Serial.printf("[ES8311] audio config apply failed: mic=0x%02X line_out=0x%02X hp_drive=%u drc=%u,%u,%u,%u ramp=%u eq_bypass=%u eq=%lu,%lu,%lu\n",
                      static_cast<unsigned>(s_mic_volume),
                      static_cast<unsigned>(s_line_out_volume),
                      s_hp_drive_enabled ? 1u : 0u,
                      s_drc_enabled ? 1u : 0u,
                      static_cast<unsigned>(s_drc_winsize),
                      static_cast<unsigned>(s_drc_maxlevel),
                      static_cast<unsigned>(s_drc_minlevel),
                      static_cast<unsigned>(s_dac_ramprate),
                      s_dac_eq_bypass ? 1u : 0u,
                      static_cast<unsigned long>(s_daceq_b0),
                      static_cast<unsigned long>(s_daceq_b1),
                      static_cast<unsigned long>(s_daceq_a1));
    }
    return ok;
}

int ES8311_GetSampleRate(void) {
    return kSampleRate;
}

size_t ES8311_GetFrameSamples(void) {
    return kFrameSamples;
}

bool ES8311_PlayTestTone(const uint32_t durationMs) {
    if (!ES8311_Init()) {
        return false;
    }

    const ES8311_AudioMode_t previous_mode = s_audio_mode;
    const bool was_passthrough_running = (s_passthrough_task != nullptr);
    if (was_passthrough_running) {
        es8311_stop_passthrough();
    }

    // test tone should be heard from speaker path
    es8311_apply_switch_level(ES8311_AUDIO_MODE_RECEIVE);

    static int16_t frame[kFrameSamples];
    size_t total_samples = (static_cast<size_t>(kSampleRate) * durationMs) / 1000u;
    if (total_samples == 0) {
        total_samples = kFrameSamples;
    }

    float phase = 0.0f;
    const float step = (kTwoPi * kToneFrequency) / static_cast<float>(kSampleRate);

    bool ok = true;
    size_t produced = 0;
    while (produced < total_samples) {
        const size_t samples_this = (total_samples - produced < kFrameSamples)
                                      ? (total_samples - produced)
                                      : kFrameSamples;

        for (size_t i = 0; i < samples_this; ++i) {
            const int16_t sample = static_cast<int16_t>(sinf(phase) * kToneAmplitude * static_cast<float>(INT16_MAX));
            frame[i] = sample;

            phase += step;
            if (phase >= kTwoPi) {
                phase -= kTwoPi;
            }
        }

        if (samples_this < kFrameSamples) {
            memset(frame + samples_this, 0, (kFrameSamples - samples_this) * sizeof(int16_t));
        }

        if (!i2s_write_frame(frame)) {
            ok = false;
            break;
        }

        produced += samples_this;
    }

    i2s_zero_dma_buffer(kI2sPort);

    if (was_passthrough_running && !es8311_start_passthrough()) {
        ok = false;
    }

    if (previous_mode != ES8311_AUDIO_MODE_RECEIVE) {
        es8311_apply_switch_level(previous_mode);
    }

    return ok;
}

bool ES8311_RecordMicAndPlayback(uint32_t durationMs) {
    if (!ES8311_Init()) {
        return false;
    }

    if (durationMs == 0U) {
        durationMs = 3000U;
    }

    size_t total_samples = (static_cast<size_t>(kSampleRate) * durationMs) / 1000U;
    if (total_samples == 0U) {
        total_samples = kFrameSamples;
    }

    int16_t *recorded = static_cast<int16_t *>(malloc(total_samples * sizeof(int16_t)));
    if (!recorded) {
        return false;
    }

    const ES8311_AudioMode_t previous_mode = s_audio_mode;
    const bool was_passthrough_running = (s_passthrough_task != nullptr);
    if (was_passthrough_running) {
        es8311_stop_passthrough();
    }

    bool ok = true;
    static int16_t frame[kFrameSamples];
    size_t captured = 0U;

    // Capture mic audio.
    es8311_apply_switch_level(ES8311_AUDIO_MODE_RECEIVE);
    i2s_zero_dma_buffer(kI2sPort);
    while (captured < total_samples) {
        if (!i2s_read_frame(frame)) {
            ok = false;
            break;
        }

        const size_t samples_this = ((total_samples - captured) < kFrameSamples)
                                        ? (total_samples - captured)
                                        : kFrameSamples;
        memcpy(recorded + captured, frame, samples_this * sizeof(int16_t));
        captured += samples_this;
    }

    // Playback captured audio to speaker.
    if (ok) {
        es8311_apply_switch_level(ES8311_AUDIO_MODE_RECEIVE);
        i2s_zero_dma_buffer(kI2sPort);

        size_t played = 0U;
        while (played < captured) {
            const size_t samples_this = ((captured - played) < kFrameSamples)
                                            ? (captured - played)
                                            : kFrameSamples;

            if (samples_this < kFrameSamples) {
                memcpy(frame, recorded + played, samples_this * sizeof(int16_t));
                memset(frame + samples_this, 0, (kFrameSamples - samples_this) * sizeof(int16_t));
            } else {
                memcpy(frame, recorded + played, kFrameBytes);
            }

            if (!i2s_write_frame(frame)) {
                ok = false;
                break;
            }
            played += samples_this;
        }

        i2s_zero_dma_buffer(kI2sPort);
    }

    free(recorded);

    if (was_passthrough_running && !es8311_start_passthrough()) {
        ok = false;
    }

    if (s_audio_mode != previous_mode) {
        es8311_apply_switch_level(previous_mode);
    }

    return ok;
}
