#include "Storage.h"
#include <esp_heap_caps.h>
#include <ArduinoJson.h>

StorageLogic storage;
extern SdFs sd_bg; 

void StorageLogic::begin() {
    Serial.println("\n[STORAGE] --- SD CARD INIT ---");
    // Tăng lên 20MHz để ổn định luồng dữ liệu 115KB ảnh
    if (sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(20)))) {
        isReady = true;
        Serial.println("[STORAGE] SD Mount OK!");
        
        // Tạo thư mục nếu chưa có
        if (!sd_bg.exists("/background")) {
            sd_bg.mkdir("/background");
        }
        
        loadFiles();
    } else {
        isReady = false;
        Serial.println("[STORAGE] SD Mount FAILED!");
    }
}

void StorageLogic::loadFiles() {
    if (!isReady) {
        if (sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(20)))) {
            isReady = true;
        } else return;
    }

    fileCount = 0;
    FsFile dir = sd_bg.open("/"); 
    if (!dir) return;
    dir.rewind();

    Serial.println("[STORAGE] Scanning root directory...");
    FsFile file;
    while (file.openNext(&dir, O_READ)) {
        char name[32];
        file.getName(name, 32);
        
        if (!file.isDir() && !file.isHidden() && strlen(name) > 0) {
            if (fileCount < 14) {
                strcpy(fileNames[fileCount], name);
                Serial.printf("  [%d] Found: %s (%llu bytes)\n", fileCount, name, (uint64_t)file.size());
                fileCount++;
            }
        }
        file.close();
    }
    dir.close();
    Serial.printf("[STORAGE] Scan finished. Total: %d files.\n", fileCount);
}

void StorageLogic::loadBgFiles() {
    if (!isReady) return;
    
    bgFileCount = 0;
    FsFile dir = sd_bg.open("/background"); 
    if (!dir) return;
    dir.rewind();

    FsFile file;
    while (file.openNext(&dir, O_READ)) {
        char name[32];
        file.getName(name, 32);
        
        if (!file.isDir() && !file.isHidden() && strlen(name) > 0) {
            if (bgFileCount < 14) {
                strcpy(bgFileNames[bgFileCount], name);
                bgFileCount++;
            }
        }
        file.close();
    }
    dir.close();
}

char* StorageLogic::readFileToPSRAM(const char* filename) {
    if (!isReady) return NULL;

    FsFile file = sd_bg.open(filename, O_READ);
    if (!file) {
        char path[40];
        sprintf(path, "/%s", filename);
        file = sd_bg.open(path, O_READ);
    }

    if (!file) {
        Serial.printf("[STORAGE] Critical Error: Cannot open %s\n", filename);
        return NULL;
    }

    if (strstr(filename, ".bin") != NULL || strstr(filename, ".img") != NULL) {
        file.close();
        char* buffer = (char*)malloc(128);
        strcpy(buffer, "[BINARY/IMAGE FILE]\n\nFile nay qua lon hoac la file nhi phan.\nRemote khong the hien thi noi dung.");
        return buffer;
    }

    size_t size = file.size();
    size_t readSize = size > 2048 ? 2048 : size;

    // Sử dụng PSRAM cho buffer text lớn
    char* buffer = (char*)heap_caps_malloc(readSize + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer) {
        file.read(buffer, readSize);
        buffer[readSize] = '\0'; 
        Serial.printf("[STORAGE] Read %d bytes from %s\n", readSize, filename);
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
        Serial.println("[STORAGE] Config saved!");
    }
}

bool StorageLogic::loadConfig(RemoteState &state) {
    if (!isReady) return false;
    FsFile file = sd_bg.open("/config.json", O_READ);
    if (file) {
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, file);
        if (!error) {
            state.sleepTimeout = doc["sleepTimeout"] | 30;
            state.oledBrightness = doc["oledBrightness"] | 50;
            state.brightness = doc["brightness"] | 50;
            state.temperature = doc["temperature"] | 50;
            if (doc.containsKey("bgFilePath")) {
                strlcpy(state.bgFilePath, doc["bgFilePath"], sizeof(state.bgFilePath));
            }
            Serial.println("[STORAGE] Config loaded successfully!");
        }
        file.close();
        return true;
    }
    return false;
}