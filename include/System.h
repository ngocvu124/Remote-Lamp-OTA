#ifndef SYSTEM_LOGIC_H
#define SYSTEM_LOGIC_H

#include "Config.h"
#include "driver/gpio.h"
class SystemLogic {
public:
    void begin();
    void goToSleep();
    void update(); // Kiểm tra timer để đi ngủ
};

extern SystemLogic sys;
extern RemoteState appState; // Biến toàn cục được định nghĩa tại đây

#endif