#include "Stock.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "ui/ui.h" 
#include <math.h>
#include <stdio.h>
#include <esp_heap_caps.h>

StockLogic stock;
extern SemaphoreHandle_t xGuiSemaphore;

static bool current_ticker_is_gold = false; 
static bool current_ticker_is_crypto = false;

static float chart_min_val = 0.0;
static float chart_max_val = 100.0;

// CÚ CHỐT: Tạo bộ cấp phát đặc biệt ép thư viện JSON đẩy data vào PSRAM
struct SpiRamAllocator {
    void* allocate(size_t size) {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }
    void deallocate(void* pointer) {
        heap_caps_free(pointer);
    }
    void* reallocate(void* ptr, size_t new_size) {
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
    }
};
// Định nghĩa lại loại Document sử dụng bộ cấp phát PSRAM
using SpiRamJsonDocument = BasicJsonDocument<SpiRamAllocator>;


const char* MASTER_TICKER_LIST = 
    "-GAS-\n"
    "GAS\n"
    "PLX\n"
    "POW\n"
    "-DRILL-\n"
    "PVD\n"
    "PVS\n"
    "PVT\n"
    "-OIL-\n"
    "BSR\n"
    "OIL\n"
    "PVC\n"
    "-GOLD-\n"
    "GOLD\n"
    "-CRYPTO-\n"
    "BTCUSDT\n"
    "ETHUSDT\n"
    "BNBUSDT\n"
    "SOLUSDT\n"
    "DOGEUSDT";

struct MyAsset {
    const char* ticker;
    float volume;      
    float buyPrice;    
};

MyAsset myPortfolio[] = {
    {"POW", 425.0, 16130.0},  
    {"PVT", 5.0, 26780.0},
    {"BTCUSDT", 0.05, 62000.0} 
};
const int portfolioCount = sizeof(myPortfolio) / sizeof(myPortfolio[0]);

static void chart_draw_event_cb(lv_event_t * e) {
    lv_obj_draw_part_dsc_t * dsc = lv_event_get_draw_part_dsc(e);
    if(!lv_obj_draw_part_check_type(dsc, &lv_chart_class, LV_CHART_DRAW_PART_TICK_LABEL)) return;

    if(dsc->id == LV_CHART_AXIS_PRIMARY_X && dsc->text) {
        if (current_ticker_is_crypto) {
            if(dsc->value == 0) lv_snprintf(dsc->text, dsc->text_length, "-2h");
            else if(dsc->value == 1) lv_snprintf(dsc->text, dsc->text_length, "-1h");
            else if(dsc->value == 2) lv_snprintf(dsc->text, dsc->text_length, "-30m");
            else if(dsc->value == 3) lv_snprintf(dsc->text, dsc->text_length, "Now");
        } else if (current_ticker_is_gold) {
            if(dsc->value == 0) lv_snprintf(dsc->text, dsc->text_length, "-4.5h");
            else if(dsc->value == 1) lv_snprintf(dsc->text, dsc->text_length, "-3h");
            else if(dsc->value == 2) lv_snprintf(dsc->text, dsc->text_length, "-1.5h");
            else if(dsc->value == 3) lv_snprintf(dsc->text, dsc->text_length, "Now");
        } else {
            if(dsc->value == 0) lv_snprintf(dsc->text, dsc->text_length, "09:00");
            else if(dsc->value == 1) lv_snprintf(dsc->text, dsc->text_length, "10:30");
            else if(dsc->value == 2) lv_snprintf(dsc->text, dsc->text_length, "13:30");
            else if(dsc->value == 3) lv_snprintf(dsc->text, dsc->text_length, "15:00");
        }
    }
    else if(dsc->id == LV_CHART_AXIS_PRIMARY_Y && dsc->text) {
        float range = chart_max_val - chart_min_val;
        float real_val = chart_min_val + (dsc->value / 1000.0f) * range;
        
        char temp_buf[16]; 
        
        if (current_ticker_is_crypto) {
            if (real_val >= 10.0) sprintf(temp_buf, "%.1f", real_val);
            else sprintf(temp_buf, "%.3f", real_val);
        } else if (current_ticker_is_gold) {
            sprintf(temp_buf, "%.1f", real_val);
        } else {
            if (real_val >= 1000.0) sprintf(temp_buf, "%.1fk", real_val / 1000.0f);
            else sprintf(temp_buf, "%d", (int)real_val);
        }
        
        strncpy(dsc->text, temp_buf, dsc->text_length);
    }
}

void StockLogic::begin() {}

void StockLogic::getTickerName(int index, char* outName) {
    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(50))) {
        if (objects.stock_roller != NULL) {
            lv_roller_get_selected_str(objects.stock_roller, outName, 32);
        } else {
            strcpy(outName, "GAS");
        }
        xSemaphoreGive(xGuiSemaphore);
    }
}

