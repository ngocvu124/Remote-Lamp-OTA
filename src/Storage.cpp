#include "Storage.h"
#include <esp_heap_caps.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <TFT_eSPI.h>

StorageLogic storage;
extern SdFs sd_bg; 
extern SemaphoreHandle_t xGuiSemaphore;
extern TFT_eSPI tft;

void StorageLogic::begin() {
    Serial.println("\n[STORAGE] --- SD CARD INIT ---");
    pinMode(SCR_CS_PIN, OUTPUT);
    digitalWrite(SCR_CS_PIN, HIGH);
    
    // Củng cố lại chân SD_CS và cho bus SPI xả nhiễu trước khi nói chuyện với thẻ nhớ
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
    vTaskDelay(pdMS_TO_TICKS(20));

    // Lấy semaphore để tránh đụng độ với TFT nếu GUI task đã chạy
    bool hasLock = false;
    if (xGuiSemaphore != NULL) {
        hasLock = (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(1000)) == pdTRUE);
    }

    // Neu wakeup tu deep sleep, SD card co the can them thoi gian on dinh.
    // Tang thoi gian cho va so lan retry de xu ly ca truong hop card cham.
    bool isWakeup = (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED);
    if (isWakeup) {
        Serial.println("[STORAGE] Wakeup detected, waiting for SD to stabilize...");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // FIX: Retry loop de chong loi the SD cham khoi dong sau khi reset
    bool mountSuccess = false;
    int retries = isWakeup ? 5 : 3;
    int retryDelay = isWakeup ? 300 : 100;
    for (int i = 0; i < retries; i++) {
        if (sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4), &SPI))) {
            mountSuccess = true;
            break;
        }
        Serial.printf("[STORAGE] SD Mount retry %d/%d...\n", i + 1, retries);
        vTaskDelay(pdMS_TO_TICKS(retryDelay));
    }

    if (!mountSuccess) {
        uint8_t errCode = sd_bg.card() ? sd_bg.card()->errorCode() : 0xFF;
        Serial.printf("[STORAGE] SD Mount Failed! Error code: 0x%X\n", errCode);
        isReady = false;
        if (hasLock) xSemaphoreGiveRecursive(xGuiSemaphore);
        return;
    }
    
    isReady = true;
    if (!sd_bg.exists("/background")) {
        sd_bg.mkdir("/background");
    }

    if (hasLock) xSemaphoreGiveRecursive(xGuiSemaphore);
    loadBgFiles(); 
}


void StorageLogic::loadBgFiles() {
    Serial.println("[STORAGE-LOG] loadBgFiles() started");
    if (!isReady) return;
    bgFileCount = 0;
    
    bool hasLock = false;
    if (xGuiSemaphore != NULL) {
        hasLock = (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(1000)) == pdTRUE);
    }
    if (!hasLock && xGuiSemaphore != NULL) {
        Serial.println("[STORAGE-LOG] loadBgFiles: Failed to get SPI lock!");
        return;
    }

    digitalWrite(SCR_CS_PIN, HIGH); // Ép tắt màn hình trên bus SPI
    FsFile dir = sd_bg.open("/background", O_RDONLY); 
    if (!dir) { // Nếu lỗi, tự động Remount thẻ nhớ
        sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
        dir = sd_bg.open("/background", O_RDONLY);
    }

    if (!dir) {
        Serial.println("[STORAGE-LOG] loadBgFiles: Failed to open /background folder!");
        if (hasLock) xSemaphoreGiveRecursive(xGuiSemaphore);
        return;
    }
    dir.rewindDirectory();
    
    FsFile file;
    while (bgFileCount < 15 && file.openNext(&dir, O_RDONLY)) {
        if (!file.isDirectory()) {
            file.getName(bgFileNames[bgFileCount], 32);
            Serial.printf("[STORAGE-LOG] Found BG: %s\n", bgFileNames[bgFileCount]);
            bgFileCount++;
        } else {
            char folderName[32];
            file.getName(folderName, 32);
            Serial.printf("[STORAGE-LOG] Skipped folder: %s\n", folderName);
        }
        file.close();
    }
    dir.close();
    if (hasLock) xSemaphoreGiveRecursive(xGuiSemaphore);
    Serial.printf("[STORAGE] Loaded %d files in /background\n", bgFileCount);
}


