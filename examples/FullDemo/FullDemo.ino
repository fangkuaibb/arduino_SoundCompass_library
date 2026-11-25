// 完整示例：对应 IDF 工程 main/main.c
//
// 功能：麦克风采集 + 声源定位 + 灯环跟随 + 串口按键切换模式
// 串口 115200，直接敲字符：
//   灯效：'1' POINT  '2' TRAIL  '3' BREATH  '4' COMET  '5' OFF
//   定位：'q' TRACK  'w' PEAK   'e' VOICE   'r' OFF
#include <SoundCompass.h>

// ================== 接线配置：按实际接线修改这里 ==================
#define PIN_I2C_SCL     5
#define PIN_I2C_SDA     6
#define PIN_I2S_MCLK    16
#define PIN_I2S_BCLK    17
#define PIN_I2S_WS      18
#define PIN_I2S_DIN     15
#define PIN_LED         4
#define LED_COUNT       24
// 声学参数
#define MIC_SPACING_MM  40       // 对角麦克风间距
#define MIC_GAIN_DB     30       // 麦克风增益 0~42
#define ANGLE_OFFSET_DEG 0       // 角度补偿，正值顺时针
#define ANGLE_CLOCKWISE 0        // 0 = 镜像（默认）
#define MIN_CONFIDENCE  0.15f    // 互相关置信度门限，越低越灵敏
#define VOICE_MIN_DB    (-52.0f) // 触发门限，必须高于环境底噪
#define TRAIL_DECAY_MS  1000     // 拖尾衰减时间，越小灭得越快
#define LED_IDLE_MS     500      // 无声多久后熄灭
#define TRAIL_LEN       3        // 拖尾长度(灯珠数)
// 逻辑 MIC1~MIC4 对应的物理采集通道
#define MIC_CH1         0
#define MIC_CH2         1
#define MIC_CH3         2
#define MIC_CH4         3
// =================================================================

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

    Serial.printf("angle=%5.1f LED%02d conf=%.2f %s db=%.0f/%.0f/%.0f/%.0f\n",
                  r.angleDeg, r.ledIndex, r.confidence,
                  r.isVoice ? "voice" : "sound",
                  r.levelDb[0], r.levelDb[1], r.levelDb[2], r.levelDb[3]);
}

void setup()
{
    Serial.begin(115200);
    delay(300);

    MicArray::Config micCfg;
    micCfg.sclGpio = PIN_I2C_SCL;
    micCfg.sdaGpio = PIN_I2C_SDA;
    micCfg.mclkGpio = PIN_I2S_MCLK;
    micCfg.bclkGpio = PIN_I2S_BCLK;
    micCfg.wsGpio = PIN_I2S_WS;
    micCfg.dinGpio = PIN_I2S_DIN;
    micCfg.gainDb = MIC_GAIN_DB;
    if (!mic.begin(micCfg)) {
        Serial.println("音频输入初始化失败");
        while (true) {
            delay(1000);
        }
    }

    LedRing::Config ringCfg;
    ringCfg.pin = PIN_LED;
    ringCfg.ledCount = LED_COUNT;
    ringCfg.mode = LedRing::TRAIL;
    ringCfg.colorMode = LedRing::RAINBOW;
    ringCfg.decayMs = TRAIL_DECAY_MS;
    ringCfg.idleTimeoutMs = LED_IDLE_MS;
    ringCfg.trailLength = TRAIL_LEN;
    if (!ring.begin(ringCfg)) {
        Serial.println("灯效初始化失败");
        while (true) {
            delay(1000);
        }
    }

    SoundLocalizer::Config cfg;
    cfg.sampleRate = 48000;
    cfg.micSpacingM = (float)MIC_SPACING_MM / 1000.0f;
    cfg.angleOffsetDeg = (float)ANGLE_OFFSET_DEG;
    cfg.clockwise = ANGLE_CLOCKWISE ? true : false;
    cfg.minConfidence = MIN_CONFIDENCE;
    cfg.voiceMinDb = VOICE_MIN_DB;
    cfg.ledCount = LED_COUNT;
    cfg.micMap[0] = MIC_CH1;
    cfg.micMap[1] = MIC_CH2;
    cfg.micMap[2] = MIC_CH3;
    cfg.micMap[3] = MIC_CH4;
    cfg.mode = SoundLocalizer::TRACK;

    if (!locator.begin(mic, cfg, onSound)) {
        Serial.println("定位模块初始化失败");
        while (true) {
            delay(1000);
        }
    }

    Serial.printf("ready: SCL=%d SDA=%d MCLK=%d BCLK=%d WS=%d DIN=%d LED=%d x%d\n",
                  PIN_I2C_SCL, PIN_I2C_SDA, PIN_I2S_MCLK, PIN_I2S_BCLK,
                  PIN_I2S_WS, PIN_I2S_DIN, PIN_LED, LED_COUNT);
    Serial.println("按键 1~5 切灯效，q/w/e/r 切定位模式");
}

void loop()
{
    while (Serial.available() > 0) {
        int c = Serial.read();
        switch (c) {
        case '1':
            ring.setMode(LedRing::POINT);
            Serial.println("灯效 = POINT（单点）");
            break;
        case '2':
            ring.setMode(LedRing::TRAIL);
            Serial.println("灯效 = TRAIL（拖尾）");
            break;
        case '3':
            ring.setMode(LedRing::BREATH);
            Serial.println("灯效 = BREATH（呼吸）");
            break;
        case '4':
            ring.setMode(LedRing::COMET);
            Serial.println("灯效 = COMET（彗星）");
            break;
        case '5':
            ring.setMode(LedRing::OFF);
            ring.clear();
            Serial.println("灯效 = OFF（全灭）");
            break;
        case 'q':
            locator.setMode(SoundLocalizer::TRACK);
            Serial.println("定位模式 = TRACK（实时跟踪）");
            break;
        case 'w':
            locator.setMode(SoundLocalizer::PEAK);
            Serial.println("定位模式 = PEAK（峰值触发）");
            break;
        case 'e':
            locator.setMode(SoundLocalizer::VOICE);
            Serial.println("定位模式 = VOICE（仅人声）");
            break;
        case 'r':
            locator.setMode(SoundLocalizer::OFF);
            Serial.println("定位模式 = OFF（暂停定位）");
            break;
        default:
            break;
        }
    }
    delay(20);
}
