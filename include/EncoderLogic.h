#ifndef ENCODER_LOGIC_H
#define ENCODER_LOGIC_H

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

private:
    int lastEncoderValue = 0;
    uint32_t lastInteractionTime = 0;
    uint8_t lastButtonState = HIGH;
    uint32_t buttonPressTime = 0;
    bool isLongPressHandled = false;
};

extern EncoderLogic encoder;

#endif