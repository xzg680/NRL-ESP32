#include "driver/es8311.h"

#include "driver/board_pins.h"
#include "driver/es7210.h"
#include "driver/i2c1.h"

#include <driver/gpio.h>
#include <driver/i2s_common.h>
#include <driver/i2s_std.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
#include "aec/aec_processor.h"
#include "driver/external_radio.h"
#endif

static const char *TAG = "ES8311";

#ifndef ES8311_FORCE_DAC_SILENCE
#define ES8311_FORCE_DAC_SILENCE 0
#endif

#ifndef ES8311_ENABLE_MIC_DEBUG_LOG
#define ES8311_ENABLE_MIC_DEBUG_LOG 0
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
constexpr int kSampleRate = 16000;
constexpr int kMclkRate = kSampleRate * 256;
constexpr size_t kFrameSamples = 160;
constexpr size_t kNetworkFrameSamples = kFrameSamples / 2u;
// I2S bus stays stereo (2 slots/frame) so BCLK timing matches ES8311's clock
// divider expectations. ES8311 is mono internally �?it reads/writes the LEFT
// slot only. We duplicate mono content into both slots in i2s_write_frame.
constexpr size_t kI2sSlotCount = 2;
constexpr size_t kFrameBytes = kFrameSamples * sizeof(int16_t);
constexpr size_t kI2sFrameBytes = kFrameSamples * kI2sSlotCount * sizeof(int16_t);
constexpr uint32_t kI2sWaitMs = 20;

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
    ES8311_REG18_ADC = 0x18,
    ES8311_REG19_ADC = 0x19,
    ES8311_REG1A_ADC = 0x1A,
    ES8311_REG1B_ADC = 0x1B,
    ES8311_REG1C_ADC = 0x1C,
    ES8311_REG1D_ADCEQ = 0x1D,
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
constexpr uint8_t kDefaultAdcRamprate = 0x04;
constexpr uint8_t kEs8311AdcVolumeDefault = 0xBF;  // reference es8311_start: REG17 = 0xBF
constexpr uint8_t kEs8311DacVolumeDefault = 180U;
// REG0D bits: PDN_ANA(7) PDN_IBIASGEN(6) PDN_ADCBIASGEN(5) PDN_ADCVERFGEN(4)
//             PDN_DACVREFGEN(3) PDN_VREF(2) VMIDSEL(1:0)
// NOTE: PDN_VREF (bit 2) has REVERSE polarity vs other PDN bits!
//   0 = disable internal reference (BAD), 1 = enable reference
// VMIDSEL: 0=down, 1=startup normal charge, 2=normal operation, 3=startup fast.
// 0x06 = all analog/bias/vref blocks enabled + VREF on + VMID in normal
// operation. Earlier 0x01 left VREF disabled and VMID in startup-charge mode,
// which starved the ADC/DAC modulators of a stable reference — that is the
// root cause of the high mic noise floor and random DC offset / distortion.
constexpr uint8_t kEs8311PowerUpAnalog = 0x06;
constexpr uint8_t kEs8311PowerUpPgaAdc = 0x02;
constexpr uint8_t kEs8311PowerUpDac = 0x00;
constexpr uint8_t kEs8311OutputDriveLine = 0x00; // REG13: HPSW=0 bit6=0
constexpr uint8_t kEs8311OutputDriveHp = 0x10;   // REG13: HPSW=1 bit6=0
constexpr uint8_t kEs8311AdcEqBypass = 0x6A;
constexpr uint8_t kDefaultDrcWinsize = 0x00;
constexpr uint8_t kDefaultDrcLevel = 0x00;
constexpr uint8_t kDefaultDacRamprate = 0x00;
constexpr uint32_t kDacEqCoefficientMask = 0x3FFFFFFFUL;
constexpr uint32_t kAdcEqCoefficientMask = 0x3FFFFFFFUL;
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
static i2s_chan_handle_t s_i2s_tx = nullptr;
static i2s_chan_handle_t s_i2s_rx = nullptr;
static TaskHandle_t s_passthrough_task = nullptr;
static volatile bool s_passthrough_running = false;
static ES8311_AudioMode_t s_audio_mode = ES8311_AUDIO_MODE_RECEIVE;
static ES8311_FrameHook_t s_frame_hook = nullptr;
static void *s_frame_hook_user_data = nullptr;

constexpr size_t kOutputQueueSamples = kNetworkFrameSamples * 64u;
static int16_t s_output_queue[kOutputQueueSamples];
static size_t s_output_queue_head = 0;
static size_t s_output_queue_tail = 0;
static size_t s_output_queue_count = 0;
static SemaphoreHandle_t s_output_queue_mutex = nullptr;
static uint32_t s_last_output_queue_log_ms = 0;
static volatile uint8_t s_aec_reference_source = 0; // 0=network playback, 1=second mic
static constexpr size_t kAecNetworkRefDelayFrames = 12; // ~120 ms at 160 samples/frame
static int16_t s_aec_network_ref[kFrameSamples * kAecNetworkRefDelayFrames];
static size_t s_aec_network_ref_head = 0;
static size_t s_aec_network_ref_fill = 0;
static uint8_t s_mic_volume = kEs8311AdcVolumeDefault;
static uint8_t s_line_out_volume = kEs8311DacVolumeDefault;
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
static bool s_adc_dmic_enabled = false;
static bool s_adc_linsel = true;
static uint8_t s_adc_pga_gain = 10U;
static uint8_t s_adc_ramprate = kDefaultAdcRamprate;
static bool s_adc_dmic_sense = false;
static bool s_adc_sync = true;
static bool s_adc_inv = false;
static bool s_adc_ramclr = false;
static uint8_t s_adc_scale = 4U;
static bool s_alc_enabled = false;
static bool s_adc_automute_enabled = false;
static uint8_t s_alc_winsize = 0U;
static uint8_t s_alc_maxlevel = 0U;
static uint8_t s_alc_minlevel = 0U;
static uint8_t s_adc_automute_winsize = 0U;
static uint8_t s_adc_automute_noise_gate = 0U;
static uint8_t s_adc_automute_volume = 0U;
static uint8_t s_adc_hpfs1 = 10U;
static bool s_adc_eq_bypass = true;
static bool s_adc_hpf = true;
static uint8_t s_adc_hpfs2 = 10U;
static uint32_t s_adceq_b0 = 0U;
static uint32_t s_adceq_a1 = 0U;
static uint32_t s_adceq_a2 = 0U;
static uint32_t s_adceq_b1 = 0U;
static uint32_t s_adceq_b2 = 0U;

