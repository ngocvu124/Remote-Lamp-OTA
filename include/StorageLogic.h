#ifndef STORAGE_LOGIC_H
#define STORAGE_LOGIC_H

#include <Arduino.h>
#include "Config.h"
#include <SdFat.h>

class StorageLogic {
public:
    bool isReady = false; 
    
    void begin();
    void loadFiles();
    char* readFileToPSRAM(const char* filename);
    void freePSRAMBuffer(char* buffer);
    void saveConfig(RemoteState &state);
    bool loadConfig(RemoteState &state);

    char fileNames[15][32]; 
    int fileCount = 0;
};

extern StorageLogic storage;
extern SdFs sd_bg; // Đối tượng dùng chung toàn hệ thống

#endif