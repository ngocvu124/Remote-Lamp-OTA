#include "EspNow.h"
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "Config.h"
#include "Display.h"
#include "Encoder.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <Preferences.h>

EspNowLogic espNow;
esp_now_peer_info_t peerInfo;
struct_message myData;
struct_message incomingData;
char currentHomeKitSetupCode[9] = HOMEKIT_SETUP_CODE;
char currentHomeKitQrId[5] = HOMEKIT_QR_ID;
bool homeKitQrSynced = false;
volatile uint16_t lastAckRequestId = 0;
volatile char lastAckCmd = 0;
volatile char lastAckOk = 0;
uint16_t nextRequestId = 1;

static char s_wifiSsid[33] = {0};
static char s_wifiPass[65] = {0};
static bool s_wifiReady = false;
static uint16_t s_wifiVersion = 0;
static uint8_t s_wifiBlob[192] = {0};
static size_t s_wifiBlobLen = 0;
static uint16_t s_wifiChunksExpected = 0;
static uint16_t s_wifiChunksSeen = 0;
static bool s_wifiHasData = false;

int currentChannel = WIFI_CHANNEL;
volatile bool foundNodeA = false; // Thêm volatile để chống lỗi tối ưu hóa khi dùng đa luồng
volatile bool espnow_needs_update = false; // Cờ báo hiệu cho AppTask

extern RemoteState appState;
extern DisplayLogic display;
extern EncoderLogic encoder;

extern QueueHandle_t xEspNowQueue;
extern SemaphoreHandle_t xGuiSemaphore;
extern char cachedSSID[32];

static void clearSyncedWifiCreds() {
    s_wifiSsid[0] = '\0';
    s_wifiPass[0] = '\0';
    s_wifiReady = false;
    s_wifiVersion = 0;
    strncpy(cachedSSID, "Not configured", sizeof(cachedSSID) - 1);
    cachedSSID[sizeof(cachedSSID) - 1] = '\0';
}

static void saveSyncedWifiCreds() {
    Preferences prefs;
    if (!prefs.begin("rlampw", false)) return;
    prefs.putBool("ok", s_wifiReady);
    prefs.putUShort("ver", s_wifiVersion);
    prefs.putString("ssid", s_wifiSsid);
    prefs.putString("pass", s_wifiPass);
    prefs.end();
}

static void loadSyncedWifiCreds() {
    Preferences prefs;
    if (!prefs.begin("rlampw", true)) {
        clearSyncedWifiCreds();
        return;
    }

    s_wifiReady = prefs.getBool("ok", false);
    s_wifiVersion = prefs.getUShort("ver", 0);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();

    if (!s_wifiReady || ssid.length() == 0) {
        clearSyncedWifiCreds();
        return;
    }

    strncpy(s_wifiSsid, ssid.c_str(), sizeof(s_wifiSsid) - 1);
    s_wifiSsid[sizeof(s_wifiSsid) - 1] = '\0';
    strncpy(s_wifiPass, pass.c_str(), sizeof(s_wifiPass) - 1);
    s_wifiPass[sizeof(s_wifiPass) - 1] = '\0';
    strncpy(cachedSSID, s_wifiSsid, sizeof(cachedSSID) - 1);
    cachedSSID[sizeof(cachedSSID) - 1] = '\0';
}

static void applyWifiBlobIfComplete() {
    if (s_wifiChunksExpected == 0 || s_wifiChunksSeen < s_wifiChunksExpected) return;

    if (!s_wifiHasData) {
        clearSyncedWifiCreds();
        saveSyncedWifiCreds();
        Serial.println("[ESP-NOW] Lamp has no Wi-Fi credentials yet");
        return;
    }

    const char *ssid = (const char *)s_wifiBlob;
    size_t ssidLen = strnlen(ssid, s_wifiBlobLen);
    if (ssidLen == s_wifiBlobLen) return;

    const char *pass = (const char *)(s_wifiBlob + ssidLen + 1);
    size_t remain = s_wifiBlobLen - (ssidLen + 1);
    size_t passLen = strnlen(pass, remain);
    if (passLen == remain) return;

    strncpy(s_wifiSsid, ssid, sizeof(s_wifiSsid) - 1);
    s_wifiSsid[sizeof(s_wifiSsid) - 1] = '\0';
    strncpy(s_wifiPass, pass, sizeof(s_wifiPass) - 1);
    s_wifiPass[sizeof(s_wifiPass) - 1] = '\0';
    s_wifiReady = (s_wifiSsid[0] != '\0');
    strncpy(cachedSSID, s_wifiReady ? s_wifiSsid : "Not configured", sizeof(cachedSSID) - 1);
    cachedSSID[sizeof(cachedSSID) - 1] = '\0';
    saveSyncedWifiCreds();
    Serial.printf("[ESP-NOW] Synced Wi-Fi from lamp: ssid=%s ver=%u\n", s_wifiSsid, s_wifiVersion);
}

