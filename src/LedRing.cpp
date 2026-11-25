#include "LedRing.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LedRing";

#define RMT_RESOLUTION_HZ   10000000
#define COMET_SPEED_LPS     120.0f
#define BREATH_PERIOD_S     1.0f
#define POINT_DECAY_S       0.04f
#define POINT_HOLD_US       120000
#define TRAIL_HIST_MAX      16

// 10 MHz 分辨率下的 WS2812 时序（0.4us / 0.9us / 0.8us / 0.4us / 150us）
static void makeSymbol(rmt_symbol_word_t *s, uint32_t d0, int l0, uint32_t d1, int l1)
{
    s->level0 = l0;
    s->duration0 = d0;
    s->level1 = l1;
    s->duration1 = d1;
}

LedRing::LedRing()
{
}

LedRing::~LedRing()
{
    end();
}

void LedRing::hueToRgb(int idx, int leds, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float h6 = (float)idx / (float)leds * 6.0f;
    int seg = (int)h6 % 6;
    float f = h6 - floorf(h6);
    uint8_t t = (uint8_t)(255.0f * f);
    uint8_t q = (uint8_t)(255.0f * (1.0f - f));
    switch (seg) {
    case 0:  *r = 255; *g = t;   *b = 0;   break;
    case 1:  *r = q;   *g = 255; *b = 0;   break;
    case 2:  *r = 0;   *g = 255; *b = t;   break;
    case 3:  *r = 0;   *g = q;   *b = 255; break;
    case 4:  *r = t;   *g = 0;   *b = 255; break;
    default: *r = 255; *g = 0;   *b = q;   break;
    }
}

void LedRing::transmit()
{
    const int leds = _cfg.ledCount;
    const int bytes = leds * 3;

    rmt_symbol_word_t sym_zero, sym_one, sym_reset;
    makeSymbol(&sym_zero, 4, 1, 9, 0);
    makeSymbol(&sym_one, 8, 1, 4, 0);
    makeSymbol(&sym_reset, 1500, 0, 1500, 0);

    for (int i = 0; i < leds; i++) {
        float lv = _level[i];
        if (lv < 0.0f) {
            lv = 0.0f;
        } else if (lv > 1.0f) {
            lv = 1.0f;
        }
        uint8_t v = (uint8_t)(lv * _cfg.maxBrightness);
        if (v < 3) {
            v = 0;
        }

        uint8_t br = _cfg.r, bg = _cfg.g, bb = _cfg.b;
        if (_colorMode == RAINBOW) {
            hueToRgb(i, leds, &br, &bg, &bb);
        }

        uint8_t r = (uint8_t)((uint32_t)br * v / 255);
        uint8_t g = (uint8_t)((uint32_t)bg * v / 255);
        uint8_t b = (uint8_t)((uint32_t)bb * v / 255);
        if (_cfg.grbOrder) {
            _pixels[i * 3 + 0] = g;
            _pixels[i * 3 + 1] = r;
            _pixels[i * 3 + 2] = b;
        } else {
            _pixels[i * 3 + 0] = r;
            _pixels[i * 3 + 1] = g;
            _pixels[i * 3 + 2] = b;
        }
    }

    size_t pos = 0;
    for (int i = 0; i < bytes; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            _syms[pos++] = (_pixels[i] & (1 << bit)) ? sym_one : sym_zero;
        }
    }
    _syms[pos++] = sym_reset;

    rmt_transmit_config_t tx_cfg = {};
    tx_cfg.loop_count = 0;
    if (rmt_transmit(_chan, _enc, _syms, pos * sizeof(rmt_symbol_word_t), &tx_cfg) == ESP_OK) {
        rmt_tx_wait_all_done(_chan, 100);
    }
}

void LedRing::setLevel(int idx, float v)
{
    const int leds = _cfg.ledCount;
    if (idx < 0) {
        idx += leds;
    }
    idx %= leds;
    if (v > _level[idx]) {
        _level[idx] = v;
    }
}

