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

void AppLogic::begin() {
    storage.begin();
    storage.loadConfig(appState);
    display.setContrast(appState.oledBrightness);
    display.loadBackgroundFromSD();
    encoder.setBoundaries(0, 100, false);
}

void AppLogic::handleEvents() {
    EncoderEvent event;
    if (xQueueReceive(xEncoderQueue, &event, pdMS_TO_TICKS(10)) == pdPASS) {
        if (event == ENC_LONG_PRESS) {
            if (isViewingFile || isViewingImage) {
                isViewingFile = false; isViewingImage = false; 
                display.forceRebuild();
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
            }
        }

        if (event == ENC_CLICK) {
            switch (appState.currentMenu) {
                case MENU_MAIN:
                    if (appState.menuIndex == 0) enterMenu(MENU_CONTROL);
                    else if (appState.menuIndex == 2) enterMenu(MENU_USB_MODE); 
                    else if (appState.menuIndex == 4) enterMenu(MENU_OTA); 
                    else if (appState.menuIndex == 5) enterMenu(MENU_WEB_SERVER); 
                    else if (appState.menuIndex == 6) exitMenu();
                    break;
                case MENU_CONTROL:
                    if (appState.menuIndex == 3) enterMenu(MENU_SELECT_BG);
                    else if (appState.menuIndex == 4) enterMenu(MENU_MAIN);
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
                case MENU_OTA:
                    if (appState.menuIndex == ota.versionCount) enterMenu(MENU_MAIN);
                    else ota.begin(ota.versions[appState.menuIndex].url);
                    break;
                case MENU_NONE:
                    appState.isTempMode = !appState.isTempMode;
                    encoder.setEncoderValue(appState.isTempMode ? appState.temperature : appState.brightness);
                    break;
            }
        }
    }
    // Chỉ cập nhật UI khi không xem ảnh để tránh giật lag
    if (!isViewingImage) {
        if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(50))) {
            display.updateUI(appState);
            xSemaphoreGive(xGuiSemaphore);
        }
    }
}

void AppLogic::enterMenu(int level) {
    appState.currentMenu = (MenuLevel)level;
    appState.menuIndex = 0;
    if (level == MENU_MAIN) encoder.setBoundaries(0, 6, true);
    else if (level == MENU_CONTROL) encoder.setBoundaries(0, 4, true);
    else if (level == MENU_SELECT_BG) { storage.loadBgFiles(); encoder.setBoundaries(0, storage.bgFileCount, true); }
    else if (level == MENU_OTA) { ota.fetchVersions(); encoder.setBoundaries(0, ota.versionCount, true); }
    encoder.setEncoderValue(0);
}

void AppLogic::exitMenu() {
    appState.currentMenu = MENU_NONE;
    storage.saveConfig(appState);
    encoder.setBoundaries(0, 100, false);
    encoder.setEncoderValue(appState.isTempMode ? appState.temperature : appState.brightness);
}