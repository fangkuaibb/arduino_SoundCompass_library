// 声源定位核心实现（编译进 libsl_core.a）
//
// 算法来自 components/sound_localizer，但去掉了 ESP-IDF 的任务/音频/Menuconfig 依赖：
// 采集由 Arduino 层负责，这里只处理一帧 4 通道交错 int16 数据。
#include "sl_core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_vad.h"

#define SL_CORE_MAX_FRAME   4096
#define SL_CORE_PI_F        3.14159265358979323846f
#define SL_VAD_SAMPLE_RATE  16000
#define SL_VAD_FRAME_MS     30
#define SL_VAD_BUF_LEN      480

struct sl_core_s {
    sl_core_cfg_t cfg;

    int16_t *sig;   // 4 通道去直流/预加重后的信号
    float *win;     // 汉宁窗
    float *a_re, *a_im, *b_re, *b_im;
    float *peak_buf;
    int max_lag;

    volatile sl_core_mode_t mode;
    float sm_x, sm_y;

    // PEAK 模式的两帧延迟发射
    int has_pending;
    sl_core_result_t pending;
    float pending_db;

    // esp-sr WebRTC VAD
    vad_handle_t vad;
    int16_t vad_buf[SL_VAD_BUF_LEN];
    int vad_len;
    int vad_speech_cnt;
    int vad_silence_cnt;
    int vad_speech;
};

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void fft_exec(float *re, float *im, int n, int inverse)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float tr = re[i];
            re[i] = re[j];
            re[j] = tr;
            float ti = im[i];
            im[i] = im[j];
            im[j] = ti;
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        float ang = 2.0f * SL_CORE_PI_F / (float)len * (inverse ? 1.0f : -1.0f);
        float wl_re = cosf(ang);
        float wl_im = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float wr = 1.0f, wi = 0.0f;
            for (int j = 0; j < len / 2; j++) {
                int u = i + j;
                int v = i + j + len / 2;
                float xr = re[v] * wr - im[v] * wi;
                float xi = re[v] * wi + im[v] * wr;
                re[v] = re[u] - xr;
                im[v] = im[u] - xi;
                re[u] += xr;
                im[u] += xi;
                float nwr = wr * wl_re - wi * wl_im;
                wi = wr * wl_im + wi * wl_re;
                wr = nwr;
            }
        }
    }

    if (inverse) {
        for (int i = 0; i < n; i++) {
            re[i] /= (float)n;
            im[i] /= (float)n;
        }
    }
}

static void fft_channel(sl_core_t *core, int ch, float *re, float *im)
{
    const int n = core->cfg.frame_len;
    const int16_t *p = core->sig + ch * n;
    for (int i = 0; i < n; i++) {
        re[i] = (float)p[i] * core->win[i];
        im[i] = 0.0f;
    }
    fft_exec(re, im, n, 0);
}

// 用 PHAT 加权求两通道时延；conf 返回时域归一化互相关峰值
static float pair_delay(sl_core_t *core, int a_ch, int b_ch, float *conf)
{
    const int n = core->cfg.frame_len;
    fft_channel(core, a_ch, core->a_re, core->a_im);
    fft_channel(core, b_ch, core->b_re, core->b_im);

    for (int k = 0; k < n; k++) {
        float ar = core->a_re[k], ai = core->a_im[k];
        float br = core->b_re[k], bi = core->b_im[k];
        float xr = ar * br + ai * bi;   // A* · B
        float xi = ar * bi - ai * br;
        float mag = sqrtf(xr * xr + xi * xi) + 1e-9f;
        core->a_re[k] = xr / mag;
        core->a_im[k] = xi / mag;
    }
    fft_exec(core->a_re, core->a_im, n, 1);

    int best = -core->max_lag;
    float best_v = -1e9f;
    float *v = core->peak_buf;
    const int span = 2 * core->max_lag + 1;
    for (int i = 0; i < span; i++) {
        int lag = -core->max_lag + i;
        int idx = (lag + n) % n;
        v[i] = core->a_re[idx];
        if (v[i] > best_v) {
            best_v = v[i];
            best = lag;
        }
    }

    float delta = 0.0f;
    int bi = best + core->max_lag;
    if (bi > 0 && bi < span - 1) {
        float y0 = v[bi - 1], y1 = v[bi], y2 = v[bi + 1];
        float denom = y0 - 2.0f * y1 + y2;
        if (fabsf(denom) > 1e-9f) {
            delta = 0.5f * (y0 - y2) / denom;
            delta = clampf(delta, -1.0f, 1.0f);
        }
    }

    const int16_t *pa = core->sig + a_ch * n;
    const int16_t *pb = core->sig + b_ch * n;
    const int lag = best;
    float num = 0.0f, ea = 0.0f, eb = 0.0f;
    for (int i = core->max_lag; i < n - core->max_lag; i++) {
        float va = (float)pa[i];
        float vb = (float)pb[i + lag];
        num += va * vb;
        ea += va * va;
        eb += vb * vb;
    }
    *conf = clampf(num / (sqrtf(ea * eb) + 1e-6f), -1.0f, 1.0f);

    return (float)best + delta;
}

