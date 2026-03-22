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

AppLogic app;
TaskHandle_t stockTaskHandle = NULL;
TaskHandle_t otaTaskHandle = NULL;
extern SemaphoreHandle_t xGuiSemaphore;
extern QueueHandle_t xEncoderQueue;
extern SdFs sd_bg; 

static int originalSleepTimeout = 60; 
static bool isViewingFile = false; 
static int selectedOtaIndex = -1; 

void stockUpdateTask(void *pvParameters) {
    while (1) {
        if (appState.currentMenu == MENU_STOCK) {
            if (!webServer.runWiFiSetup()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            
            char ticker[15];
            stock.getTickerName(appState.stockIndex, ticker); 
            
            if (ticker[0] != '-') {
                stock.fetchAndUpdateUI(appState.stockIndex); 
                int delayTime = strstr(ticker, "USDT") ? 1000 : 15000;
                vTaskDelay(pdMS_TO_TICKS(delayTime));
            } else {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
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
                char* msg_buf = (char*)heap_caps_malloc(128, MALLOC_CAP_SPIRAM);
                if (!msg_buf) msg_buf = (char*)malloc(128);
                strcpy(msg_buf, "Fetching version list...\nPlease wait!");
                
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                    display.showProgressPopup("CHECKING", msg_buf, 0);
                    xSemaphoreGive(xGuiSemaphore);
                } else heap_caps_free(msg_buf);

                bool ok = ota.fetchVersions();
                listFetched = true;

                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                    display.closeProgressPopup();
                    if (!ok) {
                        display.showFileContent("OTA ERROR", "Failed to fetch versions.json from GitHub!");
                        isViewingFile = true;
                    } else {
                        encoder.setBoundaries(0, ota.versionCount, true);
                        encoder.setEncoderValue(0);
                        appState.menuIndex = 0;
                        display.forceRebuild();
                        display.updateUI(appState);
                    }
                    xSemaphoreGive(xGuiSemaphore);
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
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(500))) {
        display.loadBackgroundFromSD(); 
        encoder.setBoundaries(0, 100, false);
        encoder.setEncoderValue(appState.isTempMode ? appState.temperature : appState.brightness);
        display.updateUI(appState);
        xSemaphoreGive(xGuiSemaphore);
    }
}

void AppLogic::handleEvents() {
    EncoderEvent event;
    bool ui_needs_update = false;

    if (xQueueReceive(xEncoderQueue, &event, pdMS_TO_TICKS(10)) == pdPASS) {
        ui_needs_update = true; 
        
        if (event == ENC_LONG_PRESS) {
            if (isViewingFile) {
                isViewingFile = false;
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(50))) {
                    display.showFileContent(NULL, NULL);
                    display.closeProgressPopup(); 
                    xSemaphoreGive(xGuiSemaphore);
                }
                if (appState.currentMenu == MENU_OTA || appState.currentMenu == MENU_UPLOAD_BG) exitMenu();
            } else {
                if (appState.currentMenu == MENU_NONE) enterMenu(MENU_MAIN);
                else exitMenu();
            }
        }

        if (event == ENC_UP || event == ENC_DOWN) {
            if (isViewingFile) {
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(50))) {
                    if (event == ENC_UP) display.showFileContent("SCROLL_UP", NULL);
                    else display.showFileContent("SCROLL_DOWN", NULL);
                    xSemaphoreGive(xGuiSemaphore);
                }
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
                }
            }
            else {
                appState.menuIndex = encoder.getEncoderValue(); 
                if (appState.currentMenu == MENU_STOCK) appState.stockIndex = appState.menuIndex;
            }
        }

        if (event == ENC_CLICK) {
            if (isViewingFile) {
                isViewingFile = false;
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(50))) {
                    display.showFileContent(NULL, NULL);
                    display.closeProgressPopup(); 
                    xSemaphoreGive(xGuiSemaphore);
                }
                if (appState.currentMenu == MENU_OTA || appState.currentMenu == MENU_UPLOAD_BG) exitMenu();
            } else {
                switch (appState.currentMenu) {
                    case MENU_MAIN:
                        if (appState.menuIndex == 0) enterMenu(MENU_CONTROL);
                        else if (appState.menuIndex == 1) enterMenu(MENU_LAMP);
                        else if (appState.menuIndex == 2) enterMenu(MENU_USB_MODE); 
                        else if (appState.menuIndex == 3) enterMenu(MENU_STOCK);
                        else if (appState.menuIndex == 4) enterMenu(MENU_OTA); 
                        else exitMenu(); 
                        break;
                    case MENU_CONTROL:
                        if (appState.menuIndex == 0) enterMenu(MENU_SET_SLEEP);
                        else if (appState.menuIndex == 1) enterMenu(MENU_SET_BACKLIGHT);
                        else if (appState.menuIndex == 2) {
                            WiFi.mode(WIFI_STA);
                            WiFi.disconnect(false, true); 
                            if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                                display.showFileContent("WIFI CLEARED", "Remote WiFi has been deleted!\n\nRebooting in 3 seconds to apply...");
                                isViewingFile = true;
                                xSemaphoreGive(xGuiSemaphore);
                            }
                            vTaskDelay(pdMS_TO_TICKS(3000));
                            ESP.restart(); 
                        }
                        else if (appState.menuIndex == 3) enterMenu(MENU_UPLOAD_BG); 
                        else enterMenu(MENU_MAIN);
                        break;
                    case MENU_LAMP:
                        if (appState.menuIndex == 4) { 
                            enterMenu(MENU_MAIN); 
                        } else {
                            char cmd = ' ';
                            if (appState.menuIndex == 0) cmd = 'R';      
                            else if (appState.menuIndex == 1) cmd = 'U'; 
                            else if (appState.menuIndex == 2) cmd = 'W'; 
                            else if (appState.menuIndex == 3) cmd = 'E'; 
                            
                            espNow.send(0, appState.brightness, appState.temperature, cmd);
                            
                            if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                                display.showFileContent("CMD SENT", "Command has been sent to the Lamp via ESP-NOW!");
                                isViewingFile = true;
                                xSemaphoreGive(xGuiSemaphore);
                            }
                        }
                        break;
                    case MENU_SET_SLEEP:         
                    case MENU_SET_BACKLIGHT:
                        storage.saveConfig(appState); 
                        enterMenu(MENU_CONTROL); 
                        break;
                    case MENU_STOCK:
                        stock.fetchAndUpdateUI(appState.stockIndex);
                        break;
                    case MENU_USB_MODE:
                        if (appState.menuIndex == storage.fileCount) {
                            enterMenu(MENU_MAIN); 
                        } else {
                            if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(500))) {
                                isViewingFile = true;
                                char* content = storage.readFileToPSRAM(storage.fileNames[appState.menuIndex]);
                                display.showFileContent(storage.fileNames[appState.menuIndex], content);
                                xSemaphoreGive(xGuiSemaphore);
                            }
                        }
                        break;
                    case MENU_OTA:
                        if (appState.menuIndex == ota.versionCount) {
                            enterMenu(MENU_MAIN); 
                        } else {
                            selectedOtaIndex = appState.menuIndex;
                        }
                        break;
                    case MENU_UPLOAD_BG:
                        exitMenu();
                        break;
                    case MENU_NONE:
                        appState.isTempMode = !appState.isTempMode;
                        encoder.setEncoderValue(appState.isTempMode ? appState.temperature : appState.brightness);
                        break;
                }
            }
        }
    }

    static uint32_t last_ui_update = 0;
    if (ui_needs_update || (millis() - last_ui_update > 200)) {
        if (!isViewingFile && xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(50))) {
            display.updateUI(appState);
            xSemaphoreGive(xGuiSemaphore);
        }
        last_ui_update = millis();
    }
}

