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
static bool isViewingImage = false; // <--- CÚ CHỐT: State mới: Đang xem ảnh preview
static int selectedOtaIndex = -1; 

// Task cập nhật chứng khoán chạy ngầm
void stockUpdateTask(void *pvParameters) {
    while (1) {
        if (appState.currentMenu == MENU_STOCK) {
            // Đảm bảo có WiFi
            if (!webServer.runWiFiSetup()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            
            char ticker[15];
            stock.getTickerName(appState.stockIndex, ticker); 
            
            if (ticker[0] != '-') {
                // Lấy data và vẽ UI (Phải chiếm Semaphore)
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(500))) {
                    stock.fetchAndUpdateUI(appState.stockIndex); 
                    xSemaphoreGive(xGuiSemaphore);
                }
                
                // US Stock: US USDT cặp, Crypto: Cập nhật nhanh, VN Stock: Cập nhật chậm (15s)
                int delayTime = strstr(ticker, "USDT") ? 1000 : 15000;
                vTaskDelay(pdMS_TO_TICKS(delayTime));
            } else {
                vTaskDelay(pdMS_TO_TICKS(500)); // Hết danh sách
            }
        } else {
            stockTaskHandle = NULL;
            vTaskDelete(NULL); // Thoát menu, tự sát Task
        }
    }
}

// Task cập nhật OTA chạy ngầm
void otaUpdateTask(void *pvParameters) {
    bool listFetched = false;
    while (1) {
        if (appState.currentMenu == MENU_OTA) {
            if (!webServer.runWiFiSetup()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            
            if (selectedOtaIndex >= 0) {
                // Đã chọn version, bắt đầu OTA
                int idx = selectedOtaIndex;
                selectedOtaIndex = -1;
                ota.begin(ota.versions[idx].url);
            }
            else if (!listFetched) {
                // Chưa lấy list, bắt đầu lấy từ Github
                char* msg_buf = (char*)heap_caps_malloc(128, MALLOC_CAP_SPIRAM);
                if (!msg_buf) msg_buf = (char*)malloc(128);
                strcpy(msg_buf, "Fetching version list...\nPlease wait!");
                
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                    display.showProgressPopup("CHECKING", msg_buf, 0);
                    xSemaphoreGive(xGuiSemaphore);
                } else heap_caps_free(msg_buf);

                bool ok = ota.fetchVersions();
                listFetched = true;

                // Cập nhật UI list version (Phải chiếm Semaphore)
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                    display.closeProgressPopup();
                    if (!ok) {
                        display.showFileContent("OTA ERROR", "Failed to fetch versions.json from GitHub!");
                        isViewingFile = true;
                    } else {
                        // Cấu hình núm xoay để lăn list
                        encoder.setBoundaries(0, ota.versionCount, true);
                        encoder.setEncoderValue(0);
                        appState.menuIndex = 0;
                        display.forceRebuild(); // Rebuild để UI list version hiện ra
                        display.updateUI(appState);
                    }
                    xSemaphoreGive(xGuiSemaphore);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            // Thoát menu
            listFetched = false;
            selectedOtaIndex = -1;
            otaTaskHandle = NULL;
            vTaskDelete(NULL); // Tự sát Task
        }
    }
}

void AppLogic::begin() {
    // Khởi tạo đồ họa: load nền, núm xoay độ sáng, vẽ màn chính
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(500))) {
        display.loadBackgroundFromSD(); // Load nền raw từ SD root
        encoder.setBoundaries(0, 100, false);
        encoder.setEncoderValue(appState.isTempMode ? appState.temperature : appState.brightness);
        display.updateUI(appState);
        xSemaphoreGive(xGuiSemaphore);
    }
}

