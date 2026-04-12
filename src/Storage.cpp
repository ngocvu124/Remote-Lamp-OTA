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

    // FIX: Retry loop để chống lỗi thẻ SD chậm khởi động sau khi reset
    bool mountSuccess = false;
    for (int i = 0; i < 3; i++) {
        if (sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4), &SPI))) {
            mountSuccess = true;
            break;
        }
        Serial.println("[STORAGE] SD Mount retry...");
        vTaskDelay(pdMS_TO_TICKS(100));
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
    if (!isReady) return;
    bgFileCount = 0;
    
    bool hasLock = false;
    if (xGuiSemaphore != NULL) {
        hasLock = (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(1000)) == pdTRUE);
    }
    if (!hasLock && xGuiSemaphore != NULL) return; // FIX: Bắt buộc dừng lại nếu không lấy được khóa SPI

    FsFile dir = sd_bg.open("/background", O_READ); 
    if (!dir) {
        if (hasLock) xSemaphoreGiveRecursive(xGuiSemaphore);
        return;
    }
    dir.rewindDirectory();
    
    FsFile file;
    while (bgFileCount < 15 && file.openNext(&dir, O_READ)) {
        if (!file.isDirectory()) {
            file.getName(bgFileNames[bgFileCount], 32);
            bgFileCount++;
        }
        file.close();
    }
    dir.close();
    if (hasLock) xSemaphoreGiveRecursive(xGuiSemaphore);
    Serial.printf("[STORAGE] Loaded %d files in /background\n", bgFileCount);
}


void StorageLogic::saveConfig(RemoteState &state) {
    if (!isReady) return;
    
    // Phải khóa Bus SPI (thông qua GuiSemaphore) trước khi cho SD Card ghi để tránh đụng độ TFT
    if (xGuiSemaphore != NULL && xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(500))) {
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
            file.write((const uint8_t*)jsonStr.c_str(), jsonStr.length());
            file.sync(); // Ép thẻ SD phải ghi vật lý ngay lập tức
            file.close();
        }
        xSemaphoreGiveRecursive(xGuiSemaphore);
    }
}

bool StorageLogic::loadConfig(RemoteState &state) {
    if (!isReady) return false;

    bool hasLock = false;
    if (xGuiSemaphore != NULL) {
        hasLock = (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(1000)) == pdTRUE);
    }
    if (!hasLock && xGuiSemaphore != NULL) return false; // FIX: Không đọc file nếu đang bận vẽ màn hình

    FsFile file = sd_bg.open("/config.json", O_READ);
    if (file) {
        size_t size = file.size();
        if (size > 0 && size < 1024) {
            char* buf = (char*)malloc(size + 1); // Cấp phát buffer tạm để chứa file
            if (buf) {
                file.read(buf, size);
                buf[size] = '\0';
                
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
                }
            }
        }
        file.close();
    }
    if (hasLock) xSemaphoreGiveRecursive(xGuiSemaphore);
    
    strcpy(state.bgFilePath, "/bg.bin");
    return false;
}