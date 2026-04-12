#include "Ota.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "Display.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>

extern SemaphoreHandle_t xGuiSemaphore;
OtaLogic ota;
bool otaStarted = false;

// Link file danh sách phiên bản trên GitHub của bác
const char* VERSIONS_URL = "https://raw.githubusercontent.com/ngocvu124/Remote-Lamp-OTA/main/versions.json";

// Cấp phát PSRAM cho JSON để không làm tràn RAM hệ thống
struct SpiRamAllocatorOta {
    void* allocate(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
    void deallocate(void* pointer) { heap_caps_free(pointer); }
    void* reallocate(void* ptr, size_t new_size) { return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM); }
};
using SpiRamJsonDocumentOta = BasicJsonDocument<SpiRamAllocatorOta>;

bool OtaLogic::fetchVersions() {
    versionCount = 0;
    if (WiFi.status() != WL_CONNECTED) return false;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.begin(client, VERSIONS_URL);
    
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        SpiRamJsonDocumentOta doc(16384);
        DeserializationError error = deserializeJson(doc, http.getStream());
        if (!error) {
            JsonArray arr = doc.as<JsonArray>();
            // Quét tối đa 10 phiên bản mới nhất
            for (int i = 0; i < arr.size() && i < 10; i++) {
                strncpy(versions[i].name, arr[i]["name"] | "Unknown", 31);
                strncpy(versions[i].url, arr[i]["url"] | "", 127);
                versionCount++;
            }
            http.end();
            return true;
        } else {
            Serial.printf("[OTA] JSON Parse Error: %s\n", error.c_str());
        }
    } else {
        Serial.printf("[OTA] HTTP Error: %d\n", httpCode);
    }
    http.end();
    return false;
}

void OtaLogic::begin(const char* firmwareUrl) {
    if (otaStarted) return;
    otaStarted = true;

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[OTA] WiFi not connected!");
        otaStarted = false;
        return;
    }

    char msg[128];
    strcpy(msg, "Preparing Update...\nPlease wait!");
    
    if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        display.showProgressPopup("GITHUB OTA", msg, 0);
        xSemaphoreGiveRecursive(xGuiSemaphore);
    }

    WiFiClientSecure client;
    client.setInsecure(); 

    httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    
    httpUpdate.onProgress([](int cur, int total) {
        static uint32_t last_update = 0;
        if (millis() - last_update > 200 || cur == total) {
            last_update = millis();
            int percent = (cur * 100) / total;
            
            char msg_prog[128];
            sprintf(msg_prog, "Installing Firmware...\n\nPROGRESS: %d%%\n\nDo not power off!", percent);
            
            if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(50))) {
                display.showProgressPopup("UPDATING", msg_prog, percent);
                xSemaphoreGiveRecursive(xGuiSemaphore);
            }
        }
    });

    Serial.printf("[OTA] Updating from: %s\n", firmwareUrl);
    t_httpUpdate_return ret = httpUpdate.update(client, firmwareUrl);

    switch (ret) {
        case HTTP_UPDATE_FAILED: {
            Serial.printf("HTTP_UPDATE_FAILED Error (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
            char err_msg[128];
            sprintf(err_msg, "Update Failed!\nError: %s\n\nPlease reset and try again.", httpUpdate.getLastErrorString().c_str());
            
            if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
                display.showProgressPopup("OTA ERROR", err_msg, 0);
                xSemaphoreGiveRecursive(xGuiSemaphore);
            }
            break;
        }
        case HTTP_UPDATE_NO_UPDATES:
            Serial.println("HTTP_UPDATE_NO_UPDATES");
            break;
        case HTTP_UPDATE_OK:
            Serial.println("HTTP_UPDATE_OK");
            break;
    }
    otaStarted = false; 
}