// Software 4th-order IIR high-pass filter (two RBJ biquads, Direct Form I) on
// captured mic frames:
//   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
// Coefficients are pre-computed for a 4th-order Butterworth response, cutoff
// fc = 200 Hz, sample rate fs = 16000 Hz. -24 dB/oct rolloff below fc,
// unity passband. See RBJ "Audio EQ Cookbook" — these are the normalized
// values (after dividing the raw cookbook formulas by a0 = 1 + alpha).
//   omega = 2*pi*fc/fs       = pi/40  ~= 0.07854
//   cos(omega)               ~= 0.996917
//   Q1 ~= 0.541196, Q2 ~= 1.306563
// Filter is enabled/disabled at runtime via the web/AT "mic_hpf_enabled"
// flag; state is reset on each toggle so the first frame after enabling
// doesn't carry a spike from stale x1/x2/y1/y2.
constexpr float kMicHpf1B0 =  0.93097528f;
constexpr float kMicHpf1B1 = -1.86195056f;
constexpr float kMicHpf1B2 =  0.93097528f;
constexpr float kMicHpf1A1 = -1.85907624f;
constexpr float kMicHpf1A2 =  0.86482488f;
constexpr float kMicHpf2B0 =  0.96935382f;
constexpr float kMicHpf2B1 = -1.93870765f;
constexpr float kMicHpf2B2 =  0.96935382f;
constexpr float kMicHpf2A1 = -1.93571484f;
constexpr float kMicHpf2A2 =  0.94170045f;
static volatile bool s_mic_hpf_enabled = false;
static float s_mic_hpf1_x1 = 0.0f;
static float s_mic_hpf1_x2 = 0.0f;
static float s_mic_hpf1_y1 = 0.0f;
static float s_mic_hpf1_y2 = 0.0f;
static float s_mic_hpf2_x1 = 0.0f;
static float s_mic_hpf2_x2 = 0.0f;
static float s_mic_hpf2_y1 = 0.0f;
static float s_mic_hpf2_y2 = 0.0f;

static inline void mic_hpf_reset(void) {
    s_mic_hpf1_x1 = 0.0f;
    s_mic_hpf1_x2 = 0.0f;
    s_mic_hpf1_y1 = 0.0f;
    s_mic_hpf1_y2 = 0.0f;
    s_mic_hpf2_x1 = 0.0f;
    s_mic_hpf2_x2 = 0.0f;
    s_mic_hpf2_y1 = 0.0f;
    s_mic_hpf2_y2 = 0.0f;
}

static inline void mic_hpf_apply(int16_t *frame, const size_t count) {
    if (!s_mic_hpf_enabled || frame == nullptr) {
        return;
    }
    float x11 = s_mic_hpf1_x1;
    float x12 = s_mic_hpf1_x2;
    float y11 = s_mic_hpf1_y1;
    float y12 = s_mic_hpf1_y2;
    float x21 = s_mic_hpf2_x1;
    float x22 = s_mic_hpf2_x2;
    float y21 = s_mic_hpf2_y1;
    float y22 = s_mic_hpf2_y2;
    for (size_t i = 0; i < count; ++i) {
        const float x = static_cast<float>(frame[i]);
        const float y_stage1 = kMicHpf1B0 * x + kMicHpf1B1 * x11 + kMicHpf1B2 * x12
                               - kMicHpf1A1 * y11 - kMicHpf1A2 * y12;
        x12 = x11;
        x11 = x;
        y12 = y11;
        y11 = y_stage1;

        const float y = kMicHpf2B0 * y_stage1 + kMicHpf2B1 * x21 + kMicHpf2B2 * x22
                        - kMicHpf2A1 * y21 - kMicHpf2A2 * y22;
        x22 = x21;
        x21 = y_stage1;
        y22 = y21;
        y21 = y;
        int32_t out = static_cast<int32_t>(y);
        if (out > INT16_MAX) { out = INT16_MAX; }
        else if (out < INT16_MIN) { out = INT16_MIN; }
        frame[i] = static_cast<int16_t>(out);
    }
    s_mic_hpf1_x1 = x11;
    s_mic_hpf1_x2 = x12;
    s_mic_hpf1_y1 = y11;
    s_mic_hpf1_y2 = y12;
    s_mic_hpf2_x1 = x21;
    s_mic_hpf2_x2 = x22;
    s_mic_hpf2_y1 = y21;
    s_mic_hpf2_y2 = y22;
}

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
        ESP_LOGI(TAG,"[ES8311] I2C write failed: reg=0x%02X value=0x%02X attempt=%d\n",
                      static_cast<unsigned>(reg),
                      static_cast<unsigned>(value),
                      attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(1));
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
        vTaskDelay(pdMS_TO_TICKS(1));
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

static uint8_t es8311_adc_reg14(void)
{
    return static_cast<uint8_t>((s_adc_dmic_enabled ? 0x40u : 0x00u) |
                                (s_adc_linsel ? 0x10u : 0x00u) |
                                (s_adc_pga_gain & 0x0Fu));
}

static uint8_t es8311_adc_reg15(void)
{
    return static_cast<uint8_t>(((s_adc_ramprate & 0x0Fu) << 4) |
                                (s_adc_dmic_sense ? 0x01u : 0x00u));
}

static uint8_t es8311_adc_reg16(void)
{
    return static_cast<uint8_t>((s_adc_sync ? 0x20u : 0x00u) |
                                (s_adc_inv ? 0x10u : 0x00u) |
                                (s_adc_ramclr ? 0x08u : 0x00u) |
                                (s_adc_scale & 0x07u));
}

static uint8_t es8311_adc_reg18(void)
{
    return static_cast<uint8_t>((s_alc_enabled ? 0x80u : 0x00u) |
                                (s_adc_automute_enabled ? 0x40u : 0x00u) |
                                (s_alc_winsize & 0x0Fu));
}

static uint8_t es8311_adc_reg19(void)
{
    return static_cast<uint8_t>(((s_alc_maxlevel & 0x0Fu) << 4) |
                                (s_alc_minlevel & 0x0Fu));
}

static uint8_t es8311_adc_reg1a(void)
{
    return static_cast<uint8_t>(((s_adc_automute_winsize & 0x0Fu) << 4) |
                                (s_adc_automute_noise_gate & 0x0Fu));
}