void LedRing::update(float dt)
{
    const int leds = _cfg.ledCount;
    float decay_s = _cfg.decayMs > 0 ? (float)_cfg.decayMs / 1000.0f : 0.5f;

    if (_hasTarget && _cfg.idleTimeoutMs > 0) {
        int64_t idle_us = (int64_t)_cfg.idleTimeoutMs * 1000;
        if (esp_timer_get_time() - _lastTargetUs > idle_us) {
            _hasTarget = false;
        }
    }

    if (_mode == POINT) {
        decay_s = POINT_DECAY_S;
    }
    if (!_hasTarget) {
        decay_s = fminf(decay_s, 0.12f);
    }
    float k = expf(-dt / decay_s);
    for (int i = 0; i < leds; i++) {
        _level[i] *= k;
        if (_level[i] < 0.02f) {
            _level[i] = 0.0f;
        }
    }

    _breathPhase += dt / BREATH_PERIOD_S;
    if (_breathPhase >= 1.0f) {
        _breathPhase -= 1.0f;
    }

    if (!_hasTarget || _mode == OFF) {
        return;
    }

    float intensity = _pendingIntensity;

    switch (_mode) {
    case POINT:
        if (esp_timer_get_time() - _lastTargetUs < POINT_HOLD_US) {
            setLevel(_target, intensity);
        }
        break;
    case TRAIL: {
        int keep = _cfg.trailLength;
        if (keep < 1) {
            keep = 1;
        } else if (keep > TRAIL_HIST_MAX) {
            keep = TRAIL_HIST_MAX;
        }
        int64_t now_us = esp_timer_get_time();

        if (_target != _trailPrev) {
            for (int k = keep - 1; k > 0; k--) {
                _trailHist[k] = _trailHist[k - 1];
            }
            _trailHist[0] = _target;
            if (_trailCnt < keep) {
                _trailCnt++;
            }
            _trailPrev = _target;
            _lastMoveUs = now_us;
        }

        if (_trailCnt > 1 &&
            now_us - _lastMoveUs > (int64_t)_cfg.decayMs * 3000) {
            _trailHist[0] = _target;
            _trailCnt = 1;
        }

        for (int i = 0; i < leds; i++) {
            _level[i] = 0.0f;
        }
        for (int k = 0; k < _trailCnt; k++) {
            float f = 1.0f - 0.3f * (float)k;
            if (f < 0.2f) {
                f = 0.2f;
            }
            int idx = (_trailHist[k] % leds + leds) % leds;
            _level[idx] = intensity * f;
        }
        break;
    }
    case COMET: {
        float diff = (float)_target - _head;
        float half = (float)leds / 2.0f;
        while (diff > half) {
            diff -= (float)leds;
        }
        while (diff < -half) {
            diff += (float)leds;
        }
        float step = COMET_SPEED_LPS * dt;
        if (diff > step) {
            diff = step;
        } else if (diff < -step) {
            diff = -step;
        }
        _head += diff;
        while (_head < 0.0f) {
            _head += (float)leds;
        }
        while (_head >= (float)leds) {
            _head -= (float)leds;
        }
        int i0 = (int)floorf(_head);
        float frac = _head - (float)i0;
        setLevel(i0, intensity * (1.0f - frac));
        setLevel(i0 + 1, intensity * frac);
        break;
    }
    case BREATH: {
        float m = 0.15f + 0.85f * (0.5f + 0.5f * sinf(2.0f * 3.14159265f * _breathPhase));
        for (int i = 0; i < leds; i++) {
            _level[i] = 0.0f;
        }
        _level[(_target % leds + leds) % leds] = intensity * m;
        break;
    }
    default:
        break;
    }
}

bool LedRing::allDark() const
{
    for (int i = 0; i < _cfg.ledCount; i++) {
        if (_level[i] > 0.01f) {
            return false;
        }
    }
    return true;
}

void LedRing::taskLoop()
{
    int64_t last = esp_timer_get_time();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(_cfg.refreshMs > 0 ? _cfg.refreshMs : 25));
        int64_t now = esp_timer_get_time();
        float dt = (float)(now - last) / 1000000.0f;
        last = now;

        update(dt);

        if (!_hasTarget && allDark()) {
            if (!_sentOff) {
                transmit();
                _sentOff = true;
            }
        } else {
            transmit();
            _sentOff = false;
        }
    }
}

