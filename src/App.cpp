#include "App.h"
#include "Config.h"
#include "Encoder.h"
#include "EspNow.h"
#include "Display.h"
#include "System.h"
#include "Storage.h" 
#include "Ota.h"
#include "WebSv.h" 
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <SdFat.h>

AppLogic app;
TaskHandle_t otaTaskHandle = NULL;
extern SemaphoreHandle_t xGuiSemaphore;
extern QueueHandle_t xEncoderQueue;
extern SdFs sd_bg; 
extern bool matterQrSynced;

static int originalSleepTimeout = 60; 
static bool isViewingFile = false; 
static bool isViewingImage = false; 
static int selectedOtaIndex = -1; 

static bool pendingImageLoad = false; // Cờ báo hiệu load ảnh preview khi cuộn
static uint32_t lastScrollTime = 0;   // Mốc thời gian để debounce việc cuộn
static uint32_t lastEncEventTime = 0; // Mốc thời gian event encoder trước — dùng cho gia tốc
static EncoderEvent lastEncDir = ENC_CLICK; // Hướng vặn trước — dùng để phát hiện bounce ngược chiều
static uint32_t lastConfigSaveMs = 0;
static bool configSavePending = false;

extern volatile bool espnow_needs_update;

// Ghi NVS tối đa 1 lần / 10 giây để bảo vệ flash.
// forceFlush = true chỉ dùng khi sắp sleep/shutdown — ghi ngay bất kể thời gian.
static void requestConfigSave(bool forceFlush = false) {
    const uint32_t now = millis();
    configSavePending = true;
    if (forceFlush || (now - lastConfigSaveMs >= 10000)) {
        if (storage.saveConfig(appState)) {
            lastConfigSaveMs = now;
            configSavePending = false;
        }
    }
}

extern volatile bool isGuiReady;
extern volatile bool isStorageReady;

