#include "WebSv.h"
#include "Config.h"
#include "Display.h"
#include "Storage.h"
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <SdFat.h>

WebServerLogic webServer;
extern SemaphoreHandle_t xGuiSemaphore;
extern SdFs sd_bg; 

void WebServerLogic::begin() {
    if (!LittleFS.begin(true)) {
        Serial.println("[WEB] LittleFS Mount Failed! (Formatting...)");
    } else {
        Serial.println("[WEB] LittleFS Mount OK!");
    }
    isRunning = false;
}

static void webTask(void* pvParameters) {
    WebServerMode mode = (WebServerMode)(intptr_t)pvParameters;
    
    WebServer* server = new WebServer(80);
    static FsFile uploadFile; 
    
    if (mode == WEB_MODE_WIFI) {
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
        
        server->on("/", [server, options]() {
            File file = LittleFS.open("/wifi.html", "r");
            if (!file) {
                server->send(500, "text/plain", "Error: File wifi.html missing in LittleFS!");
                return;
            }
            String html = file.readString();
            file.close();
            html.replace("{{OPTIONS}}", options);
            server->send(200, "text/html", html);
        });
        
        server->on("/save", [server]() {
            String qsid = server->arg("ssid");
            String qpass = server->arg("pass");
            server->send(200, "text/html", "<html><body style='text-align:center;margin-top:50px;font-family:sans-serif;background:#222;color:#fff;'><h2>Da luu! Dang ket noi...</h2><p>Ban co the dong trang web nay.</p></body></html>");
            
            vTaskDelay(pdMS_TO_TICKS(500));
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
            WiFi.begin(qsid.c_str(), qpass.c_str());
            webServer.isRunning = false; 
        });
    } 
    else if (mode == WEB_MODE_UPLOAD) {
        // ĐÃ XÓA LỆNH sd_bg.begin(...) Ở ĐÂY ĐỂ TRÁNH CRASH THẺ NHỚ
        
        server->on("/", HTTP_GET, [server]() {
            File file = LittleFS.open("/upload.html", "r");
            if (!file) {
                server->send(500, "text/plain", "Error: File upload.html missing in LittleFS!");
                return;
            }
            server->streamFile(file, "text/html"); 
            file.close();
        });

        server->on("/upload", HTTP_POST, [server]() {
            server->send(200, "text/plain", "OK");
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP.restart(); 
        }, [server]() {
            HTTPUpload& upload = server->upload();
            if (upload.status == UPLOAD_FILE_START) {
                // Thêm Semaphore chặn màn hình để thẻ nhớ ghi file
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(500))) {
                    if (sd_bg.exists("/bg.bin")) sd_bg.remove("/bg.bin"); 
                    uploadFile = sd_bg.open("/bg.bin", O_WRITE | O_CREAT);
                    xSemaphoreGive(xGuiSemaphore);
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(500))) {
                    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
                    xSemaphoreGive(xGuiSemaphore);
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(500))) {
                    if (uploadFile) uploadFile.close(); 
                    xSemaphoreGive(xGuiSemaphore);
                }
            }
        });

        server->on("/download", HTTP_GET, [server]() {
            FsFile file;
            // Dùng Semaphore xin phép mở file để tránh tranh giành SPI với LVGL
            if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(500))) {
                file = sd_bg.open("/bg.bin", O_READ);
                if (!file) file = sd_bg.open("bg.bin", O_READ); // Fallback gọi không cần gạch chéo
                xSemaphoreGive(xGuiSemaphore);
            }

            if (file) {
                server->sendHeader("Content-Disposition", "attachment; filename=\"bg.bin\"");
                server->setContentLength(file.size());
                server->send(200, "application/octet-stream", "");

                uint8_t buffer[1024]; 
                int bytesRead;
                while (1) {
                    // Cắt nhỏ việc đọc file (1KB/lần), đọc xong nhả Semaphore ra cho màn hình vẽ, rồi lại xin tiếp
                    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(500))) {
                        bytesRead = file.read(buffer, sizeof(buffer));
                        xSemaphoreGive(xGuiSemaphore);
                    } else {
                        bytesRead = 0; // Kẹt SPI
                    }
                    
                    if (bytesRead <= 0) break;
                    server->client().write(buffer, bytesRead); // Gửi data xuống WiFi không cần khóa SPI
                }
                
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(500))) {
                    file.close();
                    xSemaphoreGive(xGuiSemaphore);
                }
            } else {
                server->send(404, "text/plain", "Khong tim thay bg.bin tren the nho!");
            }
        });
    }
    
    server->begin();
    
    while(webServer.isRunning) {
        server->handleClient();
        vTaskDelay(pdMS_TO_TICKS(20));
        
        if (mode == WEB_MODE_WIFI && appState.currentMenu != MENU_STOCK && appState.currentMenu != MENU_OTA) {
            webServer.isRunning = false;
        }
        if (mode == WEB_MODE_UPLOAD && appState.currentMenu != MENU_UPLOAD_BG) {
            webServer.isRunning = false;
        }
    }
    
    server->stop();
    delete server; 
    
    if (mode == WEB_MODE_WIFI && WiFi.status() != WL_CONNECTED) {
        WiFi.softAPdisconnect(true);
        WiFi.disconnect();
    }
    
    vTaskDelete(NULL); 
}

