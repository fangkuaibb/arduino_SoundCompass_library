// ES7210 四路麦克风 + I2S TDM 采集（Arduino 封装）
#pragma once

#include <Arduino.h>
#include <stddef.h>

#include "driver/i2c_master.h"
#include "driver/i2s_tdm.h"

class MicArray {
public:
    struct Config {
        int i2sPort = 0;        // I2S 控制器序号
        int sclGpio = 5;        // ES7210 I2C 时钟
        int sdaGpio = 6;        // ES7210 I2C 数据
        int mclkGpio = 16;      // 18.432 MHz (384 x fs)
        int bclkGpio = 17;      // 6.144 MHz
        int wsGpio = 18;        // 48 kHz
        int dinGpio = 15;       // I2S 数据输入
        uint32_t sampleRate = 48000;
        int gainDb = 30;        // 0~42，内部按 3 dB 步进
    };

    MicArray();
    ~MicArray();

    // 建 I2C、扫描并配置 ES7210、启动 I2S TDM；失败返回 false
    bool begin();
    bool begin(const Config &cfg);
    void end();

    bool isReady() const { return _rx != nullptr; }
    uint32_t sampleRate() const { return _cfg.sampleRate; }
    int channels() const { return 4; }

    // 阻塞读取 frames 帧，输出 4 通道交错 int16；返回实际读到的帧数
    size_t read(int16_t *dst, size_t frames, uint32_t timeoutMs = 1000);

private:
    Config _cfg;
    i2c_master_bus_handle_t _bus = nullptr;
    i2c_master_dev_handle_t _codec = nullptr;
    i2s_chan_handle_t _rx = nullptr;
    int32_t *_raw = nullptr;
    uint8_t _codecAddr = 0;

    bool findCodec();
    bool initCodec(int gainDb);
    bool initI2s();
    bool writeReg(uint8_t reg, uint8_t val);
    bool readReg(uint8_t reg, uint8_t *val);
};
