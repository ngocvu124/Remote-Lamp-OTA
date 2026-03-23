#include "Storage.h"
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
        
        String fName = String(name);
        
        // CÚ CHỐT: Lọc bỏ toàn bộ file rác, file cấu hình và file text hệ thống
        if (fName == "System Volume Information" || 
            fName == "active_bg.txt" || 
            fName == "config.json" || 
            fName == "config.txt") {
            file.close();
            continue;
        }
        
        if (fileCount < 15) {
            strncpy(fileNames[fileCount], name, 32);
            fileCount++;
        }
        file.close();
    }
    dir.close();
}

char* StorageLogic::readFileToPSRAM(const char* filename) {
    if (!isReady) return NULL;

    FsFile file = sd_bg.open(filename, O_READ);
    if (!file) {
        Serial.println("[STORAGE] File not found!");
        return NULL;
    }

    if (String(filename).endsWith(".bin") || String(filename).endsWith(".BIN")) {
        file.close();
        char* buffer = (char*)malloc(128);
        strcpy(buffer, "[BINARY/IMAGE FILE]\n\nFile nay qua lon hoac la file nhi phan.\nRemote khong the hien thi noi dung.");
        return buffer;
    }

    size_t size = file.size();
    size_t readSize = size > 2048 ? 2048 : size;

    char* buffer = (char*)malloc(readSize + 1);
    if (buffer) {
        file.read(buffer, readSize);
        buffer[readSize] = '\0'; 
        Serial.printf("[STORAGE] Read %d bytes from %s\n", readSize, filename);
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
    }
}

bool StorageLogic::loadConfig(RemoteState &state) {
    if (!isReady) return false;
    FsFile file = sd_bg.open("/config.json", O_READ);
    if (file) {
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, file);
        file.close();
        if (!error) {
            state.sleepTimeout = doc["sleepTimeout"] | 60;
            state.oledBrightness = doc["oledBrightness"] | 80;
            return true;
        }
    }
    return false;
}