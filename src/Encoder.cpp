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
    
    // Tắt gia tốc để vặn chuẩn xác 1 khấc = 1 số
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
    // Trả lại logic nguyên bản: Để thư viện tự chống rung bằng ngắt phần cứng, vặn mượt mà
    // ==========================================
    if (rotaryEncoder.encoderChanged()) {
        int current_val = rotaryEncoder.readEncoder();
        EncoderEvent ev = (current_val > lastEncoderValue) ? ENC_UP : ENC_DOWN;
        lastEncoderValue = current_val;
        lastInteractionTime = millis();

        if (xEncoderQueue != NULL) {
            xQueueSend(xEncoderQueue, &ev, 0);
        }
    }

    // ==========================================
    // 2. XỬ LÝ BẤM NÚT (CHỐNG NHÁY ĐÚP & LONG PRESS)
    // ==========================================
    uint8_t currentButtonState = digitalRead(ROTARY_BTN_PIN);
    
    if (currentButtonState != lastButtonState) {
        vTaskDelay(pdMS_TO_TICKS(20)); // Delay cứng 20ms để chống dội phím nhấn
        currentButtonState = digitalRead(ROTARY_BTN_PIN); // Đọc lại lần 2 cho chắc
        
        if (currentButtonState != lastButtonState) {
            if (currentButtonState == LOW) { 
                // Nút bắt đầu bị đè xuống
                buttonPressTime = millis();
                isLongPressHandled = false;
            } else { 
                // Nút được nhả ra
                if (!isLongPressHandled && (millis() - buttonPressTime < 1000)) {
                    // Nếu thời gian đè < 1 giây thì tính là Click bình thường
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

    // Xử lý giữ rịt nút (Long Press > 1 giây) để thoát Menu
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