// 把 MIC1 通路从 48kHz 三点平均抽取到 16kHz，按 30ms 帧喂给 VAD
static void vad_update(sl_core_t *core, const int16_t *frame)
{
    if (core->cfg.sample_rate != 48000) {
        return;   // VAD 固定 16kHz，非 48kHz 采集时不做人声判定
    }
    if (core->vad == NULL) {
        core->vad = vad_create_with_param(VAD_MODE_2, SL_VAD_SAMPLE_RATE,
                                          SL_VAD_FRAME_MS, 200, 100);
        if (core->vad == NULL) {
            return;
        }
    }

    const int ch = core->cfg.mic_map[0];
    const int n = core->cfg.frame_len;
    for (int i = 0; i + 2 < n; i += 3) {
        int32_t acc = (int32_t)frame[i * SL_CORE_CHANNELS + ch] +
                      frame[(i + 1) * SL_CORE_CHANNELS + ch] +
                      frame[(i + 2) * SL_CORE_CHANNELS + ch];
        core->vad_buf[core->vad_len++] = (int16_t)(acc / 3);
        if (core->vad_len >= SL_VAD_BUF_LEN) {
            int frame_speech = (vad_process(core->vad, core->vad_buf,
                                            SL_VAD_SAMPLE_RATE, SL_VAD_FRAME_MS) == VAD_SPEECH);
            core->vad_len = 0;

            // 迟滞：连续 6 帧(180ms)人声才开始，连续 8 帧(240ms)静音才结束
            if (frame_speech) {
                core->vad_silence_cnt = 0;
                if (core->vad_speech_cnt < 100) {
                    core->vad_speech_cnt++;
                }
            } else {
                core->vad_speech_cnt = 0;
                if (core->vad_silence_cnt < 100) {
                    core->vad_silence_cnt++;
                }
            }
            if (!core->vad_speech && core->vad_speech_cnt >= 6) {
                core->vad_speech = 1;
            } else if (core->vad_speech && core->vad_silence_cnt >= 8) {
                core->vad_speech = 0;
            }
        }
    }
}

int sl_core_process(sl_core_t *core, const int16_t *frame, sl_core_result_t *out)
{
    if (core == NULL || frame == NULL || out == NULL) {
        return 0;
    }
    if (core->mode == SL_CORE_MODE_OFF) {
        return 0;
    }

    const int n = core->cfg.frame_len;
    const uint8_t *map = core->cfg.mic_map;

    // 去直流 + 一阶预加重，并统计能量
    float energy[SL_CORE_CHANNELS] = {0};
    for (int ch = 0; ch < SL_CORE_CHANNELS; ch++) {
        int16_t *dst = core->sig + ch * n;
        int16_t prev = frame[ch];
        float acc = 0.0f;
        for (int i = 0; i < n; i++) {
            int16_t cur = frame[i * SL_CORE_CHANNELS + ch];
            int16_t hp = (int16_t)(cur - prev);
            prev = cur;
            dst[i] = hp;
            acc += (float)hp * (float)hp;
        }
        energy[ch] = acc;
    }

    sl_core_result_t r;
    memset(&r, 0, sizeof(r));
    float max_db = -120.0f;
    for (int ch = 0; ch < SL_CORE_CHANNELS; ch++) {
        float rms = sqrtf(energy[ch] / (float)n);
        r.level_db[ch] = rms > 0 ? 20.0f * log10f(rms / 32768.0f) : -120.0f;
        if (r.level_db[ch] > max_db) {
            max_db = r.level_db[ch];
        }
    }
    r.max_level_db = max_db;

    // VAD 需要持续喂数据（包括静音段）
    vad_update(core, frame);

    if (max_db < core->cfg.voice_min_db) {
        return 0;   // 静音
    }

    // cos 轴：MIC1 与 MIC4；sin 轴：MIC3 与 MIC2
    float conf14 = 0.0f;
    float lag14 = pair_delay(core, map[0], map[3], &conf14);
    float conf23 = 0.0f;
    float lag23 = pair_delay(core, map[2], map[1], &conf23);

    r.confidence = (fabsf(conf14) + fabsf(conf23)) * 0.5f;
    r.is_voice = core->vad_speech;

    if (core->mode == SL_CORE_MODE_VOICE && !r.is_voice) {
        return 0;
    }
    if (r.confidence < core->cfg.min_confidence) {
        return 0;
    }

    float scale = core->cfg.speed_of_sound / (core->cfg.mic_spacing_m * (float)core->cfg.sample_rate);
    float cos_t = clampf(lag14 * scale, -1.0f, 1.0f);
    float sin_t = clampf(lag23 * scale, -1.0f, 1.0f);

    const float alpha = 0.35f;
    core->sm_x = (1.0f - alpha) * core->sm_x + alpha * cos_t;
    core->sm_y = (1.0f - alpha) * core->sm_y + alpha * sin_t;

    float angle = atan2f(core->sm_y, core->sm_x) * 180.0f / SL_CORE_PI_F;
    if (!core->cfg.clockwise) {
        angle = -angle;
    }
    angle += core->cfg.angle_offset_deg;
    while (angle < 0.0f) {
        angle += 360.0f;
    }
    while (angle >= 360.0f) {
        angle -= 360.0f;
    }

    r.angle_deg = angle;
    int leds = core->cfg.led_count > 0 ? core->cfg.led_count : 24;
    r.led_index = (int)lroundf(angle / (360.0f / (float)leds)) % leds;

    if (core->mode == SL_CORE_MODE_PEAK) {
        // 两帧延迟：确认上一帧是局部峰值后再发射，并把当前帧记为新的待定帧
        int emitted = 0;
        if (core->has_pending && max_db < core->pending_db) {
            *out = core->pending;
            out->max_level_db = core->pending_db;
            core->has_pending = 0;
            emitted = 1;
        }
        if (!core->has_pending) {
            core->pending = r;
            core->pending_db = max_db;
            core->has_pending = 1;
        }
        return emitted;
    }

    *out = r;
    return 1;
}

