#include "WebSv.h"
#include "Config.h"
#include "Display.h"
#include "Storage.h"
#include "System.h"
#include <WiFi.h>
#include <DNSServer.h>
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

static String g_wifiOptions = ""; // Pre-scanned options truyền từ runWiFiPortal vào webTask


// --- HÀM XÓA ĐỆ QUY CHỐNG LẶP VÔ HẠN VÀ TRÀN STACK ---
static bool deleteRecursive(const String& path) {
    digitalWrite(SCR_CS_PIN, HIGH); // Ép tắt màn hình trước khi làm việc
    FsFile target = sd_bg.open(path.c_str(), O_RDONLY);
    if (!target) {
        sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
        target = sd_bg.open(path.c_str(), O_RDONLY);
    }
    if (!target) {
        // Thử xóa mù nếu không thể mở (file bị kẹt bộ nhớ đệm hoặc lỗi FAT)
        if (sd_bg.remove(path.c_str())) return true;
        return false;
    }
    
    if (!target.isDirectory()) {
        target.close();
        if (sd_bg.remove(path.c_str())) return true;
        // Nếu xóa xịt, ép khởi động lại thẻ SD và thử lại
        sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
        return sd_bg.remove(path.c_str());
    }

    target.rewindDirectory();
    FsFile child;
    bool success = true;

    // Quét từng file trong thư mục
    while (child.openNext(&target, O_RDONLY)) {
        char name[128];
        child.getName(name, sizeof(name));
        bool isDir = child.isDirectory();
        child.close();
        
        String childPath = path;
        if (!childPath.endsWith("/")) childPath += "/";
        childPath += name;
        
        if (isDir) {
            // Đệ quy vào thư mục con, nếu lỗi thì ngắt vòng lặp ngay
            if (!deleteRecursive(childPath)) {
                success = false;
                break;
            }
        } else {
            // Xóa file, nếu lỗi (file hỏng/khóa) thì ngắt vòng lặp để chống treo
            if (!sd_bg.remove(childPath.c_str())) {
                sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
                if (!sd_bg.remove(childPath.c_str())) {
                    success = false;
                    break;
                }
            }
        }
        target.rewindDirectory(); // Đã xóa 1 phần tử nên phải quay lại đầu
    }
    target.close();
    
    if (success) {
        if (sd_bg.rmdir(path.c_str())) return true;
        sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
        return sd_bg.rmdir(path.c_str());
    }
    return false;
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
    DNSServer* dnsServer = nullptr;
    static FsFile uploadFile; 
    
    if (mode == WEB_MODE_WIFI) {
        // Dùng options đã scan sẵn từ runWiFiPortal (AP đã được start rồi)
        String options = g_wifiOptions;
        g_wifiOptions = "";

        dnsServer = new DNSServer();
        dnsServer->start(53, "*", WiFi.softAPIP());

        server->on("/", [server, options]() {
            File file = LittleFS.open("/wifi.html", "r");
            if (!file) { server->send(500, "text/plain", "Missing wifi.html"); return; }
            String html = file.readString();
            file.close();
            html.replace("{{OPTIONS}}", options);
            server->send(200, "text/html", html);
        });

        // Captive-portal checks cho Android/iOS/Windows
        server->on("/generate_204", [server]() { server->sendHeader("Location", "http://192.168.4.1/", true); server->send(302, "text/plain", ""); });
        server->on("/gen_204", [server]() { server->sendHeader("Location", "http://192.168.4.1/", true); server->send(302, "text/plain", ""); });
        server->on("/hotspot-detect.html", [server]() { server->sendHeader("Location", "http://192.168.4.1/", true); server->send(302, "text/plain", ""); });
        server->on("/library/test/success.html", [server]() { server->sendHeader("Location", "http://192.168.4.1/", true); server->send(302, "text/plain", ""); });
        server->on("/ncsi.txt", [server]() { server->sendHeader("Location", "http://192.168.4.1/", true); server->send(302, "text/plain", ""); });
        server->on("/connecttest.txt", [server]() { server->sendHeader("Location", "http://192.168.4.1/", true); server->send(302, "text/plain", ""); });
        server->on("/redirect", [server]() { server->sendHeader("Location", "http://192.168.4.1/", true); server->send(302, "text/plain", ""); });
        
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

        server->onNotFound([server]() {
            server->sendHeader("Location", "http://192.168.4.1/", true);
            server->send(302, "text/plain", "");
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
                if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                    digitalWrite(SCR_CS_PIN, HIGH);
                    if (uploadFile) uploadFile.close(); 
                    if (sd_bg.exists(fullPath.c_str())) sd_bg.remove(fullPath.c_str());
                    uploadFile = sd_bg.open(fullPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
                    if (!uploadFile) {
                        sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
                        uploadFile = sd_bg.open(fullPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
                    }
                    xSemaphoreGiveRecursive(xGuiSemaphore);
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                    digitalWrite(SCR_CS_PIN, HIGH);
                    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
                    xSemaphoreGiveRecursive(xGuiSemaphore);
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                    digitalWrite(SCR_CS_PIN, HIGH);
                    if (uploadFile) { uploadFile.sync(); uploadFile.close(); }
                    xSemaphoreGiveRecursive(xGuiSemaphore);
                }
            }
        });

        server->on("/download", HTTP_GET, [server]() {
            if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                digitalWrite(SCR_CS_PIN, HIGH);
                FsFile file = sd_bg.open(appState.bgFilePath, O_RDONLY);
                if (!file) {
                    sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
                    file = sd_bg.open(appState.bgFilePath, O_RDONLY);
                }
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
                xSemaphoreGiveRecursive(xGuiSemaphore);
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
            if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                String path = server->hasArg("dir") ? server->arg("dir") : "/";
                String json = "[";
                json.reserve(2048); // Tối ưu: Cấp phát trước bộ nhớ để chống phân mảnh Heap
                digitalWrite(SCR_CS_PIN, HIGH);
                FsFile dir = sd_bg.open(path.c_str(), O_RDONLY);
                if (!dir) {
                    sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
                    dir = sd_bg.open(path.c_str(), O_RDONLY);
                }
                if (dir && dir.isDirectory()) {
                    FsFile file;
                    bool first = true;
                    dir.rewindDirectory();
                    while (file.openNext(&dir, O_RDONLY)) {
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
                xSemaphoreGiveRecursive(xGuiSemaphore);
            } else {
                server->send(500, "text/plain", "SD busy");
            }
        });

        server->on("/mkdir", HTTP_POST, [server]() {
            if (!server->hasArg("path")) { server->send(400, "text/plain", "Missing path"); return; }
            String path = server->arg("path");
            if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                digitalWrite(SCR_CS_PIN, HIGH);
                if (sd_bg.exists(path.c_str()) || sd_bg.mkdir(path.c_str())) {
                    server->send(200, "text/plain", "OK");
                } else {
                    sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
                    if (sd_bg.mkdir(path.c_str())) server->send(200, "text/plain", "OK");
                    else server->send(500, "text/plain", "Failed");
                }
                xSemaphoreGiveRecursive(xGuiSemaphore);
            } else {
                server->send(500, "text/plain", "SD busy");
            }
        });

        // BẢO VỆ XÓA ĐỆ QUY TỐI ĐA VỚI portMAX_DELAY ĐỂ CHỐNG VỠ GIAO DỊCH
        server->on("/delete", HTTP_POST, [server]() {
            String path;
            if (server->hasArg("path")) {
                path = server->arg("path");
            } else if (server->hasArg("dir") && server->hasArg("filename")) {
                path = server->arg("dir");
                if (!path.endsWith("/")) path += "/";
                path += server->arg("filename");
            } else {
                path = server->arg("filename");
            }
            if (!path.startsWith("/")) path = "/" + path;
            
            Serial.printf("[WEB] Deleting path: %s\n", path.c_str());
            if (xSemaphoreTakeRecursive(xGuiSemaphore, portMAX_DELAY)) {
                digitalWrite(SCR_CS_PIN, HIGH);
                // Đóng ngay file upload đang dở dang (nếu có) để nhả khóa file cho thẻ SD
                if (uploadFile) uploadFile.close(); 
                if (deleteRecursive(path)) {
                    server->send(200, "text/plain", "OK");
                } else {
                    server->send(500, "text/plain", "Delete Failed");
                }
                xSemaphoreGiveRecursive(xGuiSemaphore);
            } else {
                server->send(500, "text/plain", "SD Busy");
            }
        });

        server->on("/download_file", HTTP_GET, [server]() {
            String filename = server->arg("filename");
            if (!filename.startsWith("/")) filename = "/" + filename;
            
            if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                digitalWrite(SCR_CS_PIN, HIGH);
                FsFile file = sd_bg.open(filename.c_str(), O_RDONLY);
                if (!file) {
                    sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
                    file = sd_bg.open(filename.c_str(), O_RDONLY);
                }
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
                xSemaphoreGiveRecursive(xGuiSemaphore);
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
                if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                    digitalWrite(SCR_CS_PIN, HIGH);
                    if (uploadFile) uploadFile.close(); 
                    if (sd_bg.exists(fullPath.c_str())) sd_bg.remove(fullPath.c_str()); 
                    uploadFile = sd_bg.open(fullPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
                    if (!uploadFile) {
                        sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
                        uploadFile = sd_bg.open(fullPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
                    }
                    xSemaphoreGiveRecursive(xGuiSemaphore);
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                    digitalWrite(SCR_CS_PIN, HIGH);
                    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
                    xSemaphoreGiveRecursive(xGuiSemaphore);
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
                    digitalWrite(SCR_CS_PIN, HIGH);
                    if (uploadFile) {
                        uploadFile.sync();
                        uploadFile.close(); 
                        Serial.printf("[WEB] General File Uploaded Success: %u bytes\n", upload.totalSize);
                    }
                    xSemaphoreGiveRecursive(xGuiSemaphore);
                }
            }
        });
    }
    
    server->begin();
    while(webServer.isRunning) {
        if (mode == WEB_MODE_WIFI && dnsServer) dnsServer->processNextRequest();
        server->handleClient();
        vTaskDelay(pdMS_TO_TICKS(20));
        if (mode == WEB_MODE_WIFI && appState.currentMenu != MENU_OTA && appState.currentMenu != MENU_WEB_SERVER && appState.currentMenu != MENU_WIFI_SETUP) webServer.isRunning = false;
        if (mode == WEB_MODE_UPLOAD && appState.currentMenu != MENU_WEB_SERVER) webServer.isRunning = false;
    }
    server->stop();
    if (dnsServer) {
        dnsServer->stop();
        delete dnsServer;
    }
    delete server; 

    if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
        display.closeProgressPopup();
        xSemaphoreGiveRecursive(xGuiSemaphore);
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
        if (appState.currentMenu != MENU_OTA && appState.currentMenu != MENU_WEB_SERVER) return false;
    }
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.mode(WIFI_AP_STA);
    isRunning = true;
    // Bơm 16KB Stack cho WebTask thay vì chỉ 8KB như cũ
    xTaskCreatePinnedToCore(webTask, "WebTask", 16384, (void*)WEB_MODE_WIFI, PRIO_WEB, NULL, 1);
    while (isRunning) vTaskDelay(pdMS_TO_TICKS(100));
    strncpy(cachedSSID, WiFi.SSID().c_str(), sizeof(cachedSSID));
    return WiFi.status() == WL_CONNECTED;
}

