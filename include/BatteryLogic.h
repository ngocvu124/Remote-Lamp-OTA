#ifndef BATTERY_LOGIC_H
#define BATTERY_LOGIC_H

#include <Arduino.h>
#include "Config.h"
#include "DisplayLogic.h"
#include "SystemLogic.h"

class BatteryLogic {
public:
    void begin();
    
    // Trả về phần trăm pin (0-100%)
    int getPercentage();
    
    // Trả về số Vol (để debug hoặc hiện lên màn hình)
    float getVoltage();

    void update(RemoteState &state);


private:
    float readRawVoltage();
    const int SAMPLES = 20; // Số lần đo để lấy trung bình cho mượt
};

extern BatteryLogic battery;

#endif