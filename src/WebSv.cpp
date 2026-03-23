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
        
        // --- CÁC ROUTE CŨ: UPLOAD HÌNH NỀN ---
        server->on("/", HTTP_GET, [server]() {
            File file = LittleFS.open("/upload.html", "r");
            if (!file) { server->send(500, "text/plain", "Missing upload.html"); return; }
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
                Serial.printf("[WEB] Starting bg upload: %s\n", upload.filename.c_str());
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                    if (uploadFile) uploadFile.close(); 
                    if (sd_bg.exists("/bg.bin")) sd_bg.remove("/bg.bin");
                    uploadFile = sd_bg.open("/bg.bin", O_WRITE | O_CREAT | O_TRUNC);
                    if (uploadFile) Serial.println("[WEB] File /bg.bin opened for overwriting.");
                    xSemaphoreGive(xGuiSemaphore);
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
                    xSemaphoreGive(xGuiSemaphore);
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                    if (uploadFile) uploadFile.close(); 
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
                    server->setContentLength(file.size());
                    server->send(200, "application/octet-stream", ""); 
                    uint8_t buffer[1024];
                    int bytesRead;
                    while (1) {
                        bytesRead = file.read(buffer, sizeof(buffer));
                        if (bytesRead <= 0) break;
                        server->client().write(buffer, bytesRead);
                    }
                    file.close();
                } else server->send(404, "text/plain", "File missing");
                xSemaphoreGive(xGuiSemaphore);
            } else server->send(500, "text/plain", "SD busy");
        });


        // ==========================================================
        // --- TÍNH NĂNG MỚI: QUẢN LÝ FILE TRÊN TRÌNH DUYỆT ---
        // ==========================================================

        // 1. Trả về giao diện Web HTML từ file trong LittleFS
        server->on("/files", HTTP_GET, [server]() {
            File file = LittleFS.open("/files.html", "r");
            if (!file) { 
                server->send(500, "text/plain", "Missing files.html"); 
                return; 
            }
            server->streamFile(file, "text/html"); 
            file.close();
        });

        // 2. Trả về danh sách file định dạng JSON
        server->on("/list", HTTP_GET, [server]() {
            if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                String json = "[";
                FsFile dir = sd_bg.open("/");
                if (dir) {
                    FsFile file;
                    bool first = true;
                    dir.rewind();
                    while (file.openNext(&dir, O_READ)) {
                        char name[64];
                        file.getName(name, sizeof(name));
                        // Ẩn thư mục System Volume Information rác của Windows nếu có
                        if (String(name) != "System Volume Information") {
                            if (!first) json += ",";
                            json += "{\"name\":\"" + String(name) + "\",\"size\":" + String(file.size()) + "}";
                            first = false;
                        }
                        file.close();
                    }
                    dir.close();
                }
                json += "]";
                server->send(200, "application/json", json);
                xSemaphoreGive(xGuiSemaphore);
            } else {
                server->send(500, "text/plain", "SD busy");
            }
        });

        // 3. API Xóa File
        server->on("/delete", HTTP_POST, [server]() {
            String filename = server->arg("filename");
            if (!filename.startsWith("/")) filename = "/" + filename;
            
            if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                if (sd_bg.exists(filename.c_str())) {
                    if (sd_bg.remove(filename.c_str())) {
                        server->send(200, "text/plain", "OK");
                    } else {
                        server->send(500, "text/plain", "Delete Failed");
                    }
                } else {
                    server->send(404, "text/plain", "File Not Found");
                }
                xSemaphoreGive(xGuiSemaphore);
            } else {
                server->send(500, "text/plain", "SD Busy");
            }
        });

        // 4. API Tải file vè PC theo tên (Hỗ trợ chống lệch byte bằng setContentLength)
        server->on("/download_file", HTTP_GET, [server]() {
            String filename = server->arg("filename");
            if (!filename.startsWith("/")) filename = "/" + filename;
            
            if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                FsFile file = sd_bg.open(filename.c_str(), O_READ);
                if (file) {
                    server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename.substring(1) + "\"");
                    server->sendHeader("Connection", "close");
                    server->setContentLength(file.size());
                    server->send(200, "application/octet-stream", ""); 
                    
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

        // 5. API Upload File động (Bất kỳ tên gì, bất kỳ loại file nào)
        server->on("/upload_file", HTTP_POST, [server]() {
            server->send(200, "text/plain", "OK"); // Hoàn tất thì báo OK (Không Reboot)
        }, [server]() {
            HTTPUpload& upload = server->upload();
            if (upload.status == UPLOAD_FILE_START) {
                String filename = upload.filename;
                if (!filename.startsWith("/")) filename = "/" + filename;
                
                Serial.printf("[WEB] Starting General Upload: %s\n", filename.c_str());
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                    if (uploadFile) uploadFile.close(); 
                    if (sd_bg.exists(filename.c_str())) sd_bg.remove(filename.c_str()); // Dọn dẹp thẻ nhớ
                    uploadFile = sd_bg.open(filename.c_str(), O_WRITE | O_CREAT | O_TRUNC);
                    xSemaphoreGive(xGuiSemaphore);
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
                    xSemaphoreGive(xGuiSemaphore);
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                    if (uploadFile) {
                        uploadFile.close(); 
                        Serial.printf("[WEB] General File Uploaded Success: %u bytes\n", upload.totalSize);
                    }
                    xSemaphoreGive(xGuiSemaphore);
                }
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
    
    // Cập nhật câu thông báo UI trên màn hình cho bá cháy
    char* msg_buf = (char*)heap_caps_malloc(256, MALLOC_CAP_SPIRAM);
    sprintf(msg_buf, "1. Up BG: %s\n2. Q.Ly File: %s/files", ip.c_str(), ip.c_str());
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        display.showProgressPopup("WEB SERVER", msg_buf, 0);
        xSemaphoreGive(xGuiSemaphore);
    }
    isRunning = true;
    xTaskCreatePinnedToCore(webTask, "WebTask", STACK_WEB, (void*)WEB_MODE_UPLOAD, PRIO_WEB, NULL, 1);
}