bool WebServerLogic::runWiFiSetup() {
    if (WiFi.status() == WL_CONNECTED) return true;

    WiFi.mode(WIFI_STA);
    WiFi.begin(); 

    char* msg_buf = (char*)heap_caps_malloc(128, MALLOC_CAP_SPIRAM);
    if (!msg_buf) msg_buf = (char*)malloc(128);
    strcpy(msg_buf, "Connecting to saved WiFi...\nWait up to 10s...");
    
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        display.showProgressPopup("WIFI INIT", msg_buf, 0);
        xSemaphoreGive(xGuiSemaphore);
    } else heap_caps_free(msg_buf);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        attempts++;
        if (appState.currentMenu != MENU_STOCK && appState.currentMenu != MENU_OTA && appState.currentMenu != MENU_UPLOAD_BG) {
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
    } else heap_caps_free(msg_buf);

    isRunning = true;
    xTaskCreatePinnedToCore(webTask, "WebTask", STACK_WEB, (void*)WEB_MODE_WIFI, PRIO_WEB, NULL, 1);

    msg_buf = (char*)heap_caps_malloc(256, MALLOC_CAP_SPIRAM);
    if (!msg_buf) msg_buf = (char*)malloc(256);
    strcpy(msg_buf, "1. Ket noi WiFi: REMOTE_LAMP\n2. Mo trinh duyet web\n3. Truy cap: 192.168.4.1\nDe chon mang & nhap pass!");
    
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        display.showProgressPopup("WEB SETUP", msg_buf, 0);
        xSemaphoreGive(xGuiSemaphore);
    } else heap_caps_free(msg_buf);

    while (isRunning) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (appState.currentMenu != MENU_STOCK && appState.currentMenu != MENU_OTA && appState.currentMenu != MENU_UPLOAD_BG) {
        return false;
    }

    msg_buf = (char*)heap_caps_malloc(128, MALLOC_CAP_SPIRAM);
    if (!msg_buf) msg_buf = (char*)malloc(128);
    strcpy(msg_buf, "Received new WiFi credentials!\nConnecting...");
    
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        display.showProgressPopup("WIFI LINKING", msg_buf, 50);
        xSemaphoreGive(xGuiSemaphore);
    } else heap_caps_free(msg_buf);

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

void WebServerLogic::runBgUpload() {
    if (!runWiFiSetup()) {
        if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
            display.showProgressPopup("LOI KET NOI", "Khong the ket noi WiFi!\nVui long thu lai.", 0);
            xSemaphoreGive(xGuiSemaphore);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
            display.closeProgressPopup();
            xSemaphoreGive(xGuiSemaphore);
        }
        
        appState.currentMenu = MENU_CONTROL;
        appState.menuIndex = 3; 
        if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(50))) {
            display.forceRebuild();
            display.updateUI(appState);
            xSemaphoreGive(xGuiSemaphore);
        }
        return;
    }

    String ip = WiFi.localIP().toString();
    char* msg_buf = (char*)heap_caps_malloc(256, MALLOC_CAP_SPIRAM);
    if (!msg_buf) msg_buf = (char*)malloc(256);
    
    sprintf(msg_buf, "1. Up anh web:\n%s\n2. Tai file:\n%s/download", ip.c_str(), ip.c_str());
    
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        display.showProgressPopup("UPLOAD BG", msg_buf, 0);
        xSemaphoreGive(xGuiSemaphore);
    } else heap_caps_free(msg_buf);

    isRunning = true;
    xTaskCreatePinnedToCore(webTask, "WebTask", STACK_WEB, (void*)WEB_MODE_UPLOAD, PRIO_WEB, NULL, 1);
}