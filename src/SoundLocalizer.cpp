#include "SoundLocalizer.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "SoundLocalizer";

SoundLocalizer::SoundLocalizer()
{
}

SoundLocalizer::~SoundLocalizer()
{
    end();
}

bool SoundLocalizer::begin(MicArray &mic)
{
    return begin(mic, Config(), nullptr, nullptr);
}

bool SoundLocalizer::begin(MicArray &mic, const Config &cfg)
{
    return begin(mic, cfg, nullptr, nullptr);
}

bool SoundLocalizer::begin(MicArray &mic, const Config &cfg, Callback cb, void *user)
{
    if (_core != nullptr) {
        return true;
    }
    if (!mic.isReady()) {
        ESP_LOGE(TAG, "MicArray not initialized, call mic.begin() first");
        return false;
    }

    sl_core_cfg_t core_cfg;
    memset(&core_cfg, 0, sizeof(core_cfg));
    core_cfg.sample_rate = cfg.sampleRate != 0 ? cfg.sampleRate : mic.sampleRate();
    core_cfg.mic_spacing_m = cfg.micSpacingM;
    core_cfg.speed_of_sound = 343.0f;
    core_cfg.angle_offset_deg = cfg.angleOffsetDeg;
    core_cfg.clockwise = cfg.clockwise ? 1 : 0;
    for (int i = 0; i < 4; i++) {
        core_cfg.mic_map[i] = cfg.micMap[i];
    }
    core_cfg.frame_len = cfg.frameLength;
    core_cfg.led_count = cfg.ledCount;
    core_cfg.voice_min_db = cfg.voiceMinDb;
    core_cfg.min_confidence = cfg.minConfidence;

    _core = sl_core_create(&core_cfg);
    if (_core == nullptr) {
        ESP_LOGE(TAG, "sl_core_create failed (frameLength must be 256..4096 power of 2)");
        return false;
    }
    _frameLen = sl_core_frame_len(_core);
    _frame = (int16_t *)calloc((size_t)_frameLen * 4, sizeof(int16_t));
    _mutex = xSemaphoreCreateMutex();
    if (_frame == nullptr || _mutex == nullptr) {
        ESP_LOGE(TAG, "out of memory");
        end();
        return false;
    }

    _mic = &mic;
    _cb = cb;
    _user = user;
    _stopRequested = false;
    sl_core_set_mode(_core, (sl_core_mode_t)cfg.mode);

    if (!start(cb, user)) {
        end();
        return false;
    }

    ESP_LOGI(TAG, "init: fs=%u frame=%d leds=%d spacing=%.3fm",
             (unsigned)core_cfg.sample_rate, _frameLen, core_cfg.led_count,
             core_cfg.mic_spacing_m);
    return true;
}

bool SoundLocalizer::start(Callback cb, void *user)
{
    if (_core == nullptr) {
        return false;
    }
    if (_task != nullptr) {
        return true;
    }
    _cb = cb;
    _user = user;
    _stopRequested = false;
    if (xTaskCreate(taskEntry, "sound_loc", 6144, this, 5, &_task) != pdPASS) {
        _task = nullptr;
        ESP_LOGE(TAG, "task create failed");
        return false;
    }
    return true;
}

void SoundLocalizer::stop()
{
    if (_task == nullptr) {
        return;
    }
    _stopRequested = true;
    for (int i = 0; i < 100 && _task != nullptr; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (_task != nullptr) {
        vTaskDelete(_task);
        _task = nullptr;
    }
}

void SoundLocalizer::end()
{
    stop();
    if (_core != nullptr) {
        sl_core_destroy(_core);
        _core = nullptr;
    }
    if (_mutex != nullptr) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
    free(_frame);
    _frame = nullptr;
    _frameLen = 0;
    _mic = nullptr;
    _cb = nullptr;
    _user = nullptr;
    _hasResult = false;
}

void SoundLocalizer::taskEntry(void *arg)
{
    SoundLocalizer *self = static_cast<SoundLocalizer *>(arg);
    self->run();
    self->_task = nullptr;
    vTaskDelete(nullptr);
}

void SoundLocalizer::run()
{
    while (!_stopRequested && _core != nullptr) {
        if (sl_core_get_mode(_core) == SL_CORE_MODE_OFF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t got = _mic->read(_frame, (size_t)_frameLen, 200);
        if (got != (size_t)_frameLen) {
            vTaskDelay(1);
            continue;
        }

        sl_core_result_t r;
        if (sl_core_process(_core, _frame, &r)) {
            storeResult(r);
            if (_cb != nullptr) {
                Result out;
                out.angleDeg = r.angle_deg;
                out.ledIndex = r.led_index;
                out.confidence = r.confidence;
                out.maxLevelDb = r.max_level_db;
                out.isVoice = r.is_voice != 0;
                for (int i = 0; i < 4; i++) {
                    out.levelDb[i] = r.level_db[i];
                }
                _cb(out, _user);
            }
        }
        vTaskDelay(1);
    }
}

void SoundLocalizer::storeResult(const sl_core_result_t &r)
{
    if (_mutex != nullptr) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
    }
    _last.angleDeg = r.angle_deg;
    _last.ledIndex = r.led_index;
    _last.confidence = r.confidence;
    _last.maxLevelDb = r.max_level_db;
    _last.isVoice = r.is_voice != 0;
    for (int i = 0; i < 4; i++) {
        _last.levelDb[i] = r.level_db[i];
    }
    _hasResult = true;
    if (_mutex != nullptr) {
        xSemaphoreGive(_mutex);
    }
}

void SoundLocalizer::setMode(Mode mode)
{
    if (_core != nullptr) {
        sl_core_set_mode(_core, (sl_core_mode_t)mode);
    }
}

SoundLocalizer::Mode SoundLocalizer::getMode() const
{
    if (_core == nullptr) {
        return OFF;
    }
    return (Mode)sl_core_get_mode(_core);
}

bool SoundLocalizer::getResult(Result &out) const
{
    if (_mutex == nullptr) {
        return false;
    }
    xSemaphoreTake(_mutex, portMAX_DELAY);
    out = _last;
    bool valid = _hasResult;
    xSemaphoreGive(_mutex);
    return valid;
}

float SoundLocalizer::getAngle() const
{
    if (_mutex == nullptr) {
        return NAN;
    }
    xSemaphoreTake(_mutex, portMAX_DELAY);
    float angle = _hasResult ? _last.angleDeg : NAN;
    xSemaphoreGive(_mutex);
    return angle;
}

int SoundLocalizer::getLedIndex() const
{
    if (_mutex == nullptr) {
        return -1;
    }
    xSemaphoreTake(_mutex, portMAX_DELAY);
    int index = _hasResult ? _last.ledIndex : -1;
    xSemaphoreGive(_mutex);
    return index;
}
