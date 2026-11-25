// 声源定位核心（预编译静态库 libsl_core.a 的公开接口）
//
// 该核心包含 GCC-PHAT 时延估计、角度换算、圆形平滑以及 esp-sr WebRTC VAD 人声判定。
// 实现源码见 extras/precompiled/sl_core.c，由 build_lib.ps1 编译并与 esp-sr 的
// VAD 目标文件一起打包成 src/esp32s3/libsl_core.a。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SL_CORE_CHANNELS 4

typedef enum {
    SL_CORE_MODE_TRACK = 0,   // 实时跟踪：每帧都更新
    SL_CORE_MODE_PEAK,        // 只在能量局部峰值帧更新
    SL_CORE_MODE_VOICE,       // 仅人声触发
    SL_CORE_MODE_OFF,         // 暂停处理
} sl_core_mode_t;

typedef struct {
    uint32_t sample_rate;     // 必须与采集一致，默认 48000
    float mic_spacing_m;      // 对角麦克风间距，默认 0.04
    float speed_of_sound;     // 声速 m/s，默认 343
    float angle_offset_deg;   // 安装角度校准
    int clockwise;            // 角度方向，非 0 顺时针
    uint8_t mic_map[4];       // 逻辑 MIC1..MIC4 -> 物理通道
    int frame_len;            // 分帧长度，256~4096 的 2 的幂
    int led_count;            // 灯珠数量
    float voice_min_db;       // 触发电平门限
    float min_confidence;     // 互相关置信度下限
} sl_core_cfg_t;

typedef struct {
    float angle_deg;                   // 0~360
    int led_index;                     // 对应灯珠序号
    float confidence;                  // 0~1
    float level_db[SL_CORE_CHANNELS];  // 各通道电平
    float max_level_db;
    int is_voice;                      // 本帧是否判定为人声
} sl_core_result_t;

typedef struct sl_core_s sl_core_t;

// 创建/销毁核心实例；失败返回 NULL
sl_core_t *sl_core_create(const sl_core_cfg_t *cfg);
void sl_core_destroy(sl_core_t *core);

// 分帧长度（等于 cfg->frame_len）
int sl_core_frame_len(const sl_core_t *core);

// 处理一帧 4 通道交错 int16 数据（长度 = frame_len * 4）。
// 返回 1 表示 out 被填入有效结果，0 表示本帧被丢弃（静音/置信度不足/PEAK 等待中）。
int sl_core_process(sl_core_t *core, const int16_t *interleaved, sl_core_result_t *out);

void sl_core_set_mode(sl_core_t *core, sl_core_mode_t mode);
sl_core_mode_t sl_core_get_mode(const sl_core_t *core);

// 清除平滑状态与 PEAK 待发帧（VAD 状态保留）
void sl_core_reset(sl_core_t *core);

const char *sl_core_version(void);

#ifdef __cplusplus
}
#endif