void AppLogic::handleEvents() {
    EncoderEvent event;
    bool ui_needs_update = false;

    // Nhận sự kiện từ Queue núm xoay
    if (xQueueReceive(xEncoderQueue, &event, pdMS_TO_TICKS(10)) == pdPASS) {
        ui_needs_update = true; 
        
        // --- LOGIC LONG PRESS ---
        if (event == ENC_LONG_PRESS) {
            // MẸO: Long press để thoát mọi thứ (File content, Preview, OTA, Upload...)
            if (isViewingFile || isViewingImage) {
                isViewingFile = false;
                isViewingImage = false; // Tắt state preview ảnh
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(50))) {
                    display.showFileContent(NULL, NULL); // Đóng text popup
                    display.closeProgressPopup(); 
                    display.closeImagePreview(); // <--- CÚ CHỐT: Giải phóng PSRAM và dọn ảnh
                    xSemaphoreGive(xGuiSemaphore);
                }
                // Nếu đang ở OTA hoặc Upload BG, long press cũng thoát menu luôn
                if (appState.currentMenu == MENU_OTA || appState.currentMenu == MENU_UPLOAD_BG) exitMenu();
            } else {
                // Thoát menu chính hoặc menu con về màn hình nền
                if (appState.currentMenu == MENU_NONE) enterMenu(MENU_MAIN);
                else exitMenu();
            }
        }

        // --- LOGIC SCROLL (NÚM LĂN) ---
        if (event == ENC_UP || event == ENC_DOWN) {
            // Nếu đang xem text, lăn núm xoay để cuộn văn bản
            if (isViewingFile) {
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(50))) {
                    if (event == ENC_UP) display.showFileContent("SCROLL_UP", NULL);
                    else display.showFileContent("SCROLL_DOWN", NULL);
                    xSemaphoreGive(xGuiSemaphore);
                }
            }
            // Nếu đang xem ảnh preview -> KHÔNG làm gì cả
            else if (isViewingImage) {
                ui_needs_update = false; 
            }
            // Nếu đang ở màn chính -> Tăng/giảm độ sáng/nhiệt độ màu
            else if (appState.currentMenu == MENU_NONE || appState.currentMenu == MENU_SET_SLEEP || appState.currentMenu == MENU_SET_BACKLIGHT) {
                int step = (event == ENC_UP) ? 5 : -5; 
                
                if (appState.currentMenu == MENU_SET_SLEEP) {
                    appState.sleepTimeout = constrain(appState.sleepTimeout + step, 30, 300);
                    encoder.setEncoderValue(appState.sleepTimeout); 
                }
                else if (appState.currentMenu == MENU_SET_BACKLIGHT) {
                    appState.oledBrightness = constrain(appState.oledBrightness + step, 0, 100);
                    encoder.setEncoderValue(appState.oledBrightness); 
                    display.setContrast(appState.oledBrightness); // Cập nhật đèn nền màn hình
                } 
                else if (appState.currentMenu == MENU_NONE) {
                    if (appState.isTempMode) {
                        appState.temperature = constrain(appState.temperature + step, 0, 100);
                        encoder.setEncoderValue(appState.temperature); 
                    } else {
                        appState.brightness = constrain(appState.brightness + step, 0, 100);
                        encoder.setEncoderValue(appState.brightness); 
                    }
                    // Gửi lệnh ESP-NOW ngay lập tức
                    espNow.send(0, appState.brightness, appState.temperature, ' ');
                }
            }
            // Nếu đang ở Menu -> Chuyển đổi mục menu
            else {
                appState.menuIndex = encoder.getEncoderValue(); 
                if (appState.currentMenu == MENU_STOCK) appState.stockIndex = appState.menuIndex;
            }
        }

        // --- LOGIC CLICK (NÚM BẤM) ---
        if (event == ENC_CLICK) {
            // MẸO: Click để thoát File content/Preview ảnh
            if (isViewingFile || isViewingImage) {
                isViewingFile = false;
                isViewingImage = false; // Thoát state preview ảnh
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(50))) {
                    display.showFileContent(NULL, NULL); // Đóng text popup
                    display.closeProgressPopup(); 
                    display.closeImagePreview(); // <--- CÚ CHỐT: Giải phóng PSRAM và dọn ảnh
                    xSemaphoreGive(xGuiSemaphore);
                }
                if (appState.currentMenu == MENU_OTA || appState.currentMenu == MENU_UPLOAD_BG) exitMenu();
            } else {
                // LOGIC CÂY MENU (STATE MACHINE)
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
                            // Xóa cấu hình WiFi và Reboot
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
                        // Gửi các lệnh hệ thống (Đổi màu nhanh)
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
                        // Lưu cấu hình và về lại menu con
                        storage.saveConfig(appState); 
                        enterMenu(MENU_CONTROL); 
                        break;
                    case MENU_STOCK:
                        // Click để cập nhật chứng khoán ngay lập tức
                        if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(500))) {
                            stock.fetchAndUpdateUI(appState.stockIndex);
                            xSemaphoreGive(xGuiSemaphore);
                        }
                        break;
                    case MENU_USB_MODE:
                        // --- LOGIC TRÌNH KHÁM PHÁ FILE ---
                        if (appState.menuIndex == storage.fileCount) {
                            enterMenu(MENU_MAIN); // Nút Back
                        } else {
                            // Lấy tên file đang chọn
                            char* fileName = storage.fileNames[appState.menuIndex];

                            // CÚ CHỐT 3: KIỂM TRA NẾU LÀ bg.bin THÌ PREVIEW ẢNH
                            if (strstr(fileName, "bg.bin")) {
                                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(500))) {
                                    FsFile file = sd_bg.open(fileName, O_READ);
                                    if(file) {
                                        if (display.showImagePreview(file)) {
                                            isViewingImage = true; // Chuyển state sang preview ảnh
                                        }
                                        file.close();
                                    }
                                    xSemaphoreGive(xGuiSemaphore);
                                }
                            } 
                            // Nếu là file khác -> Đọc text bình thường
                            else {
                                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(500))) {
                                    isViewingFile = true;
                                    char* content = storage.readFileToPSRAM(fileName);
                                    display.showFileContent(fileName, content);
                                    xSemaphoreGive(xGuiSemaphore);
                                }
                            }
                        }
                        break;
                    case MENU_OTA:
                        // Chọn mục OTA -> OTA Task sẽ lo việc nạp
                        if (appState.menuIndex == ota.versionCount) {
                            enterMenu(MENU_MAIN); // Nút Back
                        } else {
                            selectedOtaIndex = appState.menuIndex;
                        }
                        break;
                    case MENU_UPLOAD_BG:
                        exitMenu(); // Click vào Upload BG menu -> Thoát (Code Upload nằm trong enterMenu)
                        break;
                    case MENU_NONE:
                        // Click màn hình nền: Đổi chế độ Brightness <-> Temperature
                        appState.isTempMode = !appState.isTempMode;
                        encoder.setEncoderValue(appState.isTempMode ? appState.temperature : appState.brightness);
                        break;
                }
            }
        }
    }

    // --- CẬP NHẬT UI (PERIODIC UPDATE) ---
    static uint32_t last_ui_update = 0;
    // Cập nhật khi có sự kiện HOẶC mỗi 200ms để refresh Pin, Chart...
    if (ui_needs_update || (millis() - last_ui_update > 200)) {
        // KHÔNG cập nhật UI nếu đang xem ảnh hoặc text popup
        if (!isViewingFile && !isViewingImage && xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(50))) {
            display.updateUI(appState);
            xSemaphoreGive(xGuiSemaphore);
        }
        last_ui_update = millis();
    }
}

