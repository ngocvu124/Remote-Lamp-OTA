#include "App.h"
#include "Config.h"
#include "Encoder.h"
#include "EspNow.h"
#include "Display.h"
#include "System.h"
#include "Storage.h" 
#include "Stock.h"
#include "Ota.h"
#include "WebSv.h" 
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <SdFat.h>

AppLogic app;
TaskHandle_t stockTaskHandle = NULL;
TaskHandle_t otaTaskHandle = NULL;
extern SemaphoreHandle_t xGuiSemaphore;
extern QueueHandle_t xEncoderQueue;
extern SdFs sd_bg; 

static int originalSleepTimeout = 60; 
static bool isViewingFile = false; 
static bool isViewingImage = false; 
static int selectedOtaIndex = -1; 

static bool pendingImageLoad = false; // Cờ báo hiệu load ảnh preview khi cuộn
static uint32_t lastScrollTime = 0;   // Mốc thời gian để debounce việc cuộn
static volatile bool forceStockUpdate = false; // Cờ báo hiệu cho luồng stock (cần volatile vì dùng xuyên Task)


void stockUpdateTask(void *pvParameters) {
    uint32_t lastFetch = 0;
    while (1) {
        if (appState.currentMenu == MENU_STOCK) {
            if (!webServer.runWiFiSetup()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            char ticker[15];
            stock.getTickerName(appState.stockIndex, ticker); 
            
            // Chỉ kéo dữ liệu nếu ticker hợp lệ (không phải dòng "--GAS--")
            if (ticker[0] != '-') {
                int delayInterval = strstr(ticker, "USDT") ? 3000 : 15000;
                
                // Kéo dữ liệu khi có lệnh ép (từ con lăn) HOẶC đã hết thời gian chờ
                if (forceStockUpdate || millis() - lastFetch > delayInterval || lastFetch == 0) {
                    forceStockUpdate = false;
                    
                    if (xSemaphoreTakeRecursive(xGuiSemaphore, portMAX_DELAY)) {
                        stock.fetchAndUpdateUI(appState.stockIndex); 
                        xSemaphoreGiveRecursive(xGuiSemaphore);
                    }
                    lastFetch = millis();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100)); // Quét trạng thái cực nhanh
        } else {
            stockTaskHandle = NULL;
            vTaskDelete(NULL); 
        }
    }
}

void otaUpdateTask(void *pvParameters) {
    bool listFetched = false;
    while (1) {
        if (appState.currentMenu == MENU_OTA) {
            if (!webServer.runWiFiSetup()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            if (selectedOtaIndex >= 0) {
                int idx = selectedOtaIndex;
                selectedOtaIndex = -1;
                ota.begin(ota.versions[idx].url);
            }
            else if (!listFetched) {
                char msg_buf[128];
                strcpy(msg_buf, "Fetching version list...\nPlease wait!");
                if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                    display.showProgressPopup("CHECKING", msg_buf, 0);
                    xSemaphoreGiveRecursive(xGuiSemaphore);
                }
                bool ok = ota.fetchVersions();
                listFetched = true;
                if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                    display.closeProgressPopup();
                    if (!ok) {
                        display.showFileContent("OTA ERROR", "Failed to fetch versions.json!");
                        isViewingFile = true;
                    } else {
                        encoder.setBoundaries(0, ota.versionCount, true);
                        encoder.setEncoderValue(0);
                        appState.menuIndex = 0;
                        display.forceRebuild(); 
                        display.updateUI(appState);
                    }
                    xSemaphoreGiveRecursive(xGuiSemaphore);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            listFetched = false;
            selectedOtaIndex = -1;
            otaTaskHandle = NULL;
            vTaskDelete(NULL); 
        }
    }
}

void AppLogic::begin() {
    Serial.println("[APP] begin() called");
    if (appState.sleepTimeout > 300) appState.sleepTimeout = 60; // Gác cổng thêm lần nữa cho chắc
    
    Serial.println("[APP] Taking GUI Semaphore for initial update...");
    if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(500))) {
        Serial.println("[APP] Loading Background...");
        display.loadBackgroundFromSD(); 
        encoder.setBoundaries(0, 100, false);
        encoder.setEncoderValue(appState.isTempMode ? appState.temperature : appState.brightness);
        Serial.println("[APP] Applying updateUI()...");
        display.updateUI(appState);
        xSemaphoreGiveRecursive(xGuiSemaphore);
        Serial.println("[APP] begin() finished successfully.");
    } else {
        Serial.println("[APP-ERR] Failed to take GUI Semaphore in begin()");
    }
}

