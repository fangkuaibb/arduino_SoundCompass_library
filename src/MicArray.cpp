#include "MicArray.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "soc/soc_caps.h"

static const char *TAG = "MicArray";

#define CHUNK_FRAMES        256
#define MCLK_MULTIPLE       I2S_MCLK_MULTIPLE_384
#define TDM_SLOTS           (I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3)
#define I2C_ADDR_SCAN_START 0x40
#define I2C_ADDR_SCAN_END   0x43

// ES7210 寄存器
#define REG_RESET        0x00
#define REG_MAINCLK      0x02
#define REG_LRCK_DIVH    0x04
#define REG_LRCK_DIVL    0x05
#define REG_POWER_DOWN   0x06
#define REG_OSR          0x07
#define REG_TIME_CTRL0   0x09
#define REG_TIME_CTRL1   0x0A
#define REG_SDP_IF1      0x11
#define REG_SDP_IF2      0x12
#define REG_ADC34_HPF2   0x20
#define REG_ADC34_HPF1   0x21
#define REG_ADC12_HPF2   0x22
#define REG_ADC12_HPF1   0x23
#define REG_ANALOG       0x40
#define REG_MIC12_BIAS   0x41
#define REG_MIC34_BIAS   0x42
#define REG_MIC1_GAIN    0x43
#define REG_MIC2_GAIN    0x44
#define REG_MIC3_GAIN    0x45
#define REG_MIC4_GAIN    0x46
#define REG_MIC1_POWER   0x47
#define REG_MIC2_POWER   0x48
#define REG_MIC3_POWER   0x49
#define REG_MIC4_POWER   0x4A
#define REG_MIC12_POWER  0x4B
#define REG_MIC34_POWER  0x4C

#define MIC_BIAS_2V87    0x70

MicArray::MicArray()
{
}

MicArray::~MicArray()
{
    end();
}

bool MicArray::writeReg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(_codec, buf, sizeof(buf), 100) == ESP_OK;
}

bool MicArray::readReg(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(_codec, &reg, 1, val, 1, 100) == ESP_OK;
}

bool MicArray::findCodec()
{
    for (uint8_t addr = I2C_ADDR_SCAN_START; addr <= I2C_ADDR_SCAN_END; addr++) {
        if (i2c_master_probe(_bus, addr, 50) == ESP_OK) {
            _codecAddr = addr;
            return true;
        }
    }
    return false;
}

bool MicArray::initCodec(int gainDb)
{
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = _codecAddr;
    dev_cfg.scl_speed_hz = 100000;
    if (i2c_master_bus_add_device(_bus, &dev_cfg, &_codec) != ESP_OK) {
        ESP_LOGE(TAG, "i2c add device failed");
        return false;
    }

    int step = gainDb / 3;
    if (step < 0) {
        step = 0;
    } else if (step > 14) {
        step = 14;
    }
    uint8_t gain_reg = (uint8_t)(0x10 | step);

    struct RegVal {
        uint8_t reg;
        uint8_t val;
    };
    const RegVal seq[] = {
        {REG_RESET,       0xFF},
        {REG_RESET,       0x32},
        {REG_TIME_CTRL0,  0x30},
        {REG_TIME_CTRL1,  0x30},
        {REG_ADC12_HPF1,  0x2A},
        {REG_ADC12_HPF2,  0x0A},
        {REG_ADC34_HPF1,  0x2A},
        {REG_ADC34_HPF2,  0x0A},
        {REG_SDP_IF1,     0x60},   // I2S 格式，16bit 数据(左对齐)
        {REG_SDP_IF2,     0x01},   // 1xFS TDM
        {REG_ANALOG,      0xC3},
        {REG_MIC12_BIAS,  MIC_BIAS_2V87},
        {REG_MIC34_BIAS,  MIC_BIAS_2V87},
        {REG_MIC1_GAIN,   gain_reg},
        {REG_MIC2_GAIN,   gain_reg},
        {REG_MIC3_GAIN,   gain_reg},
        {REG_MIC4_GAIN,   gain_reg},
        {REG_MIC1_POWER,  0x08},
        {REG_MIC2_POWER,  0x08},
        {REG_MIC3_POWER,  0x08},
        {REG_MIC4_POWER,  0x08},
        {REG_OSR,         0x20},
        {REG_MAINCLK,     0xC1},   // adc_div=1, doubler=1, dll=1
        {REG_LRCK_DIVH,   0x01},   // LRCK = MCLK/384 = fs
        {REG_LRCK_DIVL,   0x80},
        {REG_POWER_DOWN,  0x04},
        {REG_MIC12_POWER, 0x0F},
        {REG_MIC34_POWER, 0x0F},
        {REG_RESET,       0x71},
        {REG_RESET,       0x41},
    };

    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        if (!writeReg(seq[i].reg, seq[i].val)) {
            ESP_LOGE(TAG, "write reg 0x%02X failed", seq[i].reg);
            return false;
        }
    }

    uint8_t reg00 = 0, reg11 = 0;
    readReg(REG_RESET, &reg00);
    readReg(REG_SDP_IF1, &reg11);
    ESP_LOGI(TAG, "ES7210 @0x%02X, gain %ddB, reg00=0x%02X reg11=0x%02X",
             _codecAddr, step * 3, reg00, reg11);
    return true;
}

