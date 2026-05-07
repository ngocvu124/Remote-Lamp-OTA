#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include "Config.h"
#include <SdFat.h>

class StorageLogic {
public:
    bool isReady = false; 
    
    void begin();
    void loadBgFiles();
    bool saveConfig(RemoteState &state);
    bool loadConfig(RemoteState &state);
    void safeSync(RemoteState &state);
    
    char bgFileNames[15][32]; 
    int bgFileCount = 0;
};

extern StorageLogic storage;
extern SdFs sd_bg; // Đối tượng dùng chung toàn hệ thống

#endif