sl_core_t *sl_core_create(const sl_core_cfg_t *cfg)
{
    if (cfg == NULL) {
        return NULL;
    }
    int n = cfg->frame_len;
    if (n < 256 || n > SL_CORE_MAX_FRAME || (n & (n - 1)) != 0) {
        return NULL;
    }

    sl_core_t *core = (sl_core_t *)calloc(1, sizeof(sl_core_t));
    if (core == NULL) {
        return NULL;
    }
    core->cfg = *cfg;
    if (core->cfg.speed_of_sound <= 0.0f) {
        core->cfg.speed_of_sound = 343.0f;
    }
    if (core->cfg.sample_rate == 0) {
        core->cfg.sample_rate = 48000;
    }
    if (core->cfg.mic_spacing_m <= 0.0f) {
        core->cfg.mic_spacing_m = 0.04f;
    }
    core->mode = SL_CORE_MODE_TRACK;
    core->sm_x = 1.0f;
    core->sm_y = 0.0f;

    core->sig = (int16_t *)calloc((size_t)n * SL_CORE_CHANNELS, sizeof(int16_t));
    core->win = (float *)calloc((size_t)n, sizeof(float));
    core->a_re = (float *)calloc((size_t)n, sizeof(float));
    core->a_im = (float *)calloc((size_t)n, sizeof(float));
    core->b_re = (float *)calloc((size_t)n, sizeof(float));
    core->b_im = (float *)calloc((size_t)n, sizeof(float));

    core->max_lag = (int)ceilf(core->cfg.mic_spacing_m * (float)core->cfg.sample_rate /
                               core->cfg.speed_of_sound) + 1;
    if (core->max_lag < 2) {
        core->max_lag = 2;
    }
    if (core->max_lag > n / 8) {
        core->max_lag = n / 8;
    }
    core->peak_buf = (float *)calloc((size_t)(2 * core->max_lag + 1), sizeof(float));

    if (!core->sig || !core->win || !core->a_re || !core->a_im ||
        !core->b_re || !core->b_im || !core->peak_buf) {
        sl_core_destroy(core);
        return NULL;
    }

    for (int i = 0; i < n; i++) {
        core->win[i] = 0.5f - 0.5f * cosf(2.0f * SL_CORE_PI_F * (float)i / (float)(n - 1));
    }
    return core;
}

void sl_core_destroy(sl_core_t *core)
{
    if (core == NULL) {
        return;
    }
    if (core->vad) {
        vad_destroy(core->vad);
        core->vad = NULL;
    }
    free(core->sig);
    free(core->win);
    free(core->a_re);
    free(core->a_im);
    free(core->b_re);
    free(core->b_im);
    free(core->peak_buf);
    free(core);
}

int sl_core_frame_len(const sl_core_t *core)
{
    return core ? core->cfg.frame_len : 0;
}

void sl_core_set_mode(sl_core_t *core, sl_core_mode_t mode)
{
    if (core == NULL) {
        return;
    }
    core->mode = mode;
    core->has_pending = 0;
}

sl_core_mode_t sl_core_get_mode(const sl_core_t *core)
{
    return core ? core->mode : SL_CORE_MODE_OFF;
}

void sl_core_reset(sl_core_t *core)
{
    if (core == NULL) {
        return;
    }
    core->sm_x = 1.0f;
    core->sm_y = 0.0f;
    core->has_pending = 0;
    core->pending_db = 0.0f;
}

const char *sl_core_version(void)
{
    return "1.0.0";
}
