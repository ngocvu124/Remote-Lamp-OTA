#include "WebSv.h"
#include "Config.h"
#include "Display.h"
#include "Storage.h"
#include "System.h"
#include "EspNow.h"
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

static String jsonEscape(const char* s) {
    String out;
    if (!s) return out;
    out.reserve(strlen(s) + 8);
    for (const char* p = s; *p; ++p) {
        const char c = *p;
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((uint8_t)c < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04X", (unsigned)c);
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

static void logSdDiag(const char* tag, const char* path, int attempt) {
    uint8_t errCode = 0xFF;
    uint8_t errData = 0xFF;
    if (sd_bg.card()) {
        errCode = sd_bg.card()->errorCode();
        errData = sd_bg.card()->errorData();
    }
    Serial.printf(
        "[WEB][SD] %s path=%s attempt=%d storageReady=%d card=%d err=0x%02X data=0x%02X heap=%lu\n",
        tag,
        path ? path : "-",
        attempt,
        storage.isReady ? 1 : 0,
        sd_bg.card() ? 1 : 0,
        errCode,
        errData,
        (unsigned long)ESP.getFreeHeap()
    );
}


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

void WebServerLogic::begin(WebServer* server) {

        // API trả về dung lượng SD
        server->on("/storage_info", HTTP_GET, [server]() {
            if (!storage.isReady || !sd_bg.card()) {
                server->send(200, "application/json", "{\"total\":0,\"used\":0,\"free\":0}");
                return;
            }
            uint64_t total = 0, used = 0;
            total = sd_bg.card()->sectorCount() * 512ULL;
            FsVolume* vol = sd_bg.vol();
            if (vol) {
                uint32_t clusters = vol->clusterCount();
                uint32_t freeClusters = vol->freeClusterCount();
                uint32_t sectorsPerCluster = vol->sectorsPerCluster();
                used = (clusters - freeClusters) * sectorsPerCluster * 512ULL;
            }
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"total\":%llu,\"used\":%llu,\"free\":%llu}", total, used, total > used ? total - used : 0);
            server->send(200, "application/json", buf);
        });
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
    webServer.begin(server);
    server->begin();
    static FsFile uploadFile; 

    // Serve static style.css
    server->on("/style.css", HTTP_GET, [server]() {
        File file = LittleFS.open("/style.css", "r");
        if (!file) { server->send(404, "text/plain", "Missing style.css"); return; }
        server->streamFile(file, "text/css");
        file.close();
    });
    // Serve static main.js
    server->on("/main.js", HTTP_GET, [server]() {
        File file = LittleFS.open("/main.js", "r");
        if (!file) { server->send(404, "text/plain", "Missing main.js"); return; }
        server->streamFile(file, "application/javascript");
        file.close();
    });
    if (mode == WEB_MODE_UPLOAD) {
        // Serve static style.css
        server->on("/style.css", HTTP_GET, [server]() {
            File file = LittleFS.open("/style.css", "r");
            if (!file) { server->send(404, "text/plain", "Missing style.css"); return; }
            server->streamFile(file, "text/css");
            file.close();
        });
        // Serve static main.js
        server->on("/main.js", HTTP_GET, [server]() {
            File file = LittleFS.open("/main.js", "r");
            if (!file) { server->send(404, "text/plain", "Missing main.js"); return; }
            server->streamFile(file, "application/javascript");
            file.close();
        });
    
        server->on("/", HTTP_GET, [server]() {
            File file = LittleFS.open("/home.html", "r");
            if (!file) { server->send(500, "text/plain", "Missing home.html"); return; }
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
            String path = server->hasArg("dir") ? server->arg("dir") : "/";
            String json = "[]";
            bool listed = false;
            uint32_t startMs = millis();

            Serial.printf("[WEB] /list request dir=%s\n", path.c_str());

            for (int attempt = 0; attempt < 3 && !listed; ++attempt) {
                if (!xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(1500))) {
                    logSdDiag("/list semaphore busy", path.c_str(), attempt + 1);
                    vTaskDelay(pdMS_TO_TICKS(40));
                    continue;
                }

                digitalWrite(SCR_CS_PIN, HIGH);
                FsFile dir = sd_bg.open(path.c_str(), O_RDONLY);
                if (!dir) {
                    logSdDiag("/list open failed before remount", path.c_str(), attempt + 1);
                    const bool remountOk = sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
                    Serial.printf("[WEB][SD] /list remount result=%d\n", remountOk ? 1 : 0);
                    dir = sd_bg.open(path.c_str(), O_RDONLY);
                }

                if (dir && dir.isDirectory()) {
                    json = "[";
                    json.reserve(2048); // Tối ưu: Cấp phát trước bộ nhớ để chống phân mảnh Heap
                    FsFile file;
                    bool first = true;
                    uint16_t itemCount = 0;
                    dir.rewindDirectory();
                    while (file.openNext(&dir, O_RDONLY)) {
                        char name[64];
                        file.getName(name, sizeof(name));
                        if (String(name) != "System Volume Information") {
                            const String safeName = jsonEscape(name);
                            if (!first) json += ",";
                            json += "{\"name\":\"" + safeName + "\",";
                            json += "\"isDir\":" + String(file.isDirectory() ? "true" : "false") + ",";
                            json += "\"size\":" + String(file.size()) + "}";
                            first = false;
                            itemCount++;
                        }
                        file.close();
                    }
                    dir.close();
                    json += "]";
                    listed = true;
                    Serial.printf("[WEB] /list ok dir=%s items=%u took=%lums\n",
                                  path.c_str(),
                                  (unsigned)itemCount,
                                  (unsigned long)(millis() - startMs));
                } else {
                    logSdDiag("/list open dir failed", path.c_str(), attempt + 1);
                    if (dir) dir.close();
                }

                xSemaphoreGiveRecursive(xGuiSemaphore);
                if (!listed) vTaskDelay(pdMS_TO_TICKS(40));
            }

            if (listed) {
                server->send(200, "application/json", json);
            } else {
                logSdDiag("/list failed after retries", path.c_str(), 3);
                server->send(503, "text/plain", "SD read failed");
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
        server->handleClient();
        vTaskDelay(pdMS_TO_TICKS(20));
        if (mode == WEB_MODE_UPLOAD && appState.currentMenu != MENU_WEB_SERVER) webServer.isRunning = false;
    }
    server->stop();
    delete server; 

    if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(1000))) {
        display.closeProgressPopup();
        xSemaphoreGiveRecursive(xGuiSemaphore);
    }

    vTaskDelete(NULL); 
}

bool WebServerLogic::runWiFiSetup() {
    if (WiFi.status() == WL_CONNECTED) return true;

    // Pull latest credentials from lamp (if available) before attempting OTA/Web Wi-Fi.
    espNow.requestWifiSync(2, 500);

    char ssid[33] = {0};
    char pass[65] = {0};
    if (!espNow.getSyncedWifiCreds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
            display.showProgressPopup("WIFI", "No synced WiFi.\nSetup WiFi on lamp\nthen retry.", 0);
            xSemaphoreGiveRecursive(xGuiSemaphore);
        }
        return false;
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        attempts++;
        if (appState.currentMenu != MENU_OTA && appState.currentMenu != MENU_WEB_SERVER) return false;
    }
    if (WiFi.status() == WL_CONNECTED) {
        strncpy(cachedSSID, ssid, sizeof(cachedSSID) - 1);
        cachedSSID[sizeof(cachedSSID) - 1] = '\0';
        return true;
    }

    if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        display.showProgressPopup("WIFI", "Cannot connect with\nsynced WiFi from lamp.", 0);
        xSemaphoreGiveRecursive(xGuiSemaphore);
    }
    return false;
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
    xTaskCreatePinnedToCore(webTask, "WebTask", STACK_TASK_WEB, (void*)WEB_MODE_UPLOAD, PRIO_WEB, NULL, 1);
}