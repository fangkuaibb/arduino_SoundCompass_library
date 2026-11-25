// WS2812 环形灯效（Arduino 封装）
#pragma once

#include <Arduino.h>

#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class LedRing {
public:
    enum Mode {
        POINT = 0,   // 单点：亮一下即灭
        TRAIL,       // 拖尾：保留最近经过的灯珠
        BREATH,      // 呼吸：目标灯亮度起伏
        COMET,       // 彗星：亮点滑向目标，带渐隐尾
        OFF,         // 全灭
    };

    enum ColorMode {
        RAINBOW = 0, // 按环形位置取色相，整圈彩虹
        SOLID,       // 全部使用 r/g/b
    };

    struct Config {
        int pin = 4;                 // WS2812 数据脚
        int ledCount = 24;           // 灯珠数量，最大 64
        Mode mode = TRAIL;
        ColorMode colorMode = RAINBOW;
        uint8_t r = 0, g = 0, b = 255;
        uint8_t maxBrightness = 255;
        bool grbOrder = true;        // 多数 WS2812 为 GRB
        int trailLength = 6;         // 拖尾长度(灯珠数)
        uint32_t decayMs = 450;      // 拖尾衰减时间
        uint32_t idleTimeoutMs = 1200; // 无更新多久后熄灭，0 表示不熄灭
        int refreshMs = 25;          // 刷新周期
    };

    LedRing();
    ~LedRing();

    // 建 RMT 通道并启动刷新任务；失败返回 false
    bool begin();
    bool begin(const Config &cfg);
    void end();

    void setMode(Mode mode);
    Mode getMode() const;

    // 点亮 0~ledCount-1 号灯，intensity 0~1
    void setTarget(int ledIndex, float intensity = 1.0f);
    void clear();

    void setColorMode(ColorMode mode);
    ColorMode getColorMode() const;
    // 设置单色基色，并自动切到 SOLID
    void setColor(uint8_t r, uint8_t g, uint8_t b);

private:
    static const int MAX_LEDS = 64;
    static const int MAX_SYMBOLS = MAX_LEDS * 24 + 1;

    Config _cfg;
    rmt_channel_handle_t _chan = nullptr;
    rmt_encoder_handle_t _enc = nullptr;
    rmt_symbol_word_t *_syms = nullptr;
    uint8_t *_pixels = nullptr;
    float *_level = nullptr;
    TaskHandle_t _task = nullptr;

    float _head = 0.0f;
    int _target = 0;
    float _pendingIntensity = 0.0f;
    bool _hasTarget = false;
    int64_t _lastTargetUs = 0;
    bool _sentOff = false;
    volatile int _mode = TRAIL;
    volatile int _colorMode = RAINBOW;
    float _breathPhase = 0.0f;

    int _trailHist[16] = {0};
    int _trailCnt = 0;
    int _trailPrev = -1;
    int64_t _lastMoveUs = 0;

    static void taskEntry(void *arg);
    void taskLoop();
    void transmit();
    void update(float dt);
    void setLevel(int idx, float v);
    bool allDark() const;
    static void hueToRgb(int idx, int leds, uint8_t *r, uint8_t *g, uint8_t *b);
};
