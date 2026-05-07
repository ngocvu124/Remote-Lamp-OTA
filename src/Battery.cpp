#include "Battery.h"
#include "Display.h"

BatteryLogic battery;
#define VOLTAGE_DIVIDER_RATIO 2.0 

extern SemaphoreHandle_t xGuiSemaphore;
extern RemoteState appState;
extern volatile bool isStorageReady;

void BatteryLogic::begin() {
    pinMode(PIN_BATTERY, INPUT);
    // ADC 11dB cho ESP32-S3
    analogSetPinAttenuation(PIN_BATTERY, ADC_11db);
    Serial.println("[BATTERY] Voltage Sensor Initialized.");
}

float BatteryLogic::readRawVoltage() {
    uint32_t mvSum = 0;
    for(int i = 0; i < BAT_SAMPLES; i++) {
        mvSum += analogReadMilliVolts(PIN_BATTERY);
        vTaskDelay(pdMS_TO_TICKS(1)); // ← Nhường CPU cho FreeRTOS scheduler
    }
    return (mvSum / (float)BAT_SAMPLES * VOLTAGE_DIVIDER_RATIO * BAT_CALIBRATION_FACTOR) / 1000.0;
}

int BatteryLogic::getPercentage() {
    float volts = readRawVoltage();
    
    // Mốc 3.2V là 0%, 4.2V là 100% theo yêu cầu của bác
    if (volts >= 4.20) return 100;
    if (volts <= 3.20) return 0;
    
    float pct = (volts - 3.20f) / (4.20f - 3.20f) * 100.0f;
    return constrain((int)pct, 0, 100);
}

bool BatteryLogic::update(RemoteState &state) {
    float volts = readRawVoltage();
    int newBat;

    if (volts >= 4.20f) newBat = 100;
    else if (volts <= 3.20f) newBat = 0;
    else {
        float pct = (volts - 3.20f) / (4.20f - 3.20f) * 100.0f;
        newBat = constrain((int)pct, 0, 100);
    }

    if (newBat != state.batteryLevel) {
        state.batteryLevel = newBat;
        return true;
    }
    return false;
}

void batteryTask(void *pvParameters) {
    while (!isStorageReady) vTaskDelay(pdMS_TO_TICKS(100));
    battery.begin();
    bool lowBatWarned = false;
    while (1) {
        bool changed = battery.update(appState);
        if (changed && xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
            display.updateUI(appState);
            xSemaphoreGiveRecursive(xGuiSemaphore);
        }

        if (appState.batteryLevel <= 15 && !lowBatWarned) {
            lowBatWarned = true;
            if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                display.showFileContent("PIN YEU", "Sac pin ngay!");
                xSemaphoreGiveRecursive(xGuiSemaphore);
            }
            // Keep warning visible briefly, then clear overlay so it cannot block UI flow.
            vTaskDelay(pdMS_TO_TICKS(2500));
            if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                display.showFileContent(NULL, NULL);
                xSemaphoreGiveRecursive(xGuiSemaphore);
            }
        } else if (appState.batteryLevel > 20) {
            lowBatWarned = false;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}