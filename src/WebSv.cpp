#include "WebSv.h"
#include "Config.h"
#include "Display.h"
#include "Storage.h"
#include <WiFi.h>
#include <esp_heap_caps.h>

// Xử lý triệt để cảnh báo xung đột Macro giữa SdFat và LittleFS
#include <SdFat.h>
#undef FILE_READ
#undef FILE_WRITE
#include <LittleFS.h>
#include <WebServer.h>

WebServerLogic webServer;
extern SemaphoreHandle_t xGuiSemaphore;
extern SdFs sd_bg; 

// --- HÀM XÓA ĐỆ QUY THỦ CÔNG CHỐNG LỖI rmRfStar ---
static bool deleteRecursive(String path) {
    FsFile target = sd_bg.open(path.c_str(), O_READ);
    if (!target) return false;
    
    if (!target.isDirectory()) {
        target.close();
        return sd_bg.remove(path.c_str()); // Nếu là file thì xóa luôn
    }

    target.rewindDirectory();
    FsFile child;
    while (child.openNext(&target, O_READ)) {
        char name[64];
        child.getName(name, sizeof(name));
        child.close();
        
        String childPath = path;
        if (!childPath.endsWith("/")) childPath += "/";
        childPath += name;
        
        deleteRecursive(childPath); // Xóa đệ quy file/thư mục con
        target.rewindDirectory();   // Cập nhật lại con trỏ do thư mục vừa bị thay đổi
    }
    target.close();
    return sd_bg.rmdir(path.c_str()); // Cuối cùng xóa thư mục đã rỗng
}
// ---------------------------------------------------

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
            server->streamFile(file, "text/html"); 
            file.close();
        });

        server->on("/upload", HTTP_POST, [server]() {
            server->send(200, "text/plain", "OK");
            Serial.println("[WEB] Upload Finished.");
        }, [server]() {
            HTTPUpload& upload = server->upload();
            if (upload.status == UPLOAD_FILE_START) {
                if (!sd_bg.exists("/background")) sd_bg.mkdir("/background");

                String filename = upload.filename;
                if (!filename.startsWith("/")) filename = "/" + filename;
                String fullPath = "/background" + filename;

                Serial.printf("[WEB] Starting bg upload: %s\n", fullPath.c_str());
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                    if (uploadFile) uploadFile.close(); 
                    if (sd_bg.exists(fullPath.c_str())) sd_bg.remove(fullPath.c_str());
                    uploadFile = sd_bg.open(fullPath.c_str(), O_WRITE | O_CREAT | O_TRUNC);
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
                FsFile file = sd_bg.open(appState.bgFilePath, O_READ);
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

        server->on("/files", HTTP_GET, [server]() {
            File file = LittleFS.open("/files.html", "r");
            if (!file) { 
                server->send(500, "text/plain", "Missing files.html"); 
                return; 
            }
            server->streamFile(file, "text/html"); 
            file.close();
        });

        server->on("/list", HTTP_GET, [server]() {
            if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                String path = server->hasArg("dir") ? server->arg("dir") : "/";
                String json = "[";
                FsFile dir = sd_bg.open(path.c_str(), O_READ);
                if (dir && dir.isDirectory()) {
                    FsFile file;
                    bool first = true;
                    dir.rewindDirectory();
                    while (file.openNext(&dir, O_READ)) {
                        char name[64];
                        file.getName(name, sizeof(name));
                        if (String(name) != "System Volume Information") {
                            if (!first) json += ",";
                            json += "{\"name\":\"" + String(name) + "\",";
                            json += "\"isDir\":" + String(file.isDirectory() ? "true" : "false") + ",";
                            json += "\"size\":" + String(file.size()) + "}";
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

        server->on("/mkdir", HTTP_POST, [server]() {
            if (!server->hasArg("path")) { server->send(400, "text/plain", "Missing path"); return; }
            String path = server->arg("path");
            if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                if (sd_bg.mkdir(path.c_str())) {
                    server->send(200, "text/plain", "OK");
                } else {
                    server->send(500, "text/plain", "Failed");
                }
                xSemaphoreGive(xGuiSemaphore);
            } else {
                server->send(500, "text/plain", "SD busy");
            }
        });

        // NÂNG CẤP TÍCH HỢP HÀM XÓA ĐỆ QUY VỪA TẠO
        server->on("/delete", HTTP_POST, [server]() {
            String path = server->hasArg("path") ? server->arg("path") : server->arg("filename");
            if (!path.startsWith("/")) path = "/" + path;
            
            // Cho phép lấy Semaphore lâu hơn một chút vì quá trình xóa đệ quy tốn thời gian
            if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(3000))) {
                if (deleteRecursive(path)) {
                    server->send(200, "text/plain", "OK");
                } else {
                    server->send(500, "text/plain", "Delete Failed");
                }
                xSemaphoreGive(xGuiSemaphore);
            } else {
                server->send(500, "text/plain", "SD Busy");
            }
        });

        server->on("/download_file", HTTP_GET, [server]() {
            String filename = server->arg("filename");
            if (!filename.startsWith("/")) filename = "/" + filename;
            
            if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                FsFile file = sd_bg.open(filename.c_str(), O_READ);
                if (file && !file.isDirectory()) { 
                    String dlName = filename;
                    int lastSlash = dlName.lastIndexOf('/');
                    if (lastSlash >= 0) dlName = dlName.substring(lastSlash + 1);
                    
                    server->sendHeader("Content-Disposition", "attachment; filename=\"" + dlName + "\"");
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
                    server->send(404, "text/plain", "File missing or is directory");
                }
                xSemaphoreGive(xGuiSemaphore);
            } else {
                server->send(500, "text/plain", "SD busy");
            }
        });

        server->on("/upload_file", HTTP_POST, [server]() {
            server->send(200, "text/plain", "OK"); 
        }, [server]() {
            HTTPUpload& upload = server->upload();
            if (upload.status == UPLOAD_FILE_START) {
                String dir = server->hasArg("dir") ? server->arg("dir") : "/";
                if (!dir.endsWith("/")) dir += "/";
                
                String filename = upload.filename;
                if (filename.startsWith("/")) filename = filename.substring(1);
                String fullPath = dir + filename;
                
                Serial.printf("[WEB] Starting General Upload: %s\n", fullPath.c_str());
                if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                    if (uploadFile) uploadFile.close(); 
                    if (sd_bg.exists(fullPath.c_str())) sd_bg.remove(fullPath.c_str()); 
                    uploadFile = sd_bg.open(fullPath.c_str(), O_WRITE | O_CREAT | O_TRUNC);
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
        if (mode == WEB_MODE_WIFI && appState.currentMenu != MENU_STOCK && appState.currentMenu != MENU_OTA && appState.currentMenu != MENU_WEB_SERVER) webServer.isRunning = false;
        if (mode == WEB_MODE_UPLOAD && appState.currentMenu != MENU_WEB_SERVER) webServer.isRunning = false;
    }
    server->stop();
    delete server; 

    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
        display.closeProgressPopup();
        xSemaphoreGive(xGuiSemaphore);
    }

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
        if (appState.currentMenu != MENU_STOCK && appState.currentMenu != MENU_OTA && appState.currentMenu != MENU_WEB_SERVER) return false;
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
    sprintf(msg_buf, "1. Up BG: %s\n2. Q.Ly File: %s/files", ip.c_str(), ip.c_str());
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        display.showProgressPopup("WEB SERVER", msg_buf, 0);
        xSemaphoreGive(xGuiSemaphore);
    }
    isRunning = true;
    xTaskCreatePinnedToCore(webTask, "WebTask", STACK_WEB, (void*)WEB_MODE_UPLOAD, PRIO_WEB, NULL, 1);
}