// ===== CALLBACK NHẬN =====
void OnDataRecv(const uint8_t *mac, const uint8_t *incoming, int len) {
    if (len != sizeof(struct_message)) return;

    memcpy(&incomingData, incoming, sizeof(incomingData));

    if (incomingData.mode == 0) {
        appState.brightness  = incomingData.brightness;
        appState.temperature = incomingData.temperature;

        // Chỉ phất cờ, KHÔNG CHẠM VÀO MÀN HÌNH Ở ĐÂY
        espnow_needs_update = true; 
    } else if (incomingData.mode == 2 && incomingData.sysCmd == 'Q') {
        strncpy(currentHomeKitSetupCode, incomingData.setupCode, sizeof(currentHomeKitSetupCode) - 1);
        currentHomeKitSetupCode[sizeof(currentHomeKitSetupCode) - 1] = '\0';
        strncpy(currentHomeKitQrId, incomingData.qrId, sizeof(currentHomeKitQrId) - 1);
        currentHomeKitQrId[sizeof(currentHomeKitQrId) - 1] = '\0';
        homeKitQrSynced = true;
        Serial.printf("[ESP-NOW] Synced HomeKit setup info: code=%s qr=%s\n", currentHomeKitSetupCode, currentHomeKitQrId);
    } else if (incomingData.mode == 2 && incomingData.sysCmd == 'W') {
        if (incomingData.brightness == 0) {
            s_wifiChunksExpected = incomingData.temperature > 0 ? (uint16_t)incomingData.temperature : 1;
            s_wifiChunksSeen = 0;
            s_wifiBlobLen = 0;
            s_wifiVersion = incomingData.requestId;
            s_wifiHasData = (incomingData.ackOk == 1);
        }

        const uint8_t chunkLen = (uint8_t)incomingData.ackCmd;
        if (s_wifiHasData && chunkLen > 0) {
            uint8_t chunk[14];
            memcpy(chunk, incomingData.setupCode, sizeof(incomingData.setupCode));
            memcpy(chunk + sizeof(incomingData.setupCode), incomingData.qrId, sizeof(incomingData.qrId));
            const size_t copyLen = (chunkLen > sizeof(chunk)) ? sizeof(chunk) : chunkLen;
            if (s_wifiBlobLen + copyLen <= sizeof(s_wifiBlob)) {
                memcpy(s_wifiBlob + s_wifiBlobLen, chunk, copyLen);
                s_wifiBlobLen += copyLen;
            }
        }

        s_wifiChunksSeen++;
        applyWifiBlobIfComplete();
    } else if (incomingData.mode == 3 && incomingData.sysCmd == 'A') {
        lastAckRequestId = incomingData.requestId;
        lastAckCmd = incomingData.ackCmd;
        lastAckOk = incomingData.ackOk;
        Serial.printf("[ESP-NOW] ACK received req=%u cmd=%c ok=%d\n", incomingData.requestId, incomingData.ackCmd, incomingData.ackOk);
    }
}

// ===== CALLBACK GỬI =====
void OnDataSent(const uint8_t *mac, esp_now_send_status_t status) {
    foundNodeA = (status == ESP_NOW_SEND_SUCCESS);
}

