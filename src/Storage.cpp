#include "Storage.h"
#include <esp_heap_caps.h>
#include <ArduinoJson.h>

StorageLogic storage;
extern SdFs sd_bg; 

void StorageLogic::begin() {
    Serial.println("\n[STORAGE] --- SD CARD INIT ---");
    
    // Đảm bảo các chân CS được kéo cao ngay từ đầu để không tranh chấp
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
    pinMode(SCR_CS_PIN, OUTPUT);
    digitalWrite(SCR_CS_PIN, HIGH);

    // Dùng tốc độ 8MHz để đảm bảo KHÔNG TREO bus khi dây dẫn không chuẩn
    if (sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(8)))) {
        isReady = true;
        Serial.println("[STORAGE] SD Mount OK!");
        
        if (!sd_bg.exists("/background")) {
            sd_bg.mkdir("/background");
        }
        
        loadFiles();
        loadBgFiles(); 
    } else {
        isReady = false;
        Serial.println("[STORAGE] SD Mount FAILED!");
    }
}

void StorageLogic::loadFiles() {
    if (!isReady) return;
    fileCount = 0;
    FsFile dir = sd_bg.open("/"); 
    if (!dir) return;
    dir.rewind();

    FsFile file;
    while (file.openNext(&dir, O_READ)) {
        char name[32];
        file.getName(name, 32);
        if (!file.isDir() && !file.isHidden() && strlen(name) > 0) {
            if (fileCount < 14) {
                strcpy(fileNames[fileCount], name);
                fileCount++;
            }
        }
        file.close();
    }
    dir.close();
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
            if (bgFileCount < 15) {
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
        char path[64];
        sprintf(path, "/%s", filename);
        file = sd_bg.open(path, O_READ);
    }
    if (!file) return NULL;

    if (strstr(filename, ".bin") != NULL) {
        file.close();
        char* buffer = (char*)heap_caps_malloc(128, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if(buffer) strcpy(buffer, "[BINARY FILE] - Cannot preview text.");
        return buffer;
    }

    size_t size = file.size();
    size_t readSize = size > 2048 ? 2048 : size;
    char* buffer = (char*)heap_caps_malloc(readSize + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
            state.sleepTimeout = doc["sleepTimeout"] | 30;
            state.oledBrightness = doc["oledBrightness"] | 50;
            state.brightness = doc["brightness"] | 50;
            state.temperature = doc["temperature"] | 50;
            if (doc.containsKey("bgFilePath")) {
                strlcpy(state.bgFilePath, doc["bgFilePath"], sizeof(state.bgFilePath));
            }
        }
        file.close();
        return true;
    }
    return false;
}