void StorageLogic::saveConfig(RemoteState &state) {
    Serial.println("[STORAGE-LOG] saveConfig() started");
    if (!isReady) return;
    
    // Phải khóa Bus SPI (thông qua GuiSemaphore) trước khi cho SD Card ghi để tránh đụng độ TFT
    if (xGuiSemaphore != NULL && xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(500))) {
        digitalWrite(SCR_CS_PIN, HIGH);
        // Dùng config.txt thay cho .json để tương thích chuẩn 8.3 của FAT32
        FsFile file = sd_bg.open("/config.txt", O_WRONLY | O_CREAT | O_TRUNC);
        if (!file) {
            sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
            file = sd_bg.open("/config.txt", O_WRONLY | O_CREAT | O_TRUNC);
        }
        if (file) {
            StaticJsonDocument<512> doc;
            char safeBgPath[sizeof(state.bgFilePath)] = {0};
            size_t bgLen = strnlen(state.bgFilePath, sizeof(state.bgFilePath));
            if (bgLen > 0 && bgLen < sizeof(state.bgFilePath) && state.bgFilePath[0] == '/') {
                memcpy(safeBgPath, state.bgFilePath, bgLen);
                safeBgPath[bgLen] = '\0';
            } else {
                strcpy(safeBgPath, "/bg.bin");
                strncpy(state.bgFilePath, safeBgPath, sizeof(state.bgFilePath) - 1);
                state.bgFilePath[sizeof(state.bgFilePath) - 1] = '\0';
            }

            doc["sleepTimeout"] = (state.sleepTimeout > 300) ? 60 : state.sleepTimeout;
            doc["oledBrightness"] = state.oledBrightness;
            doc["brightness"] = state.brightness;
            doc["temperature"] = state.temperature;
            doc["bgFilePath"] = safeBgPath;
            
            String jsonStr;
            serializeJson(doc, jsonStr);
            file.write((const uint8_t*)jsonStr.c_str(), jsonStr.length());
            file.sync(); // Ép thẻ SD phải ghi vật lý ngay lập tức
            file.close();
            Serial.println("[STORAGE-LOG] Config saved successfully.");
        } else {
            Serial.println("[STORAGE-LOG] saveConfig: Failed to open /config.txt for writing!");
        }
        xSemaphoreGiveRecursive(xGuiSemaphore);
    } else {
        Serial.println("[STORAGE-LOG] saveConfig: Failed to get SPI lock!");
    }
}

bool StorageLogic::loadConfig(RemoteState &state) {
    Serial.println("[STORAGE-LOG] loadConfig() started");
    if (!isReady) return false;
    bool needsRewriteDefault = false;

    bool hasLock = false;
    if (xGuiSemaphore != NULL) {
        hasLock = (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(1000)) == pdTRUE);
    }
    if (!hasLock && xGuiSemaphore != NULL) {
        Serial.println("[STORAGE-LOG] loadConfig: Failed to get SPI lock!");
        return false;
    }

    digitalWrite(SCR_CS_PIN, HIGH);
    FsFile file = sd_bg.open("/config.txt", O_RDONLY);
    if (!file) file = sd_bg.open("/config.json", O_RDONLY); // Fallback cho file cũ
    if (!file) {
        sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
        file = sd_bg.open("/config.txt", O_RDONLY);
        if (!file) file = sd_bg.open("/config.json", O_RDONLY);
    }

    if (file) {
        size_t size = file.size();
        if (size > 0 && size < 1024) {
            char* buf = (char*)malloc(size + 1); // Cấp phát buffer tạm để chứa file
            if (buf) {
                file.read(buf, size);
                buf[size] = '\0';
                Serial.printf("[STORAGE-LOG] Config content: %s\n", buf);
                
                StaticJsonDocument<512> doc;
                DeserializationError error = deserializeJson(doc, buf); // Đọc JSON từ Buffer 100% an toàn
                free(buf); // Dùng xong dọn ngay
                
                if (!error) {
                    state.sleepTimeout = doc["sleepTimeout"] | 60;
                    if (state.sleepTimeout > 300) state.sleepTimeout = 60;
                    
                    state.oledBrightness = doc["oledBrightness"] | 50;
                    state.brightness = doc["brightness"] | 50;
                    state.temperature = doc["temperature"] | 50;
                    const char* bg = doc["bgFilePath"];
                    if (bg) {
                        strncpy(state.bgFilePath, bg, sizeof(state.bgFilePath) - 1);
                        state.bgFilePath[sizeof(state.bgFilePath) - 1] = '\0';
                    } else {
                        strcpy(state.bgFilePath, "/bg.bin");
                    }
                    file.close();
                    if (hasLock) xSemaphoreGiveRecursive(xGuiSemaphore);
                    return true;
                } else {
                    Serial.printf("[STORAGE] JSON Parse Error: %s\n", error.c_str());
                    needsRewriteDefault = true;
                }
            } else {
                Serial.println("[STORAGE-LOG] loadConfig: Memory allocation failed!");
            }
        } else {
            Serial.println("[STORAGE-LOG] loadConfig: Invalid file size!");
        }
        file.close();
    } else {
        Serial.println("[STORAGE-LOG] loadConfig: Failed to open config file. File might not exist yet.");
    }
    if (hasLock) xSemaphoreGiveRecursive(xGuiSemaphore);

    state.sleepTimeout = 60;
    state.oledBrightness = 50;
    state.brightness = 50;
    state.temperature = 50;
    strcpy(state.bgFilePath, "/bg.bin");

    if (needsRewriteDefault) {
        Serial.println("[STORAGE] Rewriting default config due to invalid JSON...");
        saveConfig(state);
    }

    return false;
}