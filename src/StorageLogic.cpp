#include "StorageLogic.h"
#include <esp_heap_caps.h>
#include <ArduinoJson.h>

StorageLogic storage;
extern SdFs sd_bg; 

void StorageLogic::begin() {
    Serial.println("\n[STORAGE] --- SD CARD INIT ---");
    if (sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(10)))) {
        isReady = true;
        Serial.println("[STORAGE] SD Mount OK!");
        loadFiles();
    } else {
        isReady = false;
        Serial.println("[STORAGE] SD Mount FAILED!");
    }
}

void StorageLogic::loadFiles() {
    if (!isReady) {
        if (sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(10)))) {
            isReady = true;
        } else {
            return;
        }
    }

    fileCount = 0;
    FsFile dir = sd_bg.open("/"); 
    if (!dir) {
        Serial.println("[STORAGE] Error: Cannot open root.");
        return;
    }
    dir.rewind();

    Serial.println("[STORAGE] Scanning files...");
    FsFile file;
    while (file.openNext(&dir, O_READ)) {
        char name[32];
        file.getName(name, 32);
        
        if (!file.isDir() && !file.isHidden()) {
            if (fileCount < 14) {
                strcpy(fileNames[fileCount], name);
                Serial.printf("  Found: %s\n", name);
                fileCount++;
            }
        }
        file.close();
    }
    dir.close();
    Serial.printf("[STORAGE] Scan finished. Total: %d files.\n", fileCount);
}

char* StorageLogic::readFileToPSRAM(const char* filename) {
    if (!isReady) return NULL;

    char path[40];
    sprintf(path, "/%s", filename);
    FsFile file = sd_bg.open(path, O_READ);
    if (!file) {
        Serial.printf("[STORAGE] Error: Cannot open %s\n", path);
        return NULL;
    }

    if (strstr(filename, ".bin") != NULL) {
        file.close();
        char* buffer = (char*)malloc(128);
        strcpy(buffer, "[BINARY FILE]\n\nKhong the hien thi noi dung.");
        return buffer;
    }

    size_t size = file.size();
    size_t readSize = size > 2048 ? 2048 : size;

    char* buffer = (char*)malloc(readSize + 1);
    if (buffer) {
        file.read(buffer, readSize);
        buffer[readSize] = '\0'; 
    }
    file.close();
    return buffer;
}

void StorageLogic::freePSRAMBuffer(char* buffer) {
    if (buffer) free(buffer);
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
        Serial.println("[STORAGE] Config saved.");
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
            Serial.println("[STORAGE] Config loaded.");
        }
        file.close();
        return true;
    }
    return false;
}