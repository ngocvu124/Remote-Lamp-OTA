#include "System.h"
#include "Display.h"
#include "driver/rtc_io.h"
#include "driver/gpio.h" 

SystemLogic sys;
RemoteState appState;

RTC_DATA_ATTR int savedBrightness = 50;
RTC_DATA_ATTR int savedOledBrightness = 50;

void SystemLogic::begin() {
    Serial.begin(115200);
    gpio_hold_dis((gpio_num_t)SCR_BLK_PIN); 
    gpio_deep_sleep_hold_dis();
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
        appState.brightness = savedBrightness;
        appState.oledBrightness = savedOledBrightness;
    }
}

void SystemLogic::goToSleep() {
    savedBrightness = appState.brightness;
    savedOledBrightness = appState.oledBrightness;

    display.turnOff();
    ledcDetachPin(SCR_BLK_PIN); 
    pinMode(SCR_BLK_PIN, OUTPUT);
    digitalWrite(SCR_BLK_PIN, LOW); 
    gpio_hold_en((gpio_num_t)SCR_BLK_PIN); 
    gpio_deep_sleep_hold_en(); 
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    rtc_gpio_pullup_en((gpio_num_t)ROTARY_BTN_PIN);
    rtc_gpio_pulldown_dis((gpio_num_t)ROTARY_BTN_PIN);
    uint64_t mask = (1ULL << ROTARY_BTN_PIN);
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    Serial.println("Deep Sleep...");
    Serial.flush();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();
}