void AppLogic::enterMenu(int level) {
    appState.currentMenu = (MenuLevel)level;
    appState.menuIndex = 0;
    
    if (level == MENU_STOCK || level == MENU_OTA || level == MENU_UPLOAD_BG) {
        originalSleepTimeout = appState.sleepTimeout;
        appState.sleepTimeout = 999999; 
        
        if (level == MENU_STOCK) {
            encoder.setBoundaries(0, 19, true); 
            if (stockTaskHandle == NULL) {
                xTaskCreatePinnedToCore(stockUpdateTask, "StockTask", STACK_NETWORK, NULL, PRIO_NETWORK, &stockTaskHandle, 1);
            }
        } else if (level == MENU_OTA) {
            isViewingFile = false; 
            selectedOtaIndex = -1;
            
            if (otaTaskHandle == NULL) {
                xTaskCreatePinnedToCore(otaUpdateTask, "OtaTask", STACK_NETWORK, NULL, PRIO_NETWORK, &otaTaskHandle, 1);
            }
        } else if (level == MENU_UPLOAD_BG) {
            isViewingFile = false;
            webServer.runBgUpload();
        }
        return; 
    }
    
    if (level == MENU_MAIN) encoder.setBoundaries(0, 5, true);         
    else if (level == MENU_CONTROL) encoder.setBoundaries(0, 4, true); 
    else if (level == MENU_LAMP) encoder.setBoundaries(0, 4, true);    
    else if (level == MENU_USB_MODE) {
        storage.loadFiles(); 
        encoder.setBoundaries(0, storage.fileCount, true);
    }
    else if (level == MENU_SET_SLEEP || level == MENU_SET_BACKLIGHT) encoder.setBoundaries(0, 1000, false); 
    
    encoder.setEncoderValue(0);
}

void AppLogic::exitMenu() {
    if (appState.currentMenu == MENU_STOCK || appState.currentMenu == MENU_OTA || appState.currentMenu == MENU_UPLOAD_BG) {
        appState.sleepTimeout = originalSleepTimeout;
        WiFi.disconnect(); 
        WiFi.mode(WIFI_OFF);
        espNow.begin();
    }
    appState.currentMenu = MENU_NONE;
    encoder.setBoundaries(0, 100, false);
    encoder.setEncoderValue(appState.isTempMode ? appState.temperature : appState.brightness);
}

extern "C" void action_on_stock_changed_cb(lv_event_t * e) {
    lv_obj_t * roller = lv_event_get_target(e);
    appState.stockIndex = lv_roller_get_selected(roller);
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(50))) {
        stock.fetchAndUpdateUI(appState.stockIndex);
        xSemaphoreGive(xGuiSemaphore);
    }
}