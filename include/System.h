#ifndef SYSTEM_H
#define SYSTEM_H

#include "Config.h"
#include "driver/gpio.h"
class SystemLogic {
public:
    void begin();
    void goToSleep();
};

extern SystemLogic sys;
extern RemoteState appState; // Biến toàn cục được định nghĩa tại đây

#endif