void appTask(void *pvParameters) {
    // Đợi display.begin() xong (guiTask đã vẽ boot log phase 1+2)
    while (!isGuiReady) vTaskDelay(pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(100));

    if (xSemaphoreTakeRecursive(xGuiSemaphore, portMAX_DELAY)) {

        // ── Boot log phase 3: SD Card ──────────────────────────
        if (appState.devMode) display.bootPrint("SD", "Mounting SD card");
        storage.begin();
        // In kết quả thật của việc mount SD
        if (appState.devMode && !storage.isReady) {
            display.bootPrint("SD", "SD mount failed", false);
        }

        // ── Boot log phase 4: Config ───────────────────────────
        if (storage.isReady) {
            if (appState.devMode) display.bootPrint("CFG", "Loading config");
            bool cfgOk = storage.loadConfig(appState);
            if (appState.devMode) display.bootPrint("CFG", "Config loaded", cfgOk);
            display.setContrast(appState.oledBrightness);
        }

        isStorageReady = true;

        // ── Boot log phase 5: Dòng cuối ───────────────────────
        if (appState.devMode) display.bootPrint("APP", "All systems ready!");

        xSemaphoreGiveRecursive(xGuiSemaphore);
    }

    WebServer* server = new WebServer(80);
    webServer.begin(server);
    app.begin();

    while (1) {
        app.handleEvents();
        if (millis() > 5000 && encoder.shouldSleep(appState.sleepTimeout * 1000UL)) {
            sys.goToSleep();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
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

    if (espnow_needs_update) {
        espnow_needs_update = false;
        if (appState.currentMenu == MENU_NONE) {
            encoder.setEncoderValue(appState.isTempMode ? appState.temperature : appState.brightness);
        }
        ui_needs_update = true;
    }

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
                    display.closeMatterQr();
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
                // Gia tốc: vặn nhanh hơn → bước nhảy lớn hơn
                // Nếu hướng đảo chiều → encoder bounce phần cứng → reset accel về 1
                const uint32_t now_enc = millis();
                const uint32_t enc_interval = now_enc - lastEncEventTime;
                const bool dirChanged = (lastEncDir != event);
                lastEncEventTime = now_enc;
                lastEncDir = event;
                int accel = 1;
                if (!dirChanged) {
                    if      (enc_interval < 80)  accel = 6;
                    else if (enc_interval < 150) accel = 4;
                    else if (enc_interval < 300) accel = 2;
                }
                int step = (event == ENC_UP) ? (5 * accel) : -(5 * accel);
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
                    requestConfigSave(false);
                }
            }
            else {
                appState.menuIndex = encoder.getEncoderValue(); 

                if (appState.currentMenu == MENU_SELECT_BG) {
                    if (isViewingImage) {
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
                    } else {
                        // Auto-preview on scroll even before first click
                        if (appState.menuIndex < storage.bgFileCount) {
                            pendingImageLoad = true;
                            lastScrollTime = millis();
                        }
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
                    display.closeMatterQr();
                    xSemaphoreGiveRecursive(xGuiSemaphore);
                }
                if (appState.currentMenu == MENU_ABOUT) {
                    enterMenu(MENU_CONTROL);
                    encoder.setEncoderValue(6); 
                } else {
                    encoder.setEncoderValue(appState.menuIndex); 
                }
            } else {
                switch (appState.currentMenu) {
                    case MENU_MAIN:
                        if (appState.menuIndex == 0) enterMenu(MENU_CONTROL);
                        else if (appState.menuIndex == 1) enterMenu(MENU_LAMP);
                        else exitMenu(); 
                        break;
                    case MENU_CONTROL:
                        if (appState.menuIndex == 0) enterMenu(MENU_SET_SLEEP);
                        else if (appState.menuIndex == 1) enterMenu(MENU_SET_BACKLIGHT);
                        else if (appState.menuIndex == 2) enterMenu(MENU_OTA);
                        else if (appState.menuIndex == 3) enterMenu(MENU_WEB_SERVER);
                        else if (appState.menuIndex == 4) enterMenu(MENU_SELECT_BG); 
                        else if (appState.menuIndex == 5) {
                            appState.devMode = !appState.devMode;
                            requestConfigSave(true);
                            display.forceRebuild();
                            if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                                display.showFileContent("DEV MODE", appState.devMode ? "Enabled" : "Disabled");
                                xSemaphoreGiveRecursive(xGuiSemaphore);
                            }
                            isViewingFile = true;
                        }
                        else if (appState.menuIndex == 6) enterMenu(MENU_ABOUT);
                        else if (appState.menuIndex == 7) {
                            if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                                display.showFileContent("REMOTE", "Restarting...");
                                xSemaphoreGiveRecursive(xGuiSemaphore);
                            }
                            storage.safeSync(appState);
                            vTaskDelay(pdMS_TO_TICKS(250));
                            ESP.restart();
                        }
                        else enterMenu(MENU_MAIN);
                        break;
                    case MENU_LAMP:
                        if (appState.menuIndex == 0) {
                            bool ok = espNow.sendCommandWithAck('I', 2, 350);
                            if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                                display.showFileContent("LAMP", ok ? "Identify command ACK." : "Identify ACK timeout.");
                                xSemaphoreGiveRecursive(xGuiSemaphore);
                            }
                            isViewingFile = true;
                        } else if (appState.menuIndex == 1) {
                            // Pair assistant: open commissioning window first, then show setup QR.
                            bool ok = espNow.sendCommandWithAck('P', 2, 350);
                            matterQrSynced = false;
                            espNow.sendCommandWithAck('Q', 2, 350);
                            vTaskDelay(pdMS_TO_TICKS(120));
                            if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                                display.showMatterQr();
                                xSemaphoreGiveRecursive(xGuiSemaphore);
                            }
                            if (!ok) {
                                Serial.println("[APP] Pair: Open Pair ACK timeout, still showing QR");
                            }
                            isViewingFile = true;
                        } else if (appState.menuIndex == 2) {
                            espNow.sendCommandWithAck('R', 0, 0);
                            if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                                display.showFileContent("LAMP", "Restart command sent.");
                                xSemaphoreGiveRecursive(xGuiSemaphore);
                            }
                            isViewingFile = true;
                        } else if (appState.menuIndex == 3) {
                            bool ok = espNow.sendCommandWithAck('U', 2, 350);
                            if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                                display.showFileContent("LAMP", ok ? "Unpair command ACK." : "Unpair ACK timeout.");
                                xSemaphoreGiveRecursive(xGuiSemaphore);
                            }
                            isViewingFile = true;
                        } else if (appState.menuIndex == 4) {
                            espNow.sendCommandWithAck('F', 0, 0);
                            if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                                display.showFileContent("LAMP", "Factory reset command sent.");
                                xSemaphoreGiveRecursive(xGuiSemaphore);
                            }
                            isViewingFile = true;
                        } else if (appState.menuIndex == 5) {
                            enterMenu(MENU_MAIN);
                        }
                        break;
                    case MENU_SET_SLEEP:         
                    case MENU_SET_BACKLIGHT:
                        requestConfigSave(true); enterMenu(MENU_CONTROL); break;
                    case MENU_SELECT_BG:
                        if (appState.menuIndex == storage.bgFileCount) {
                            // "Back" — close preview and exit
                            pendingImageLoad = false;
                            isViewingImage = false;
                            if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(50))) {
                                display.closeImagePreview(); xSemaphoreGiveRecursive(xGuiSemaphore);
                            }
                            enterMenu(MENU_CONTROL);
                        } else {
                            // Apply selected BG immediately (preview already visible via auto-scroll)
                            char fullPath[64];
                            snprintf(fullPath, sizeof(fullPath), "/background/%s", storage.bgFileNames[appState.menuIndex]);
                            pendingImageLoad = false;
                            strncpy(appState.bgFilePath, fullPath, sizeof(appState.bgFilePath) - 1);
                            appState.bgFilePath[sizeof(appState.bgFilePath) - 1] = '\0';
                            requestConfigSave(true);
                            Serial.printf("[APP] Applied BG: %s\n", fullPath);
                            if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(500))) {
                                display.closeImagePreview();
                                display.loadBackgroundFromSD();
                                xSemaphoreGiveRecursive(xGuiSemaphore);
                            }
                            isViewingImage = false; exitMenu();
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
                        requestConfigSave(true); break;
                    default: break;
                }
            }
        }
    }

    if (configSavePending && (millis() - lastConfigSaveMs >= 10000)) {
        requestConfigSave(false);
    }

    if (pendingImageLoad && (millis() - lastScrollTime > 300)) {
        pendingImageLoad = false;
        char fullPath[64];
        snprintf(fullPath, sizeof(fullPath), "/background/%s", storage.bgFileNames[appState.menuIndex]);
        Serial.printf("[APP] Previewing image on scroll: %s\n", fullPath);
        
        if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(500))) {
            digitalWrite(SCR_CS_PIN, HIGH);
            FsFile file = sd_bg.open(fullPath, O_RDONLY); 
            if (!file) {
                sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
                file = sd_bg.open(fullPath, O_RDONLY);
            }
            if (file) {
                if (display.showImagePreview(file)) {
                    isViewingImage = true;
                }
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
    
    if (level == MENU_OTA || level == MENU_WEB_SERVER) {
        originalSleepTimeout = appState.sleepTimeout;
        if (originalSleepTimeout > 300) originalSleepTimeout = 60; // Gác cổng
        appState.sleepTimeout = 999999; 
        
        if (level == MENU_OTA) {
            isViewingFile = false; selectedOtaIndex = -1;
            if (!otaTaskHandle) xTaskCreatePinnedToCore(otaUpdateTask, "OtaTask", STACK_NETWORK, NULL, PRIO_NETWORK, &otaTaskHandle, 1);
        } else if (level == MENU_WEB_SERVER) { isViewingFile = false; webServer.runBgUpload(); }
        return; 
    }
    
    if (level == MENU_MAIN) {
        encoder.setBoundaries(0, 2, true);         
    } 
    else if (level == MENU_CONTROL) {
        encoder.setBoundaries(0, 8, true); 
    }
    else if (level == MENU_LAMP) {
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
    // Always clear transient overlays when leaving menu to avoid stale popups
    // (e.g. "No synced WiFi" in WEB/OTA flow remaining on next menu entry).
    if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(80))) {
        display.closeProgressPopup();
        display.showFileContent(NULL, NULL);
        display.closeImagePreview();
        display.closeMatterQr();
        xSemaphoreGiveRecursive(xGuiSemaphore);
    }
    isViewingFile = false;
    isViewingImage = false;
    pendingImageLoad = false;

    if (appState.currentMenu == MENU_OTA || appState.currentMenu == MENU_WEB_SERVER) {
        appState.sleepTimeout = originalSleepTimeout; 
        WiFi.disconnect(); WiFi.mode(WIFI_OFF); espNow.begin(); 
    }
    appState.currentMenu = MENU_NONE;
    encoder.setBoundaries(0, 100, false);
    encoder.setEncoderValue(appState.isTempMode ? appState.temperature : appState.brightness);
    requestConfigSave(true); 
}
