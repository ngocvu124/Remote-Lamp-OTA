#include "Encoder.h"

extern QueueHandle_t xEncoderQueue;

EncoderLogic encoder;
AiEsp32RotaryEncoder rotaryEncoder = AiEsp32RotaryEncoder(ROTARY_A_PIN, ROTARY_B_PIN, ROTARY_BTN_PIN, -1, ROTARY_STEPS);

void IRAM_ATTR readEncoderISR() {
    rotaryEncoder.readEncoder_ISR();
}

void EncoderLogic::begin() {
    rotaryEncoder.begin();
    rotaryEncoder.setup(readEncoderISR);
    
    // Mặc định khởi động lên màn hình chính là bật sẵn gia tốc
    rotaryEncoder.setAcceleration(250); 

    pinMode(ROTARY_BTN_PIN, INPUT_PULLUP);
    lastInteractionTime = millis();
}

// Hàm này sẽ được AppTask gọi để bật/tắt tùy theo Menu
void EncoderLogic::setAcceleration(bool enabled) {
    if (enabled) {
        rotaryEncoder.setAcceleration(250);
    } else {
        rotaryEncoder.disableAcceleration();
    }
}

void EncoderLogic::setBoundaries(int minVal, int maxVal, bool circleValues) {
    rotaryEncoder.setBoundaries(minVal, maxVal, circleValues);
}

void EncoderLogic::setEncoderValue(int val) {
    rotaryEncoder.setEncoderValue(val);
    lastEncoderValue = val;
}

int EncoderLogic::getEncoderValue() {
    return rotaryEncoder.readEncoder();
}

bool EncoderLogic::shouldSleep(uint32_t timeout) {
    return (millis() - lastInteractionTime > timeout);
}

void EncoderLogic::loop() {
    if (rotaryEncoder.encoderChanged()) {
        int current_val = rotaryEncoder.readEncoder();
        EncoderEvent ev = (current_val > lastEncoderValue) ? ENC_UP : ENC_DOWN;
        lastEncoderValue = current_val;
        lastInteractionTime = millis();

        if (xEncoderQueue != NULL) {
            xQueueSend(xEncoderQueue, &ev, 0);
        }
    }

    uint8_t currentButtonState = digitalRead(ROTARY_BTN_PIN);
    
    if (currentButtonState != lastButtonState) {
        vTaskDelay(pdMS_TO_TICKS(20)); // Delay cứng 20ms để chống dội phím nhấn
        currentButtonState = digitalRead(ROTARY_BTN_PIN); // Đọc lại lần 2 cho chắc
        
        if (currentButtonState != lastButtonState) {
            if (currentButtonState == LOW) { 
                buttonPressTime = millis();
                isLongPressHandled = false;
            } else { 
                if (!isLongPressHandled && (millis() - buttonPressTime < 1000)) {
                    lastInteractionTime = millis();
                    EncoderEvent ev = ENC_CLICK;
                    if (xEncoderQueue != NULL) {
                        xQueueSend(xEncoderQueue, &ev, 0);
                    }
                }
            }
            lastButtonState = currentButtonState;
        }
    }

    if (currentButtonState == LOW && !isLongPressHandled) {
        if (millis() - buttonPressTime >= 1000) {
            isLongPressHandled = true;
            lastInteractionTime = millis();
            EncoderEvent ev = ENC_LONG_PRESS;
            if (xEncoderQueue != NULL) {
                xQueueSend(xEncoderQueue, &ev, 0);
            }
        }
    }
}