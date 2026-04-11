#include "Encoder.h"

// --- FIX LỖI ENCODER: Khai báo mượn biến Queue từ main ---
extern QueueHandle_t xEncoderQueue;

EncoderLogic encoder;
AiEsp32RotaryEncoder rotaryEncoder = AiEsp32RotaryEncoder(ROTARY_A_PIN, ROTARY_B_PIN, ROTARY_BTN_PIN, -1, ROTARY_STEPS);

void IRAM_ATTR readEncoderISR() {
    rotaryEncoder.readEncoder_ISR();
}

void EncoderLogic::begin() {
    rotaryEncoder.begin();
    rotaryEncoder.setup(readEncoderISR);
    
    // CÚ CHỐT 1: Tắt hoàn toàn gia tốc. Vặn 1 khấc là chỉ nhảy 1 đơn vị, cấm nhảy cóc!
    rotaryEncoder.disableAcceleration(); 

    pinMode(ROTARY_BTN_PIN, INPUT_PULLUP);
    lastInteractionTime = millis();
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
    // ==========================================
    // 1. XỬ LÝ VẶN NÚM
    // ==========================================
    if (rotaryEncoder.encoderChanged()) {
        uint32_t current_time = millis();
        if (current_time - last_enc_time > 30) { 
            int current_val = rotaryEncoder.readEncoder();
            EncoderEvent ev = (current_val > lastEncoderValue) ? ENC_UP : ENC_DOWN;
            lastEncoderValue = current_val;
            lastInteractionTime = current_time;
            last_enc_time = current_time;
            if (xEncoderQueue != NULL) xQueueSend(xEncoderQueue, &ev, 0);
        } else {
            rotaryEncoder.setEncoderValue(lastEncoderValue);
        }
    }

    // ==========================================
    // 2. XỬ LÝ NÚT BẤM — KHÔNG dùng vTaskDelay
    // ==========================================
    uint8_t currentButtonState = digitalRead(ROTARY_BTN_PIN);

    if (currentButtonState != lastButtonState) {
        if (millis() - btnDebounceTime > 20) {  // ← debounce bằng timestamp
            btnDebounceTime = millis();
            lastButtonState = currentButtonState;

            if (currentButtonState == LOW) {
                buttonPressTime = millis();
                isLongPressHandled = false;
            } else {
                if (!isLongPressHandled && (millis() - buttonPressTime < 1000)) {
                    lastInteractionTime = millis();
                    EncoderEvent ev = ENC_CLICK;
                    if (xEncoderQueue != NULL) xQueueSend(xEncoderQueue, &ev, 0);
                }
            }
        }
    }

    // Long press
    if (currentButtonState == LOW && !isLongPressHandled) {
        if (millis() - buttonPressTime >= 1000) {
            isLongPressHandled = true;
            lastInteractionTime = millis();
            EncoderEvent ev = ENC_LONG_PRESS;
            if (xEncoderQueue != NULL) xQueueSend(xEncoderQueue, &ev, 0);
        }
    }
}