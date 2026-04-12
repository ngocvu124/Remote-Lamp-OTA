#include "System.h"
#include "Display.h"
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
    gpio_hold_dis((gpio_num_t)ROTARY_BTN_PIN); // Mở khóa chân nút bấm sau khi thức dậy
    gpio_deep_sleep_hold_dis();
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1 || esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
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

    // Bật Pull-up và khóa cứng trạng thái nút bấm (hoạt động cho MỌI loại chân)
    pinMode(ROTARY_BTN_PIN, INPUT_PULLUP);
    gpio_hold_en((gpio_num_t)ROTARY_BTN_PIN);

    gpio_deep_sleep_hold_en(); 
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    uint64_t mask = (1ULL << ROTARY_BTN_PIN);
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    gpio_wakeup_enable((gpio_num_t)ROTARY_BTN_PIN, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    Serial.println("Deep Sleep...");
    Serial.flush();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();
}