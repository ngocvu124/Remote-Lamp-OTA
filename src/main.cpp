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

// --- KHAI BÁO CẤU TRÚC STATIC TASK (BỘ NHỚ QUẢN LÝ) ---
StaticTask_t guiTaskBuf, inputTaskBuf, appTaskBuf, batTaskBuf, nowTaskBuf;

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
    delay(1000); 
    
    Serial.println("\n--- REMOTE LAMP STARTING (PSRAM OPTIMIZED) ---");

    // 1. Kiểm tra PSRAM
    if (!psramInit()) {
        Serial.println("[ERROR] PSRAM not found! Tasks will use Internal RAM.");
    } else {
        Serial.printf("[OK] PSRAM Detected: %d MB\n", ESP.getPsramSize() / 1024 / 1024);
    }

    xGuiSemaphore = xSemaphoreCreateMutex();
    xEncoderQueue = xQueueCreate(10, sizeof(EncoderEvent));
    xEspNowQueue = xQueueCreate(10, sizeof(struct_message)); 

    // 2. Hàm cấp phát thông minh: Thử PSRAM trước, xịt thì lấy RAM nội
    auto safeAlloc = [](size_t size, const char* taskName) -> StackType_t* {
        StackType_t* ptr = (StackType_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ptr) {
            Serial.printf("[MEM] %s Stack allocated on PSRAM\n", taskName);
        } else {
            ptr = (StackType_t*)heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            Serial.printf("[MEM] %s Stack allocated on Internal RAM (PSRAM Full/Missing)\n", taskName);
        }
        return ptr;
    };

    // 3. Cấp phát vùng nhớ Stack
    StackType_t *guiStack   = safeAlloc(STACK_GUI, "GuiTask");
    StackType_t *inputStack = safeAlloc(4096, "InputTask");
    StackType_t *appStack   = safeAlloc(STACK_SYSTEM, "AppTask");
    StackType_t *batStack   = safeAlloc(4096, "BatTask");
    StackType_t *nowStack   = safeAlloc(8192, "EspTask");

    if (guiStack && inputStack && appStack && batStack && nowStack) {
        
        // CORE 0: Đồ họa hạng nặng
        xTaskCreateStaticPinnedToCore(guiTask, "GuiTask", STACK_GUI, NULL, PRIO_GUI, guiStack, &guiTaskBuf, 0);
        
        // CORE 1: Các logic còn lại
        xTaskCreateStaticPinnedToCore(inputTask, "InputTask", 4096, NULL, PRIO_INPUT, inputStack, &inputTaskBuf, 1);
        xTaskCreateStaticPinnedToCore(appTask, "AppTask", STACK_SYSTEM, NULL, PRIO_SYSTEM, appStack, &appTaskBuf, 1);
        xTaskCreateStaticPinnedToCore(espNowTask, "EspTask", 8192, NULL, PRIO_SYSTEM + 1, nowStack, &nowTaskBuf, 1);
        xTaskCreateStaticPinnedToCore(batteryTask, "BatTask", 4096, NULL, 1, batStack, &batTaskBuf, 1); 
        
        Serial.println("[SYSTEM] All Static Tasks created successfully.");
    } else {
        Serial.println("[CRITICAL] Memory Allocation Failed! Check PSRAM configuration.");
        while(1) { delay(1000); }
    }
}

void loop() {
    // Xóa task setup để giải phóng tài nguyên
    vTaskDelete(NULL);
}