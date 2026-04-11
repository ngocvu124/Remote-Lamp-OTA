#include "Storage.h"
#include <esp_heap_caps.h>
#include <ArduinoJson.h>
#include <SPI.h>

StorageLogic storage;
extern SdFs sd_bg; 
extern SemaphoreHandle_t xGuiSemaphore;

void StorageLogic::begin() {
    Serial.println("\n[STORAGE] --- SD CARD INIT ---");
    pinMode(SCR_CS_PIN, OUTPUT);
    digitalWrite(SCR_CS_PIN, HIGH);
    
    // Củng cố lại chân SD_CS và cho bus SPI xả nhiễu trước khi nói chuyện với thẻ nhớ
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
    vTaskDelay(pdMS_TO_TICKS(20));

    // Phải kiểm tra kết quả trả về và bật cờ isReady
    if (!sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4)))) {
        Serial.printf("[STORAGE] SD Mount Failed! Error code: 0x%X\n", sd_bg.card()->errorCode());
        isReady = false;
        return;
    }
    
    isReady = true;
    if (!sd_bg.exists("/background")) {
        sd_bg.mkdir("/background");
    }

    loadBgFiles(); 
}


void StorageLogic::loadBgFiles() {
    if (!isReady) return;
    bgFileCount = 0;
    FsFile dir = sd_bg.open("/background", O_READ); 
    if (!dir) return;
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


void StorageLogic::saveConfig(RemoteState &state) {
    if (!isReady) return;
    
    // Phải khóa Bus SPI (thông qua GuiSemaphore) trước khi cho SD Card ghi để tránh đụng độ TFT
    if (xGuiSemaphore != NULL && xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(500))) {
        FsFile file = sd_bg.open("/config.json", O_WRITE | O_CREAT | O_TRUNC);
        if (file) {
            StaticJsonDocument<512> doc;
            doc["sleepTimeout"] = (state.sleepTimeout > 300) ? 60 : state.sleepTimeout;
            doc["oledBrightness"] = state.oledBrightness;
            doc["brightness"] = state.brightness;
            doc["temperature"] = state.temperature;
            doc["bgFilePath"] = state.bgFilePath;
            
            String jsonStr;
            serializeJson(doc, jsonStr);
            file.print(jsonStr);
            file.close();
        }
        xSemaphoreGive(xGuiSemaphore);
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
            // BỘ LỌC CHỐNG NGÁO LÚC ĐỌC FILE LÊN
            if (state.sleepTimeout > 300) state.sleepTimeout = 60;
            
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