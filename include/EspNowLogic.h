#ifndef ESP_NOW_LOGIC_H
#define ESP_NOW_LOGIC_H

#include <Arduino.h>
#include "Config.h"

class EspNowLogic {
public:
    void begin();
    // AppTask gọi hàm này để thả thư vào hòm
    void send(int mode, int brightness, int temperature, char sysCmd); 
    // EspNowTask gọi hàm này để thực thi việc chuyển phát sóng
    void sendInternal(struct_message msg); 
};

extern EspNowLogic espNow;

#endif