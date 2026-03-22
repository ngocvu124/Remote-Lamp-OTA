#ifndef STOCK_H
#define STOCK_H

#include <Arduino.h>
#include <lvgl.h>
#include "Config.h"

class StockLogic {
public:
    void begin();
    void initChartStyle();
    void fetchAndUpdateUI(int tickerIndex);
    void getTickerName(int index, char* outName);

private:
    bool isChartInitialized = false;
    lv_chart_series_t * ce_series = NULL;
    lv_chart_series_t * fl_series = NULL;
    lv_chart_series_t * ref_series = NULL;
    lv_chart_series_t * stock_series = NULL;

    bool fetchYahooData(const char* ticker, float* historyData, int &last_id, float &currentPrice, float &refPrice, String &errorMsg);
    bool fetchBinanceData(const char* ticker, float* historyData, int &last_id, float &currentPrice, float &refPrice, String &errorMsg);
    void updateUILayout(float currentPrice, float refPrice, float* historyData, int last_id, bool success, const char* ticker, String errorMsg);
};

extern StockLogic stock;

#endif