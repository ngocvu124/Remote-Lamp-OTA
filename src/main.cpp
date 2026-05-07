#include <Arduino.h>
#include "Config.h"
#include "Display.h"
#include "EspNow.h"
#include "Encoder.h"
#include "Battery.h"
#include "App.h"
#include "System.h"
#include <Update.h>
#include <Preferences.h>

SemaphoreHandle_t xGuiSemaphore = NULL;
QueueHandle_t xEncoderQueue = NULL;
QueueHandle_t xEspNowQueue = NULL;
volatile bool isGuiReady = false;
volatile bool isStorageReady = false;
RTC_NOINIT_ATTR int crashCount;

void setup() {
    sys.begin(); 
    Serial.begin(115200);

    // Load dev-mode flag early so GUI can decide whether to show boot logs.
    Preferences prefs;
    if (prefs.begin("rlamp", true)) {
        appState.devMode = prefs.getBool("dev", false);
        prefs.end();
    }

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
        xTaskCreatePinnedToCore(guiTask,     "GuiTask",   STACK_TASK_GUI,      NULL, PRIO_GUI,         NULL, 0);
        xTaskCreatePinnedToCore(inputTask,   "InputTask", STACK_TASK_INPUT,    NULL, PRIO_INPUT,       NULL, 1);
        xTaskCreatePinnedToCore(appTask,     "AppTask",   STACK_TASK_APP,      NULL, PRIO_SYSTEM,      NULL, 1);
        xTaskCreatePinnedToCore(espNowTask,  "EspTask",   STACK_TASK_ESPNOW,   NULL, PRIO_SYSTEM + 1,  NULL, 1);
        xTaskCreatePinnedToCore(batteryTask, "BatTask",   STACK_TASK_BATTERY,  NULL, 1,                NULL, 1);
    } else {
        Serial.println("FATAL: RTOS Resources creation failed!");
        ESP.restart(); // Nếu cạn RAM ngay lúc khởi động, nên reset
    }
}

void loop() { vTaskDelete(NULL); }