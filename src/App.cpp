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
            char ticker[15]; stock.getTickerName(appState.stockIndex, ticker); 
            if (ticker[0] != '-') {
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(500))) {
                    stock.fetchAndUpdateUI(appState.stockIndex); 
                    xSemaphoreGive(xGuiSemaphore);
                }
                vTaskDelay(pdMS_TO_TICKS(15000));
            } else vTaskDelay(pdMS_TO_TICKS(500)); 
        } else { stockTaskHandle = NULL; vTaskDelete(NULL); }
    }
}

void otaUpdateTask(void *pvParameters) {
    while (1) {
        if (appState.currentMenu == MENU_OTA) {
            if (!webServer.runWiFiSetup()) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
            if (selectedOtaIndex >= 0) {
                int idx = selectedOtaIndex; selectedOtaIndex = -1;
                ota.begin(ota.versions[idx].url);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        } else { otaTaskHandle = NULL; vTaskDelete(NULL); }
    }
}

void AppLogic::begin() {
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(500))) {
        display.loadBackgroundFromSD(); 
        encoder.setBoundaries(0, 100, false);
        encoder.setEncoderValue(appState.brightness);
        display.updateUI(appState);
        xSemaphoreGive(xGuiSemaphore);
    }
}

void AppLogic::handleEvents() {
    EncoderEvent event;
    if (xQueueReceive(xEncoderQueue, &event, pdMS_TO_TICKS(10)) == pdPASS) {
        if (event == ENC_LONG_PRESS) {
            if (isViewingFile || isViewingImage) {
                isViewingFile = false; isViewingImage = false; 
                display.showFileContent(NULL, NULL); display.closeImagePreview();
            } else {
                if (appState.currentMenu == MENU_NONE) enterMenu(MENU_MAIN);
                else exitMenu();
            }
            return;
        }

        if (event == ENC_UP || event == ENC_DOWN) {
            if (appState.currentMenu == MENU_NONE || appState.currentMenu == MENU_SET_SLEEP || appState.currentMenu == MENU_SET_BACKLIGHT) {
                int step = (event == ENC_UP) ? 5 : -5; 
                if (appState.currentMenu == MENU_SET_SLEEP) appState.sleepTimeout = constrain(appState.sleepTimeout + step, 30, 300);
                else if (appState.currentMenu == MENU_SET_BACKLIGHT) {
                    appState.oledBrightness = constrain(appState.oledBrightness + step, 0, 100);
                    display.setContrast(appState.oledBrightness);
                } else {
                    if (appState.isTempMode) appState.temperature = constrain(appState.temperature + step, 0, 100);
                    else appState.brightness = constrain(appState.brightness + step, 0, 100);
                    espNow.send(0, appState.brightness, appState.temperature, ' ');
                }
            } else {
                appState.menuIndex = encoder.getEncoderValue();
                if (appState.currentMenu == MENU_STOCK) appState.stockIndex = appState.menuIndex;
            }
        }

        if (event == ENC_CLICK) {
            switch (appState.currentMenu) {
                case MENU_MAIN:
                    if (appState.menuIndex == 0) enterMenu(MENU_CONTROL);
                    else if (appState.menuIndex == 1) enterMenu(MENU_LAMP);
                    else if (appState.menuIndex == 2) enterMenu(MENU_USB_MODE); 
                    else if (appState.menuIndex == 3) enterMenu(MENU_STOCK);
                    else if (appState.menuIndex == 4) enterMenu(MENU_OTA); 
                    else if (appState.menuIndex == 5) enterMenu(MENU_WEB_SERVER); 
                    else exitMenu(); 
                    break;
                case MENU_CONTROL:
                    if (appState.menuIndex == 0) enterMenu(MENU_SET_SLEEP);
                    else if (appState.menuIndex == 1) enterMenu(MENU_SET_BACKLIGHT);
                    else if (appState.menuIndex == 3) enterMenu(MENU_SELECT_BG); 
                    else enterMenu(MENU_MAIN);
                    break;
                case MENU_LAMP:
                    if (appState.menuIndex == 4) enterMenu(MENU_MAIN);
                    else {
                        char cmds[] = {'R','U','W','E'};
                        espNow.send(0, appState.brightness, appState.temperature, cmds[appState.menuIndex]);
                    }
                    break;
                case MENU_SELECT_BG:
                    if (appState.menuIndex == storage.bgFileCount) enterMenu(MENU_CONTROL);
                    else {
                        char path[64]; snprintf(path, 64, "/background/%s", storage.bgFileNames[appState.menuIndex]);
                        if (isViewingImage && strcmp(appState.bgFilePath, path) == 0) {
                            storage.saveConfig(appState); display.loadBackgroundFromSD(); exitMenu();
                        } else {
                            strncpy(appState.bgFilePath, path, 64);
                            FsFile f = sd_bg.open(path, O_READ);
                            if (f) { isViewingImage = display.showImagePreview(f); f.close(); }
                        }
                    }
                    break;
                case MENU_USB_MODE:
                    if (appState.menuIndex == storage.fileCount) enterMenu(MENU_MAIN);
                    else {
                        char* f = storage.fileNames[appState.menuIndex];
                        if (strstr(f, ".bin")) { FsFile file = sd_bg.open(f, O_READ); if(file){ isViewingImage = display.showImagePreview(file); file.close(); }}
                        else { isViewingFile = true; display.showFileContent(f, storage.readFileToPSRAM(f)); }
                    }
                    break;
                case MENU_OTA:
                    if (appState.menuIndex == ota.versionCount) enterMenu(MENU_MAIN);
                    else selectedOtaIndex = appState.menuIndex;
                    break;
                case MENU_NONE:
                    appState.isTempMode = !appState.isTempMode;
                    encoder.setEncoderValue(appState.isTempMode ? appState.temperature : appState.brightness);
                    break;
            }
        }
    }

    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(50))) {
        display.updateUI(appState);
        xSemaphoreGive(xGuiSemaphore);
    }
}

void AppLogic::enterMenu(int level) {
    appState.currentMenu = (MenuLevel)level;
    appState.menuIndex = 0;
    if (level == MENU_MAIN) encoder.setBoundaries(0, 6, true);
    else if (level == MENU_CONTROL) encoder.setBoundaries(0, 4, true);
    else if (level == MENU_SELECT_BG) { storage.loadBgFiles(); encoder.setBoundaries(0, storage.bgFileCount, true); }
    else if (level == MENU_STOCK) { if(!stockTaskHandle) xTaskCreatePinnedToCore(stockUpdateTask, "StockTask", 8192, NULL, 2, &stockTaskHandle, 1); encoder.setBoundaries(0, 19, true); }
    else if (level == MENU_OTA) { ota.fetchVersions(); if(!otaTaskHandle) xTaskCreatePinnedToCore(otaUpdateTask, "OtaTask", 8192, NULL, 2, &otaTaskHandle, 1); encoder.setBoundaries(0, ota.versionCount, true); }
    else if (level == MENU_WEB_SERVER) { webServer.runBgUpload(); }
    encoder.setEncoderValue(0);
}

void AppLogic::exitMenu() {
    appState.currentMenu = MENU_NONE;
    storage.saveConfig(appState);
    encoder.setBoundaries(0, 100, false);
    encoder.setEncoderValue(appState.brightness);
}