void LedRing::taskEntry(void *arg)
{
    static_cast<LedRing *>(arg)->taskLoop();
}

bool LedRing::begin()
{
    return begin(Config());
}

bool LedRing::begin(const Config &cfg)
{
    if (_chan != nullptr) {
        return true;
    }
    if (cfg.ledCount <= 0 || cfg.ledCount > MAX_LEDS) {
        ESP_LOGE(TAG, "ledCount must be 1..%d", MAX_LEDS);
        return false;
    }
    _cfg = cfg;
    if (_cfg.refreshMs <= 0) {
        _cfg.refreshMs = 25;
    }
    if (_cfg.maxBrightness == 0) {
        _cfg.maxBrightness = 255;
    }
    _mode = _cfg.mode;
    _colorMode = _cfg.colorMode;
    _target = 0;
    _head = 0.0f;
    _hasTarget = false;
    _sentOff = false;
    _trailPrev = -1;
    _trailCnt = 0;

    _syms = (rmt_symbol_word_t *)calloc(MAX_SYMBOLS, sizeof(rmt_symbol_word_t));
    _pixels = (uint8_t *)calloc(MAX_LEDS * 3, sizeof(uint8_t));
    _level = (float *)calloc(MAX_LEDS, sizeof(float));
    if (_syms == nullptr || _pixels == nullptr || _level == nullptr) {
        ESP_LOGE(TAG, "out of memory");
        end();
        return false;
    }

    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num = (gpio_num_t)_cfg.pin;
    tx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz = RMT_RESOLUTION_HZ;
    tx_cfg.mem_block_symbols = 64;
    tx_cfg.trans_queue_depth = 4;
    if (rmt_new_tx_channel(&tx_cfg, &_chan) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed");
        end();
        return false;
    }

    rmt_copy_encoder_config_t enc_cfg = {};
    if (rmt_new_copy_encoder(&enc_cfg, &_enc) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_copy_encoder failed");
        end();
        return false;
    }
    if (rmt_enable(_chan) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed");
        end();
        return false;
    }

    transmit();

    if (xTaskCreate(taskEntry, "led_fx", 3072, this, 4, &_task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        _task = nullptr;
        end();
        return false;
    }
    ESP_LOGI(TAG, "init: gpio=%d leds=%d mode=%d", _cfg.pin, _cfg.ledCount, (int)_cfg.mode);
    return true;
}

void LedRing::end()
{
    if (_task != nullptr) {
        vTaskDelete(_task);
        _task = nullptr;
    }
    if (_enc != nullptr) {
        rmt_del_encoder(_enc);
        _enc = nullptr;
    }
    if (_chan != nullptr) {
        rmt_disable(_chan);
        rmt_del_channel(_chan);
        _chan = nullptr;
    }
    free(_syms);
    _syms = nullptr;
    free(_pixels);
    _pixels = nullptr;
    free(_level);
    _level = nullptr;
}

void LedRing::setMode(Mode mode)
{
    _mode = mode;
}

LedRing::Mode LedRing::getMode() const
{
    return (Mode)_mode;
}

void LedRing::setTarget(int ledIndex, float intensity)
{
    _target = ledIndex;
    _pendingIntensity = intensity;
    _hasTarget = true;
    _lastTargetUs = esp_timer_get_time();
    _sentOff = false;
}

void LedRing::clear()
{
    _hasTarget = false;
    _sentOff = false;
    if (_level != nullptr) {
        memset(_level, 0, sizeof(float) * MAX_LEDS);
    }
}

void LedRing::setColorMode(ColorMode mode)
{
    if (mode != RAINBOW && mode != SOLID) {
        return;
    }
    _colorMode = mode;
}

LedRing::ColorMode LedRing::getColorMode() const
{
    return (ColorMode)_colorMode;
}

void LedRing::setColor(uint8_t r, uint8_t g, uint8_t b)
{
    _cfg.r = r;
    _cfg.g = g;
    _cfg.b = b;
    _colorMode = SOLID;
}
