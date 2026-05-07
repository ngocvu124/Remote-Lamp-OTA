#include "Ota.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Update.h>
#include "Display.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>

extern SemaphoreHandle_t xGuiSemaphore;
OtaLogic ota;
bool otaStarted = false;

// Link file danh sách phiên bản trên GitHub của bác
const char* VERSIONS_URL = "https://raw.githubusercontent.com/ngocvu124/Remote-Lamp-OTA/main/versions.json";

// Cấp phát PSRAM cho JSON
struct SpiRamAllocatorOta {
    void* allocate(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
    void deallocate(void* pointer) { heap_caps_free(pointer); }
    void* reallocate(void* ptr, size_t new_size) { return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM); }
};
using SpiRamJsonDocumentOta = BasicJsonDocument<SpiRamAllocatorOta>;

static void showOtaProgress(int percent) {
    static uint32_t lastUpdate = 0;
    const uint32_t now = millis();
    if ((now - lastUpdate) < 200 && percent < 100) return;
    lastUpdate = now;

    char msgProg[128];
    snprintf(msgProg, sizeof(msgProg), "Installing Firmware...\n\nPROGRESS: %d%%\n\nDo not power off!", percent);
    if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(50))) {
        display.showProgressPopup("UPDATING", msgProg, percent);
        xSemaphoreGiveRecursive(xGuiSemaphore);
    }
}

static bool runOtaWithPsramBuffer(const char* firmwareUrl, String& errMsg) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(client, firmwareUrl)) {
        errMsg = "HTTP begin failed";
        return false;
    }

    http.addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    http.addHeader("Pragma", "no-cache");
    http.addHeader("Expires", "0");

    const int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        errMsg = "HTTP " + String(httpCode);
        http.end();
        return false;
    }

    const int totalSize = http.getSize();
    if (totalSize <= 0) {
        errMsg = "Content-Length missing";
        http.end();
        return false;
    }

    if (!Update.begin((size_t)totalSize)) {
        errMsg = String("Update.begin failed: ") + Update.errorString();
        http.end();
        return false;
    }

    const size_t preferredChunk = 32 * 1024;
    size_t chunkSize = preferredChunk;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(chunkSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        chunkSize = 8 * 1024;
        buf = (uint8_t*)heap_caps_malloc(chunkSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!buf) {
        Update.abort();
        errMsg = "No RAM for OTA buffer";
        http.end();
        return false;
    }

    Serial.printf("[OTA] Buffer size: %u bytes (%s)\n",
                  (unsigned)chunkSize,
                  (chunkSize == preferredChunk) ? "PSRAM" : "Internal RAM");

    WiFiClient* stream = http.getStreamPtr();
    size_t writtenTotal = 0;
    uint32_t lastDataMs = millis();

    while (writtenTotal < (size_t)totalSize) {
        size_t avail = stream->available();
        if (avail == 0) {
            if (!http.connected()) break;
            if (millis() - lastDataMs > 15000) {
                errMsg = "Read timeout";
                free(buf);
                Update.abort();
                http.end();
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        const size_t toRead = min(min(avail, chunkSize), (size_t)totalSize - writtenTotal);
        const int n = stream->readBytes((char*)buf, toRead);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        lastDataMs = millis();
        const size_t wrote = Update.write(buf, (size_t)n);
        if (wrote != (size_t)n) {
            errMsg = String("Flash write failed: ") + Update.errorString();
            free(buf);
            Update.abort();
            http.end();
            return false;
        }

        writtenTotal += wrote;
        const int percent = (int)((writtenTotal * 100U) / (size_t)totalSize);
        showOtaProgress(percent);
    }

    free(buf);
    http.end();

    if (writtenTotal != (size_t)totalSize) {
        Update.abort();
        errMsg = String("Download incomplete: ") + String((unsigned)writtenTotal) + "/" + String(totalSize);
        return false;
    }

    if (!Update.end()) {
        errMsg = String("Update.end failed: ") + Update.errorString();
        return false;
    }

    if (!Update.isFinished()) {
        errMsg = "Update not finished";
        return false;
    }

    showOtaProgress(100);
    return true;
}

bool OtaLogic::fetchVersions() {
    versionCount = 0;
    if (WiFi.status() != WL_CONNECTED) return false;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    String versionsUrl = String(VERSIONS_URL) + "?ts=" + String(millis());
    http.begin(client, versionsUrl);
    http.addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    http.addHeader("Pragma", "no-cache");
    http.addHeader("Expires", "0");
    
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        SpiRamJsonDocumentOta doc(16384);
        DeserializationError error = deserializeJson(doc, http.getStream());
        if (!error) {
            JsonArray arr = doc.as<JsonArray>();
            // Quét tối đa 10 phiên bản mới nhất
            for (int i = 0; i < arr.size() && i < 10; i++) {
                const char *name = arr[i]["name"] | "Unknown";
                const char *url = arr[i]["url"] | "";
                strncpy(versions[i].name, name, sizeof(versions[i].name) - 1);
                versions[i].name[sizeof(versions[i].name) - 1] = '\0';
                strncpy(versions[i].url, url, sizeof(versions[i].url) - 1);
                versions[i].url[sizeof(versions[i].url) - 1] = '\0';
                versionCount++;
            }
            if (versionCount > 0) {
                Serial.printf("[OTA] Latest from JSON: %s\n", versions[0].name);
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

    Serial.printf("[OTA] Updating from: %s\n", firmwareUrl);
    String err;
    if (!runOtaWithPsramBuffer(firmwareUrl, err)) {
        Serial.printf("[OTA] Update failed: %s\n", err.c_str());
        char errMsg[160];
        snprintf(errMsg, sizeof(errMsg), "Update Failed!\nError: %s\n\nPlease reset and try again.", err.c_str());
        if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
            display.showProgressPopup("OTA ERROR", errMsg, 0);
            xSemaphoreGiveRecursive(xGuiSemaphore);
        }
    } else {
        Serial.println("[OTA] Update completed. Rebooting...");
        if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(100))) {
            display.showProgressPopup("OTA", "Update OK!\nRebooting...", 100);
            xSemaphoreGiveRecursive(xGuiSemaphore);
        }
        vTaskDelay(pdMS_TO_TICKS(300));
        ESP.restart();
    }

    otaStarted = false; 
}