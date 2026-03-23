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
        Serial.println("[WEB] LittleFS Mount Failed!");
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
        for (int i = 0; i < n; ++i) {
            String ssid = WiFi.SSID(i);
            if(ssid.length() > 0) options += "<option value='" + ssid + "'>" + ssid + "</option>";
        }
        WiFi.scanDelete();
        WiFi.softAP("REMOTE_LAMP");
        
        server->on("/", [server, options]() {
            File file = LittleFS.open("/wifi.html", "r");
            if (!file) { server->send(500, "text/plain", "Missing wifi.html"); return; }
            String html = file.readString();
            file.close();
            html.replace("{{OPTIONS}}", options);
            server->send(200, "text/html", html);
        });
        
        server->on("/save", [server]() {
            String qsid = server->arg("ssid");
            String qpass = server->arg("pass");
            server->send(200, "text/html", "<h2>Da luu! Dang ket noi...</h2>");
            vTaskDelay(pdMS_TO_TICKS(500));
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
            WiFi.begin(qsid.c_str(), qpass.c_str());
            webServer.isRunning = false; 
        });
    } 
    else if (mode == WEB_MODE_UPLOAD) {
        server->on("/", HTTP_GET, [server]() {
            File file = LittleFS.open("/upload.html", "r");
            if (!file) { server->send(500, "text/plain", "Missing upload.html"); return; }
            // streamFile hoạt động bình thường với LittleFS
            server->streamFile(file, "text/html"); 
            file.close();
        });

        server->on("/upload", HTTP_POST, [server]() {
            server->send(200, "text/plain", "OK");
            Serial.println("[WEB] Upload Finished. Rebooting...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP.restart(); 
        }, [server]() {
            HTTPUpload& upload = server->upload();
            if (upload.status == UPLOAD_FILE_START) {
                Serial.printf("[WEB] Starting upload: %s\n", upload.filename.c_str());
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                    if (uploadFile) uploadFile.close(); 
                    uploadFile = sd_bg.open("/bg.bin", O_WRITE | O_CREAT | O_TRUNC);
                    if (uploadFile) Serial.println("[WEB] File /bg.bin opened for overwriting.");
                    else Serial.println("[WEB] ERROR: Could not open file for writing!");
                    xSemaphoreGive(xGuiSemaphore);
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                    if (uploadFile) {
                        size_t written = uploadFile.write(upload.buf, upload.currentSize);
                        if (written != upload.currentSize) Serial.println("[WEB] WRITE ERROR!");
                    }
                    xSemaphoreGive(xGuiSemaphore);
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                    if (uploadFile) {
                        uploadFile.close(); 
                        Serial.printf("[WEB] Uploaded success: %u bytes\n", upload.totalSize);
                    }
                    xSemaphoreGive(xGuiSemaphore);
                }
            }
        });

        server->on("/download", HTTP_GET, [server]() {
            if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                FsFile file = sd_bg.open("/bg.bin", O_READ);
                if (file) {
                    server->sendHeader("Content-Disposition", "attachment; filename=\"bg.bin\"");
                    server->sendHeader("Connection", "close");
                    
                    // CÚ CHỐT: Gắn chặt Content-Length để dập tắt chế độ chunked rác của ESP32
                    server->setContentLength(file.size());
                    
                    // Gửi đi header
                    server->send(200, "application/octet-stream", ""); 

                    // Tự tay bơm từng byte từ SdFat ra trình duyệt
                    uint8_t buffer[1024];
                    int bytesRead;
                    while (1) {
                        bytesRead = file.read(buffer, sizeof(buffer));
                        if (bytesRead <= 0) break;
                        server->client().write(buffer, bytesRead);
                    }
                    file.close();
                } else {
                    server->send(404, "text/plain", "File missing");
                }
                xSemaphoreGive(xGuiSemaphore);
            } else {
                server->send(500, "text/plain", "SD busy");
            }
        });
    }
    
    server->begin();
    while(webServer.isRunning) {
        server->handleClient();
        vTaskDelay(pdMS_TO_TICKS(20));
        if (mode == WEB_MODE_WIFI && appState.currentMenu != MENU_STOCK && appState.currentMenu != MENU_OTA) webServer.isRunning = false;
        if (mode == WEB_MODE_UPLOAD && appState.currentMenu != MENU_UPLOAD_BG) webServer.isRunning = false;
    }
    server->stop();
    delete server; 
    vTaskDelete(NULL); 
}

bool WebServerLogic::runWiFiSetup() {
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.mode(WIFI_STA);
    WiFi.begin(); 
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        attempts++;
        if (appState.currentMenu != MENU_STOCK && appState.currentMenu != MENU_OTA && appState.currentMenu != MENU_UPLOAD_BG) return false;
    }
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.mode(WIFI_AP_STA);
    isRunning = true;
    xTaskCreatePinnedToCore(webTask, "WebTask", STACK_WEB, (void*)WEB_MODE_WIFI, PRIO_WEB, NULL, 1);
    while (isRunning) vTaskDelay(pdMS_TO_TICKS(100));
    return WiFi.status() == WL_CONNECTED;
}

void WebServerLogic::runBgUpload() {
    if (!runWiFiSetup()) return;
    String ip = WiFi.localIP().toString();
    char* msg_buf = (char*)heap_caps_malloc(256, MALLOC_CAP_SPIRAM);
    sprintf(msg_buf, "1. Up anh web:\n%s\n2. Tai file:\n%s/download", ip.c_str(), ip.c_str());
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        display.showProgressPopup("UPLOAD BG", msg_buf, 0);
        xSemaphoreGive(xGuiSemaphore);
    }
    isRunning = true;
    xTaskCreatePinnedToCore(webTask, "WebTask", STACK_WEB, (void*)WEB_MODE_UPLOAD, PRIO_WEB, NULL, 1);
}