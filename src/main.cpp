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

// --- KHAI BÁO CẤU TRÚC STATIC TASK ---
StaticTask_t guiTaskBuf, inputTaskBuf, appTaskBuf, batTaskBuf, nowTaskBuf;

// --- CẤP PHÁT STACK TRÊN PSRAM ---
// Sử dụng heap_caps_malloc với cờ MALLOC_CAP_SPIRAM
StackType_t *guiStack = (StackType_t *)heap_caps_malloc(STACK_GUI, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
StackType_t *inputStack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
StackType_t *appStack = (StackType_t *)heap_caps_malloc(STACK_SYSTEM, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
StackType_t *batStack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
StackType_t *nowStack = (StackType_t *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

// ==========================================
// TASK 1: GUI (CORE 0)
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
// TASK 2: INPUT (CORE 1)
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
// TASK 3: APP LOGIC (CORE 1)
// ==========================================
void appTask(void *pvParameters) {
    while (!isGuiReady) vTaskDelay(pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(500)); 

    webServer.begin(); 

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
// TASK 4: BATTERY (CORE 1)
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
// TASK 5: ESP NOW (CORE 1)
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

    // Kiểm tra PSRAM đã sẵn sàng chưa
    if (psramInit()) {
        Serial.println("[SYSTEM] PSRAM Initialized OK!");
    } else {
        Serial.println("[SYSTEM] PSRAM Failed! System might crash.");
    }

    xGuiSemaphore = xSemaphoreCreateMutex();
    // Queue vẫn nên để ở RAM nội để đảm bảo tốc độ phản hồi
    xEncoderQueue = xQueueCreate(10, sizeof(EncoderEvent));
    xEspNowQueue = xQueueCreate(10, sizeof(struct_message)); 

    // Kiểm tra việc cấp phát Stack trên PSRAM
    if (guiStack == NULL || inputStack == NULL || appStack == NULL || batStack == NULL || nowStack == NULL) {
        Serial.println("[CRITICAL] Failed to allocate Task Stacks on PSRAM!");
        return;
    }

    if (xGuiSemaphore != NULL && xEncoderQueue != NULL && xEspNowQueue != NULL) {
        
        // --- CHUYỂN SANG XTASKCREATESTATIC ĐỂ DÙNG PSRAM ---

        // CORE 0
        xTaskCreateStaticPinnedToCore(guiTask, "GuiTask", STACK_GUI, NULL, PRIO_GUI, guiStack, &guiTaskBuf, 0);
        
        // CORE 1
        xTaskCreateStaticPinnedToCore(inputTask, "InputTask", 4096, NULL, PRIO_INPUT, inputStack, &inputTaskBuf, 1);
        xTaskCreateStaticPinnedToCore(appTask, "AppTask", STACK_SYSTEM, NULL, PRIO_SYSTEM, appStack, &appTaskBuf, 1);
        xTaskCreateStaticPinnedToCore(espNowTask, "EspTask", 8192, NULL, PRIO_SYSTEM + 1, nowStack, &nowTaskBuf, 1);
        xTaskCreateStaticPinnedToCore(batteryTask, "BatTask", 4096, NULL, 1, batStack, &batTaskBuf, 1); 
    }
}

void loop() {
    vTaskDelete(NULL);
}