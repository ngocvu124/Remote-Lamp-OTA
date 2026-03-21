#include "EspNowLogic.h"
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "Config.h"
#include "DisplayLogic.h"
#include "EncoderLogic.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

EspNowLogic espNow;
esp_now_peer_info_t peerInfo;
struct_message myData;
struct_message incomingData;

int currentChannel = WIFI_CHANNEL;
volatile bool foundNodeA = false; // Thêm volatile để chống lỗi tối ưu hóa khi dùng đa luồng

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

        if (appState.isTempMode) {
            encoder.setEncoderValue(appState.temperature);
        } else {
            encoder.setEncoderValue(appState.brightness);
        }

        // CÚ CHỐT RTOS: Phải lấy chìa khóa xGuiSemaphore trước khi cập nhật màn hình
        // để không bị xung đột với Luồng GuiTask đang vẽ dở
        if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(50))) {
            display.updateUI(appState);
            xSemaphoreGive(xGuiSemaphore);
        }
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
    struct_message msg;
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

    if (esp_now_send(BROADCAST_ADDRESS,
                     (uint8_t *)&myData,
                     sizeof(myData)) == ESP_OK) {
        return;
    }

    if (currentChannel != 1) {
        esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
        for (int i = 0; i < 2; i++) {
            esp_now_send(BROADCAST_ADDRESS,
                         (uint8_t *)&myData,
                         sizeof(myData));
            
            // Đổi delay thành vTaskDelay để tuân thủ luật FreeRTOS
            vTaskDelay(pdMS_TO_TICKS(10)); 
            
            if (foundNodeA) {
                currentChannel = 1;
                return;
            }
        }
    }

    static unsigned long lastScan = 0;
    if (millis() - lastScan < 2000) return;
    lastScan = millis();

    for (int ch = 1; ch <= 11; ch++) {
        if (ch == 6) continue;

        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        esp_now_send(BROADCAST_ADDRESS,
                     (uint8_t *)&myData,
                     sizeof(myData));
                     
        vTaskDelay(pdMS_TO_TICKS(15)); // Đổi delay thành vTaskDelay

        if (foundNodeA) {
            currentChannel = ch;
            break;
        }
    }
}