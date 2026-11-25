// 基础示例：只做声源定位，通过串口打印角度
//
// 接线（默认值，可在 MicArray::Config 中修改）：
//   ES7210 SCL=5  SDA=6
//   I2S MCLK=16 BCLK=17 WS=18 DIN=15
#include <SoundCompass.h>

MicArray mic;
SoundLocalizer locator;

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("\nSoundCompass - BasicAngle");

    MicArray::Config micCfg;
    if (!mic.begin(micCfg)) {
        Serial.println("MicArray init failed: check ES7210 wiring / I2C pins");
        while (true) {
            delay(1000);
        }
    }

    SoundLocalizer::Config cfg;
    cfg.micSpacingM = 0.040f;   // 对角麦克风间距 40mm
    cfg.clockwise = false;      // 与灯环方向相反时置为 false
    cfg.ledCount = 24;
    cfg.minConfidence = 0.15f;
    cfg.voiceMinDb = -52.0f;
    cfg.mode = SoundLocalizer::TRACK;

    if (!locator.begin(mic, cfg)) {
        Serial.println("SoundLocalizer init failed");
        while (true) {
            delay(1000);
        }
    }
    Serial.println("ready");
}

void loop()
{
    SoundLocalizer::Result r;
    if (locator.getResult(r)) {
        Serial.printf("angle=%5.1f  led=%02d  conf=%.2f  %s  db=%.0f/%.0f/%.0f/%.0f\n",
                      r.angleDeg, r.ledIndex, r.confidence,
                      r.isVoice ? "voice" : "sound",
                      r.levelDb[0], r.levelDb[1], r.levelDb[2], r.levelDb[3]);
    }
    delay(100);
}
