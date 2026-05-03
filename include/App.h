#ifndef APP_H
#define APP_H

#include <Arduino.h>
#include "Config.h"

class AppLogic {
public:
    void begin();
    void handleEvents(); // Chạy trong SysTask (Core 1)
    void enterMenu(int level);
    void exitMenu();
};

extern AppLogic app;

#endif