void AppLogic::enterMenu(int level) {
    appState.currentMenu = (MenuLevel)level;
    appState.menuIndex = 0;
    
    // --- LOGIC KHI VÀO CÁC CHẾ ĐỘ CÓ TASK RIÊNG / CẦN TẮT SLEEP ---
    if (level == MENU_STOCK || level == MENU_OTA || level == MENU_UPLOAD_BG) {
        originalSleepTimeout = appState.sleepTimeout;
        appState.sleepTimeout = 999999; // Tạm thời tắt sleep
        
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
            // Bật WebTask ở chế độ Upload BG (Chuyển WiFi sang mode nhà bác/mode AP)
            webServer.runBgUpload();
        }
        return; 
    }
    
    // --- CẤU HÌNH BIÊN NÚM XOAY CHO CÁC MENU ---
    if (level == MENU_MAIN) encoder.setBoundaries(0, 5, true);         // 5 mục + 1 mục Back
    else if (level == MENU_CONTROL) encoder.setBoundaries(0, 4, true); // 4 mục + 1 mục Back
    else if (level == MENU_LAMP) encoder.setBoundaries(0, 4, true);    // 4 mục + 1 mục Back
    else if (level == MENU_USB_MODE) {
        storage.loadFiles(); // Quét lại SD khi vào
        encoder.setBoundaries(0, storage.fileCount, true); // Biên = fileCount + 1 nút Back
    }
    // Các menu Set giá trị -> biên 0-1000 (map về 0-100)
    else if (level == MENU_SET_SLEEP || level == MENU_SET_BACKLIGHT) encoder.setBoundaries(0, 1000, false); 
    
    encoder.setEncoderValue(0);
}

void AppLogic::exitMenu() {
    // --- LOGIC KHI THOÁT CÁC CHẾ ĐỘ ĐẶC BIỆT ---
    if (appState.currentMenu == MENU_STOCK || appState.currentMenu == MENU_OTA || appState.currentMenu == MENU_UPLOAD_BG) {
        appState.sleepTimeout = originalSleepTimeout; // Khôi phục sleep
        WiFi.disconnect(); 
        WiFi.mode(WIFI_OFF);
        espNow.begin(); // Reset ESP-NOW sau khi dùng WiFi
    }
    appState.currentMenu = MENU_NONE;
    encoder.setBoundaries(0, 100, false);
    encoder.setEncoderValue(appState.isTempMode ? appState.temperature : appState.brightness);
}

// Callback từ UI: Mục Roller chứng khoán thay đổi
extern "C" void action_on_stock_changed_cb(lv_event_t * e) {
    lv_obj_t * roller = lv_event_get_target(e);
    appState.stockIndex = lv_roller_get_selected(roller);
    // Cập nhật giá chứng khoán ngay lập tức (Phải chiếm Semaphore)
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(50))) {
        stock.fetchAndUpdateUI(appState.stockIndex);
        xSemaphoreGive(xGuiSemaphore);
    }
}