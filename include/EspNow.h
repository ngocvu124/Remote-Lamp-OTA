#ifndef ESP_NOW_H
#define ESP_NOW_H

#include <Arduino.h>
#include "Config.h"

class EspNowLogic {
public:
    void begin();
    // AppTask gọi hàm này để thả thư vào hòm
    void send(int mode, int brightness, int temperature, char sysCmd); 
    bool sendCommandWithAck(char sysCmd, uint8_t maxRetries = 2, uint16_t ackTimeoutMs = 350);
    // EspNowTask gọi hàm này để thực thi việc chuyển phát sóng
    void sendInternal(struct_message msg); 
};

extern EspNowLogic espNow;

#endif