#ifndef ENCODER_H
#define ENCODER_H

#include <Arduino.h>
#include "Config.h"
#include "AiEsp32RotaryEncoder.h"

class EncoderLogic {
public:
    void begin();
    void loop();
    void setBoundaries(int minVal, int maxVal, bool circleValues);
    void setEncoderValue(int val);
    int getEncoderValue();
    bool shouldSleep(uint32_t timeout);
    void markInteraction();

private:
    int lastEncoderValue = 0;
    uint32_t lastInteractionTime = 0;
    uint8_t lastButtonState = HIGH;
    uint32_t buttonPressTime = 0;
    bool isLongPressHandled = false;
    uint32_t last_enc_time = 0;    // ← THÊM DÒNG NÀY
    uint32_t btnDebounceTime = 0;  // ← THÊM DÒNG NÀY (để khỏi dùng static trong loop)
};

extern EncoderLogic encoder;

#endif