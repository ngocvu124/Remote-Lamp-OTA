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
#include <Update.h>

SemaphoreHandle_t xGuiSemaphore = NULL;
QueueHandle_t xEncoderQueue = NULL;
QueueHandle_t xEspNowQueue = NULL;

// Biến đếm số lần Crash liên tiếp, DÙNG RTC_NOINIT_ATTR để chống bị reset về 0
RTC_NOINIT_ATTR int crashCount;

volatile bool isGuiReady = false; 
volatile bool isStorageReady = false;

void guiTask(void *pvParameters) {
    display.begin();  
    isGuiReady = true; 

    // Chờ appTask hoàn tất boot log + storage init
    while (!isStorageReady) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Delay nhỏ để người dùng kịp đọc dòng cuối boot log
    vTaskDelay(pdMS_TO_TICKS(1000));

    uint32_t lastTick = millis();
    while (1) {
        uint32_t currentTick = millis();
        uint32_t diff = currentTick - lastTick;
        lastTick = currentTick;

        uint32_t delay_ms = 5;
        if (xSemaphoreTakeRecursive(xGuiSemaphore, pdMS_TO_TICKS(20))) {
            lv_tick_inc(diff); 
            delay_ms = display.loop(); // lần đầu gọi này sẽ render UI đè lên boot log
            xSemaphoreGiveRecursive(xGuiSemaphore);
        }
        
        if (delay_ms == 0 || delay_ms == 0xFFFFFFFF) delay_ms = 5;
        else if (delay_ms > 50) delay_ms = 50;
        vTaskDelay(pdMS_TO_TICKS(delay_ms)); 
    }
}

void inputTask(void *pvParameters) {
    while (!isStorageReady) vTaskDelay(pdMS_TO_TICKS(50));
    encoder.begin();  
    while (1) {
        encoder.loop(); 
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}

void appTask(void *pvParameters) {
    // Đợi display.begin() xong (guiTask đã vẽ boot log phase 1+2)
    while (!isGuiReady) vTaskDelay(pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(100)); 

    if (xSemaphoreTakeRecursive(xGuiSemaphore, portMAX_DELAY)) {

        // ── Boot log phase 3: SD Card ──────────────────────────
        display.bootPrint("SD", "Mounting SD card");
        storage.begin(); 
        // In kết quả thật của việc mount SD
        if (!storage.isReady) {
            display.bootPrint("SD", "SD mount failed", false);
        }

        // ── Boot log phase 4: Config ───────────────────────────
        if (storage.isReady) {
            display.bootPrint("CFG", "Loading config");
            bool cfgOk = storage.loadConfig(appState);
            display.bootPrint("CFG", "Config loaded", cfgOk);
            display.setContrast(appState.oledBrightness);
        }

        isStorageReady = true;

        // ── Boot log phase 5: Dòng cuối ───────────────────────
        display.bootPrint("APP", "All systems ready!");

        xSemaphoreGiveRecursive(xGuiSemaphore);
    }

    webServer.begin(); 
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

void setup() {
    sys.begin(); 
    Serial.begin(115200);

    // =========================================================================
    // CƠ CHẾ CHỐNG BRICK TỰ ĐỘNG (AUTO ROLLBACK)
    // =========================================================================
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT || reason == ESP_RST_SW) {
        crashCount = 0; // Chỉ reset biến khi cấp nguồn mới
    }

    // Nếu reset do Crash (PANIC) hoặc bị Treo (Watchdog Timeout)
    if (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT) {
        crashCount++;
        Serial.printf("\n[SAFE BOOT] Crash detected! Consecutive crashes: %d\n", crashCount);
        
        if (crashCount >= 3) {
            Serial.println("[SAFE BOOT] Bootloop detected! Attempting Rollback to previous firmware...");
            if (Update.canRollBack()) {
                Update.rollBack(); 
                Serial.println("[SAFE BOOT] Rollback successful! Rebooting into old firmware...");
                crashCount = 0; 

                uint8_t colors[7][3] = {
                    {255, 0, 0}, {255, 127, 0}, {255, 255, 0}, 
                    {0, 255, 0}, {0, 0, 255}, {75, 0, 130}, {148, 0, 211}
                };
                for (int i = 0; i < 14; i++) { 
                    neopixelWrite(48, colors[i % 7][0], colors[i % 7][1], colors[i % 7][2]);
                    delay(150);
                }
                neopixelWrite(48, 0, 0, 0);
                ESP.restart();
            } else {
                Serial.println("[SAFE BOOT] Cannot rollback! No alternative firmware found.");
            }
        }
    }

    // [QUAN TRỌNG] Ép chân CS của SD Card và màn hình lên HIGH ngay lập tức.
    // Đảm bảo thẻ nhớ bị "khóa" và không đọc nhầm rác từ bus SPI lúc màn hình khởi tạo.
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
    pinMode(SCR_CS_PIN, OUTPUT);
    digitalWrite(SCR_CS_PIN, HIGH);

    xGuiSemaphore = xSemaphoreCreateRecursiveMutex();
    xEncoderQueue = xQueueCreate(10, sizeof(EncoderEvent));
    xEspNowQueue = xQueueCreate(10, sizeof(struct_message)); 

    if (xGuiSemaphore != NULL && xEncoderQueue != NULL && xEspNowQueue != NULL) {
        xTaskCreatePinnedToCore(guiTask,     "GuiTask",   16384,        NULL, PRIO_GUI,         NULL, 0);
        xTaskCreatePinnedToCore(inputTask,   "InputTask", 4096,         NULL, PRIO_INPUT,        NULL, 1);
        xTaskCreatePinnedToCore(appTask,     "AppTask",   8192,         NULL, PRIO_SYSTEM,       NULL, 1);
        xTaskCreatePinnedToCore(espNowTask,  "EspTask",   8192,         NULL, PRIO_SYSTEM + 1,   NULL, 1);
        xTaskCreatePinnedToCore(batteryTask, "BatTask",   4096,         NULL, 1,                 NULL, 1); 
    } else {
        Serial.println("FATAL: RTOS Resources creation failed!");
        ESP.restart(); // Nếu cạn RAM ngay lúc khởi động, nên reset
    }
}

void loop() { vTaskDelete(NULL); }