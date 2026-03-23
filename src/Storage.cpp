#include "Storage.h"
#include <esp_heap_caps.h>
#include <ArduinoJson.h>

StorageLogic storage;
extern SdFs sd_bg; 

void StorageLogic::begin() {
    Serial.println("\n[STORAGE] --- SD CARD INIT ---");
    
    if (sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(16)))) {
        isReady = true;
        Serial.println("[STORAGE] SD Mount OK (16MHz)!");
    } else if (sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(8)))) {
        isReady = true;
        Serial.println("[STORAGE] SD Mount OK (8MHz Fallback)!");
    } else {
        isReady = false;
        Serial.println("[STORAGE] SD Mount FAILED!");
        return;
    }

    if (!sd_bg.exists("/background")) {
        sd_bg.mkdir("/background");
    }
    
    loadFiles();
    loadBgFiles(); 
}

void StorageLogic::loadFiles() {
    if (!isReady) return;
    fileCount = 0;
    
    FsFile dir = sd_bg.open("/", O_READ); 
    // CÚ CHỐT: Nếu nhiễu SPI làm rớt thẻ, ép Remount ngay lập tức!
    if (!dir) {
        Serial.println("[STORAGE-WARN] SD state lost! Auto-Remounting...");
        sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(16)));
        dir = sd_bg.open("/", O_READ);
        if (!dir) {
            Serial.println("[STORAGE-ERR] Auto-Remount failed!");
            return;
        }
    }
    
    dir.rewindDirectory();
    
    while (fileCount < 15) {
        FsFile file = dir.openNextFile();
        if (!file) break;
        if (!file.isDirectory()) {
            file.getName(fileNames[fileCount], 32);
            fileCount++; // Hiện toàn bộ file, kể cả config.json và bg.bin
        }
        file.close();
    }
    dir.close();
    Serial.printf("[STORAGE] Loaded %d files in ROOT\n", fileCount);
}

void StorageLogic::loadBgFiles() {
    if (!isReady) return;
    bgFileCount = 0;
    
    FsFile dir = sd_bg.open("/background", O_READ); 
    // CÚ CHỐT: Auto-Remount cho mục Change BG
    if (!dir) {
        Serial.println("[STORAGE-WARN] SD state lost! Auto-Remounting...");
        sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(16)));
        dir = sd_bg.open("/background", O_READ);
        if (!dir) {
            Serial.println("[STORAGE-ERR] Auto-Remount failed!");
            return;
        }
    }
    
    dir.rewindDirectory();
    
    while (bgFileCount < 15) {
        FsFile file = dir.openNextFile();
        if (!file) break;
        if (!file.isDirectory()) {
            file.getName(bgFileNames[bgFileCount], 32);
            bgFileCount++;
        }
        file.close();
    }
    dir.close();
    Serial.printf("[STORAGE] Loaded %d files in /background\n", bgFileCount);
}

char* StorageLogic::readFileToPSRAM(const char* filename) {
    if (!isReady) return NULL;
    FsFile file = sd_bg.open(filename, O_READ);
    if (!file) return NULL;

    size_t fileSize = file.size();
    if (fileSize == 0 || fileSize > 1024 * 1024) { 
        file.close();
        return NULL;
    }

    char* buffer = (char*)heap_caps_malloc(fileSize + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer) {
        size_t readSize = file.read(buffer, fileSize);
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
        StaticJsonDocument<512> doc;
        doc["sleepTimeout"] = state.sleepTimeout;
        doc["oledBrightness"] = state.oledBrightness;
        doc["brightness"] = state.brightness;
        doc["temperature"] = state.temperature;
        doc["bgFilePath"] = state.bgFilePath;
        serializeJson(doc, file);
        file.close();
    }
}

bool StorageLogic::loadConfig(RemoteState &state) {
    if (!isReady) return false;
    FsFile file = sd_bg.open("/config.json", O_READ);
    if (file) {
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, file);
        if (!error) {
            state.sleepTimeout = doc["sleepTimeout"] | 60;
            state.oledBrightness = doc["oledBrightness"] | 50;
            state.brightness = doc["brightness"] | 50;
            state.temperature = doc["temperature"] | 50;
            const char* bg = doc["bgFilePath"];
            if (bg) {
                strncpy(state.bgFilePath, bg, sizeof(state.bgFilePath));
            } else {
                strcpy(state.bgFilePath, "/bg.bin");
            }
        }
        file.close();
        return true;
    }
    strcpy(state.bgFilePath, "/bg.bin");
    return false;
}