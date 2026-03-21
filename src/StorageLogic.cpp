#include "StorageLogic.h"
#include <esp_heap_caps.h>
#include <ArduinoJson.h>

StorageLogic storage;
extern SdFs sd_bg; 

void StorageLogic::begin() {
    Serial.println("[STORAGE] Initializing SD Card...");
    
    // Thử Mount thẻ nhớ với cấu hình Shared SPI
    if (sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(10)))) {
        isReady = true;
        Serial.println("[STORAGE] SD Card Mounted Success!");
        loadFiles();
    } else {
        isReady = false;
        Serial.println("[STORAGE] SD Card Mount FAILED! Check wiring/card.");
    }
}

void StorageLogic::loadFiles() {
    fileCount = 0;
    if (!isReady) return;

    FsFile dir = sd_bg.open("/");
    if (!dir) {
        Serial.println("[STORAGE] Error: Cannot open Root directory.");
        return;
    }
    dir.rewind();

    Serial.println("[STORAGE] Scanning files:");
    FsFile file;
    while (file.openNext(&dir, O_READ)) {
        if (!file.isHidden() && !file.isDir() && fileCount < 14) {
            file.getName(fileNames[fileCount], 32);
            Serial.printf("  - Found: %s\n", fileNames[fileCount]);
            fileCount++;
        }
        file.close();
    }
    dir.close();
    Serial.printf("[STORAGE] Total files found: %d\n", fileCount);
}

char* StorageLogic::readFileToPSRAM(const char* filename) {
    if (!isReady) return NULL;

    char path[40];
    sprintf(path, "/%s", filename);
    Serial.printf("[STORAGE] Reading file: %s\n", path);

    FsFile file = sd_bg.open(path, O_READ);
    if (!file) {
        Serial.println("[STORAGE] Error: Could not open file.");
        return NULL;
    }

    if (strstr(filename, ".bin") != NULL) {
        file.close();
        char* buffer = (char*)heap_caps_malloc(128, MALLOC_CAP_SPIRAM);
        if (!buffer) buffer = (char*)malloc(128);
        strcpy(buffer, "[BINARY FILE]\n\nKhong the hien thi noi dung file nay.");
        return buffer;
    }

    size_t size = file.size();
    size_t readSize = size > 2048 ? 2048 : size;

    char* buffer = (char*)heap_caps_malloc(readSize + 1, MALLOC_CAP_SPIRAM);
    if (!buffer) buffer = (char*)malloc(readSize + 1);

    if (buffer) {
        file.read(buffer, readSize);
        buffer[readSize] = '\0'; 
    }
    file.close();
    return buffer;
}

void StorageLogic::freePSRAMBuffer(char* buffer) {
    if (buffer) heap_caps_free(buffer);
}

void StorageLogic::saveConfig(RemoteState &state) {
    if (!isReady) return;
    FsFile file = sd_bg.open("/config.json", O_WRITE | O_CREAT | O_TRUNC);
    if (file) {
        StaticJsonDocument<256> doc;
        doc["sleepTimeout"] = state.sleepTimeout;
        doc["oledBrightness"] = state.oledBrightness;
        serializeJson(doc, file);
        file.close();
        Serial.println("[STORAGE] Config saved to SD.");
    }
}

bool StorageLogic::loadConfig(RemoteState &state) {
    if (!isReady) return false;
    FsFile file = sd_bg.open("/config.json", O_READ);
    if (file) {
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, file);
        if (!error) {
            state.sleepTimeout = doc["sleepTimeout"] | 30;
            state.oledBrightness = doc["oledBrightness"] | 50;
            Serial.println("[STORAGE] Config loaded from SD.");
        }
        file.close();
        return true;
    }
    return false;
}