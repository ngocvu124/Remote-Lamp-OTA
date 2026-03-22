#include "App.h"
#include "Config.h"
#include "Encoder.h"
#include "EspNow.h"
#include "Display.h"
#include "System.h"
#include "Storage.h" 
#include "Stock.h"
#include "Ota.h" 
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <WebServer.h> 
#include <SdFat.h> 

AppLogic app;
TaskHandle_t stockTaskHandle = NULL;
TaskHandle_t otaTaskHandle = NULL;
TaskHandle_t bgUploadTaskHandle = NULL; 
extern SemaphoreHandle_t xGuiSemaphore;
extern QueueHandle_t xEncoderQueue;
extern SdFs sd_bg; 

static int originalSleepTimeout = 60; 
static bool isViewingFile = false; 
static int selectedOtaIndex = -1; 

bool connectWiFiHelper() {
    if (WiFi.status() == WL_CONNECTED) return true;

    WiFi.mode(WIFI_STA);
    WiFi.begin(); 

    char* msg_buf = (char*)heap_caps_malloc(128, MALLOC_CAP_SPIRAM);
    if (!msg_buf) msg_buf = (char*)malloc(128);
    strcpy(msg_buf, "Connecting to saved WiFi...\nWait up to 10s...");
    
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        display.showProgressPopup("WIFI INIT", msg_buf, 0);
        xSemaphoreGive(xGuiSemaphore);
    } else {
        heap_caps_free(msg_buf);
    }

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        attempts++;
        if (appState.currentMenu != MENU_STOCK && appState.currentMenu != MENU_OTA) {
            WiFi.disconnect();
            return false;
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
            display.closeProgressPopup();
            xSemaphoreGive(xGuiSemaphore);
        }
        return true;
    }

    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect(); 
    vTaskDelay(pdMS_TO_TICKS(100));

    msg_buf = (char*)heap_caps_malloc(128, MALLOC_CAP_SPIRAM);
    if (!msg_buf) msg_buf = (char*)malloc(128);
    strcpy(msg_buf, "Scanning WiFi networks...\nPlease wait...");
    
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        display.showProgressPopup("SCANNING", msg_buf, 0);
        xSemaphoreGive(xGuiSemaphore);
    } else {
        heap_caps_free(msg_buf);
    }

    int n = WiFi.scanNetworks();
    String options = "";
    if (n == 0) {
        options = "<option value=''>Khong tim thay mang nao!</option>";
    } else {
        for (int i = 0; i < n; ++i) {
            String ssid = WiFi.SSID(i);
            if(ssid.length() > 0) {
                options += "<option value='" + ssid + "'>" + ssid + " (" + String(WiFi.RSSI(i)) + "dBm)</option>";
            }
        }
    }
    WiFi.scanDelete(); 

    WiFi.softAP("REMOTE_LAMP"); 

    msg_buf = (char*)heap_caps_malloc(256, MALLOC_CAP_SPIRAM);
    if (!msg_buf) msg_buf = (char*)malloc(256);
    strcpy(msg_buf, "1. Ket noi WiFi: REMOTE_LAMP\n2. Mo trinh duyet web\n3. Truy cap: 192.168.4.1\nDe chon mang & nhap pass!");
    
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        display.showProgressPopup("WEB SETUP", msg_buf, 0);
        xSemaphoreGive(xGuiSemaphore);
    } else {
        heap_caps_free(msg_buf);
    }

    WebServer server(80);
    static bool wifiSetupDone = false;
    wifiSetupDone = false;

    server.on("/", [&server, options]() {
        String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
                      "<style>body{font-family:sans-serif;text-align:center;margin-top:50px;background:#222;color:#fff;}"
                      "select,input{padding:12px;margin:8px;width:80%;max-width:300px;border-radius:6px;border:none;font-size:16px;}</style></head>"
                      "<body><h2>CHON WIFI NHA BAC</h2>"
                      "<form action='/save'>"
                      "<select name='ssid' required>" + options + "</select><br>"
                      "<input type='password' name='pass' placeholder='Mat khau WiFi (Neu co)'><br>"
                      "<input type='submit' value='KET NOI NGAY' style='background:#ff7200;color:#fff;font-weight:bold;cursor:pointer;'></form>"
                      "</body></html>";
        server.send(200, "text/html", html);
    });

    server.on("/save", [&server]() {
        String qsid = server.arg("ssid");
        String qpass = server.arg("pass");
        
        server.send(200, "text/html", "<html><body style='text-align:center;margin-top:50px;font-family:sans-serif;background:#222;color:#fff;'><h2>Da luu! Dang ket noi...</h2><p>Ban co the dong trang web nay.</p></body></html>");
        
        vTaskDelay(pdMS_TO_TICKS(500));
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.begin(qsid.c_str(), qpass.c_str());
        wifiSetupDone = true;
    });

    server.begin();

    while (!wifiSetupDone) {
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(20)); 
        
        if (appState.currentMenu != MENU_STOCK && appState.currentMenu != MENU_OTA) {
            server.stop();
            WiFi.softAPdisconnect(true);
            WiFi.disconnect();
            return false;
        }
    }

    server.stop();

    msg_buf = (char*)heap_caps_malloc(128, MALLOC_CAP_SPIRAM);
    if (!msg_buf) msg_buf = (char*)malloc(128);
    strcpy(msg_buf, "Received new WiFi credentials!\nConnecting...");
    
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        display.showProgressPopup("WIFI LINKING", msg_buf, 50);
        xSemaphoreGive(xGuiSemaphore);
    } else {
        heap_caps_free(msg_buf);
    }

    attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        vTaskDelay(pdMS_TO_TICKS(500));
        attempts++;
    }
    
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        display.closeProgressPopup();
        xSemaphoreGive(xGuiSemaphore);
    }
    return WiFi.status() == WL_CONNECTED;
}

