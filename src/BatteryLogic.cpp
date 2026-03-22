#include "BatteryLogic.h"

BatteryLogic battery;
#define VOLTAGE_DIVIDER_RATIO 2.0 

void BatteryLogic::begin() {
    pinMode(PIN_BATTERY, INPUT);
    // ADC 11dB cho ESP32-S3
    analogSetPinAttenuation(PIN_BATTERY, ADC_11db);
    Serial.println("[BATTERY] Voltage Sensor Initialized.");
}

float BatteryLogic::readRawVoltage() {
    uint32_t mvSum = 0;
    // Sử dụng BAT_SAMPLES đã định nghĩa trong Config.h
    for(int i = 0; i < BAT_SAMPLES; i++) {
        mvSum += analogReadMilliVolts(PIN_BATTERY);
        delay(1);
    }
    return (mvSum / (float)BAT_SAMPLES * VOLTAGE_DIVIDER_RATIO * BAT_CALIBRATION_FACTOR) / 1000.0;
}

int BatteryLogic::getPercentage() {
    float volts = readRawVoltage();
    
    // Mốc 3.3V là 0%, 4.2V là 100% theo yêu cầu của bác
    if (volts >= 4.20) return 100;
    if (volts <= 3.30) return 0;
    
    float pct = (volts - 3.30f) / (4.20f - 3.30f) * 100.0f;
    return constrain((int)pct, 0, 100);
}

void BatteryLogic::update(RemoteState &state) {
    float currentVolts = readRawVoltage();
    int newBat = getPercentage();

    // Hiện log để bác soi Volt
    Serial.printf("[BATTERY] Real-time: %.2fV | Capacity: %d%%\n", currentVolts, newBat);

    if (newBat != state.batteryLevel) {
        state.batteryLevel = newBat;
    }
}