bool MicArray::initI2s()
{
#if !SOC_I2S_SUPPORTS_TDM
    ESP_LOGE(TAG, "this chip has no I2S TDM, cannot use ES7210 4-channel output");
    return false;
#else
    i2s_chan_config_t chan_cfg = {};
    chan_cfg.id = (i2s_port_t)_cfg.i2sPort;
    chan_cfg.role = I2S_ROLE_MASTER;
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear_after_cb = false;
    chan_cfg.allow_pd = false;
    if (i2s_new_channel(&chan_cfg, nullptr, &_rx) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed");
        return false;
    }

    i2s_tdm_config_t tdm_cfg = {};
    tdm_cfg.clk_cfg.sample_rate_hz = _cfg.sampleRate;
    tdm_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    tdm_cfg.clk_cfg.ext_clk_freq_hz = 0;
    tdm_cfg.clk_cfg.mclk_multiple = MCLK_MULTIPLE;
    tdm_cfg.clk_cfg.bclk_div = 0;

    tdm_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    tdm_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    tdm_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    tdm_cfg.slot_cfg.slot_mask = (i2s_tdm_slot_mask_t)TDM_SLOTS;
    tdm_cfg.slot_cfg.ws_width = I2S_TDM_AUTO_WS_WIDTH;
    tdm_cfg.slot_cfg.ws_pol = false;
    tdm_cfg.slot_cfg.bit_shift = true;
    tdm_cfg.slot_cfg.left_align = false;
    tdm_cfg.slot_cfg.big_endian = false;
    tdm_cfg.slot_cfg.bit_order_lsb = false;
    tdm_cfg.slot_cfg.skip_mask = false;
    tdm_cfg.slot_cfg.total_slot = I2S_TDM_AUTO_SLOT_NUM;

    tdm_cfg.gpio_cfg.mclk = (gpio_num_t)_cfg.mclkGpio;
    tdm_cfg.gpio_cfg.bclk = (gpio_num_t)_cfg.bclkGpio;
    tdm_cfg.gpio_cfg.ws = (gpio_num_t)_cfg.wsGpio;
    tdm_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    tdm_cfg.gpio_cfg.din = (gpio_num_t)_cfg.dinGpio;

    if (i2s_channel_init_tdm_mode(_rx, &tdm_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_tdm_mode failed");
        return false;
    }
    if (i2s_channel_enable(_rx) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed");
        return false;
    }
    return true;
#endif
}

bool MicArray::begin()
{
    return begin(Config());
}

bool MicArray::begin(const Config &cfg)
{
    if (_rx != nullptr) {
        return true;
    }
    _cfg = cfg;

    _raw = (int32_t *)calloc(CHUNK_FRAMES * 4, sizeof(int32_t));
    if (_raw == nullptr) {
        return false;
    }

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = (gpio_num_t)_cfg.sdaGpio;
    bus_cfg.scl_io_num = (gpio_num_t)_cfg.sclGpio;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bus_cfg, &_bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed");
        end();
        return false;
    }

    if (!findCodec()) {
        ESP_LOGE(TAG, "ES7210 not found on SCL=%d SDA=%d", _cfg.sclGpio, _cfg.sdaGpio);
        end();
        return false;
    }

    if (!initI2s() || !initCodec(_cfg.gainDb)) {
        end();
        return false;
    }
    return true;
}

size_t MicArray::read(int16_t *dst, size_t frames, uint32_t timeoutMs)
{
    if (_rx == nullptr || dst == nullptr) {
        return 0;
    }

    size_t done = 0;
    while (done < frames) {
        size_t chunk = frames - done;
        if (chunk > CHUNK_FRAMES) {
            chunk = CHUNK_FRAMES;
        }
        size_t want = chunk * 4 * sizeof(int32_t);
        size_t got = 0;
        if (i2s_channel_read(_rx, _raw, want, &got, pdMS_TO_TICKS(timeoutMs)) != ESP_OK) {
            break;
        }
        size_t n = got / (4 * sizeof(int32_t));
        if (n == 0) {
            break;
        }
        for (size_t i = 0; i < n * 4; i++) {
            dst[done * 4 + i] = (int16_t)(_raw[i] >> 16);
        }
        done += n;
    }
    return done;
}

void MicArray::end()
{
    if (_rx != nullptr) {
        i2s_channel_disable(_rx);
        i2s_del_channel(_rx);
        _rx = nullptr;
    }
    if (_codec != nullptr) {
        i2c_master_bus_rm_device(_codec);
        _codec = nullptr;
    }
    if (_bus != nullptr) {
        i2c_del_master_bus(_bus);
        _bus = nullptr;
    }
    if (_raw != nullptr) {
        free(_raw);
        _raw = nullptr;
    }
    _codecAddr = 0;
}