// ===== INIT =====
void EspNowLogic::begin() {
    WiFi.mode(WIFI_STA);
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    loadSyncedWifiCreds();

    if (esp_now_init() != ESP_OK) return;

    esp_now_set_pmk((uint8_t *)ESPNOW_PMK);

    esp_now_register_send_cb(OnDataSent);
    esp_now_register_recv_cb(OnDataRecv);

    if (esp_now_is_peer_exist(BROADCAST_ADDRESS)) {
        esp_now_del_peer(BROADCAST_ADDRESS);
    }
    memcpy(peerInfo.peer_addr, BROADCAST_ADDRESS, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = true;
    memcpy(peerInfo.lmk, ESPNOW_LMK, sizeof(ESPNOW_LMK));

    if (!esp_now_is_peer_exist(BROADCAST_ADDRESS)) {
        esp_now_add_peer(&peerInfo);
    }

    requestWifiSync(1, 500);
}

// ===== HÀM GỬI TÍN HIỆU (GIAO CHO QUEUE) =====
void EspNowLogic::send(int mode, int bri, int temp, char sysCmd) {
    struct_message msg = {};
    msg.mode = mode;
    msg.brightness = bri;
    msg.temperature = temp;
    msg.sysCmd = sysCmd;

    // Ném thư vào Queue cho luồng EspNowTask làm việc, AppTask không phải chờ!
    if (xEspNowQueue != NULL) {
        xQueueSend(xEspNowQueue, &msg, 0);
    }
}

// ===== HÀM THỰC THI QUÉT KÊNH (CHẠY TRONG ESP-NOW TASK) =====
void EspNowLogic::sendInternal(struct_message msg) {
    myData.mode = msg.mode;
    myData.brightness = msg.brightness;
    myData.temperature = msg.temperature;
    myData.sysCmd = msg.sysCmd;
    myData.requestId = msg.requestId;
    myData.ackCmd = msg.ackCmd;
    myData.ackOk = msg.ackOk;
    memcpy(myData.setupCode, msg.setupCode, sizeof(myData.setupCode));
    memcpy(myData.qrId, msg.qrId, sizeof(myData.qrId));

    // Luôn restore về currentChannel trước khi gửi
    // (sau scan thất bại, WiFi có thể bị kẹt ở channel khác)
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);

    foundNodeA = false;
    esp_now_send(BROADCAST_ADDRESS, (uint8_t *)&myData, sizeof(myData));
    vTaskDelay(pdMS_TO_TICKS(25));

    if (foundNodeA) return; // Gửi thành công

    // Gửi thất bại → scan toàn bộ channel 1-13
    static unsigned long lastScan = 0;

    if (millis() - lastScan < 3000) return;
    lastScan = millis();

    for (int ch = 1; ch <= 13; ch++) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        foundNodeA = false;
        esp_now_send(BROADCAST_ADDRESS, (uint8_t *)&myData, sizeof(myData));
        vTaskDelay(pdMS_TO_TICKS(25));
        if (foundNodeA) {
            currentChannel = ch;
            return;
        }
    }

    // Scan thất bại: restore lại currentChannel để lần sau không kẹt trên ch13
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
}

bool EspNowLogic::requestWifiSync(uint8_t maxRetries, uint16_t ackTimeoutMs) {
    return sendCommandWithAck('W', maxRetries, ackTimeoutMs);
}

bool EspNowLogic::hasSyncedWifiCreds() const {
    return s_wifiReady;
}

bool EspNowLogic::getSyncedWifiCreds(char *ssidOut, size_t ssidSize, char *passOut, size_t passSize) const {
    if (!s_wifiReady || !ssidOut || !passOut || ssidSize == 0 || passSize == 0) return false;
    strncpy(ssidOut, s_wifiSsid, ssidSize - 1);
    ssidOut[ssidSize - 1] = '\0';
    strncpy(passOut, s_wifiPass, passSize - 1);
    passOut[passSize - 1] = '\0';
    return true;
}

bool EspNowLogic::sendCommandWithAck(char sysCmd, uint8_t maxRetries, uint16_t ackTimeoutMs) {
    struct_message cmd = {};
    cmd.mode = 1;
    cmd.brightness = appState.brightness;
    cmd.temperature = appState.temperature;
    cmd.sysCmd = sysCmd;
    cmd.requestId = nextRequestId++;

    // Restart/Factory reset: fire-and-forget (khong fallback)
    if (sysCmd == 'R' || sysCmd == 'F') {
        sendInternal(cmd);
        return true;
    }

    for (uint8_t attempt = 0; attempt <= maxRetries; ++attempt) {
        lastAckRequestId = 0;
        lastAckCmd = 0;
        lastAckOk = 0;

        sendInternal(cmd);

        const uint32_t start = millis();
        while (millis() - start < ackTimeoutMs) {
            if (lastAckRequestId == cmd.requestId && lastAckCmd == sysCmd) {
                return lastAckOk == 1;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        Serial.printf("[ESP-NOW] ACK timeout req=%u cmd=%c retry=%u/%u\n", cmd.requestId, sysCmd, attempt + 1, maxRetries + 1);
    }

    return false;
}