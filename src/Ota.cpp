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
#include <mbedtls/sha256.h>
#include <time.h>

extern SemaphoreHandle_t xGuiSemaphore;
OtaLogic ota;
bool otaStarted = false;

// Link file danh sách phiên bản trên GitHub của bác
const char* VERSIONS_URL = "https://raw.githubusercontent.com/ngocvu124/Remote-Lamp-OTA/main/versions.json";
static const char* FIRMWARE_URL_PREFIX = "https://raw.githubusercontent.com/ngocvu124/Remote-Lamp-OTA/main/";
static const char* GITHUB_ROOT_CA = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";

// Cấp phát PSRAM cho JSON
struct SpiRamAllocatorOta {
    void* allocate(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
    void deallocate(void* pointer) { heap_caps_free(pointer); }
    void* reallocate(void* ptr, size_t new_size) { return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM); }
};
using SpiRamJsonDocumentOta = BasicJsonDocument<SpiRamAllocatorOta>;

static bool ensureOtaClock() {
    time_t now = time(nullptr);
    if (now > 1700000000) return true;

    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    const uint32_t start = millis();
    while (millis() - start < 7000) {
        now = time(nullptr);
        if (now > 1700000000) return true;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    Serial.println("[OTA] NTP time sync failed");
    return false;
}

static bool configureSecureClient(WiFiClientSecure& client) {
    if (!ensureOtaClock()) return false;
    client.setCACert(GITHUB_ROOT_CA);
    client.setTimeout(15000);
    return true;
}

static bool isAllowedFirmwareUrl(const char* url) {
    return url && strncmp(url, FIRMWARE_URL_PREFIX, strlen(FIRMWARE_URL_PREFIX)) == 0;
}

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

static bool isValidSha256Hex(const char* hash) {
    if (!hash || strlen(hash) != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        const char c = hash[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static void bytesToHex(const uint8_t* bytes, size_t len, char* out, size_t outSize) {
    static const char hex[] = "0123456789abcdef";
    if (outSize < (len * 2 + 1)) {
        if (outSize) out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

static bool runOtaWithPsramBuffer(const char* firmwareUrl, const char* expectedSha256, String& errMsg) {
    if (!isValidSha256Hex(expectedSha256)) {
        errMsg = "Missing firmware SHA256";
        return false;
    }

    if (!isAllowedFirmwareUrl(firmwareUrl)) {
        errMsg = "Untrusted firmware URL";
        return false;
    }

    WiFiClientSecure client;
    if (!configureSecureClient(client)) {
        errMsg = "TLS time sync failed";
        return false;
    }

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
    mbedtls_sha256_context shaCtx;
    mbedtls_sha256_init(&shaCtx);
    mbedtls_sha256_starts(&shaCtx, 0);

    while (writtenTotal < (size_t)totalSize) {
        size_t avail = stream->available();
        if (avail == 0) {
            if (!http.connected()) break;
            if (millis() - lastDataMs > 15000) {
                errMsg = "Read timeout";
                mbedtls_sha256_free(&shaCtx);
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
        mbedtls_sha256_update(&shaCtx, buf, (size_t)n);
        const size_t wrote = Update.write(buf, (size_t)n);
        if (wrote != (size_t)n) {
            errMsg = String("Flash write failed: ") + Update.errorString();
            mbedtls_sha256_free(&shaCtx);
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
    uint8_t actualDigest[32];
    char actualSha256[65];
    mbedtls_sha256_finish(&shaCtx, actualDigest);
    mbedtls_sha256_free(&shaCtx);
    bytesToHex(actualDigest, sizeof(actualDigest), actualSha256, sizeof(actualSha256));

    if (writtenTotal != (size_t)totalSize) {
        Update.abort();
        errMsg = String("Download incomplete: ") + String((unsigned)writtenTotal) + "/" + String(totalSize);
        return false;
    }

    if (!String(actualSha256).equalsIgnoreCase(expectedSha256)) {
        Update.abort();
        errMsg = "Firmware SHA256 mismatch";
        Serial.printf("[OTA] Expected SHA256: %s\n", expectedSha256);
        Serial.printf("[OTA] Actual SHA256:   %s\n", actualSha256);
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
    if (!configureSecureClient(client)) return false;
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
                const char *sha256 = arr[i]["sha256"] | "";
                strncpy(versions[i].sha256, sha256, sizeof(versions[i].sha256) - 1);
                versions[i].sha256[sizeof(versions[i].sha256) - 1] = '\0';
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
    const char* expectedSha256 = "";
    for (int i = 0; i < versionCount; ++i) {
        if (strcmp(ota.versions[i].url, firmwareUrl) == 0) {
            expectedSha256 = ota.versions[i].sha256;
            break;
        }
    }

    if (!runOtaWithPsramBuffer(firmwareUrl, expectedSha256, err)) {
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
