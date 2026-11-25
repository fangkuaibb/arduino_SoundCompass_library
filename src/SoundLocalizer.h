// 四麦声源定位（Arduino 风格封装）
//
// 采集：MicArray（ES7210 + I2S TDM，Arduino 源码）
// 算法：libsl_core.a（GCC-PHAT + esp-sr WebRTC VAD，预编译）
#pragma once

#include <Arduino.h>
#include <math.h>

#include "MicArray.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sl_core.h"

class SoundLocalizer {
public:
    enum Mode {
        TRACK = SL_CORE_MODE_TRACK,   // 实时跟踪
        PEAK = SL_CORE_MODE_PEAK,     // 只在能量峰值帧更新
        VOICE = SL_CORE_MODE_VOICE,   // 仅人声触发
        OFF = SL_CORE_MODE_OFF,       // 暂停计算
    };

    struct Config {
        uint32_t sampleRate = 48000;   // 必须与 MicArray 一致
        float micSpacingM = 0.04f;     // 对角麦克风间距
        float angleOffsetDeg = 0.0f;   // 安装角度校准
        bool clockwise = true;         // 角度方向
        uint8_t micMap[4] = {0, 1, 2, 3}; // 逻辑 MIC1..4 -> 物理通道
        int frameLength = 2048;        // 2 的幂，256~4096
        int ledCount = 24;             // 灯珠数量
        float voiceMinDb = -40.0f;     // 触发电平门限
        float minConfidence = 0.35f;   // 互相关置信度下限
        Mode mode = TRACK;
    };

    struct Result {
        float angleDeg = 0.0f;              // 0~360
        int ledIndex = -1;                  // 对应灯珠
        float confidence = 0.0f;            // 0~1
        float levelDb[4] = {0, 0, 0, 0};    // 各通道电平
        float maxLevelDb = -120.0f;
        bool isVoice = false;
    };

    typedef void (*Callback)(const Result &result, void *user);

    SoundLocalizer();
    ~SoundLocalizer();

    // 初始化并启动后台定位任务；cb 可为 nullptr（之后用 getResult() 轮询）
    bool begin(MicArray &mic);
    bool begin(MicArray &mic, const Config &cfg);
    bool begin(MicArray &mic, const Config &cfg, Callback cb, void *user = nullptr);
    void end();

    bool start(Callback cb = nullptr, void *user = nullptr);
    void stop();
    bool isRunning() const { return _task != nullptr; }

    void setMode(Mode mode);
    Mode getMode() const;

    // 读取最近一次结果；返回 false 表示还没有有效定位
    bool getResult(Result &out) const;

    // 当前声源角度；无有效结果返回 NAN
    float getAngle() const;
    // 当前灯珠序号；无有效结果返回 -1
    int getLedIndex() const;

private:
    MicArray *_mic = nullptr;
    sl_core_t *_core = nullptr;
    int16_t *_frame = nullptr;
    int _frameLen = 0;
    Callback _cb = nullptr;
    void *_user = nullptr;
    TaskHandle_t _task = nullptr;
    SemaphoreHandle_t _mutex = nullptr;
    volatile bool _stopRequested = false;

    Result _last;
    bool _hasResult = false;

    static void taskEntry(void *arg);
    void run();
    void storeResult(const sl_core_result_t &r);
};