void StockLogic::initChartStyle() {
    if (isChartInitialized || objects.stock_chart == NULL) return;

    if (objects.stock_roller != NULL) {
        lv_roller_set_options(objects.stock_roller, MASTER_TICKER_LIST, LV_ROLLER_MODE_NORMAL);
    }

    lv_chart_set_axis_tick(objects.stock_chart, LV_CHART_AXIS_PRIMARY_Y, 4, 2, 5, 1, true, 45); 
    lv_chart_set_axis_tick(objects.stock_chart, LV_CHART_AXIS_PRIMARY_X, 3, 2, 4, 1, true, 20);

    lv_obj_set_style_text_font(objects.stock_chart, &lv_font_montserrat_10, LV_PART_TICKS); 
    lv_obj_set_style_text_color(objects.stock_chart, lv_color_hex(0xaaaaaa), LV_PART_TICKS); 
    lv_chart_set_div_line_count(objects.stock_chart, 5, 4); 
    lv_obj_set_style_line_color(objects.stock_chart, lv_color_hex(0x333333), LV_PART_MAIN); 
    lv_obj_set_style_line_opa(objects.stock_chart, 150, LV_PART_MAIN); 

    lv_obj_set_style_size(objects.stock_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(objects.stock_chart, 0, LV_PART_ITEMS | LV_STATE_DEFAULT); 
    lv_obj_set_style_line_width(objects.stock_chart, 2, LV_PART_ITEMS);

    lv_obj_add_event_cb(objects.stock_chart, chart_draw_event_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    ce_series = lv_chart_add_series(objects.stock_chart, lv_color_hex(0xFF00FF), LV_CHART_AXIS_PRIMARY_Y);
    fl_series = lv_chart_add_series(objects.stock_chart, lv_color_hex(0x00FFFF), LV_CHART_AXIS_PRIMARY_Y);
    ref_series = lv_chart_add_series(objects.stock_chart, lv_color_hex(0xFFD700), LV_CHART_AXIS_PRIMARY_Y); 
    stock_series = lv_chart_add_series(objects.stock_chart, lv_palette_main(LV_PALETTE_GREEN), LV_CHART_AXIS_PRIMARY_Y);
    
    isChartInitialized = true;
}

bool StockLogic::fetchYahooData(const char* ticker, float* historyData, int &last_id, float &currentPrice, float &refPrice, String &errorMsg) {
    HTTPClient http;
    char url[256];
    bool isGold = (strcmp(ticker, "GOLD") == 0);
    
    if (isGold) sprintf(url, "https://query1.finance.yahoo.com/v8/finance/chart/GC=F?range=1d&interval=5m");
    else sprintf(url, "https://query1.finance.yahoo.com/v8/finance/chart/%s.VN?range=1d&interval=1m", ticker);
    
    Serial.printf("[HTTP] GET %s\n", url);
    http.useHTTP10(true);
    http.begin(url);
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
        // Dùng SpiRamJsonDocument để ăn trọn PSRAM thay vì SRAM nội
        SpiRamJsonDocument doc(65536); 
        DeserializationError error = deserializeJson(doc, http.getStream());
        
        if (!error) {
            refPrice = doc["chart"]["result"][0]["meta"]["chartPreviousClose"].as<float>();
            currentPrice = doc["chart"]["result"][0]["meta"]["regularMarketPrice"].as<float>();
            JsonArray closeArray = doc["chart"]["result"][0]["indicators"]["quote"][0]["close"].as<JsonArray>();
            
            int dataPoints = closeArray.size();
            last_id = (dataPoints > 271) ? 270 : dataPoints - 1;
            for(int i = 0; i < 271; i++) historyData[i] = -1.0;
            
            int startIdx = (dataPoints > 271) ? (dataPoints - 271) : 0;
            float current_fill = refPrice; 
            
            for(int i = 0; i <= last_id; i++) {
                if(!closeArray[startIdx + i].isNull()) {
                    current_fill = closeArray[startIdx + i].as<float>();
                }
                historyData[i] = current_fill; 
            }
            http.end(); return true;
        } else { 
            errorMsg = "JSON Err"; 
            Serial.printf("[JSON] Yahoo Parse failed: %s\n", error.c_str()); 
        }
    } else { 
        errorMsg = "HTTP Err " + String(httpCode);
        Serial.printf("[HTTP] Failed, code: %d\n", httpCode);
    }
    http.end(); return false;
}

bool StockLogic::fetchBinanceData(const char* ticker, float* historyData, int &last_id, float &currentPrice, float &refPrice, String &errorMsg) {
    HTTPClient http;
    char url[256];
    sprintf(url, "https://api.binance.com/api/v3/klines?symbol=%s&interval=1m&limit=120", ticker);
    Serial.printf("[HTTP] GET %s\n", url);
    
    http.useHTTP10(true);
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        // Dùng SpiRamJsonDocument để ăn trọn PSRAM
        SpiRamJsonDocument doc(49152);
        DeserializationError error = deserializeJson(doc, http.getStream());
        
        if (!error) {
            JsonArray array = doc.as<JsonArray>();
            last_id = array.size() - 1;
            for(int i = 0; i < 271; i++) historyData[i] = -1.0;
            
            float current_fill = 0.0;
            for (int i = 0; i <= last_id; i++) {
                if (!array[i][4].isNull()) current_fill = array[i][4].as<float>();
                historyData[i] = current_fill;
                if (i == 0) refPrice = array[i][1].as<float>(); 
            }
            currentPrice = historyData[last_id];
            http.end(); return true;
        } else { 
            errorMsg = "JSON Err"; 
            Serial.printf("[JSON] Binance Parse failed: %s\n", error.c_str()); 
        }
    } else { 
        errorMsg = "Net Err";
        Serial.printf("[HTTP] Failed, code: %d\n", httpCode); 
    }
    http.end(); return false;
}

