#include "System.h"
#include "Display.h"
#include "Encoder.h"
#include "driver/rtc_io.h"
#include "driver/gpio.h" 

SystemLogic sys;
RemoteState appState;
char cachedSSID[32] = "Not configured";
RTC_DATA_ATTR int savedBrightness = 50;
RTC_DATA_ATTR int savedOledBrightness = 50;

void SystemLogic::begin() {
    Serial.begin(115200);
    gpio_hold_dis((gpio_num_t)SCR_BLK_PIN); 
    gpio_deep_sleep_hold_dis();

    // Neu wake tu deep sleep: giai phong RTC GPIO hold tren nut nhan truoc khi encoder dung no.
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_EXT1) {
        rtc_gpio_hold_dis((gpio_num_t)ROTARY_BTN_PIN);
        rtc_gpio_deinit((gpio_num_t)ROTARY_BTN_PIN); // Tra ve digital GPIO mode
        appState.brightness = savedBrightness;
        appState.oledBrightness = savedOledBrightness;
        Serial.println("[SYS] Wakeup: EXT1 (button)");
    }
}

void SystemLogic::goToSleep() {
    savedBrightness = appState.brightness;
    savedOledBrightness = appState.oledBrightness;

    display.turnOff();

    // Giu den nen tat hoan toan trong deep sleep.
    ledcDetachPin(SCR_BLK_PIN);
    pinMode(SCR_BLK_PIN, OUTPUT);
    digitalWrite(SCR_BLK_PIN, LOW);
    gpio_hold_en((gpio_num_t)SCR_BLK_PIN);
    gpio_deep_sleep_hold_en();

    // Chuyen GPIO nut nhan sang RTC domain de EXT1 hoat dong chinh xac.
    // gpio_deep_sleep_hold_en() chi hold digital GPIO, con EXT1 can RTC GPIO.
    // rtc_gpio_init() chuyen pin ra khoi GPIO matrix -> tranh xung dot.
    rtc_gpio_init((gpio_num_t)ROTARY_BTN_PIN);
    rtc_gpio_set_direction((gpio_num_t)ROTARY_BTN_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis((gpio_num_t)ROTARY_BTN_PIN);
    if (ROTARY_BTN_USE_PULLDOWN) {
        rtc_gpio_pulldown_en((gpio_num_t)ROTARY_BTN_PIN);
    } else {
        rtc_gpio_pullup_en((gpio_num_t)ROTARY_BTN_PIN);
    }
    rtc_gpio_hold_en((gpio_num_t)ROTARY_BTN_PIN); // Giu pull-up/down trong deep sleep

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    esp_err_t err = esp_sleep_enable_ext1_wakeup(
        (1ULL << ROTARY_BTN_PIN),
        ROTARY_BTN_PRESSED_LEVEL == LOW ? ESP_EXT1_WAKEUP_ANY_LOW : ESP_EXT1_WAKEUP_ANY_HIGH
    );
    Serial.printf("[SYS] EXT1 setup: %s | GPIO%d | mask=0x%llX\n",
        esp_err_to_name(err), ROTARY_BTN_PIN, (1ULL << ROTARY_BTN_PIN));

    Serial.println("[SYS] Deep Sleep...");
    Serial.flush();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();
}