#ifndef WEBSV_H
#define WEBSV_H

#include <Arduino.h>

enum WebServerMode {
    WEB_MODE_NONE = 0,
    WEB_MODE_WIFI = 1,
    WEB_MODE_UPLOAD = 2
};

class WebServerLogic {
public:
    void begin();
    bool runWiFiSetup(); 
    void runBgUpload();  
    void runWebServerOnly(); // HÀM MỚI KẾT NỐI WIFI NHÀ
    
    volatile bool isRunning;
};

extern WebServerLogic webServer;

#endif