void AppLogic::handleEvents() {
    EncoderEvent event;
    bool ui_needs_update = false;

    if (xQueueReceive(xEncoderQueue, &event, pdMS_TO_TICKS(10)) == pdPASS) {
        ui_needs_update = true; 
        
        if (event == ENC_LONG_PRESS) {
            if (isViewingFile || isViewingImage) {
                isViewingFile = false;
                isViewingImage = false; 
                pendingImageLoad = false;
                if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(50))) {
                    display.showFileContent(NULL, NULL); 
                    display.closeProgressPopup(); 
                    display.closeImagePreview(); 
                    xSemaphoreGiveRecursive(xGuiSemaphore);
                }
                if (appState.currentMenu == MENU_ABOUT) {
                    enterMenu(MENU_CONTROL);
                    encoder.setEncoderValue(4); 
                } else {
                    encoder.setEncoderValue(appState.menuIndex);
                    if (appState.currentMenu == MENU_OTA || appState.currentMenu == MENU_WEB_SERVER) exitMenu();
                }
            } else {
                if (appState.currentMenu == MENU_NONE) enterMenu(MENU_MAIN);
                else exitMenu();
            }
        }

        if (event == ENC_UP || event == ENC_DOWN) {
            if (isViewingFile) {
                if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(50))) {
                    if (event == ENC_UP) display.showFileContent("SCROLL_UP", NULL);
                    else display.showFileContent("SCROLL_DOWN", NULL);
                    xSemaphoreGiveRecursive(xGuiSemaphore);
                }
            }
            else if (isViewingImage && appState.currentMenu != MENU_SELECT_BG) {
                ui_needs_update = false; 
            }
            else if (appState.currentMenu == MENU_NONE || appState.currentMenu == MENU_SET_SLEEP || appState.currentMenu == MENU_SET_BACKLIGHT) {
                int step = (event == ENC_UP) ? 5 : -5; 
                if (appState.currentMenu == MENU_SET_SLEEP) {
                    appState.sleepTimeout = constrain(appState.sleepTimeout + step, 30, 300);
                    encoder.setEncoderValue(appState.sleepTimeout); 
                }
                else if (appState.currentMenu == MENU_SET_BACKLIGHT) {
                    appState.oledBrightness = constrain(appState.oledBrightness + step, 0, 100);
                    encoder.setEncoderValue(appState.oledBrightness); 
                    display.setContrast(appState.oledBrightness); 
                } 
                else if (appState.currentMenu == MENU_NONE) {
                    if (appState.isTempMode) {
                        appState.temperature = constrain(appState.temperature + step, 0, 100);
                        encoder.setEncoderValue(appState.temperature); 
                    } else {
                        appState.brightness = constrain(appState.brightness + step, 0, 100);
                        encoder.setEncoderValue(appState.brightness); 
                    }
                    espNow.send(0, appState.brightness, appState.temperature, ' ');
                    storage.saveConfig(appState);
                }
            }
            else {
                appState.menuIndex = encoder.getEncoderValue(); 
                if (appState.currentMenu == MENU_STOCK) {
                    appState.stockIndex = appState.menuIndex;
                }

                if (isViewingImage && appState.currentMenu == MENU_SELECT_BG) {
                    if (appState.menuIndex == storage.bgFileCount) {
                        pendingImageLoad = false;
                        isViewingImage = false; 
                                if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(50))) {
                            display.closeImagePreview();
                                    xSemaphoreGiveRecursive(xGuiSemaphore);
                        }
                    } else {
                        pendingImageLoad = true;
                        lastScrollTime = millis();
                    }
                }
            }
        }

        if (event == ENC_CLICK) {
            if ((isViewingFile || isViewingImage) && appState.currentMenu != MENU_SELECT_BG) {
                isViewingFile = false;
                isViewingImage = false;
                pendingImageLoad = false;
                if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(50))) {
                    display.showFileContent(NULL, NULL); 
                    display.closeImagePreview();
                    xSemaphoreGiveRecursive(xGuiSemaphore);
                }
                if (appState.currentMenu == MENU_ABOUT) {
                    enterMenu(MENU_CONTROL);
                    encoder.setEncoderValue(4); 
                } else {
                    encoder.setEncoderValue(appState.menuIndex); 
                }
            } else {
                switch (appState.currentMenu) {
                    case MENU_MAIN:
                        if (appState.menuIndex == 0) enterMenu(MENU_CONTROL);
                        else if (appState.menuIndex == 1) enterMenu(MENU_LAMP);
                        else if (appState.menuIndex == 2) enterMenu(MENU_STOCK);
                        else if (appState.menuIndex == 3) enterMenu(MENU_OTA); 
                        else if (appState.menuIndex == 4) enterMenu(MENU_WEB_SERVER); 
                        else exitMenu(); 
                        break;
                    case MENU_CONTROL:
                        if (appState.menuIndex == 0) enterMenu(MENU_SET_SLEEP);
                        else if (appState.menuIndex == 1) enterMenu(MENU_SET_BACKLIGHT);
                        else if (appState.menuIndex == 2) {
                            WiFi.mode(WIFI_STA); WiFi.disconnect(false, true); 
                            if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                                display.showFileContent("WIFI CLEARED", "WiFi deleted! Rebooting...");
                                xSemaphoreGiveRecursive(xGuiSemaphore);
                            }
                            vTaskDelay(pdMS_TO_TICKS(2000)); ESP.restart(); 
                        }
                        else if (appState.menuIndex == 3) enterMenu(MENU_SELECT_BG); 
                        else if (appState.menuIndex == 4) enterMenu(MENU_ABOUT);
                        else enterMenu(MENU_MAIN);
                        break;
                    case MENU_SET_SLEEP:         
                    case MENU_SET_BACKLIGHT:
                        storage.saveConfig(appState); enterMenu(MENU_CONTROL); break;
                    case MENU_SELECT_BG:
                        if (appState.menuIndex == storage.bgFileCount) {
                            if (isViewingImage) {
                                isViewingImage = false;
                                pendingImageLoad = false;
                                if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(50))) {
                                    display.closeImagePreview(); xSemaphoreGiveRecursive(xGuiSemaphore);
                                }
                            }
                            enterMenu(MENU_CONTROL);
                        } else {
                            char fullPath[64];
                            snprintf(fullPath, sizeof(fullPath), "/background/%s", storage.bgFileNames[appState.menuIndex]);
                            if (isViewingImage) {
                                pendingImageLoad = false; 
                                strncpy(appState.bgFilePath, fullPath, sizeof(appState.bgFilePath));
                                storage.saveConfig(appState); 
                                Serial.printf("[APP] Applied new BG: %s\n", fullPath);
                                if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(500))) {
                                    display.closeImagePreview();
                                    display.loadBackgroundFromSD();
                                    xSemaphoreGiveRecursive(xGuiSemaphore);
                                }
                                isViewingImage = false; exitMenu(); 
                            } else {
                                strncpy(appState.bgFilePath, fullPath, sizeof(appState.bgFilePath));
                                Serial.printf("[APP] First click on BG: %s, showing preview.\n", fullPath);
                                if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(500))) {
                                    digitalWrite(SCR_CS_PIN, HIGH);
                                    FsFile file = sd_bg.open(fullPath, O_RDONLY); 
                                    if (file) {
                                        if (display.showImagePreview(file)) {
                                            isViewingImage = true;
                                            Serial.println("[APP] Preview SUCCESS!");
                                        } else {
                                            Serial.println("[APP] Preview FAILED: showImagePreview returned false.");
                                        }
                                        file.close();
                                    } else {
                                        Serial.printf("[APP] SD Error: Could not open %s\n", fullPath);
                                    }
                                    xSemaphoreGiveRecursive(xGuiSemaphore);
                                } else {
                                    Serial.println("[APP] Error: GuiSemaphore timeout in preview!");
                                }
                            }
                        }
                        break;
                    case MENU_OTA:
                        if (appState.menuIndex == ota.versionCount) {
                            exitMenu(); 
                        } else {
                            selectedOtaIndex = appState.menuIndex; 
                        }
                        break;
                    case MENU_NONE:
                        appState.isTempMode = !appState.isTempMode;
                        encoder.setEncoderValue(appState.isTempMode ? appState.temperature : appState.brightness);
                        storage.saveConfig(appState); break;
                    default: break;
                }
            }
        }
    }

    if (pendingImageLoad && (millis() - lastScrollTime > 300)) {
        pendingImageLoad = false;
        char fullPath[64];
        snprintf(fullPath, sizeof(fullPath), "/background/%s", storage.bgFileNames[appState.menuIndex]);
        Serial.printf("[APP] Previewing image on scroll: %s\n", fullPath);
        
        if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(500))) {
            digitalWrite(SCR_CS_PIN, HIGH);
            FsFile file = sd_bg.open(fullPath, O_RDONLY); 
            if (file) {
                display.showImagePreview(file);
                file.close();
            } else {
                Serial.printf("[APP] SD Error on scroll: Could not open %s\n", fullPath);
            }
            xSemaphoreGiveRecursive(xGuiSemaphore);
        }
    }

    if (ui_needs_update) {
        if (!isViewingFile && !isViewingImage && xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(50))) {
            display.updateUI(appState);
            xSemaphoreGiveRecursive(xGuiSemaphore);
        }
    }
}

