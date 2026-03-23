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

// Cấu trúc quản lý Static Task
StaticTask_t guiTaskBuf, inputTaskBuf, appTaskBuf, batTaskBuf, nowTaskBuf;

// --- CÁC HÀM TASK ---
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

void inputTask(void *pvParameters) {
    while (!isGuiReady) vTaskDelay(pdMS_TO_TICKS(50));
    encoder.begin();  
    while (1) {
        encoder.loop(); 
        vTaskDelay(pdMS_TO_TICKS(2)); 
    }
}

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

void batteryTask(void *pvParameters) {
    while (!isStorageReady) vTaskDelay(pdMS_TO_TICKS(100)); 
    battery.begin();
    while (1) {
        battery.update(appState); 
        vTaskDelay(pdMS_TO_TICKS(5000)); 
    }
}

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
// SETUP
// ==========================================
void setup() {
    sys.begin(); 
    Serial.begin(115200);
    delay(1500); 
    
    Serial.println("\n--- REMOTE LAMP STARTING (STABLE MODE) ---");

    if (!psramInit()) {
        Serial.println("[ERROR] PSRAM NOT FOUND!");
    } else {
        Serial.printf("[OK] PSRAM Size: %d KB\n", ESP.getPsramSize() / 1024);
    }

    xGuiSemaphore = xSemaphoreCreateMutex();
    xEncoderQueue = xQueueCreate(10, sizeof(EncoderEvent));
    xEspNowQueue = xQueueCreate(10, sizeof(struct_message)); 

    // HÀM CẤP PHÁT AN TOÀN TUYỆT ĐỐI
    auto safeAlloc = [](size_t size, const char* name, bool forceInternal) -> StackType_t* {
        StackType_t* p = NULL;
        if (!forceInternal) {
            p = (StackType_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (p) {
            Serial.printf("[MEM] %s: PSRAM OK\n", name);
        } else {
            p = (StackType_t*)heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            Serial.printf("[MEM] %s: Internal RAM\n", name);
        }
        return p;
    };

    // Đẩy GUI và APP (nặng nhất) vào PSRAM. Input và Now (nhạy cảm) để RAM nội.
    StackType_t *sGui  = safeAlloc(STACK_GUI, "Gui", false);
    StackType_t *sInp  = safeAlloc(4096, "Input", true); // Ép RAM nội cho mượt
    StackType_t *sApp  = safeAlloc(STACK_SYSTEM, "App", false);
    StackType_t *sNow  = safeAlloc(8192, "EspNow", true); // Ép RAM nội tránh delay
    StackType_t *sBat  = safeAlloc(4096, "Bat", false);

    if (sGui && sInp && sApp && sNow && sBat) {
        // Tạo task - Nếu vẫn assert lỗi PSRAM, FreeRTOS sẽ tự hiểu nhờ flag ở platformio.ini
        xTaskCreateStaticPinnedToCore(guiTask, "GuiTask", STACK_GUI, NULL, PRIO_GUI, sGui, &guiTaskBuf, 0);
        xTaskCreateStaticPinnedToCore(inputTask, "InputTask", 4096, NULL, PRIO_INPUT, sInp, &inputTaskBuf, 1);
        xTaskCreateStaticPinnedToCore(appTask, "AppTask", STACK_SYSTEM, NULL, PRIO_SYSTEM, sApp, &appTaskBuf, 1);
        xTaskCreateStaticPinnedToCore(espNowTask, "EspTask", 8192, NULL, PRIO_SYSTEM + 1, sNow, &nowTaskBuf, 1);
        xTaskCreateStaticPinnedToCore(batteryTask, "BatTask", 4096, NULL, 1, sBat, &batTaskBuf, 1); 
        Serial.println("[SYSTEM] All Static Tasks Started Successfully.");
    } else {
        Serial.println("[CRITICAL] Memory allocation failed!");
        while(1) delay(1000);
    }
}

void loop() {
    vTaskDelete(NULL);
}