void stockUpdateTask(void *pvParameters) {
    while (1) {
        if (appState.currentMenu == MENU_STOCK) {
            if (!connectWiFiHelper()) {
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
            if (!connectWiFiHelper()) {
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

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="vi">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Đổi Hình Nền ESP32</title>
    <style>
        body { font-family: sans-serif; text-align: center; background: #222; color: #fff; margin: 0; padding: 20px; }
        canvas { border: 2px solid #ff7200; border-radius: 8px; margin-top: 15px; max-width: 100%; box-shadow: 0px 4px 10px rgba(0,0,0,0.5); }
        .btn { background: #ff7200; color: #fff; border: none; padding: 12px 24px; font-size: 16px; font-weight: bold; border-radius: 6px; cursor: pointer; margin-top: 15px; width: 100%; max-width: 240px; }
        .btn:disabled { background: #555; cursor: not-allowed; }
        input[type="file"] { display: none; }
        .upload-label { display: inline-block; background: #444; color: #fff; padding: 12px 24px; font-size: 16px; border-radius: 6px; cursor: pointer; margin-top: 10px; width: calc(100% - 48px); max-width: 192px; border: 1px solid #777; }
        #status { margin-top: 15px; color: #00ff00; font-weight: bold; font-size: 14px; }
    </style>
</head>
<body>
    <h2>ĐỔI HÌNH NỀN (Tự Động Crop)</h2>
    <label class="upload-label">
        <input type="file" id="fileInput" accept="image/jpeg, image/png">
        CHỌN ẢNH TỪ MÁY
    </label>
    <br>
    <canvas id="canvas" width="240" height="240"></canvas>
    <br>
    <button id="uploadBtn" class="btn" disabled>TẢI LÊN MÀN HÌNH</button>
    <div id="status"></div>

    <script>
        const fileInput = document.getElementById('fileInput');
        const canvas = document.getElementById('canvas');
        const ctx = canvas.getContext('2d');
        const uploadBtn = document.getElementById('uploadBtn');
        const statusDiv = document.getElementById('status');
        let rgb565Data = null;

        ctx.fillStyle = '#333';
        ctx.fillRect(0, 0, 240, 240);
        ctx.fillStyle = '#aaa';
        ctx.font = '16px sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText('Ảnh xem trước (240x240)', 120, 120);

        fileInput.addEventListener('change', function(e) {
            const file = e.target.files[0];
            if (!file) return;
            
            const reader = new FileReader();
            reader.onload = function(event) {
                const img = new Image();
                img.onload = function() {
                    const scale = Math.max(240 / img.width, 240 / img.height);
                    const w = img.width * scale;
                    const h = img.height * scale;
                    const x = (240 - w) / 2;
                    const y = (240 - h) / 2;
                    
                    ctx.clearRect(0, 0, 240, 240);
                    ctx.drawImage(img, x, y, w, h);
                    
                    const imgData = ctx.getImageData(0, 0, 240, 240).data;
                    rgb565Data = new Uint8Array(240 * 240 * 2); 
                    
                    let j = 0;
                    for (let i = 0; i < imgData.length; i += 4) {
                        const r = imgData[i] >> 3;
                        const g = imgData[i+1] >> 2;
                        const b = imgData[i+2] >> 3;
                        const rgb565 = (r << 11) | (g << 5) | b;
                        
                        rgb565Data[j++] = rgb565 & 0xFF;
                        rgb565Data[j++] = (rgb565 >> 8) & 0xFF;
                    }
                    
                    uploadBtn.disabled = false;
                    statusDiv.style.color = '#fff';
                    statusDiv.innerText = 'Đã dịch xong màu! Sẵn sàng tải lên.';
                }
                img.src = event.target.result;
            }
            reader.readAsDataURL(file);
        });

        uploadBtn.addEventListener('click', function() {
            if (!rgb565Data) return;
            uploadBtn.disabled = true;
            statusDiv.innerText = 'Đang bắn file qua WiFi... Chờ xíu!';
            statusDiv.style.color = '#ff7200';

            const blob = new Blob([rgb565Data], { type: 'application/octet-stream' });
            const formData = new FormData();
            formData.append('bg', blob, 'bg.bin');

            fetch('/upload', { method: 'POST', body: formData })
            .then(response => {
                if(response.ok) {
                    statusDiv.style.color = '#00ff00';
                    statusDiv.innerText = 'XONG! Màn hình đang tự khởi động lại...';
                } else throw new Error('Upload failed');
            })
            .catch(error => {
                statusDiv.style.color = 'red';
                statusDiv.innerText = 'Lỗi mạng: ' + error.message;
                uploadBtn.disabled = false;
            });
        });
    </script>
</body>
</html>
)rawliteral";

void uploadBgTask(void *pvParameters) {
    sd_bg.begin(SD_CS_PIN); 

    WiFi.mode(WIFI_AP);
    WiFi.softAP("REMOTE_LAMP_BG"); 

    char* msg_buf = (char*)heap_caps_malloc(256, MALLOC_CAP_SPIRAM);
    if (!msg_buf) msg_buf = (char*)malloc(256);
    strcpy(msg_buf, "1. Ket noi WiFi: REMOTE_LAMP_BG\n2. Mo trinh duyet web\n3. Truy cap: 192.168.4.1\nDe chon anh tu dien thoai!");
    
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        display.showProgressPopup("CHANGE BACKGROUND", msg_buf, 0);
        xSemaphoreGive(xGuiSemaphore);
    } else heap_caps_free(msg_buf);

    WebServer server(80);
    FsFile uploadFile; 

    server.on("/", HTTP_GET, [&server]() {
        server.send(200, "text/html", index_html);
    });

    server.on("/upload", HTTP_POST, [&server]() {
        server.send(200, "text/plain", "OK");
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP.restart(); 
    }, [&server, &uploadFile]() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            if (sd_bg.exists("/bg.bin")) sd_bg.remove("/bg.bin"); 
            uploadFile = sd_bg.open("/bg.bin", O_WRITE | O_CREAT);
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
        } else if (upload.status == UPLOAD_FILE_END) {
            if (uploadFile) uploadFile.close(); 
        }
    });

    server.begin();

    while (appState.currentMenu == MENU_UPLOAD_BG) {
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    bgUploadTaskHandle = NULL;
    vTaskDelete(NULL);
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
            if (bgUploadTaskHandle == NULL) {
                xTaskCreatePinnedToCore(uploadBgTask, "BgUpload", STACK_NETWORK, NULL, PRIO_NETWORK, &bgUploadTaskHandle, 1);
            }
        }
        return; 
    }
    
    if (level == MENU_MAIN) encoder.setBoundaries(0, 5, true);         
    else if (level == MENU_CONTROL) encoder.setBoundaries(0, 4, true); 
    else if (level == MENU_LAMP) encoder.setBoundaries(0, 4, true);    
    else if (level == MENU_USB_MODE) {
        // CÚ CHỐT: Mỗi khi vào SD Explorer là tự động load lại toàn bộ danh sách file
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