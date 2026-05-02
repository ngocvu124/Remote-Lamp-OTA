#include "Display.h"
#include "ui/ui.h"      
#include "ui/screens.h" 
#include "Storage.h" 
#include "Ota.h" 
#include <esp_heap_caps.h>
#include <SdFat.h> 
#include "System.h"

TFT_eSPI tft = TFT_eSPI();
DisplayLogic display;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = NULL; 
static lv_img_dsc_t custom_bg;
static uint8_t* bg_data_buffer = NULL;
SdFs sd_bg; 

static lv_obj_t* scr_image_preview = NULL; 
static uint8_t* preview_data_buffer = NULL; 
static lv_img_dsc_t preview_img_dsc;        
static lv_obj_t* preview_img_obj = NULL;    

static lv_obj_t* g_menuBtns[30]; 

#define BACKLIGHT_CHANNEL 0

static const size_t FRAME_BYTES = SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(lv_color_t);

static size_t readFully(FsFile& file, uint8_t* dst, size_t bytesToRead) {
    size_t total = 0;
    while (total < bytesToRead) {
        int n = file.read(dst + total, bytesToRead - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    return total;
}

// =============================================
// BOOT LOG — dùng tft trực tiếp, không qua LVGL
// =============================================
static int bootY = 0;
static const int BOOT_LINE_H = 14;

void DisplayLogic::bootPrint(const char* tag, const char* msg, bool ok) {
    if (bootY > 220) return; // tràn màn hình thì thôi

    tft.setTextSize(1);
    tft.setTextWrap(false);

    // [TAG] màu cyan
    tft.setTextColor(TFT_CYAN);
    tft.setCursor(5, bootY);
    tft.print("[");
    tft.print(tag);
    tft.print("] ");

    // Nội dung màu trắng
    tft.setTextColor(TFT_WHITE);
    tft.print(msg);
    tft.print("... ");

    // OK/FAIL
    if (ok) {
        tft.setTextColor(TFT_GREEN);
        tft.print("OK");
    } else {
        tft.setTextColor(TFT_RED);
        tft.print("FAIL");
    }

    bootY += BOOT_LINE_H;
}

// =============================================

const char* mainMenuItems[] = {"1. Control Set", "2. Lamp Set", "3. Stock Monitor", "4. OTA Update", "5. Web Server", "6. Exit"}; 
const char* controlMenuItems[] = {"1. Sleep Time", "2. Backlight", "3. Reset WiFi", "4. Change BG", "5. About", "6. WiFi Setup", "7. Back"}; 
const char* lampMenuItems[] = {"1. Restart", "2. Unpair", "3. Del WiFi", "4. Reset", "5. Back"};

extern "C" void action_on_stock_changed_cb(lv_event_t * e);

void DisplayLogic::my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    tft.pushImage(area->x1, area->y1, w, h, (uint16_t *)&color_p->full);
    lv_disp_flush_ready(disp);
}

void DisplayLogic::my_indev_read(lv_indev_drv_t * drv, lv_indev_data_t*data){
    // Dummy read: Luôn trả về trạng thái nhả nút và không xoay.
    // Việc này giúp thỏa mãn code sinh tự động của UI Builder (tránh crash),
    // đồng thời không gây xung đột sự kiện với luồng xử lý riêng của AppTask.
    data->state = LV_INDEV_STATE_RELEASED;
    data->enc_diff = 0; 
}

void DisplayLogic::begin() {
    // ── Phase 1: Khởi tạo phần cứng màn hình ──────────────────
    pinMode(SCR_BLK_PIN, OUTPUT);
    ledcSetup(BACKLIGHT_CHANNEL, 5000, 8);
    ledcAttachPin(SCR_BLK_PIN, BACKLIGHT_CHANNEL);
    ledcWrite(BACKLIGHT_CHANNEL, 255);      

    tft.begin();
    tft.setSwapBytes(true); 
    tft.setRotation(2); 
    tft.invertDisplay(true);
    tft.fillScreen(TFT_BLACK);
    
    // Vẽ header boot screen
    bootY = 8;
    tft.setTextSize(1);
    tft.setTextColor(TFT_ORANGE);
    tft.setCursor(5, bootY);
    tft.print("Remote Lamp ");
    tft.print(FIRMWARE_VERSION);
    bootY += BOOT_LINE_H;

    tft.setTextColor(0x4208); // xám tối — đường kẻ phân cách
    tft.setCursor(5, bootY);
    tft.print("--------------------------------");
    bootY += BOOT_LINE_H + 2;

    // Log các bước khởi tạo phần cứng
    bootPrint("SYS",  "Booting system");
    bootPrint("GPIO", "Initializing pins");
    bootPrint("SPI",  "Display driver loaded");
    
    Serial.printf("\n[SYS] PSRAM Total: %d, Free: %d\n", ESP.getPsramSize(), ESP.getFreePsram());
    if (ESP.getPsramSize() == 0) {
        Serial.println("[WARNING] PSRAM INIT FAILED! Falling back to Internal SRAM.");
    }
    

    // ── Phase 2: Khởi tạo LVGL ────────────────────────────────
    bootPrint("LVGL", "Init graphics engine");

    lv_init();

    // Uu tien bo dem full-frame trong PSRAM de render tron man hinh moi lan flush.
    uint32_t drawBufPixels = SCREEN_WIDTH * SCREEN_HEIGHT;
    buf1 = (lv_color_t*)heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    // Fallback: neu PSRAM khong du, quay ve bo dem nho (40 lines) de he thong van chay.
    if (!buf1) {
        drawBufPixels = SCREEN_WIDTH * 40;
        buf1 = (lv_color_t*)heap_caps_malloc(drawBufPixels * sizeof(lv_color_t), MALLOC_CAP_INTERNAL);
    }
    if (!buf1) buf1 = (lv_color_t*)malloc(drawBufPixels * sizeof(lv_color_t)); 
    if (!buf1) {
        Serial.println("\n[FATAL] lv_disp_draw_buf_init FAILED! No RAM for buf1.");
        delay(2000); ESP.restart();
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, drawBufPixels);
    Serial.printf("[DISPLAY] Draw buffer: %lu bytes (%s)\n",
                  (unsigned long)(drawBufPixels * sizeof(lv_color_t)),
                  (drawBufPixels == (SCREEN_WIDTH * SCREEN_HEIGHT)) ? "PSRAM full-frame" : "fallback");

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_WIDTH;
    disp_drv.ver_res = SCREEN_HEIGHT;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_t * disp = lv_disp_drv_register(&disp_drv);
    if (!disp) {
        Serial.println("\n[FATAL] lv_disp_drv_register FAILED! Memory pool exhausted.");
        delay(2000); ESP.restart();
    }

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_ENCODER;
    indev_drv.read_cb = my_indev_read;
    lv_indev_t * indev_encoder = lv_indev_drv_register(&indev_drv);

    // TẠO GROUP MẶC ĐỊNH: Bắt buộc phải có để tránh lỗi StoreProhibited!
    // LVGL và UI Builder sẽ tự động gán các widget cần tương tác vào group này.
    lv_group_t * g = lv_group_create();
    
    lv_group_set_default(g);
    lv_indev_set_group(indev_encoder, g);
    

    // ── Phase 3: Tạo màn hình LVGL ────────────────────────────
    bootPrint("UI", "Building screens");
    ui_init(); // Dùng ui_init() để nạp đầy đủ Theme và Fonts mặc định
    
    if (objects.stock_roller != NULL) {
        lv_obj_add_event_cb(objects.stock_roller, action_on_stock_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
    
    scr_image_preview = lv_obj_create(NULL);
    
    lv_obj_set_style_bg_color(scr_image_preview, lv_color_black(), 0);

    bootPrint("UI", "Screens ready");

    Serial.println("[DISPLAY] LVGL UI Initialized.");
    // Phần còn lại (SD, CFG, APP) sẽ do appTask gọi bootPrint() tiếp
}


void DisplayLogic::loadBackgroundFromSD() {
    if (!storage.isReady) return; // Thêm dòng này để chống crash nếu thẻ nhớ chưa mount hoặc bị lỏng

    digitalWrite(SCR_CS_PIN, HIGH);
    FsFile file = sd_bg.open(appState.bgFilePath, O_RDONLY);
    if (!file) {
        sd_bg.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
        file = sd_bg.open(appState.bgFilePath, O_RDONLY);
    }
    if (!file) file = sd_bg.open("/bg.bin", O_RDONLY);
    if (!file) return;

    size_t fileSize = file.size();
    if (fileSize < FRAME_BYTES) {
        Serial.printf("[DISPLAY] BG file too small: %lu bytes\n", (unsigned long)fileSize);
        file.close();
        return;
    }

    if (bg_data_buffer != NULL) { 
        free(bg_data_buffer); 
        bg_data_buffer = NULL; 
    }

    size_t allocSize = FRAME_BYTES; // Full frame 240x240 RGB565

    Serial.printf("[DISPLAY] BG Alloc: %d bytes\n", allocSize);
    bg_data_buffer = (uint8_t*)heap_caps_calloc(1, allocSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bg_data_buffer) Serial.println("[DISPLAY] BG Alloc FAIL: PSRAM not available or full!");
    
    if (bg_data_buffer) {
        size_t totalRead = readFully(file, bg_data_buffer, allocSize);

        if (totalRead == allocSize) { 
            custom_bg.header.always_zero = 0;
            custom_bg.header.w = 240;
            custom_bg.header.h = 240;
            custom_bg.header.cf = LV_IMG_CF_TRUE_COLOR;
            custom_bg.data_size = allocSize;
            custom_bg.data = bg_data_buffer;
            
            lv_img_cache_invalidate_src(NULL);
            
            if (objects.main) {
                lv_obj_set_style_bg_img_src(objects.main, NULL, 0);
                lv_obj_set_style_bg_img_src(objects.main, &custom_bg, 0);
                lv_obj_set_style_bg_color(objects.main, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(objects.main, 255, 0); // Bắt buộc phải là 255 (LV_OPA_COVER) để xóa màn hình cũ
                lv_obj_invalidate(objects.main);
            }
            if (objects.menu) {
                lv_obj_set_style_bg_img_src(objects.menu, NULL, 0);
                lv_obj_set_style_bg_img_src(objects.menu, &custom_bg, 0);
                lv_obj_set_style_bg_color(objects.menu, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(objects.menu, 255, 0);
                lv_obj_invalidate(objects.menu);
            }
            if (objects.stock) {
                lv_obj_set_style_bg_img_src(objects.stock, NULL, 0);
                lv_obj_set_style_bg_img_src(objects.stock, &custom_bg, 0);
                lv_obj_set_style_bg_color(objects.stock, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(objects.stock, 255, 0);
                lv_obj_invalidate(objects.stock);
            }
        } else {
            Serial.printf("[DISPLAY] BG read short: %lu/%lu bytes\n",
                          (unsigned long)totalRead, (unsigned long)allocSize);
        }
    }
    file.close();
}

uint32_t DisplayLogic::loop() { return lv_timer_handler(); }

void DisplayLogic::buildMenu(const char* items[], int count) {
    lv_obj_clean(objects.cont_menu_text); 
    currentMenuCount = count;
    
    for(int i = 0; i < 30; i++) g_menuBtns[i] = NULL;
    
    for(int i = 0; i < count; i++) {
        lv_obj_t * btn = lv_btn_create(objects.cont_menu_text);
        lv_obj_set_size(btn, 220, 35); 
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), LV_PART_MAIN); 
        lv_obj_set_style_bg_opa(btn, 150, LV_PART_MAIN); 
        lv_obj_set_style_text_color(btn, lv_color_white(), LV_PART_MAIN);
        
        lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_ORANGE), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(btn, 255, LV_STATE_CHECKED); 
        lv_obj_set_style_text_color(btn, lv_color_black(), LV_STATE_CHECKED);
        
        lv_obj_t * label = lv_label_create(btn);
        lv_label_set_text(label, items[i]);
        lv_obj_center(label); 
        
        g_menuBtns[i] = btn;
    }
}

void DisplayLogic::forceRebuild() { lastMenuType = (MenuLevel)-1; }

void DisplayLogic::updateUI(RemoteState &state) {
    // Bỏ qua việc ghi đè màn hình nếu đang ở chế độ xem trước ảnh (preview)
    if (scr_image_preview != NULL && lv_scr_act() == scr_image_preview) {
        return;
    }

    if (state.currentMenu == MENU_STOCK) {
        if (lv_scr_act() != objects.stock) { 
            lv_scr_load(objects.stock); 
            lv_obj_invalidate(objects.stock); // Ép vẽ lại toàn bộ màn hình mới
            lastMenuType = (MenuLevel)-1; 
        }
        if (objects.stock_roller != NULL) lv_roller_set_selected(objects.stock_roller, state.stockIndex, LV_ANIM_ON);
        return;
    }

    if (state.currentMenu == MENU_NONE || state.currentMenu == MENU_SET_SLEEP || state.currentMenu == MENU_SET_BACKLIGHT) {
        if (lv_scr_act() != objects.main) { 
            lv_scr_load(objects.main); 
            lv_obj_invalidate(objects.main); // Ép vẽ lại toàn bộ màn hình mới
            lastMenuType = (MenuLevel)-1; 
        }
        if (objects.ui_batbar) lv_bar_set_value(objects.ui_batbar, state.batteryLevel, LV_ANIM_ON);
        if (objects.bat_value) lv_label_set_text_fmt(objects.bat_value, "%d%%", state.batteryLevel);
        
        int val = 0; const char* title = ""; lv_color_t arc_color;

        if (state.currentMenu == MENU_SET_SLEEP) {
            val = (state.sleepTimeout - 30) * 100 / (300 - 30);
            if (objects.value) lv_label_set_text_fmt(objects.value, "%ds", state.sleepTimeout);
            title = "SLEEP TIMER"; 
            arc_color = lv_palette_main(LV_PALETTE_CYAN);
        } else if (state.currentMenu == MENU_SET_BACKLIGHT) {
            val = state.oledBrightness;
            if (objects.value) lv_label_set_text_fmt(objects.value, "%d%%", val);
            title = "BACKLIGHT"; 
            arc_color = lv_palette_main(LV_PALETTE_PURPLE);
        } else {
            val = state.isTempMode ? state.temperature : state.brightness;
            
            if (state.isTempMode) {
                int kelvin = map(val, 0, 100, 2700, 6500); 
                if (objects.value) lv_label_set_text_fmt(objects.value, "%dK", kelvin);
            } else {
                if (objects.value) lv_label_set_text_fmt(objects.value, "%d%%", val);
            }
            
            title = state.isTempMode ? "COLOR TEMP" : "BRIGHTNESS";
            arc_color = state.isTempMode ? lv_palette_main(LV_PALETTE_ORANGE) : lv_color_hex(0xffff7200);
        }

        if (objects.arc_value) {
            lv_bar_set_value(objects.arc_value, val, LV_ANIM_ON);
            lv_obj_set_style_bg_color(objects.arc_value, arc_color, LV_PART_INDICATOR);
        }
        if (objects.label_value) lv_label_set_text(objects.label_value, title);
    } 
    else {
        if (lv_scr_act() != objects.menu) {
            lv_scr_load(objects.menu);
            lv_obj_invalidate(objects.menu); // Ép vẽ lại toàn bộ màn hình mới
        }

        if (state.currentMenu != lastMenuType) {
            if (state.currentMenu == MENU_MAIN) {
                lv_label_set_text(objects.label_menu, "Main Menu"); buildMenu(mainMenuItems, 6); 
            } 
            else if (state.currentMenu == MENU_CONTROL) {
                lv_label_set_text(objects.label_menu, "Control Setup"); buildMenu(controlMenuItems, 6); 
            }
            else if (state.currentMenu == MENU_LAMP) {
                lv_label_set_text(objects.label_menu, "Lamp Setup"); buildMenu(lampMenuItems, 5);
            }
            else if (state.currentMenu == MENU_OTA || state.currentMenu == MENU_SELECT_BG) {
                const char* items[30]; 
                int count = 0;
                
                if (state.currentMenu == MENU_OTA) {
                    lv_label_set_text(objects.label_menu, "Select Version");
                    // An toàn: Không để array index bị nhảy cóc tạo con trỏ rác
                    count = (ota.versionCount > 28) ? 28 : ota.versionCount; 
                    for (int i = 0; i < count; i++) items[i] = ota.versions[i].name;
                } else if (state.currentMenu == MENU_SELECT_BG) {
                    lv_label_set_text(objects.label_menu, "Select BG"); 
                    // An toàn: Không để array index bị nhảy cóc tạo con trỏ rác
                    count = (storage.bgFileCount > 28) ? 28 : storage.bgFileCount; 
                    for (int i = 0; i < count; i++) items[i] = storage.bgFileNames[i];
                }
                items[count] = "Back"; buildMenu(items, count + 1);
            }
            lastMenuType = state.currentMenu;
        }

        for (int i = 0; i < currentMenuCount; i++) {
            if (g_menuBtns[i] == NULL) continue;
            
            if (i == state.menuIndex) {
                lv_obj_add_state(g_menuBtns[i], LV_STATE_CHECKED);
                lv_obj_scroll_to_view(g_menuBtns[i], LV_ANIM_ON);
            } else {
                lv_obj_clear_state(g_menuBtns[i], LV_STATE_CHECKED);
            }
        }
    }
}

void DisplayLogic::setContrast(int level) { ledcWrite(BACKLIGHT_CHANNEL, map(level, 0, 100, 0, 255)); }
void DisplayLogic::turnOff() { ledcWrite(BACKLIGHT_CHANNEL, 0); tft.writecommand(0x10); /* Sleep in */ }
void DisplayLogic::turnOn() {
    tft.writecommand(0x11); // Sleep out
    delay(120);
    setContrast(appState.oledBrightness);
}

static lv_obj_t * file_overlay = NULL;

void DisplayLogic::showFileContent(const char* title, const char* content) {
    if (title != NULL && strcmp(title, "SCROLL_UP") == 0) { if (file_overlay) lv_obj_scroll_by(file_overlay, 0, 40, LV_ANIM_ON); return; }
    if (title != NULL && strcmp(title, "SCROLL_DOWN") == 0) { if (file_overlay) lv_obj_scroll_by(file_overlay, 0, -40, LV_ANIM_ON); return; }

    if (file_overlay != NULL) { lv_obj_del(file_overlay); file_overlay = NULL; }
    if (title == NULL && content == NULL) return; 


    file_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(file_overlay, 230, 230); lv_obj_center(file_overlay);
    lv_obj_set_style_bg_color(file_overlay, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_color(file_overlay, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_border_width(file_overlay, 2, 0);
    lv_obj_set_style_pad_all(file_overlay, 10, 0);
    lv_obj_set_scroll_dir(file_overlay, LV_DIR_VER);

    lv_obj_t * label_title = lv_label_create(file_overlay);
    lv_label_set_text(label_title, title);
    lv_obj_set_style_text_color(label_title, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t * line = lv_line_create(file_overlay);
    static lv_point_t line_points[] = { {0, 0}, {200, 0} };
    lv_line_set_points(line, line_points, 2);
    lv_obj_set_style_line_color(line, lv_color_hex(0x444444), 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t * label_content = lv_label_create(file_overlay);
    lv_label_set_long_mode(label_content, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label_content, 200);
    lv_obj_set_style_text_font(label_content, &lv_font_montserrat_12, 0); 
    lv_obj_set_style_text_color(label_content, lv_color_hex(0xFFFFFF), 0); 
    if (content) lv_label_set_text(label_content, content);
    else lv_label_set_text(label_content, "SD Card Error!");
    lv_obj_align(label_content, LV_ALIGN_TOP_LEFT, 0, 30);
}

static lv_obj_t * ota_overlay = NULL;
static lv_obj_t * ota_label_content = NULL;
static lv_obj_t * ota_bar = NULL;

void DisplayLogic::showProgressPopup(const char* title, const char* msg, int percent) {
    if (ota_overlay == NULL) {
        ota_overlay = lv_obj_create(lv_scr_act());
        if (ota_overlay == NULL) return; // Bảo vệ Crash StoreProhibited 0x10
        
        lv_obj_set_size(ota_overlay, 230, 230); lv_obj_center(ota_overlay);
        lv_obj_set_style_bg_color(ota_overlay, lv_color_hex(0x111111), 0);
        lv_obj_set_style_border_color(ota_overlay, lv_palette_main(LV_PALETTE_ORANGE), 0);
        lv_obj_set_style_border_width(ota_overlay, 2, 0);
        lv_obj_set_style_pad_all(ota_overlay, 10, 0);
        lv_obj_set_scroll_dir(ota_overlay, LV_DIR_NONE);

        lv_obj_t * label_title = lv_label_create(ota_overlay);
        lv_label_set_text(label_title, title); 
        lv_obj_set_style_text_color(label_title, lv_palette_main(LV_PALETTE_ORANGE), 0);
        lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 0);

        lv_obj_t * line = lv_line_create(ota_overlay);
        static lv_point_t line_points[] = { {0, 0}, {200, 0} };
        lv_line_set_points(line, line_points, 2);
        lv_obj_set_style_line_color(line, lv_color_hex(0x444444), 0);
        lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 20);

        ota_label_content = lv_label_create(ota_overlay);
        lv_label_set_long_mode(ota_label_content, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(ota_label_content, 200);
        lv_obj_set_style_text_font(ota_label_content, &lv_font_montserrat_12, 0); 
        lv_obj_set_style_text_color(ota_label_content, lv_color_hex(0xFFFFFF), 0); 
        lv_obj_align(ota_label_content, LV_ALIGN_TOP_LEFT, 0, 30);

        ota_bar = lv_bar_create(ota_overlay);
        lv_obj_set_size(ota_bar, 200, 20); lv_obj_align(ota_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(ota_bar, lv_color_hex(0x444444), LV_PART_MAIN);
        lv_obj_set_style_bg_color(ota_bar, lv_color_hex(0x00cc00), LV_PART_INDICATOR);
    } else {
        lv_obj_t * label_title = lv_obj_get_child(ota_overlay, 0);
        if(label_title) lv_label_set_text(label_title, title);
    }
    if (msg != NULL) lv_label_set_text(ota_label_content, msg);
    if (percent >= 0 && percent <= 100) lv_bar_set_value(ota_bar, percent, LV_ANIM_ON);
}

void DisplayLogic::closeProgressPopup() {
    if (ota_overlay != NULL) { lv_obj_del(ota_overlay); ota_overlay = NULL; ota_label_content = NULL; ota_bar = NULL; }
}

bool DisplayLogic::showImagePreview(FsFile& file) {
    if(!file) {
        Serial.println("[DISPLAY] Error: File is invalid or not open!");
        return false;
    }
    
    size_t fileSize = file.size();
    if (fileSize < FRAME_BYTES) {
        Serial.printf("[DISPLAY] Error: File too small (%lu bytes)\n", (unsigned long)fileSize);
        return false;
    }

    if (!scr_image_preview) {
        scr_image_preview = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr_image_preview, lv_color_black(), 0);
    }

    // [QUAN TRỌNG] Gỡ ảnh khỏi màn hình TRƯỚC KHI giải phóng bộ nhớ để tránh LVGL render vào vùng nhớ rác
    if (preview_img_obj) {
        lv_img_set_src(preview_img_obj, NULL);
    }

    if (preview_data_buffer != NULL) { free(preview_data_buffer); preview_data_buffer = NULL; }
    
    size_t allocSize = FRAME_BYTES; // Full frame 240x240 RGB565

    Serial.printf("[DISPLAY] Preview Alloc: %d bytes\n", allocSize);
    preview_data_buffer = (uint8_t*)heap_caps_calloc(1, allocSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!preview_data_buffer) {
        Serial.println("[DISPLAY] Preview Alloc FAIL: PSRAM not available or full!");
        return false;
    }

    size_t totalRead = readFully(file, preview_data_buffer, allocSize);
    
    if (totalRead == allocSize) { 
        preview_img_dsc.header.always_zero = 0;
        preview_img_dsc.header.w = 240;
        preview_img_dsc.header.h = 240;
        preview_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        preview_img_dsc.data_size = allocSize;
        preview_img_dsc.data = preview_data_buffer;

        lv_img_cache_invalidate_src(NULL);

        if (!preview_img_obj) {
            preview_img_obj = lv_img_create(scr_image_preview);
            lv_obj_center(preview_img_obj);
        }
        lv_img_set_src(preview_img_obj, &preview_img_dsc);
        lv_scr_load(scr_image_preview);
        Serial.println("[DISPLAY] Preview loaded successfully");
        return true;
    }
    Serial.printf("[DISPLAY] Error: Preview read short %lu/%lu bytes\n",
                  (unsigned long)totalRead, (unsigned long)allocSize);
    free(preview_data_buffer);
    preview_data_buffer = NULL;
    return false;
}

void DisplayLogic::closeImagePreview() {
    // Thay "" thành NULL để xóa rỗng bộ đệm của obj
    if (preview_img_obj) { lv_img_set_src(preview_img_obj, NULL); }
    if (preview_data_buffer != NULL) { free(preview_data_buffer); preview_data_buffer = NULL; }
    
    if (lv_scr_act() == scr_image_preview && objects.menu != NULL) {
        lv_scr_load(objects.menu);
    }
}