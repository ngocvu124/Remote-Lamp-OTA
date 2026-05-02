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
            uint32_t sz = file.size();
            if (sz >= 115200) { // 240x240 RGB565 raw
                file.getName(bgFileNames[bgFileCount], 32);
                Serial.printf("[STORAGE-LOG] Found BG: %s\n", bgFileNames[bgFileCount]);
                bgFileCount++;
            } else {
                char badName[32] = {0};
                file.getName(badName, 32);
                Serial.printf("[STORAGE-LOG] Skip BG (size=%lu): %s\n", (unsigned long)sz, badName);
            }
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
            size_t wr = file.write((const uint8_t*)jsonStr.c_str(), jsonStr.length());
            file.sync(); // Ép thẻ SD phải ghi vật lý ngay lập tức
            file.close();

            // Ghi them ban backup de co the phuc hoi neu config chinh bi hong.
            FsFile bak = sd_bg.open("/config.bak", O_WRONLY | O_CREAT | O_TRUNC);
            if (bak) {
                bak.write((const uint8_t*)jsonStr.c_str(), jsonStr.length());
                bak.sync();
                bak.close();
            }

            if (wr == jsonStr.length()) {
                Serial.println("[STORAGE-LOG] Config saved successfully.");
            } else {
                Serial.printf("[STORAGE-LOG] Config write short! expected=%u written=%u\n",
                              (unsigned)jsonStr.length(), (unsigned)wr);
            }
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
    bool tryBackup = false;

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
            char* buf = (char*)calloc(size + 1, 1); // Cấp phát buffer tạm để chứa file
            if (buf) {
                int rd = file.read(buf, size);
                buf[size] = '\0';

                if (rd != (int)size) {
                    Serial.printf("[STORAGE-LOG] loadConfig: Short read! expected=%u read=%d\n",
                                  (unsigned)size, rd);
                    free(buf);
                    file.close();
                    needsRewriteDefault = true;
                    tryBackup = true;
                    goto LOADCFG_END;
                }

                // Sanity-check: config phai bat dau bang '{' (bo qua khoang trang).
                char* p = buf;
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
                if (*p != '{') {
                    Serial.println("[STORAGE-LOG] loadConfig: Corrupted config (not JSON object)");
                    free(buf);
                    file.close();
                    needsRewriteDefault = true;
                    tryBackup = true;
                    goto LOADCFG_END;
                }
                
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
                    tryBackup = true;
                }
            } else {
                Serial.println("[STORAGE-LOG] loadConfig: Memory allocation failed!");
            }
        } else {
            Serial.println("[STORAGE-LOG] loadConfig: Invalid file size!");
            needsRewriteDefault = true;
            tryBackup = true;
        }
        file.close();
    } else {
        Serial.println("[STORAGE-LOG] loadConfig: Failed to open config file. File might not exist yet.");
        needsRewriteDefault = true;
    }

LOADCFG_END:
    if (hasLock) xSemaphoreGiveRecursive(xGuiSemaphore);

    state.sleepTimeout = 60;
    state.oledBrightness = 50;
    state.brightness = 50;
    state.temperature = 50;
    strcpy(state.bgFilePath, "/bg.bin");

    if (tryBackup) {
        bool bakLock = false;
        if (xGuiSemaphore != NULL) {
            bakLock = (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(500)) == pdTRUE);
        }
        if (bakLock || xGuiSemaphore == NULL) {
            FsFile bak = sd_bg.open("/config.bak", O_RDONLY);
            if (bak) {
                size_t bsz = bak.size();
                if (bsz > 0 && bsz < 1024) {
                    char* bbuf = (char*)calloc(bsz + 1, 1);
                    if (bbuf) {
                        int brd = bak.read(bbuf, bsz);
                        if (brd == (int)bsz) {
                            StaticJsonDocument<512> bdoc;
                            if (!deserializeJson(bdoc, bbuf)) {
                                state.sleepTimeout = bdoc["sleepTimeout"] | 60;
                                if (state.sleepTimeout > 300) state.sleepTimeout = 60;
                                state.oledBrightness = bdoc["oledBrightness"] | 50;
                                state.brightness = bdoc["brightness"] | 50;
                                state.temperature = bdoc["temperature"] | 50;
                                const char* bg = bdoc["bgFilePath"];
                                if (bg) {
                                    strncpy(state.bgFilePath, bg, sizeof(state.bgFilePath) - 1);
                                    state.bgFilePath[sizeof(state.bgFilePath) - 1] = '\0';
                                }
                                bak.close();
                                free(bbuf);
                                if (bakLock) xSemaphoreGiveRecursive(xGuiSemaphore);
                                Serial.println("[STORAGE] Recovered config from /config.bak");
                                saveConfig(state); // Ghi lai config.txt chinh
                                return true;
                            }
                        }
                        free(bbuf);
                    }
                }
                bak.close();
            }
            if (bakLock) xSemaphoreGiveRecursive(xGuiSemaphore);
        }
    }

    if (needsRewriteDefault) {
        bool delLock = false;
        if (xGuiSemaphore != NULL) {
            delLock = (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(500)) == pdTRUE);
        }
        if (delLock || xGuiSemaphore == NULL) {
            if (sd_bg.exists("/config.txt")) sd_bg.remove("/config.txt");
            if (sd_bg.exists("/config.json")) sd_bg.remove("/config.json");
            if (delLock) xSemaphoreGiveRecursive(xGuiSemaphore);
        }
        Serial.println("[STORAGE] Rewriting default config due to invalid JSON...");
        saveConfig(state);
    }

    return false;
}