static uint8_t es8311_adc_reg1b(void)
{
    return static_cast<uint8_t>(((s_adc_automute_volume & 0x07u) << 5) |
                                (s_adc_hpfs1 & 0x1Fu));
}

static uint8_t es8311_adc_reg1c(void)
{
    return static_cast<uint8_t>((s_adc_eq_bypass ? 0x40u : 0x00u) |
                                (s_adc_hpf ? 0x20u : 0x00u) |
                                (s_adc_hpfs2 & 0x1Fu));
}

static bool es8311_write_adceq_coeff(const uint8_t first_reg, const uint32_t value)
{
    const uint32_t coeff = value & kAdcEqCoefficientMask;
    return es8311_write_reg(first_reg, static_cast<uint8_t>((coeff >> 24) & 0x3Fu)) &&
           es8311_write_reg(static_cast<uint8_t>(first_reg + 1U), static_cast<uint8_t>((coeff >> 16) & 0xFFu)) &&
           es8311_write_reg(static_cast<uint8_t>(first_reg + 2U), static_cast<uint8_t>((coeff >> 8) & 0xFFu)) &&
           es8311_write_reg(static_cast<uint8_t>(first_reg + 3U), static_cast<uint8_t>(coeff & 0xFFu));
}

static bool es8311_apply_adc_config(void)
{
    return es8311_write_reg(ES8311_REG14_SYSTEM, es8311_adc_reg14()) &&
           es8311_write_reg(ES8311_REG15_ADC, es8311_adc_reg15()) &&
           es8311_write_reg(ES8311_REG16_ADC, es8311_adc_reg16()) &&
           es8311_write_reg(ES8311_REG18_ADC, es8311_adc_reg18()) &&
           es8311_write_reg(ES8311_REG19_ADC, es8311_adc_reg19()) &&
           es8311_write_reg(ES8311_REG1A_ADC, es8311_adc_reg1a()) &&
           es8311_write_reg(ES8311_REG1B_ADC, es8311_adc_reg1b()) &&
           es8311_write_reg(ES8311_REG1C_ADC, es8311_adc_reg1c()) &&
           es8311_write_adceq_coeff(ES8311_REG1D_ADCEQ, s_adceq_b0) &&
           es8311_write_adceq_coeff(static_cast<uint8_t>(ES8311_REG1D_ADCEQ + 4U), s_adceq_a1) &&
           es8311_write_adceq_coeff(static_cast<uint8_t>(ES8311_REG1D_ADCEQ + 8U), s_adceq_a2) &&
           es8311_write_adceq_coeff(static_cast<uint8_t>(ES8311_REG1D_ADCEQ + 12U), s_adceq_b1) &&
           es8311_write_adceq_coeff(static_cast<uint8_t>(ES8311_REG1D_ADCEQ + 16U), s_adceq_b2);
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
            ESP_LOGI(TAG,"[ES8311] REG0D pre-power failed");
            return false;
        }
    }

    if (!es8311_write_reg(ES8311_REG44_GPIO, 0x08)) {
        ESP_LOGI(TAG,"[ES8311] REG44 (1) failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG44_GPIO, 0x08)) {
        ESP_LOGI(TAG,"[ES8311] REG44 (2) failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG01_CLK_MANAGER, 0x30)) {
        ESP_LOGI(TAG,"[ES8311] REG01 init failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG02_CLK_MANAGER, 0x00)) {
        ESP_LOGI(TAG,"[ES8311] REG02 init failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG03_CLK_MANAGER, 0x10)) {
        ESP_LOGI(TAG,"[ES8311] REG03 init failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG16_ADC, es8311_adc_reg16())) {
        ESP_LOGI(TAG,"[ES8311] REG16 init failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG04_CLK_MANAGER, 0x10)) {
        ESP_LOGI(TAG,"[ES8311] REG04 init failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG05_CLK_MANAGER, 0x00)) {
        ESP_LOGI(TAG,"[ES8311] REG05 init failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG0B_SYSTEM, 0x00)) {
        ESP_LOGI(TAG,"[ES8311] REG0B failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG0C_SYSTEM, 0x00)) {
        ESP_LOGI(TAG,"[ES8311] REG0C failed");
        return false;
    }
    // Analog bias (Espressif known-good values)
    if (!es8311_write_reg(ES8311_REG10_SYSTEM, 0x1F)) {
        ESP_LOGI(TAG,"[ES8311] REG10 bias config failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG11_SYSTEM, 0x7F)) {
        ESP_LOGI(TAG,"[ES8311] REG11 bias config failed");
        return false;
    }
    // Let the bias generators settle before powering up the rest of the
    // analog block. Without this delay the ADC sees transient bias and
    // produces an audible "settling" hiss for the first second.
    vTaskDelay(pdMS_TO_TICKS(40));

    if (!es8311_write_reg(ES8311_REG00_RESET, 0x80)) {
        ESP_LOGI(TAG,"[ES8311] REG00 reset/slave failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG01_CLK_MANAGER, 0x3F)) {
        ESP_LOGI(TAG,"[ES8311] REG01 use_mclk failed");
        return false;
    }

    if (es8311_read_reg(ES8311_REG06_CLK_MANAGER, &regv)) {
        regv = static_cast<uint8_t>(regv & ~0x20u);
        if (!es8311_write_reg(ES8311_REG06_CLK_MANAGER, regv)) {
            ESP_LOGI(TAG,"[ES8311] REG06 sclk polarity failed");
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
        ESP_LOGI(TAG,"[ES8311] clock configuration failed");
        return false;
    }

    if (!es8311_config_i2s_format(16)) {
        ESP_LOGI(TAG,"[ES8311] I2S format configuration failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG13_SYSTEM, es8311_output_drive_reg())) {
        ESP_LOGI(TAG,"[ES8311] REG13 failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG1B_ADC, es8311_adc_reg1b())) {
        ESP_LOGI(TAG,"[ES8311] REG1B (ADC HPF) failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG1C_ADC, es8311_adc_reg1c())) {
        ESP_LOGI(TAG,"[ES8311] REG1C (ADC EQ) failed");
        return false;
    }
    // REG44 ADCDAT_SEL (bits 6:4): 0 = "ADC + ADC" duplicates the mic on
    // both I2S slots (default). The earlier 0x58 value selected
    // "ADC + DACR", which fed the DAC output back onto the right I2S slot
    // as an AEC reference — but the current board variants run AFE as a
    // single-mic path unless a true playback reference is explicitly wired,
    // and routing DAC content onto a mic slot only invites
    // crosstalk. Keep bit3 (I2C_WL, "internal use") set, matching the
    // earlier 0x08 writes from the init sequence.
    if (!es8311_write_reg(ES8311_REG44_GPIO, 0x08)) {
        ESP_LOGI(TAG,"[ES8311] REG44 final failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG00_RESET, 0x80)) {
        ESP_LOGI(TAG,"[ES8311] REG00 start failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG01_CLK_MANAGER, 0x3F)) {
        ESP_LOGI(TAG,"[ES8311] REG01 start clock failed");
        return false;
    }

    uint8_t dac_iface = 0u;
    uint8_t adc_iface = 0u;
    if (!es8311_read_reg(ES8311_REG09_SDPIN, &dac_iface) ||
        !es8311_read_reg(ES8311_REG0A_SDPOUT, &adc_iface)) {
        ESP_LOGI(TAG,"[ES8311] interface readback failed");
        return false;
    }
    dac_iface = static_cast<uint8_t>(dac_iface & ~0x40u);
    adc_iface = static_cast<uint8_t>(adc_iface & ~0x40u);
    if (!es8311_write_reg(ES8311_REG09_SDPIN, dac_iface) ||
        !es8311_write_reg(ES8311_REG0A_SDPOUT, adc_iface)) {
        ESP_LOGI(TAG,"[ES8311] interface enable failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG17_ADC, s_mic_volume)) {
        ESP_LOGI(TAG,"[ES8311] REG17 ADC volume failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG0E_SYSTEM, kEs8311PowerUpPgaAdc)) {
        ESP_LOGI(TAG,"[ES8311] REG0E power-up PGA/ADC failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG12_SYSTEM, kEs8311PowerUpDac)) {
        ESP_LOGI(TAG,"[ES8311] REG12 enable DAC failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG14_SYSTEM, es8311_adc_reg14())) {
        ESP_LOGI(TAG,"[ES8311] REG14 analog mic PGA failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG0D_SYSTEM, kEs8311PowerUpAnalog)) {
        ESP_LOGI(TAG,"[ES8311] REG0D power-up analog failed");
        return false;
    }
    // VMID needs ~tens of ms to charge up after enabling the analog block.
    // Apply ADC/DAC config after VMID is stable, otherwise the first frames
    // captured by the ADC carry a sliding DC bias that sounds like a
    // low-frequency "thump" plus elevated noise.
    vTaskDelay(pdMS_TO_TICKS(40));
    if (!es8311_apply_adc_config()) {
        ESP_LOGI(TAG,"[ES8311] REG15 ADC ramp rate failed");
        return false;
    }
    if (!es8311_apply_drc_config() || !es8311_apply_daceq_coefficients()) {
        ESP_LOGI(TAG,"[ES8311] DAC DRC/EQ config failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG45_GP, 0x00)) {
        ESP_LOGI(TAG,"[ES8311] REG45 GP control failed");
        return false;
    }

    if (!es8311_write_reg(ES8311_REG31_DAC, kEs8311DacUnmute)) {
        ESP_LOGI(TAG,"[ES8311] REG31 unmute failed");
        return false;
    }
    if (!es8311_write_reg(ES8311_REG32_DAC, s_line_out_volume)) {
        ESP_LOGI(TAG,"[ES8311] REG32 DAC volume failed");
        return false;
    }

    // ---- Debug: read back ADC/mic-path registers to verify the codec
    //      actually accepted the values. If the mic is silent, compare
    //      these against the expected values printed alongside.
    {
        struct RegCheck { uint8_t reg; const char *name; uint8_t expect; };
        const RegCheck checks[] = {
            { ES8311_REG0A_SDPOUT, "0A SDPOUT(ADC fmt)", 0x0C },
            { ES8311_REG0D_SYSTEM, "0D PDN_ANA",         kEs8311PowerUpAnalog },
            { ES8311_REG0E_SYSTEM, "0E PDN_PGA/ADC",     kEs8311PowerUpPgaAdc },
            { ES8311_REG14_SYSTEM, "14 mic PGA/select",  es8311_adc_reg14() },
            { ES8311_REG15_ADC,    "15 ADC ramp",        es8311_adc_reg15() },
            { ES8311_REG16_ADC,    "16 ADC gain",        es8311_adc_reg16() },
            { ES8311_REG17_ADC,    "17 ADC volume",      s_mic_volume },
            { ES8311_REG1B_ADC,    "1B ADC HPF",         es8311_adc_reg1b() },
            { ES8311_REG1C_ADC,    "1C ADC EQ",          es8311_adc_reg1c() },
        };
        for (const RegCheck &c : checks) {
            uint8_t v = 0;
            const bool ok = es8311_read_reg(c.reg, &v);
            ESP_LOGI(TAG,"[ES8311] REG%-22s = %s0x%02X (expect 0x%02X)%s\n",
                          c.name,
                          ok ? "" : "READ-ERR ",
                          static_cast<unsigned>(v),
                          static_cast<unsigned>(c.expect),
                          (ok && v == c.expect) ? "" : "  <-- MISMATCH");
        }
    }

    return true;
}

static void i2s_teardown(void) {
    if (s_i2s_tx != nullptr) {
        (void)i2s_channel_disable(s_i2s_tx);
        (void)i2s_del_channel(s_i2s_tx);
        s_i2s_tx = nullptr;
    }

    if (s_i2s_rx != nullptr) {
        (void)i2s_channel_disable(s_i2s_rx);
        (void)i2s_del_channel(s_i2s_rx);
        s_i2s_rx = nullptr;
    }

    s_i2s_ready = false;
    s_i2s_driver_installed = false;
}

static void i2s_clear_dma(void) {
    if (s_i2s_tx == nullptr) {
        return;
    }

    int16_t silence[kFrameSamples * kI2sSlotCount] = {};
    for (int i = 0; i < 2; ++i) {
        size_t bytes_written = 0;
        (void)i2s_channel_write(s_i2s_tx, silence, sizeof(silence), &bytes_written, kI2sWaitMs);
    }
}

static bool i2s_setup(void) {
    if (s_i2s_ready) {
        return true;
    }

    if (s_i2s_driver_installed || s_i2s_tx != nullptr || s_i2s_rx != nullptr) {
        i2s_teardown();
    }

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(kI2sPort, I2S_ROLE_MASTER);
    // Keep only a few 10 ms DMA frames queued. Larger rings hide scheduling
    // hiccups by building capture latency that can keep growing under load.
    channel_config.dma_desc_num = 3;
    channel_config.dma_frame_num = kFrameSamples;
    channel_config.auto_clear_after_cb = true;

    esp_err_t err = i2s_new_channel(&channel_config, &s_i2s_tx, &s_i2s_rx);
    if (err != ESP_OK) {
        ESP_LOGI(TAG,"[ES8311] i2s_new_channel failed: err=%d\n", static_cast<int>(err));
        i2s_teardown();
        return false;
    }
    s_i2s_driver_installed = true;

    i2s_std_config_t std_config = {};
    std_config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    std_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
    std_config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_config.gpio_cfg.mclk = static_cast<gpio_num_t>(kPinMclk);
    std_config.gpio_cfg.bclk = static_cast<gpio_num_t>(kPinBclk);
    std_config.gpio_cfg.ws = static_cast<gpio_num_t>(kPinLrclk);
    std_config.gpio_cfg.dout = static_cast<gpio_num_t>(kPinEspDout);
    std_config.gpio_cfg.din = static_cast<gpio_num_t>(kPinEspDin);
    std_config.gpio_cfg.invert_flags.mclk_inv = false;
    std_config.gpio_cfg.invert_flags.bclk_inv = false;
    std_config.gpio_cfg.invert_flags.ws_inv = false;

    err = i2s_channel_init_std_mode(s_i2s_tx, &std_config);
    if (err != ESP_OK) {
        ESP_LOGI(TAG,"[ES8311] i2s tx std init failed: err=%d\n", static_cast<int>(err));
        i2s_teardown();
        return false;
    }

    err = i2s_channel_init_std_mode(s_i2s_rx, &std_config);
    if (err != ESP_OK) {
        ESP_LOGI(TAG,"[ES8311] i2s rx std init failed: err=%d\n", static_cast<int>(err));
        i2s_teardown();
        return false;
    }

    err = i2s_channel_enable(s_i2s_tx);
    if (err != ESP_OK) {
        ESP_LOGI(TAG,"[ES8311] i2s tx enable failed: err=%d\n", static_cast<int>(err));
        i2s_teardown();
        return false;
    }

    err = i2s_channel_enable(s_i2s_rx);
    if (err != ESP_OK) {
        ESP_LOGI(TAG,"[ES8311] i2s rx enable failed: err=%d\n", static_cast<int>(err));
        i2s_teardown();
        return false;
    }

    s_i2s_ready = true;
    i2s_clear_dma();
    ESP_LOGI(TAG,"[ES8311] i2s std clocks: rate=%dHz bits=%d stereo mclk=%dHz\n",
                  kSampleRate,
                  16,
                  kMclkRate);
    return true;
}

static bool i2s_read_frame(int16_t *dst, int16_t *dst_ref = nullptr) {
    if (s_i2s_rx == nullptr) {
        return false;
    }

    // Stereo I2S: 2 int16 per LRCK frame (L,R). ES8311 outputs ADC data on
    // LEFT only �?extract LEFT samples into dst.
    static_assert(kI2sSlotCount == 2, "i2s_read_frame assumes stereo I2S frame");
    int16_t raw[kFrameSamples * kI2sSlotCount];  // size = 160*2 = 320
    size_t bytes_in_frame = 0;
    while (bytes_in_frame < kI2sFrameBytes) {
        size_t bytes_read = 0;
        if (i2s_channel_read(s_i2s_rx,
                             reinterpret_cast<uint8_t *>(raw) + bytes_in_frame,
                             kI2sFrameBytes - bytes_in_frame,
                             &bytes_read,
                             kI2sWaitMs) != ESP_OK) {
            return false;
        }

        if (bytes_read == 0) {
            vTaskDelay(1);
            continue;
        }

        bytes_in_frame += bytes_read;
    }

#if ES8311_ENABLE_MIC_DEBUG_LOG
    // Debug: every ~5 s, compare LEFT vs RIGHT slot energy and dump the first
    // few raw samples. ES8311 should drive ADC data on the LEFT slot only; if
    // RIGHT has the energy instead, the slot mapping / I2S format is wrong.
    {
        static uint32_t last_dump_ms = 0;
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if ((now - last_dump_ms) >= 5000u) {
            last_dump_ms = now;
            int32_t left_peak = 0;
            int32_t right_peak = 0;
            for (size_t i = 0; i < kFrameSamples; ++i) {
                const int32_t l = abs(static_cast<int32_t>(raw[i * 2]));
                const int32_t r = abs(static_cast<int32_t>(raw[i * 2 + 1]));
                if (l > left_peak) { left_peak = l; }
                if (r > right_peak) { right_peak = r; }
            }
            ESP_LOGI(TAG,"[ES8311][MIC] raw I2S slots: LEFT peak=%ld RIGHT peak=%ld | "
                          "raw[0..7]=%d,%d %d,%d %d,%d %d,%d\n",
                          static_cast<long>(left_peak),
                          static_cast<long>(right_peak),
                          raw[0], raw[1], raw[2], raw[3],
                          raw[4], raw[5], raw[6], raw[7]);
        }
    }
#endif

    // Take LEFT slot (mic). When dst_ref is given, also take the RIGHT slot,
    // which is only used when a board-specific AEC reference is compiled in.
    for (size_t i = 0; i < kFrameSamples; ++i) {
        dst[i] = raw[i * 2];
        if (dst_ref != nullptr) {
            dst_ref[i] = raw[i * 2 + 1];
        }
    }
    return true;
}

static bool i2s_write_frame(const int16_t *src) {
    if (s_i2s_tx == nullptr) {
        return false;
    }

    // Stereo I2S: 2 int16 per LRCK frame. Duplicate mono src into both slots.
    static_assert(kI2sSlotCount == 2, "i2s_write_frame assumes stereo I2S frame");
    int16_t raw[kFrameSamples * kI2sSlotCount];  // size = 160*2 = 320
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
        if (i2s_channel_write(s_i2s_tx,
                              reinterpret_cast<const uint8_t *>(raw) + bytes_out_frame,
                              kI2sFrameBytes - bytes_out_frame,
                              &bytes_written,
                              kI2sWaitMs) != ESP_OK) {
            return false;
        }

        if (bytes_written == 0) {
            vTaskDelay(1);
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

static void aec_network_ref_clear(void) {
    memset(s_aec_network_ref, 0, sizeof(s_aec_network_ref));
    s_aec_network_ref_head = 0;
    s_aec_network_ref_fill = 0;
}

static void aec_network_ref_read(int16_t *dst, const size_t sample_count) {
    if (dst == nullptr || sample_count == 0) {
        return;
    }
    if (sample_count != kFrameSamples ||
        s_aec_network_ref_fill < kAecNetworkRefDelayFrames) {
        memset(dst, 0, sample_count * sizeof(int16_t));
        return;
    }
    memcpy(dst,
           s_aec_network_ref + (s_aec_network_ref_head * kFrameSamples),
           kFrameSamples * sizeof(int16_t));
}

static void aec_network_ref_push(const int16_t *src, const size_t sample_count) {
    if (src == nullptr || sample_count != kFrameSamples) {
        return;
    }
    memcpy(s_aec_network_ref + (s_aec_network_ref_head * kFrameSamples),
           src,
           kFrameSamples * sizeof(int16_t));
    s_aec_network_ref_head = (s_aec_network_ref_head + 1u) % kAecNetworkRefDelayFrames;
    if (s_aec_network_ref_fill < kAecNetworkRefDelayFrames) {
        ++s_aec_network_ref_fill;
    }
}

static void upsample_8k_to_16k_frame(const int16_t *src8, int16_t *dst16) {
    if (src8 == nullptr || dst16 == nullptr) {
        return;
    }
    for (size_t i = 0; i < kNetworkFrameSamples; ++i) {
        const int16_t current = src8[i];
        const int16_t next = (i + 1u < kNetworkFrameSamples) ? src8[i + 1u] : current;
        dst16[i * 2u] = current;
        dst16[i * 2u + 1u] = static_cast<int16_t>(
            (static_cast<int32_t>(current) + static_cast<int32_t>(next)) / 2);
    }
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
    aec_network_ref_clear();
    xSemaphoreGive(s_output_queue_mutex);
}

static void es8311_log_mic_frame_stats(const int16_t *frame) {
#if ES8311_ENABLE_MIC_DEBUG_LOG
    // Aggregate peak / RMS / non-zero count over ~1 s of captured frames and
    // print a single summary line. Lets you see at a glance whether the ADC
    // is returning real audio or a flat (all-zero / stuck-DC) signal.
    static uint32_t window_start_ms = 0;
    static uint32_t frame_count = 0;
    static int16_t window_peak = 0;
    static int16_t window_min = 0;
    static int16_t window_max = 0;
    static uint64_t window_sum_sq = 0;
    static uint32_t window_samples = 0;
    static uint32_t window_nonzero = 0;

    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (window_start_ms == 0) {
        window_start_ms = now;
        window_min = frame[0];
        window_max = frame[0];
    }

    for (size_t i = 0; i < kFrameSamples; ++i) {
        const int16_t s = frame[i];
        const int32_t mag = (s < 0) ? -static_cast<int32_t>(s) : static_cast<int32_t>(s);
        if (mag > window_peak) {
            window_peak = static_cast<int16_t>(mag);
        }
        if (s < window_min) { window_min = s; }
        if (s > window_max) { window_max = s; }
        if (s != 0) { ++window_nonzero; }
        window_sum_sq += static_cast<uint64_t>(static_cast<int32_t>(s) * static_cast<int32_t>(s));
        ++window_samples;
    }
    ++frame_count;

    if ((now - window_start_ms) >= 1000u && window_samples > 0) {
        const uint32_t rms = static_cast<uint32_t>(
            sqrt(static_cast<double>(window_sum_sq) / static_cast<double>(window_samples)));
        ESP_LOGI(TAG,"[ES8311][MIC] frames=%lu samples=%lu peak=%d rms=%lu "
                      "min=%d max=%d nonzero=%lu/%lu%s\n",
                      static_cast<unsigned long>(frame_count),
                      static_cast<unsigned long>(window_samples),
                      static_cast<int>(window_peak),
                      static_cast<unsigned long>(rms),
                      static_cast<int>(window_min),
                      static_cast<int>(window_max),
                      static_cast<unsigned long>(window_nonzero),
                      static_cast<unsigned long>(window_samples),
                      (window_peak == 0) ? "  <-- SILENT (ADC all zero)" : "");

        window_start_ms = now;
        frame_count = 0;
        window_peak = 0;
        window_min = frame[0];
        window_max = frame[0];
        window_sum_sq = 0;
        window_samples = 0;
        window_nonzero = 0;
    }
#else
    (void)frame;
#endif
}

#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
// Sink for echo-cancelled audio from the AEC processor: forward it to the
// registered frame hook, exactly as raw mic capture would have been.
static void es8311_aec_output(const int16_t *clean, size_t count, void *) {
    if (AEC_IsRuntimeActive() && s_frame_hook != nullptr) {
        s_frame_hook(clean, count, s_audio_mode, s_frame_hook_user_data);
    }
}
#endif

static void es8311_passthrough_task(void *) {
    static int16_t frame[kFrameSamples];
    static int16_t playback_8k[kNetworkFrameSamples];
    static int16_t playback_frame[kFrameSamples];

    while (s_passthrough_running) {
        const ES8311_AudioMode_t mode = s_audio_mode;

#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
        static int16_t ref_frame[kFrameSamples];
        static int16_t network_ref_frame[kFrameSamples];
        // The software HPF is a local passthrough filter. Do not use this
        // switch to decide whether esp-sr should own the uplink; otherwise
        // enabling the HPF can accidentally route BH4TDV mic audio into the
        // AFE and silence the stream if the AFE is not producing frames.
        const bool software_filter_enabled = s_mic_hpf_enabled;
        const bool afe_ready = AEC_IsReady();
        const bool processed_route = AEC_IsRuntimeActive();
        const bool needs_ref = afe_ready && AEC_UsesReference();
        if (!i2s_read_frame(frame, needs_ref ? ref_frame : nullptr)) {
            ESP_LOGI(TAG,"[ES8311][MIC] i2s_read_frame failed");
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        // Log raw mic energy first so the debug stats reflect what the ADC
        // actually saw (helps diagnose whether elevated noise is upstream of
        // the software HPF or not). Then apply the HPF, so AEC and the
        // network uplink both see the filtered signal.
        es8311_log_mic_frame_stats(frame);
        if (software_filter_enabled) {
            mic_hpf_apply(frame, kFrameSamples);
        }

        if (afe_ready) {
            const int16_t *ref = nullptr;
            if (needs_ref) {
                if (s_aec_reference_source == 1u) {
                    ref = ref_frame;
                } else {
                    aec_network_ref_read(network_ref_frame, kFrameSamples);
                    ref = network_ref_frame;
                }
            }
            // mic = LEFT slot; reference is either RIGHT slot or delayed DAC playback.
            // The processed result returns via es8311_aec_output.
            AEC_SubmitCapture(frame, ref, kFrameSamples);
        }
        if (!processed_route && s_frame_hook != nullptr) {
            s_frame_hook(frame, kFrameSamples, mode, s_frame_hook_user_data);
        }
#else
        if (!i2s_read_frame(frame)) {
            ESP_LOGI(TAG,"[ES8311][MIC] i2s_read_frame failed");
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        es8311_log_mic_frame_stats(frame);
        mic_hpf_apply(frame, kFrameSamples);

        if (s_frame_hook != nullptr) {
            s_frame_hook(frame, kFrameSamples, mode, s_frame_hook_user_data);
        }
#endif

        // ADC and DAC are physically independent paths in this hardware.
        // Do NOT echo captured ADC audio back to DAC �?that would re-transmit
        // received radio audio out the TO_MIC line.
        // In RX mode, DAC plays whatever is in the output queue (NRL downlink).
        // If the queue is empty, write silence so the DAC stays at VMID.
        const size_t popped = output_queue_pop_frame(playback_8k, kNetworkFrameSamples);
        (void)popped;
        upsample_8k_to_16k_frame(playback_8k, playback_frame);
        aec_network_ref_push(playback_frame, kFrameSamples);
        if (!i2s_write_frame(playback_frame)) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        taskYIELD();
    }

    i2s_clear_dma();
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
    // Stack lives in PSRAM (MALLOC_CAP_SPIRAM): 8 KB is too big to find as a
    // contiguous block in internal SRAM once AEC_Init has done its ~50 KB of
    // mallocs just above. The passthrough task only touches DMA buffers,
    // I2S/I2C drivers, ES8311 register state, AEC feed/output queues, and the
    // (already PSRAM-resident) G.711 encode LUT -- none of which require
    // stack access while flash cache is disabled, so PSRAM stack is safe.
    if (xTaskCreateWithCaps(es8311_passthrough_task,
                            "es8311_passthrough",
                            8192,
                            nullptr,
                            5,
                            &s_passthrough_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
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
        gpio_reset_pin((gpio_num_t)kPinPaEn);
        gpio_set_direction((gpio_num_t)kPinPaEn, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)kPinPaEn, 1);
        ESP_LOGI(TAG,"[ES8311] PA enable pin %d set HIGH\n", kPinPaEn);
    }

    I2C_Init();

    // I2S/MCLK must be running BEFORE codec register config.
    // ES8311 internal DAC bias circuits require MCLK to establish
    // proper VMID and output common-mode voltage.
    if (!i2s_setup()) {
        s_es8311_ready = false;
        ESP_LOGI(TAG,"[ES8311] initialization failed during I2S setup");
        return false;
    }

    if (!es8311_configure_codec()) {
        s_es8311_ready = false;
        ESP_LOGI(TAG,"[ES8311] initialization failed during codec configuration");
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
            ESP_LOGI(TAG,"[ES8311] WARNING chip id mismatch: fd=%s0x%02X (expect 0x83) fe=%s0x%02X (expect 0x11)\n",
                          ok_fd ? "" : "ERR:", static_cast<unsigned>(reg_fd),
                          ok_fe ? "" : "ERR:", static_cast<unsigned>(reg_fe));
        }
    }

#if defined(NRL_ENABLE_AUDIO_AFE) && NRL_ENABLE_AUDIO_AFE
    // Bring up esp-sr before the passthrough task starts feeding it. Runtime
    // switches only choose whether to use processed frames and which reference
    // source to feed; the resident AFE stays alive.
    const ExternalRadioConfig *aec_cfg = EXTERNAL_RADIO_GetConfig();
    const bool runtime_ai_noise = (aec_cfg != nullptr) && aec_cfg->ai_noise_enabled;
    ES8311_SetAecReferenceSource((aec_cfg != nullptr) ? aec_cfg->aec_reference_source : 0u);
#if defined(NRL_ENABLE_AEC) && NRL_ENABLE_AEC
    const bool afe_has_aec = true;
    const bool runtime_aec = (aec_cfg != nullptr) && aec_cfg->aec_enabled;
#else
    const bool afe_has_aec = false;
    const bool runtime_aec = false;
#endif
    const bool afe_has_ai_noise = true;
    AEC_SetOutputCallback(es8311_aec_output, nullptr);
    if (AEC_Init(afe_has_aec, afe_has_ai_noise)) {
        AEC_SetRuntimeEnabled(runtime_aec, runtime_ai_noise);
        ESP_LOGI(TAG,"[ES8311] esp-sr resident: aec_cap=%u ai_cap=%u route_aec=%u route_ai=%u\n",
                      afe_has_aec ? 1u : 0u,
                      afe_has_ai_noise ? 1u : 0u,
                      runtime_aec ? 1u : 0u,
                      runtime_ai_noise ? 1u : 0u);
    } else {
        ESP_LOGI(TAG,"[ES8311] esp-sr resident init failed -- mic uplink falls back to raw");
    }
#endif

    if (!es8311_start_passthrough()) {
        s_es8311_ready = false;
        ESP_LOGI(TAG,"[ES8311] initialization failed starting passthrough task");
        return false;
    }

    s_es8311_ready = true;
#if ES8311_FORCE_DAC_SILENCE
    ESP_LOGI(TAG,"[ES8311] *** FORCE_DAC_SILENCE=1 (DAC always writes zeros) ***");
#else
    ESP_LOGI(TAG,"[ES8311] FORCE_DAC_SILENCE=0 (normal DAC writes)");
#endif
    ESP_LOGI(TAG,"[ES8311] ready: i2c=0x%02X rate=%dHz bits=%d stereo_slots=2 mclk=%d bclk=%d esp_dout=%d lrclk=%d esp_din=%d\n",
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
        ESP_LOGI(TAG,"[ES8311] failed to configure DAC output path");
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

void ES8311_SetAecReferenceSource(const uint8_t source) {
    s_aec_reference_source = (source == 1u) ? 1u : 0u;
    aec_network_ref_clear();
}

size_t ES8311_QueueOutputSamples(const int16_t *samples, size_t sample_count) {
    if (!ES8311_Init()) {
        return 0;
    }

    const size_t written = output_queue_push(samples, sample_count);
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (written != sample_count && (now - s_last_output_queue_log_ms) >= 1000u) {
        s_last_output_queue_log_ms = now;
        ESP_LOGI(TAG,"[ES8311] queue short write samples=%u written=%u\n",
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
                             const uint32_t daceq_a1,
                             const bool adc_dmic_enabled,
                             const bool adc_linsel,
                             const uint8_t adc_pga_gain,
                             const uint8_t adc_ramprate,
                             const bool adc_dmic_sense,
                             const bool adc_sync,
                             const bool adc_inv,
                             const bool adc_ramclr,
                             const uint8_t adc_scale,
                             const bool alc_enabled,
                             const bool adc_automute_enabled,
                             const uint8_t alc_winsize,
                             const uint8_t alc_maxlevel,
                             const uint8_t alc_minlevel,
                             const uint8_t adc_automute_winsize,
                             const uint8_t adc_automute_noise_gate,
                             const uint8_t adc_automute_volume,
                             const uint8_t adc_hpfs1,
                             const bool adc_eq_bypass,
                             const bool adc_hpf,
                             const uint8_t adc_hpfs2,
                             const uint32_t adceq_b0,
                             const uint32_t adceq_a1,
                             const uint32_t adceq_a2,
                             const uint32_t adceq_b1,
                             const uint32_t adceq_b2) {
    s_mic_volume = mic_volume;
    s_line_out_volume = line_out_volume;
    s_hp_drive_enabled = hp_drive_enabled;

    // On boards with a separate ES7210 mic ADC, the microphone is captured
    // by the ES7210, not the ES8311 -- route the mic volume there too. The
    // ES8311 REG17 write below is then harmless (its ADC path is unused).
    // On boards without an ES7210 this call is a no-op.
    ES7210_SetMicVolume(mic_volume);
    s_drc_enabled = drc_enabled;
    s_drc_winsize = drc_winsize & 0x0Fu;
    s_drc_maxlevel = drc_maxlevel & 0x0Fu;
    s_drc_minlevel = drc_minlevel & 0x0Fu;
    s_dac_ramprate = dac_ramprate & 0x0Fu;
    s_dac_eq_bypass = dac_eq_bypass;
    s_daceq_b0 = daceq_b0 & kDacEqCoefficientMask;
    s_daceq_b1 = daceq_b1 & kDacEqCoefficientMask;
    s_daceq_a1 = daceq_a1 & kDacEqCoefficientMask;
    s_adc_dmic_enabled = adc_dmic_enabled;
    s_adc_linsel = adc_linsel;
    s_adc_pga_gain = adc_pga_gain & 0x0Fu;
    s_adc_ramprate = adc_ramprate & 0x0Fu;
    s_adc_dmic_sense = adc_dmic_sense;
    s_adc_sync = adc_sync;
    s_adc_inv = adc_inv;
    s_adc_ramclr = adc_ramclr;
    s_adc_scale = adc_scale & 0x07u;
    s_alc_enabled = alc_enabled;
    s_adc_automute_enabled = adc_automute_enabled;
    s_alc_winsize = alc_winsize & 0x0Fu;
    s_alc_maxlevel = alc_maxlevel & 0x0Fu;
    s_alc_minlevel = alc_minlevel & 0x0Fu;
    s_adc_automute_winsize = adc_automute_winsize & 0x0Fu;
    s_adc_automute_noise_gate = adc_automute_noise_gate & 0x0Fu;
    s_adc_automute_volume = adc_automute_volume & 0x07u;
    s_adc_hpfs1 = adc_hpfs1 & 0x1Fu;
    s_adc_eq_bypass = adc_eq_bypass;
    s_adc_hpf = adc_hpf;
    s_adc_hpfs2 = adc_hpfs2 & 0x1Fu;
    s_adceq_b0 = adceq_b0 & kAdcEqCoefficientMask;
    s_adceq_a1 = adceq_a1 & kAdcEqCoefficientMask;
    s_adceq_a2 = adceq_a2 & kAdcEqCoefficientMask;
    s_adceq_b1 = adceq_b1 & kAdcEqCoefficientMask;
    s_adceq_b2 = adceq_b2 & kAdcEqCoefficientMask;

    if (!s_es8311_ready) {
        ESP_LOGI(TAG,"[ES8311] audio config pending: mic=0x%02X line_out=0x%02X hp_drive=%u drc=%u,%u,%u,%u ramp=%u eq_bypass=%u eq=%lu,%lu,%lu\n",
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
        es8311_apply_daceq_coefficients() &&
        es8311_apply_adc_config();
    if (!ok) {
        ESP_LOGI(TAG,"[ES8311] audio config apply failed: mic=0x%02X line_out=0x%02X hp_drive=%u drc=%u,%u,%u,%u ramp=%u eq_bypass=%u eq=%lu,%lu,%lu\n",
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

void ES8311_SetMicHpfEnabled(const bool enabled) {
    if (s_mic_hpf_enabled != enabled) {
        s_mic_hpf_enabled = enabled;
        mic_hpf_reset();
        ESP_LOGI(TAG,"[ES8311] mic HPF %s (4th-order, fc~=200Hz @ fs=%dHz, s1=%.6f/%.6f/%.6f/%.6f/%.6f s2=%.6f/%.6f/%.6f/%.6f/%.6f)\n",
                      enabled ? "ENABLED" : "disabled",
                      kSampleRate,
                      static_cast<double>(kMicHpf1B0),
                      static_cast<double>(kMicHpf1B1),
                      static_cast<double>(kMicHpf1B2),
                      static_cast<double>(kMicHpf1A1),
                      static_cast<double>(kMicHpf1A2),
                      static_cast<double>(kMicHpf2B0),
                      static_cast<double>(kMicHpf2B1),
                      static_cast<double>(kMicHpf2B2),
                      static_cast<double>(kMicHpf2A1),
                      static_cast<double>(kMicHpf2A2));
    }
}

bool ES8311_GetMicHpfEnabled(void) {
    return s_mic_hpf_enabled;
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

    i2s_clear_dma();

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
    i2s_clear_dma();
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
        i2s_clear_dma();

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

        i2s_clear_dma();
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
