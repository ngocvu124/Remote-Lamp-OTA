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

        if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(20))) {
            lv_tick_inc(diff); 
            display.loop(); // lần đầu gọi này sẽ render UI đè lên boot log
            xSemaphoreGive(xGuiSemaphore);
        }
        vTaskDelay(pdMS_TO_TICKS(5)); 
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

    if (xSemaphoreTake(xGuiSemaphore, portMAX_DELAY)) {

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

        xSemaphoreGive(xGuiSemaphore);
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

    SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);

    xGuiSemaphore = xSemaphoreCreateMutex();
    xEncoderQueue = xQueueCreate(10, sizeof(EncoderEvent));
    xEspNowQueue = xQueueCreate(10, sizeof(struct_message)); 

    if (xGuiSemaphore != NULL && xEncoderQueue != NULL && xEspNowQueue != NULL) {
        xTaskCreatePinnedToCore(guiTask,     "GuiTask",   STACK_GUI,    NULL, PRIO_GUI,         NULL, 0);
        xTaskCreatePinnedToCore(inputTask,   "InputTask", 4096,         NULL, PRIO_INPUT,        NULL, 1);
        xTaskCreatePinnedToCore(appTask,     "AppTask",   STACK_SYSTEM, NULL, PRIO_SYSTEM,       NULL, 1);
        xTaskCreatePinnedToCore(espNowTask,  "EspTask",   8192,         NULL, PRIO_SYSTEM + 1,   NULL, 1);
        xTaskCreatePinnedToCore(batteryTask, "BatTask",   4096,         NULL, 1,                 NULL, 1); 
    } else {
        Serial.println("FATAL: RTOS Resources creation failed!");
        ESP.restart(); // Nếu cạn RAM ngay lúc khởi động, nên reset
    }
}

void loop() { vTaskDelete(NULL); }