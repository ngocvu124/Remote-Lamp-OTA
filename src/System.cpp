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
    
    // 1. Tắt khóa tổng Deep Sleep
    gpio_deep_sleep_hold_dis();

    // 2. MỞ KHÓA CHO TẤT CẢ CÁC CHÂN ĐÃ BỊ "ĐÓNG BĂNG" LÚC NGỦ
    gpio_hold_dis((gpio_num_t)SCR_BLK_PIN); 
    gpio_hold_dis((gpio_num_t)SD_CS_PIN);
    gpio_hold_dis((gpio_num_t)SPI_SCK_PIN);
    gpio_hold_dis((gpio_num_t)SPI_MOSI_PIN);
    gpio_hold_dis((gpio_num_t)SPI_MISO_PIN);
    gpio_hold_dis((gpio_num_t)TFT_CS);
    gpio_hold_dis((gpio_num_t)TFT_DC);
    gpio_hold_dis((gpio_num_t)TFT_RST);

    // Log wake reason
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

    // Đảm bảo bên trong hàm này có gửi mã lệnh Sleep In (0x10) cho IC màn hình
    display.turnOff();

    // Tắt WiFi và ESP-NOW trước khi ngủ
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();

    // Sync thẻ nhớ
    storage.safeSync(appState);
    vTaskDelay(pdMS_TO_TICKS(50));

    // --- XỬ LÝ CHỐNG RÒ ĐIỆN QUA BUS SPI VÀ TFT ---
    
    // 1. Ép thẻ SD ngưng hoạt động
    pinMode(SD_CS_PIN, OUTPUT);    
    digitalWrite(SD_CS_PIN, HIGH);
    
    // 2. Ép các chân SPI bus về idle
    pinMode(SPI_SCK_PIN, OUTPUT);  
    digitalWrite(SPI_SCK_PIN, LOW);
    pinMode(SPI_MOSI_PIN, OUTPUT); 
    digitalWrite(SPI_MOSI_PIN, LOW);
    
    // 3. Kéo MISO lên cao chống thả nổi
    pinMode(SPI_MISO_PIN, INPUT_PULLUP); 

    // 4. Xử lý các chân điều khiển riêng của TFT (CS, DC, RST từ file config)
    pinMode(TFT_CS, OUTPUT);  digitalWrite(TFT_CS, HIGH); // Ngắt chọn chip màn hình
    pinMode(TFT_DC, OUTPUT);  digitalWrite(TFT_DC, LOW);
    pinMode(TFT_RST, OUTPUT); digitalWrite(TFT_RST, HIGH); // Giữ mức HIGH để IC không bị reset liên tục

    // 5. KHÓA TẤT CẢ CÁC CHÂN NÀY LẠI
    gpio_hold_en((gpio_num_t)SD_CS_PIN);
    gpio_hold_en((gpio_num_t)SPI_SCK_PIN);
    gpio_hold_en((gpio_num_t)SPI_MOSI_PIN);
    gpio_hold_en((gpio_num_t)SPI_MISO_PIN);
    gpio_hold_en((gpio_num_t)TFT_CS);
    gpio_hold_en((gpio_num_t)TFT_DC);
    gpio_hold_en((gpio_num_t)TFT_RST);

    // --- XỬ LÝ ĐÈN NỀN ---
    ledcDetachPin(SCR_BLK_PIN);
    pinMode(SCR_BLK_PIN, OUTPUT);
    digitalWrite(SCR_BLK_PIN, LOW);
    gpio_hold_en((gpio_num_t)SCR_BLK_PIN);
    
    // Kích hoạt khóa tổng
    gpio_deep_sleep_hold_en();

    // --- CẤU HÌNH WAKEUP BẰNG NÚT NHẤN (RTC DOMAIN) ---
    rtc_gpio_hold_dis((gpio_num_t)ROTARY_BTN_PIN);
    rtc_gpio_init((gpio_num_t)ROTARY_BTN_PIN);
    rtc_gpio_set_direction((gpio_num_t)ROTARY_BTN_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis((gpio_num_t)ROTARY_BTN_PIN);
    if (ROTARY_BTN_USE_PULLDOWN) {
        rtc_gpio_pulldown_en((gpio_num_t)ROTARY_BTN_PIN);
    } else {
        rtc_gpio_pullup_en((gpio_num_t)ROTARY_BTN_PIN);
    }

    rtc_gpio_hold_dis((gpio_num_t)WAKE_AUX_PIN);
    rtc_gpio_init((gpio_num_t)WAKE_AUX_PIN);
    rtc_gpio_set_direction((gpio_num_t)WAKE_AUX_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis((gpio_num_t)WAKE_AUX_PIN);
    rtc_gpio_pullup_en((gpio_num_t)WAKE_AUX_PIN);

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    uint64_t wakeMask = (1ULL << ROTARY_BTN_PIN) | (1ULL << WAKE_AUX_PIN);
    esp_err_t err = esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
    Serial.printf("[SYS] EXT1 setup: %s | mask=0x%llX (btn=%d aux=%d)\n",
        esp_err_to_name(err), wakeMask, ROTARY_BTN_PIN, WAKE_AUX_PIN);

    Serial.println("[SYS] Deep Sleep...");
    delay(20);
    esp_deep_sleep_start();
}