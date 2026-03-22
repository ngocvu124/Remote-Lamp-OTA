#ifndef BATTERYLOGIC_H
#define BATTERYLOGIC_H

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