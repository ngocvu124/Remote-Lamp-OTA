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
    
    // Hàm này chạy chặn (blocking) các task Network cho đến khi cấu hình WiFi xong
    bool runWiFiSetup(); 
    
    // Hàm này chỉ kích hoạt Task Upload và trả về ngay để AppTask còn xử lý Menu
    void runBgUpload();  
    
    volatile bool isRunning;
};

extern WebServerLogic webServer;

#endif