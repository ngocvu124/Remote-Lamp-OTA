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
    static uint32_t last_enc_time = 0;
    
    // ==========================================
    // 1. XỬ LÝ VẶN NÚM (CÓ DEBOUNCE CHỐNG RUNG)
    // ==========================================
    if (rotaryEncoder.encoderChanged()) {
        uint32_t current_time = millis();
        
        // CÚ CHỐT 2: Phải cách nhau ít nhất 30ms mới tính là 1 lần vặn.
        // Dưới 30ms thì chắc chắn là do lá đồng bên trong đang bị rung (Bouncing)
        if (current_time - last_enc_time > 30) { 
            int current_val = rotaryEncoder.readEncoder();
            EncoderEvent ev = (current_val > lastEncoderValue) ? ENC_UP : ENC_DOWN;
            lastEncoderValue = current_val;
            lastInteractionTime = current_time;
            last_enc_time = current_time;

            if (xEncoderQueue != NULL) {
                xQueueSend(xEncoderQueue, &ev, 0);
            }
        } else {
            // Nếu phát hiện rung nhiễu, ép nó trả lại giá trị cũ, không cho nhảy số!
            rotaryEncoder.setEncoderValue(lastEncoderValue);
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