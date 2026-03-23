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

void stockUpdateTask(void *pvParameters) {
    while (1) {
        if (appState.currentMenu == MENU_STOCK) {
            if (!webServer.runWiFiSetup()) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
            char ticker[15];
            stock.getTickerName(appState.stockIndex, ticker);
            stock.fetchAndUpdateUI(appState.stockIndex);
            vTaskDelay(pdMS_TO_TICKS(5000));
        } else vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void otaUpdateTask(void *pvParameters) {
    if (selectedOtaIndex >= 0 && selectedOtaIndex < ota.versionCount) {
        ota.begin(ota.versions[selectedOtaIndex].url);
    }
    vTaskDelete(NULL);
}

void AppLogic::begin() {
    appState.currentMenu = MENU_NONE;
    originalSleepTimeout = appState.sleepTimeout;
    xTaskCreatePinnedToCore(stockUpdateTask, "StockTask", 8192, NULL, 1, &stockTaskHandle, 1);
}

void AppLogic::enterMenu(int level) {
    appState.currentMenu = (MenuLevel)level;
    appState.menuIndex = 0;

    // VÀO MENU LÀ TẮT GIA TỐC (1 KHẤC = 1 MỤC)
    if (level == MENU_MAIN) {
        encoder.setBoundaries(0, 6, true);
        encoder.setAcceleration(false);
    }
    else if (level == MENU_CONTROL || level == MENU_LAMP) {
        encoder.setBoundaries(0, 4, true);
        encoder.setAcceleration(false);
    }
    else if (level == MENU_USB_MODE || level == MENU_CHANGE_BG_LOCAL) {
        storage.loadFiles(); 
        encoder.setBoundaries(0, storage.fileCount, true); 
        encoder.setAcceleration(false);
    }
    else if (level == MENU_STOCK) {
        originalSleepTimeout = appState.sleepTimeout;
        appState.sleepTimeout = 300;
        encoder.setBoundaries(0, 10, true);
        encoder.setAcceleration(false);
        encoder.setEncoderValue(appState.stockIndex);
        return;
    }
    else if (level == MENU_OTA) {
        originalSleepTimeout = appState.sleepTimeout;
        appState.sleepTimeout = 300;
        encoder.setAcceleration(false);
        if (webServer.runWiFiSetup()) {
            if (ota.fetchVersions()) encoder.setBoundaries(0, ota.versionCount, true);
            else { appState.currentMenu = MENU_MAIN; encoder.setBoundaries(0, 6, true); }
        } else {
            appState.currentMenu = MENU_MAIN; encoder.setBoundaries(0, 6, true);
        }
    }
    else if (level == MENU_WEB_SERVER) {
        originalSleepTimeout = appState.sleepTimeout;
        appState.sleepTimeout = 600; 
        encoder.setAcceleration(false);
        webServer.runWebServerOnly();
        return;
    }
    // CÚ CHỐT: CHỈ BẬT LẠI GIA TỐC KHI CHỈNH SỐ TO
    else if (level == MENU_SET_SLEEP || level == MENU_SET_BACKLIGHT) {
        encoder.setBoundaries(0, 1000, false); 
        encoder.setAcceleration(true); 
    }
    
    encoder.setEncoderValue(0);
}

void AppLogic::exitMenu() {
    if (appState.currentMenu == MENU_STOCK || appState.currentMenu == MENU_OTA || appState.currentMenu == MENU_WEB_SERVER) {
        appState.sleepTimeout = originalSleepTimeout; 
        webServer.isRunning = false;
        WiFi.disconnect(); 
        WiFi.mode(WIFI_OFF);
        espNow.begin(); 
    }
    appState.currentMenu = MENU_NONE;
    encoder.setBoundaries(0, 100, false);
    
    // THOÁT RA MÀN HÌNH CHÍNH (ĐỘ SÁNG/TEMP) THÌ BẬT GIA TỐC
    encoder.setAcceleration(true); 
    encoder.setEncoderValue(appState.isTempMode ? appState.temperature : appState.brightness);
}

void AppLogic::handleEvents() {
    EncoderEvent ev;
    if (xQueueReceive(xEncoderQueue, &ev, 0) == pdPASS) {
        bool needUpdateUI = false;

        if (ev == ENC_LONG_PRESS) {
            if (isViewingFile || isViewingImage) {
                isViewingFile = false;
                isViewingImage = false;
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                    display.showFileContent(NULL, NULL);
                    display.closeImagePreview();
                    xSemaphoreGive(xGuiSemaphore);
                }
                encoder.setBoundaries(0, storage.fileCount, true);
                encoder.setAcceleration(false); // Khóa gia tốc khi về lại menu file
                encoder.setEncoderValue(appState.menuIndex);
            }
            else if (appState.currentMenu != MENU_NONE) exitMenu();
            else enterMenu(MENU_MAIN);
            needUpdateUI = true;
        }
        else if (appState.currentMenu == MENU_NONE) {
            if (ev == ENC_UP || ev == ENC_DOWN) {
                int val = encoder.getEncoderValue();
                if (appState.isTempMode) appState.temperature = val;
                else appState.brightness = val;
                espNow.send(appState.isTempMode ? 1 : 0, appState.brightness, appState.temperature, 0);
                needUpdateUI = true;
            }
            else if (ev == ENC_CLICK) {
                appState.isTempMode = !appState.isTempMode;
                encoder.setEncoderValue(appState.isTempMode ? appState.temperature : appState.brightness);
                needUpdateUI = true;
            }
        }
        else {
            if (isViewingFile) {
                if (ev == ENC_UP) {
                    if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY)) { 
                        display.showFileContent("SCROLL_UP", NULL); 
                        xSemaphoreGive(xGuiSemaphore); 
                    }
                }
                else if (ev == ENC_DOWN) {
                    if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY)) { 
                        display.showFileContent("SCROLL_DOWN", NULL); 
                        xSemaphoreGive(xGuiSemaphore); 
                    }
                }
                return;
            }

            if (ev == ENC_UP || ev == ENC_DOWN) {
                if (appState.currentMenu == MENU_STOCK) appState.stockIndex = encoder.getEncoderValue();
                else if (appState.currentMenu == MENU_SET_SLEEP) {
                    int v = encoder.getEncoderValue();
                    appState.sleepTimeout = 30 + (v * (300 - 30) / 1000);
                } else if (appState.currentMenu == MENU_SET_BACKLIGHT) {
                    appState.oledBrightness = encoder.getEncoderValue() / 10;
                    display.setContrast(appState.oledBrightness);
                } else {
                    appState.menuIndex = encoder.getEncoderValue();
                }
                needUpdateUI = true;
            }
            else if (ev == ENC_CLICK) {
                if (appState.currentMenu == MENU_MAIN) {
                    switch(appState.menuIndex) {
                        case 0: enterMenu(MENU_CONTROL); break;
                        case 1: enterMenu(MENU_LAMP); break;
                        case 2: enterMenu(MENU_USB_MODE); break;
                        case 3: enterMenu(MENU_STOCK); break;
                        case 4: enterMenu(MENU_OTA); break;
                        case 5: enterMenu(MENU_WEB_SERVER); break;
                        case 6: exitMenu(); break;
                    }
                }
                else if (appState.currentMenu == MENU_CONTROL) {
                    switch(appState.menuIndex) {
                        case 0: enterMenu(MENU_SET_SLEEP); break;
                        case 1: enterMenu(MENU_SET_BACKLIGHT); break;
                        case 2: espNow.send(0,0,0, 'W'); exitMenu(); break;
                        case 3: enterMenu(MENU_CHANGE_BG_LOCAL); break;
                        case 4: enterMenu(MENU_MAIN); break;
                    }
                }
                else if (appState.currentMenu == MENU_LAMP) {
                    switch(appState.menuIndex) {
                        case 0: espNow.send(0,0,0, 'R'); exitMenu(); break;
                        case 1: espNow.send(0,0,0, 'U'); exitMenu(); break;
                        case 2: espNow.send(0,0,0, 'W'); exitMenu(); break;
                        case 3: espNow.send(0,0,0, 'F'); exitMenu(); break;
                        case 4: enterMenu(MENU_MAIN); break;
                    }
                }
                else if (appState.currentMenu == MENU_SET_SLEEP || appState.currentMenu == MENU_SET_BACKLIGHT) {
                    storage.saveConfig(appState);
                    enterMenu(MENU_CONTROL);
                }
                else if (appState.currentMenu == MENU_CHANGE_BG_LOCAL) {
                    if (appState.menuIndex == storage.fileCount) enterMenu(MENU_CONTROL);
                    else {
                        char* selectedFile = storage.fileNames[appState.menuIndex];
                        if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                            FsFile activeFile = sd_bg.open("/active_bg.txt", O_WRITE | O_CREAT | O_TRUNC);
                            if (activeFile) {
                                activeFile.print("/");
                                activeFile.print(selectedFile);
                                activeFile.close();
                            }
                            display.loadBackgroundFromSD();
                            display.showProgressPopup("SUCCESS", "Doi hinh nen thanh cong!", 100);
                            xSemaphoreGive(xGuiSemaphore);
                        }
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                            display.closeProgressPopup();
                            xSemaphoreGive(xGuiSemaphore);
                        }
                        enterMenu(MENU_CONTROL);
                    }
                }
                else if (appState.currentMenu == MENU_USB_MODE) {
                    if (appState.menuIndex == storage.fileCount) enterMenu(MENU_MAIN);
                    else {
                        char* fName = storage.fileNames[appState.menuIndex];
                        if (String(fName).endsWith(".bin") || String(fName).endsWith(".BIN")) {
                            if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                                FsFile f = sd_bg.open(fName, O_READ);
                                if(display.showImagePreview(f)) {
                                    isViewingImage = true;
                                    encoder.setBoundaries(0, 0, false);
                                    encoder.setAcceleration(false);
                                }
                                f.close();
                                xSemaphoreGive(xGuiSemaphore);
                            }
                        } else {
                            char* content = storage.readFileToPSRAM(fName);
                            if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                                display.showFileContent(fName, content);
                                xSemaphoreGive(xGuiSemaphore);
                            }
                            isViewingFile = true;
                            encoder.setBoundaries(0, 100, false);
                            encoder.setAcceleration(true); // Nếu bác đọc file text dài thì có tý gia tốc cho lướt nhanh
                            encoder.setEncoderValue(0);
                        }
                    }
                }
                else if (appState.currentMenu == MENU_OTA) {
                    if (appState.menuIndex == ota.versionCount) enterMenu(MENU_MAIN);
                    else {
                        selectedOtaIndex = appState.menuIndex;
                        xTaskCreatePinnedToCore(otaUpdateTask, "OtaTask", 8192, NULL, 1, &otaTaskHandle, 1);
                    }
                }
                needUpdateUI = true;
            }
        }

        if (needUpdateUI) {
            if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY)) {
                display.updateUI(appState);
                xSemaphoreGive(xGuiSemaphore);
            }
        }
    }
}