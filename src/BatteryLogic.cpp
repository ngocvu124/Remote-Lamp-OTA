#include "BatteryLogic.h"

BatteryLogic battery;
#define VOLTAGE_DIVIDER_RATIO 2.0 

void BatteryLogic::begin() {
    pinMode(PIN_BATTERY, INPUT);
    analogSetPinAttenuation(PIN_BATTERY, ADC_11db);
}

float BatteryLogic::readRawVoltage() {
    uint32_t mvSum = 0;
    for(int i = 0; i < SAMPLES; i++) {
        mvSum += analogReadMilliVolts(PIN_BATTERY);
        delay(1);
    }
    return (mvSum / (float)SAMPLES * VOLTAGE_DIVIDER_RATIO * BAT_CALIBRATION_FACTOR) / 1000.0;
}

int BatteryLogic::getPercentage() {
    float volts = readRawVoltage();
    if (volts > 4.25) return 100;
    int pct = map((int)(volts * 100), 350, 420, 0, 100);
    return constrain(pct, 0, 100);
}

void BatteryLogic::update(RemoteState &state) {
    int newBat = getPercentage();
    if (newBat != state.batteryLevel) {
        state.batteryLevel = newBat;
        // Việc cập nhật UI sẽ do Task UI đảm nhận khi thấy biến state thay đổi
    }
}