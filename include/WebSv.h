#include <WebServer.h>
class WebServer;
#ifndef WEBSV_H
#define WEBSV_H

#include <Arduino.h>

enum WebServerMode {
    WEB_MODE_NONE = 0,
    WEB_MODE_UPLOAD = 1
};

class WebServerLogic {
public:
    void begin(WebServer* server);
    bool runWiFiSetup(); 
    void runBgUpload();  
    
    volatile bool isRunning;
};

extern WebServerLogic webServer;

#endif