void StockLogic::fetchAndUpdateUI(int tickerIndex) {
    char ticker[32];
    getTickerName(tickerIndex, ticker);
    if (ticker[0] == '-') return;

    Serial.printf("[Stock] Syncing: %s\n", ticker);
    float chartData[271]; 
    int last_id = -1;
    float currentPrice = 0.0, refPrice = 0.0;
    bool success = false;
    String errorMsg = "";

    if (strstr(ticker, "USDT")) success = this->fetchBinanceData(ticker, chartData, last_id, currentPrice, refPrice, errorMsg);
    else success = this->fetchYahooData(ticker, chartData, last_id, currentPrice, refPrice, errorMsg);

    if (xSemaphoreTake(xGuiSemaphore, pdMS_TO_TICKS(100))) {
        this->updateUILayout(currentPrice, refPrice, chartData, last_id, success, ticker, errorMsg);
        xSemaphoreGive(xGuiSemaphore);
    }
}

void StockLogic::updateUILayout(float currentPrice, float refPrice, float* historyData, int last_id, bool success, const char* ticker, String errorMsg) {
    current_ticker_is_crypto = (strstr(ticker, "USDT") != NULL);
    current_ticker_is_gold = (strcmp(ticker, "GOLD") == 0);

    if (!success || refPrice == 0 || currentPrice == 0) {
        lv_label_set_text(objects.stock_status, errorMsg.c_str());
        return;
    }

    initChartStyle();
    
    if (objects.stock_price != NULL) {
        if (current_ticker_is_crypto) {
            lv_label_set_text(objects.stock_price, "USDT");
            lv_obj_set_style_text_color(objects.stock_price, lv_color_hex(0x00d1ff), 0);
        } else if (current_ticker_is_gold) {
            lv_label_set_text(objects.stock_price, "USD/oz");
            lv_obj_set_style_text_color(objects.stock_price, lv_color_hex(0xFFD700), 0);
        } else {
            lv_label_set_text(objects.stock_price, "VND");
            lv_obj_set_style_text_color(objects.stock_price, lv_color_hex(0xfff7ff00), 0);
        }
    }

    float priceChange = currentPrice - refPrice;
    lv_color_t main_color = (priceChange >= 0) ? lv_color_hex(0x00ff0f) : lv_palette_main(LV_PALETTE_RED);

    char priceStr[32];
    if (current_ticker_is_crypto) {
        if (currentPrice >= 10.0) sprintf(priceStr, "%.2f", currentPrice);        
        else if (currentPrice >= 0.1) sprintf(priceStr, "%.4f", currentPrice);    
        else sprintf(priceStr, "%.6f", currentPrice);                             
    } else {
        sprintf(priceStr, "%d", (int)currentPrice); 
    }
    lv_label_set_text(objects.stock_price_value, priceStr);
    lv_obj_set_style_text_color(objects.stock_price_value, main_color, 0);

    if (objects.percent != NULL) {
        float percentDaily = 0.0;
        if (refPrice > 0) percentDaily = (priceChange / refPrice) * 100.0;
        char percentStr[16];
        if (percentDaily >= 0) sprintf(percentStr, "+%.2f%%", percentDaily);
        else sprintf(percentStr, "%.2f%%", percentDaily); 
        lv_label_set_text(objects.percent, percentStr);
        lv_obj_set_style_text_color(objects.percent, main_color, 0);
    }

    float pnl = 0.0;
    bool isOwned = false;
    for (int i = 0; i < portfolioCount; i++) {
        if (strcmp(ticker, myPortfolio[i].ticker) == 0 && myPortfolio[i].volume > 0.0) {
            pnl = (currentPrice - myPortfolio[i].buyPrice) * myPortfolio[i].volume;
            isOwned = true; break;
        }
    }

    if (isOwned) {
        char diffStr[32];
        char pnlTrend[5] = "";
        if (pnl >= 0) strcpy(pnlTrend, "+"); else strcpy(pnlTrend, "-");
        float absPnl = fabs(pnl);
        
        if (current_ticker_is_crypto) {
            sprintf(diffStr, "%s$%.2f", pnlTrend, absPnl);
        } else {
            if (absPnl >= 1000000.0) sprintf(diffStr, "%s%.1fM", pnlTrend, absPnl / 1000000.0);
            else if (absPnl >= 1000.0) sprintf(diffStr, "%s%.1fK", pnlTrend, absPnl / 1000.0);
            else sprintf(diffStr, "%s%d", pnlTrend, (int)absPnl);
        }
        lv_label_set_text(objects.profit_loss_value, diffStr);
        lv_obj_set_style_text_color(objects.profit_loss_value, (pnl >= 0) ? lv_color_hex(0x00ff0f) : lv_palette_main(LV_PALETTE_RED), 0);
    } else {
        lv_label_set_text(objects.profit_loss_value, "N/A");
        lv_obj_set_style_text_color(objects.profit_loss_value, lv_color_hex(0xaaaaaa), 0);
    }

    float minVal = 999999999.0, maxVal = 0.0;
    for(int i = 0; i <= last_id; i++) {
        if(historyData[i] > 0) {
            if(historyData[i] < minVal) minVal = historyData[i];
            if(historyData[i] > maxVal) maxVal = historyData[i];
        }
    }
    if(refPrice < minVal) minVal = refPrice;
    if(refPrice > maxVal) maxVal = refPrice;

    float margin = (maxVal - minVal) * 0.15;
    if (margin == 0) margin = currentPrice * 0.001; 
    minVal -= margin; maxVal += margin;

    chart_min_val = minVal;
    chart_max_val = maxVal;

    lv_chart_set_range(objects.stock_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    int totalPoints = current_ticker_is_crypto ? 120 : 271;
    lv_chart_set_point_count(objects.stock_chart, totalPoints);

    int mappedCE = 0, mappedFL = 0;
    if (!current_ticker_is_crypto && !current_ticker_is_gold) {
        float bien_do = 0.07;
        if (strstr(ticker, "BSR") || strstr(ticker, "OIL")) bien_do = 0.15;
        else if (strstr(ticker, "PVS") || strstr(ticker, "PVC")) bien_do = 0.10;
        mappedCE = (int)((((refPrice * (1.0 + bien_do)) - minVal) / (maxVal - minVal)) * 1000.0);
        mappedFL = (int)((((refPrice * (1.0 - bien_do)) - minVal) / (maxVal - minVal)) * 1000.0);
    }

    for (int i = 0; i < totalPoints; i++) {
        int mappedRef = (int)(((refPrice - minVal) / (maxVal - minVal)) * 1000.0);
        lv_chart_set_value_by_id(objects.stock_chart, ref_series, i, mappedRef);
        
        if (!current_ticker_is_crypto && !current_ticker_is_gold) {
            lv_chart_set_value_by_id(objects.stock_chart, ce_series, i, mappedCE);
            lv_chart_set_value_by_id(objects.stock_chart, fl_series, i, mappedFL);
        }

        if (i <= last_id && historyData[i] > 0) {
            int mappedVal = (int)(((historyData[i] - minVal) / (maxVal - minVal)) * 1000.0);
            if(mappedVal < 0) mappedVal = 0;
            if(mappedVal > 1000) mappedVal = 1000;
            lv_chart_set_value_by_id(objects.stock_chart, stock_series, i, mappedVal);
        } else {
            lv_chart_set_value_by_id(objects.stock_chart, stock_series, i, LV_CHART_POINT_NONE);
        }
    }
    
    lv_chart_set_series_color(objects.stock_chart, stock_series, main_color);
    lv_obj_set_style_line_color(objects.stock_chart, main_color, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_chart_refresh(objects.stock_chart);

    char statusStr[32];
    sprintf(statusStr, isOwned ? "My Bag (%d)" : "Market (%d)", last_id);
    lv_label_set_text(objects.stock_status, statusStr);
    lv_obj_set_style_text_color(objects.stock_status, lv_color_hex(0x888888), 0);
}