void WebServerLogic::runWiFiPortal() {
    // Bước 1: Hiện trạng thái scanning
    if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        display.showProgressPopup("WIFI SETUP", "Scanning networks...\nPlease wait!", 0);
        xSemaphoreGiveRecursive(xGuiSemaphore);
    }

    // Bước 2: Scan mạng (blocking, chạy trên luồng hiện tại)
    WiFi.disconnect(false, true);
    WiFi.mode(WIFI_AP_STA);
    int n = WiFi.scanNetworks();

    // Bước 3: Build options cho web và danh sách hiển thị trên màn hình
    g_wifiOptions = "";
    char* displayText = (char*)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!displayText) displayText = (char*)malloc(1024);
    if (displayText) {
        int pos = 0;
        pos += snprintf(displayText + pos, 1024 - pos,
            "1. Ket noi WiFi:\n"
            "   SSID: REMOTE_LAMP\n"
            "2. Mo trinh duyet:\n"
            "   192.168.4.1\n"
            "---Mang da quet---\n");
        for (int i = 0; i < n && pos < 980; i++) {
            String ssid = WiFi.SSID(i);
            if (ssid.length() > 0) {
                g_wifiOptions += "<option value='" + ssid + "'>" + ssid + "</option>";
                pos += snprintf(displayText + pos, 1024 - pos, "%d. %s\n", i + 1, ssid.c_str());
            }
        }
        if (n == 0) snprintf(displayText + pos, 1024 - pos, "(Khong tim thay mang)");
    }
    WiFi.scanDelete();

    // Bước 4: Bật AP
    WiFi.softAP("REMOTE_LAMP");

    // Bước 5: Hiện danh sách mạng + hướng dẫn lên màn hình
    if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        display.closeProgressPopup();
        if (displayText) display.showFileContent("WIFI SETUP", displayText);
        xSemaphoreGiveRecursive(xGuiSemaphore);
    }
    if (displayText) free(displayText);

    // Bước 6: Khởi động web server trong task riêng
    isRunning = true;
    xTaskCreatePinnedToCore(webTask, "WebTask", 16384, (void*)WEB_MODE_WIFI, PRIO_WEB, NULL, 1);
}

void WebServerLogic::runBgUpload() {
    if (!runWiFiSetup()) return;
    String ip = WiFi.localIP().toString();

    char msg_buf[256];
    sprintf(msg_buf, "1. Up BG: %s\n2. Q.Ly File: %s/files", ip.c_str(), ip.c_str());
    if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        display.showProgressPopup("WEB SERVER", msg_buf, 0);
        xSemaphoreGiveRecursive(xGuiSemaphore);
    }
    isRunning = true;
    // Bơm 16KB Stack cho WebTask chống tràn bộ nhớ
    xTaskCreatePinnedToCore(webTask, "WebTask", 16384, (void*)WEB_MODE_UPLOAD, PRIO_WEB, NULL, 1);
}