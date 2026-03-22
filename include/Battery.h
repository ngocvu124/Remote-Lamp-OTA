#ifndef BATTERY_H
#define BATTERY_H

#include "Config.h"

class BatteryLogic {
public:
    void begin();
    void update(RemoteState &state);
    int getPercentage();
    float readRawVoltage();
};

extern BatteryLogic battery;

#endif