#include "BatteryLogic.h"

BatteryLogic battery;
#define VOLTAGE_DIVIDER_RATIO 2.0 

void BatteryLogic::begin() {
    pinMode(PIN_BATTERY, INPUT);
    // Cấu hình ADC để đọc được dải điện áp lên tới 3.1V (sau phân áp)
    analogSetPinAttenuation(PIN_BATTERY, ADC_11db);
    Serial.println("[BATTERY] Voltage Sensor Initialized.");
}

float BatteryLogic::readRawVoltage() {
    uint32_t mvSum = 0;
    // Lấy trung bình nhiều mẫu để tránh nhiễu nhảy số lung tung
    for(int i = 0; i < SAMPLES; i++) {
        mvSum += analogReadMilliVolts(PIN_BATTERY);
        delay(1);
    }
    // Công thức: (MilliVolts trung bình * Hệ số cầu phân áp * Hệ số hiệu chuẩn) / 1000 để ra Volt
    return (mvSum / (float)SAMPLES * VOLTAGE_DIVIDER_RATIO * BAT_CALIBRATION_FACTOR) / 1000.0;
}

int BatteryLogic::getPercentage() {
    float volts = readRawVoltage();
    if (volts > 4.25) return 100; // Pin đầy hoặc đang cắm sạc
    // Ánh xạ dải 3.5V - 4.2V sang 0% - 100%
    int pct = map((int)(volts * 100), 350, 420, 0, 100);
    return constrain(pct, 0, 100);
}

void BatteryLogic::update(RemoteState &state) {
    // Đọc Volt thực tế
    float currentVolts = readRawVoltage();
    
    // Tính toán % dựa trên số Volt vừa đọc
    int newBat;
    if (currentVolts > 4.25) {
        newBat = 100;
    } else {
        newBat = map((int)(currentVolts * 100), 350, 420, 0, 100);
        newBat = constrain(newBat, 0, 100);
    }

    // CÚ CHỐT: IN LOG RA TERMINAL ĐỂ BÁC BẮT MẠCH PIN
    Serial.printf("[BATTERY] Real-time: %.2fV | Capacity: %d%%\n", currentVolts, newBat);

    // Cập nhật vào hệ thống nếu có sự thay đổi
    if (newBat != state.batteryLevel) {
        state.batteryLevel = newBat;
    }
}