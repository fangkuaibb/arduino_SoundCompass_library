// 环形灯跟随声源示例
//
// 接线（默认值，可在各自 Config 中修改）：
//   ES7210 SCL=5  SDA=6
//   I2S MCLK=16 BCLK=17 WS=18 DIN=15
//   WS2812 DIN=4，24 颗灯
#include <SoundCompass.h>

MicArray mic;
SoundLocalizer locator;
LedRing ring;

// 定位回调在库的后台任务里执行，不要在里面做长时间阻塞操作
void onSound(const SoundLocalizer::Result &r, void *user)
{
    (void)user;
    float lvl = (r.maxLevelDb + 60.0f) / 40.0f;
    if (lvl < 0.15f) {
        lvl = 0.15f;
    } else if (lvl > 1.0f) {
        lvl = 1.0f;
    }
    ring.setTarget(r.ledIndex, lvl);
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("\nSoundCompass - RingFollow");

    MicArray::Config micCfg;
    if (!mic.begin(micCfg)) {
        Serial.println("MicArray init failed");
        while (true) {
            delay(1000);
        }
    }

    LedRing::Config ringCfg;
    ringCfg.pin = 4;
    ringCfg.ledCount = 24;
    ringCfg.mode = LedRing::TRAIL;   // 可换成 POINT / BREATH / COMET / OFF
    ringCfg.colorMode = LedRing::RAINBOW;
    ringCfg.trailLength = 3;
    ringCfg.decayMs = 1000;
    ringCfg.idleTimeoutMs = 500;
    if (!ring.begin(ringCfg)) {
        Serial.println("LedRing init failed");
        while (true) {
            delay(1000);
        }
    }

    SoundLocalizer::Config cfg;
    cfg.micSpacingM = 0.040f;
    cfg.clockwise = false;
    cfg.ledCount = 24;
    cfg.minConfidence = 0.15f;
    cfg.voiceMinDb = -52.0f;
    cfg.mode = SoundLocalizer::TRACK;

    if (!locator.begin(mic, cfg, onSound)) {
        Serial.println("SoundLocalizer init failed");
        while (true) {
            delay(1000);
        }
    }
    Serial.println("ready");
}

void loop()
{
    // 灯效与定位都在库内部的任务里运行，这里可以做别的事
    delay(1000);
}
