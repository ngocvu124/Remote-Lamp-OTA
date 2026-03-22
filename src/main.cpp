#include <Arduino.h>
#include "Config.h"
#include "Display.h"
#include "EspNow.h"
#include "Encoder.h"
#include "Battery.h"
#include "App.h"
#include "System.h"
#include "Storage.h"
#include "WebSv.h"
#include <SPI.h>

SemaphoreHandle_t xGuiSemaphore = NULL;
QueueHandle_t xEncoderQueue = NULL;
QueueHandle_t xEspNowQueue = NULL;

volatile bool isGuiReady = false; 
volatile bool isStorageReady = false;

// ==========================================
// TASK 1: GUI (CORE 0) - HỌA SĨ MÀN HÌNH
// ==========================================
void guiTask(void *pvParameters) {
    display.begin();  
    isGuiReady = true; 

    uint32_t lastTick = millis();
    while (1) {
        uint32_t currentTick = millis();
        uint32_t diff = currentTick - lastTick;
        lastTick = currentTick;

        if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(20))) {
            lv_tick_inc(diff); 
            display.loop(); 
            xSemaphoreGive(xGuiSemaphore);
        }
        vTaskDelay(pdMS_TO_TICKS(5)); 
    }
}

// ==========================================
// TASK 2: INPUT (CORE 1) - LÍNH GÁC CỔNG
// ==========================================
void inputTask(void *pvParameters) {
    while (!isGuiReady) vTaskDelay(pdMS_TO_TICKS(50));
    
    encoder.begin();  

    while (1) {
        encoder.loop(); 
        vTaskDelay(pdMS_TO_TICKS(2)); 
    }
}

// ==========================================
// TASK 3: APP LOGIC (CORE 1) - TỔNG GIÁM ĐỐC
// ==========================================
void appTask(void *pvParameters) {
    while (!isGuiReady) vTaskDelay(pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(500)); 

    webServer.begin(); // CÚ CHỐT: Mount LittleFS tại đây

    if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY)) {
        pinMode(SD_CS_PIN, OUTPUT);
        digitalWrite(SD_CS_PIN, HIGH);
        pinMode(SCR_CS_PIN, OUTPUT);
        digitalWrite(SCR_CS_PIN, HIGH);

        storage.begin(); 
        if (storage.isReady && storage.loadConfig(appState)) {
            display.setContrast(appState.oledBrightness); 
        }
        isStorageReady = true; 
        xSemaphoreGive(xGuiSemaphore);
    }

    app.begin();     

    while (1) {
        app.handleEvents(); 
        
        if (millis() > 5000 && encoder.shouldSleep(appState.sleepTimeout * 1000UL)) {
            sys.goToSleep(); 
        }
        vTaskDelay(pdMS_TO_TICKS(20)); 
    }
}

// ==========================================
// TASK 4: BATTERY (CORE 1) - BÁC SĨ KHÁM BỆNH
// ==========================================
void batteryTask(void *pvParameters) {
    while (!isStorageReady) vTaskDelay(pdMS_TO_TICKS(100)); 
    battery.begin();

    while (1) {
        battery.update(appState); 
        vTaskDelay(pdMS_TO_TICKS(5000)); 
    }
}

// ==========================================
// TASK 5: ESP NOW (CORE 1) - NGƯỜI ĐƯA THƯ
// ==========================================
void espNowTask(void *pvParameters) {
    while (!isStorageReady) vTaskDelay(pdMS_TO_TICKS(100));
    espNow.begin();
    
    struct_message msg;
    while (1) {
        if (xQueueReceive(xEspNowQueue, &msg, portMAX_DELAY) == pdPASS) {
            espNow.sendInternal(msg); 
        }
    }
}

// ==========================================
// TỔNG TƯ LỆNH SETUP
// ==========================================
void setup() {
    sys.begin(); 
    Serial.begin(115200);

    xGuiSemaphore = xSemaphoreCreateMutex();
    xEncoderQueue = xQueueCreate(10, sizeof(EncoderEvent));
    xEspNowQueue = xQueueCreate(10, sizeof(struct_message)); 

    if (xGuiSemaphore != NULL && xEncoderQueue != NULL && xEspNowQueue != NULL) {
        
        // CORE 0: Đồ họa hạng nặng
        xTaskCreatePinnedToCore(guiTask, "GuiTask", STACK_GUI, NULL, PRIO_GUI, NULL, 0);
        
        // CORE 1: Chia 4 Luồng theo độ ưu tiên từ cao xuống thấp
        xTaskCreatePinnedToCore(inputTask, "InputTask", 4096, NULL, PRIO_INPUT, NULL, 1);
        xTaskCreatePinnedToCore(appTask, "AppTask", STACK_SYSTEM, NULL, PRIO_SYSTEM, NULL, 1);
        xTaskCreatePinnedToCore(espNowTask, "EspTask", 8192, NULL, PRIO_SYSTEM + 1, NULL, 1);
        xTaskCreatePinnedToCore(batteryTask, "BatTask", 4096, NULL, 1, NULL, 1); 
    }
}

void loop() {
    vTaskDelete(NULL);
}