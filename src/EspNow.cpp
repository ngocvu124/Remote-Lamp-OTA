#include "EspNow.h"
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "Config.h"
#include "Display.h"
#include "Encoder.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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

int currentChannel = WIFI_CHANNEL;
volatile bool foundNodeA = false; // Thêm volatile để chống lỗi tối ưu hóa khi dùng đa luồng
volatile bool espnow_needs_update = false; // Cờ báo hiệu cho AppTask

extern RemoteState appState;
extern DisplayLogic display;
extern EncoderLogic encoder;

extern QueueHandle_t xEspNowQueue;
extern SemaphoreHandle_t xGuiSemaphore;

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

    if (esp_now_init() != ESP_OK) return;

    esp_now_register_send_cb(OnDataSent);
    esp_now_register_recv_cb(OnDataRecv);

    memcpy(peerInfo.peer_addr, BROADCAST_ADDRESS, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (!esp_now_is_peer_exist(BROADCAST_ADDRESS)) {
        esp_now_add_peer(&peerInfo);
    }
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
    strncpy(myData.setupCode, msg.setupCode, sizeof(myData.setupCode) - 1);
    myData.setupCode[sizeof(myData.setupCode) - 1] = '\0';
    strncpy(myData.qrId, msg.qrId, sizeof(myData.qrId) - 1);
    myData.qrId[sizeof(myData.qrId) - 1] = '\0';

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