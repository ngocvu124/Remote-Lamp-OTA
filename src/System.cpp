#include "System.h"
#include "Display.h"
#include "Encoder.h"
#include "Storage.h"
#include "driver/rtc_io.h"
#include "driver/gpio.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

SystemLogic sys;
RemoteState appState;
char cachedSSID[32] = "Not configured";
RTC_DATA_ATTR int savedBrightness = 50;
RTC_DATA_ATTR int savedOledBrightness = 50;

void SystemLogic::begin() {
    Serial.begin(115200);
    gpio_hold_dis((gpio_num_t)SCR_BLK_PIN); 
    gpio_deep_sleep_hold_dis();

    // Log wake reason de phan biet wake bang nut hay reset USB.
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    Serial.printf("[SYS] Wake cause: %d\n", (int)cause);
    if (cause == ESP_SLEEP_WAKEUP_EXT1 || cause == ESP_SLEEP_WAKEUP_EXT0) {
        rtc_gpio_hold_dis((gpio_num_t)ROTARY_BTN_PIN);
        rtc_gpio_deinit((gpio_num_t)ROTARY_BTN_PIN); // Tra ve digital GPIO mode
        rtc_gpio_hold_dis((gpio_num_t)WAKE_AUX_PIN);
        rtc_gpio_deinit((gpio_num_t)WAKE_AUX_PIN);
        appState.brightness = savedBrightness;
        appState.oledBrightness = savedOledBrightness;
        uint64_t wakeMask = esp_sleep_get_ext1_wakeup_status();
        Serial.printf("[SYS] Wakeup: button mask=0x%llX\n", wakeMask);
    }
}

void SystemLogic::goToSleep() {
    savedBrightness = appState.brightness;
    savedOledBrightness = appState.oledBrightness;

    display.turnOff();

    // Tat WiFi va ESP-NOW truoc khi vao deep sleep.
    // Neu khong tắt, WiFi van chay (~150mA) va lam nong device trong khi ngu.
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();

    // KHONG goi sd_bg.end()/SPI.end() khi he thong con da task dang chay.
    // Vi viec teardown bus giua luc GUI/ISR dang su dung de gay WDT va stack canary.
    // Chi ep cac chan SPI ve idle an toan roi vao deep sleep ngay.
    pinMode(SD_CS_PIN, OUTPUT);    digitalWrite(SD_CS_PIN, HIGH);
    pinMode(SPI_SCK_PIN, OUTPUT);  digitalWrite(SPI_SCK_PIN, LOW);
    pinMode(SPI_MOSI_PIN, OUTPUT); digitalWrite(SPI_MOSI_PIN, LOW);

    // Tat den nen va giu pin o muc LOW trong suot thoi gian ngu.
    // gpio_deep_sleep_hold_en() chi anh huong digital GPIO (khong phai RTC GPIO),
    // nen wake pin (da chuyen sang RTC domain) khong bi dong bang.
    ledcDetachPin(SCR_BLK_PIN);
    pinMode(SCR_BLK_PIN, OUTPUT);
    digitalWrite(SCR_BLK_PIN, LOW);
    gpio_hold_en((gpio_num_t)SCR_BLK_PIN);
    gpio_deep_sleep_hold_en();

    // Cau hinh chan nut chinh (GPIO10) trong RTC domain.
    rtc_gpio_hold_dis((gpio_num_t)ROTARY_BTN_PIN); // Xoa trang thai hold cu (neu co)
    rtc_gpio_init((gpio_num_t)ROTARY_BTN_PIN);
    rtc_gpio_set_direction((gpio_num_t)ROTARY_BTN_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis((gpio_num_t)ROTARY_BTN_PIN);
    if (ROTARY_BTN_USE_PULLDOWN) {
        rtc_gpio_pulldown_en((gpio_num_t)ROTARY_BTN_PIN);
    } else {
        rtc_gpio_pullup_en((gpio_num_t)ROTARY_BTN_PIN);
    }

    // Cau hinh chan wake phu (GPIO12) de test loi phan cung nut/chân chinh.
    rtc_gpio_hold_dis((gpio_num_t)WAKE_AUX_PIN);
    rtc_gpio_init((gpio_num_t)WAKE_AUX_PIN);
    rtc_gpio_set_direction((gpio_num_t)WAKE_AUX_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis((gpio_num_t)WAKE_AUX_PIN);
    rtc_gpio_pullup_en((gpio_num_t)WAKE_AUX_PIN);

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    // EXT1 ANY_LOW: 1 trong 2 chan bi keo LOW se wake.
    uint64_t wakeMask = (1ULL << ROTARY_BTN_PIN) | (1ULL << WAKE_AUX_PIN);
    esp_err_t err = esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
    Serial.printf("[SYS] EXT1 setup: %s | mask=0x%llX (btn=%d aux=%d)\n",
        esp_err_to_name(err), wakeMask, ROTARY_BTN_PIN, WAKE_AUX_PIN);

    Serial.println("[SYS] Deep Sleep...");
    // KHONG dung Serial.flush() voi USB CDC: khi chay bang pin (khong co USB host),
    // flush() se block mai mai va esp_deep_sleep_start() khong bao gio duoc goi.
    delay(20);
    esp_deep_sleep_start();
}