void AppLogic::enterMenu(int level) {
    appState.currentMenu = (MenuLevel)level;
    appState.menuIndex = 0;
    
    encoder.setEncoderValue(0);
    
    if (level == MENU_STOCK || level == MENU_OTA || level == MENU_WEB_SERVER) {
        originalSleepTimeout = appState.sleepTimeout;
        if (originalSleepTimeout > 300) originalSleepTimeout = 60; // Gác cổng
        appState.sleepTimeout = 999999; 
        
        if (level == MENU_STOCK) {
            encoder.setBoundaries(0, 19, true); 
            if (!stockTaskHandle) xTaskCreatePinnedToCore(stockUpdateTask, "StockTask", STACK_NETWORK, NULL, PRIO_NETWORK, &stockTaskHandle, 1);
        } else if (level == MENU_OTA) {
            isViewingFile = false; selectedOtaIndex = -1;
            if (!otaTaskHandle) xTaskCreatePinnedToCore(otaUpdateTask, "OtaTask", STACK_NETWORK, NULL, PRIO_NETWORK, &otaTaskHandle, 1);
        } else if (level == MENU_WEB_SERVER) { isViewingFile = false; webServer.runBgUpload(); }
        return; 
    }
    
    if (level == MENU_MAIN) {
        encoder.setBoundaries(0, 5, true);         
    } 
    else if (level == MENU_CONTROL) {
        encoder.setBoundaries(0, 5, true); 
    } 
    else if (level == MENU_ABOUT) { 
        char* about_text = (char*)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (about_text) {
            sprintf(about_text,
                "Author: Ngoc Vu\n"
                "Firmware: %s\n"
                "-------------------\n"
                "WiFi: %s\n"
                "-------------------\n"
                "Sleep Time: %ds\n"
                "Backlight: %d%%\n"
                "Lamp Bright: %d%%\n"
                "Lamp Temp: %d%%",
                FIRMWARE_VERSION,
                cachedSSID,
                appState.sleepTimeout,
                appState.oledBrightness,
                appState.brightness,
                appState.temperature
            );
            if (xSemaphoreTakeRecursive(xGuiSemaphore, portMAX_DELAY)) {
                isViewingFile = true;
                display.showFileContent("ABOUT", about_text);
                xSemaphoreGiveRecursive(xGuiSemaphore);
            }
            
            // LVGL đã copy chuỗi vào bộ nhớ riêng, giải phóng biến tạm ngay lập tức
            free(about_text); 
        }
        encoder.setBoundaries(0, 0, false); 
    } 
    else if (level == MENU_SELECT_BG) { 
        if (xSemaphoreTakeRecursive(xGuiSemaphore, portMAX_DELAY)) {
            storage.loadBgFiles(); 
            xSemaphoreGiveRecursive(xGuiSemaphore);
        }
        encoder.setBoundaries(0, storage.bgFileCount, true); 
    } 
    else if (level == MENU_SET_SLEEP || level == MENU_SET_BACKLIGHT) {
        encoder.setBoundaries(0, 1000, false); 
    }
}

void AppLogic::exitMenu() {
    if (appState.currentMenu == MENU_STOCK || appState.currentMenu == MENU_OTA || appState.currentMenu == MENU_WEB_SERVER) {
        appState.sleepTimeout = originalSleepTimeout; 
        WiFi.disconnect(); WiFi.mode(WIFI_OFF); espNow.begin(); 
    }
    appState.currentMenu = MENU_NONE;
    encoder.setBoundaries(0, 100, false);
    encoder.setEncoderValue(appState.isTempMode ? appState.temperature : appState.brightness);
    storage.saveConfig(appState); 
}

// Hàm Callback tách rời hoàn toàn: Khi xoay con lăn chỉ cần phất cờ
extern "C" void action_on_stock_changed_cb(lv_event_t * e) {
    lv_obj_t * roller = lv_event_get_target(e);
    appState.stockIndex = lv_roller_get_selected(roller);
    forceStockUpdate = true; // Phát tín hiệu để task ngầm tự kéo mạng
}