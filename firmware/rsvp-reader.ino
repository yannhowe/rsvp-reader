// =============================================================================
// RSVP Reader — ESP32-S3-LCD-3.16
// =============================================================================
// Speed-reading e-reader using Rapid Serial Visual Presentation.
// Displays one word at a time with ORP (Optimal Recognition Point) highlighting.
//
// Hardware: Waveshare ESP32-S3-LCD-3.16 (SKU 31786)
//   - 320×820 RGB565 display (ST7701S)
//   - SD card (SDMMC 4-wire)
//   - QMI8658 IMU (tilt gestures)
//   - BOOT button (play/pause/menu)
//
// Dependencies:
//   - LVGL v8 (Arduino library)
//   - ESP32 Arduino Core ≥3.0.0
// =============================================================================

#include <Arduino.h>
#include <lvgl.h>
#include "esp_lcd_panel_rgb.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "user_config.h"
#include "sd_manager.h"
#include "epub_parser.h"
#include "rsvp_engine.h"
#include "ui_manager.h"
#include "imu_controls.h"
#include "bookmark_manager.h"

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------

static esp_lcd_panel_handle_t s_panel = NULL;
static lv_disp_draw_buf_t     s_draw_buf;
static lv_disp_drv_t          s_disp_drv;
static lv_color_t*             s_buf1 = NULL;
static lv_color_t*             s_buf2 = NULL;

static unsigned long s_last_bookmark_ms = 0;
static const unsigned long BOOKMARK_INTERVAL_MS = 30000;  // auto-save every 30s

// Forward declarations for callbacks
static void on_control_event(ControlEvent evt);
static void on_word_change(const char* word, int orp_idx);
static void on_state_change(bool playing);
static void on_chapter_change(int chapter, int total);
static void on_file_selected(const char* path);
static void on_resume_choice(bool resume);
static void on_menu_action(int action);

// -----------------------------------------------------------------------------
// 3-Wire SPI Bit-Bang (ST7701S init only)
// -----------------------------------------------------------------------------

static void spi_cmd(uint8_t cmd) {
    gpio_set_level((gpio_num_t)LCD_SPI_CS, 0);
    gpio_set_level((gpio_num_t)LCD_SPI_SDA, 0);  // DC=0 → command
    gpio_set_level((gpio_num_t)LCD_SPI_SCL, 0);
    delayMicroseconds(1);
    gpio_set_level((gpio_num_t)LCD_SPI_SCL, 1);
    delayMicroseconds(1);
    for (int i = 7; i >= 0; i--) {
        gpio_set_level((gpio_num_t)LCD_SPI_SDA, (cmd >> i) & 1);
        gpio_set_level((gpio_num_t)LCD_SPI_SCL, 0);
        delayMicroseconds(1);
        gpio_set_level((gpio_num_t)LCD_SPI_SCL, 1);
        delayMicroseconds(1);
    }
    gpio_set_level((gpio_num_t)LCD_SPI_CS, 1);
}

static void spi_data(uint8_t data) {
    gpio_set_level((gpio_num_t)LCD_SPI_CS, 0);
    gpio_set_level((gpio_num_t)LCD_SPI_SDA, 1);  // DC=1 → data
    gpio_set_level((gpio_num_t)LCD_SPI_SCL, 0);
    delayMicroseconds(1);
    gpio_set_level((gpio_num_t)LCD_SPI_SCL, 1);
    delayMicroseconds(1);
    for (int i = 7; i >= 0; i--) {
        gpio_set_level((gpio_num_t)LCD_SPI_SDA, (data >> i) & 1);
        gpio_set_level((gpio_num_t)LCD_SPI_SCL, 0);
        delayMicroseconds(1);
        gpio_set_level((gpio_num_t)LCD_SPI_SCL, 1);
        delayMicroseconds(1);
    }
    gpio_set_level((gpio_num_t)LCD_SPI_CS, 1);
}

static void st7701_send(uint8_t cmd, const uint8_t* data, uint8_t len) {
    spi_cmd(cmd);
    for (uint8_t i = 0; i < len; i++) spi_data(data[i]);
}

// ST7701S initialization sequence (from official Waveshare LVGL V9 example)
static void st7701_init() {
    gpio_set_level((gpio_num_t)LCD_RST, 0);
    delay(10);
    gpio_set_level((gpio_num_t)LCD_RST, 1);
    delay(120);

    const uint8_t c1[] = {0x77,0x01,0x00,0x00,0x13};
    st7701_send(0xFF, c1, 5);
    const uint8_t c2[] = {0x08};
    st7701_send(0xEF, c2, 1);

    const uint8_t c3[] = {0x77,0x01,0x00,0x00,0x10};
    st7701_send(0xFF, c3, 5);
    const uint8_t c4[] = {0xE5,0x02};
    st7701_send(0xC0, c4, 2);
    const uint8_t c5[] = {0x15,0x0A};
    st7701_send(0xC1, c5, 2);
    const uint8_t c6[] = {0x07,0x02};
    st7701_send(0xC2, c6, 2);
    const uint8_t c7[] = {0x10};
    st7701_send(0xCC, c7, 1);

    const uint8_t b0[] = {0x00,0x08,0x51,0x0D,0xCE,0x06,0x00,0x08,0x08,0x24,0x05,0xD0,0x0F,0x6F,0x36,0x1F};
    st7701_send(0xB0, b0, 16);
    const uint8_t b1[] = {0x00,0x10,0x4F,0x0C,0x11,0x05,0x00,0x07,0x07,0x18,0x02,0xD3,0x11,0x6E,0x34,0x1F};
    st7701_send(0xB1, b1, 16);

    const uint8_t c8[] = {0x77,0x01,0x00,0x00,0x11};
    st7701_send(0xFF, c8, 5);
    st7701_send(0xB0, (const uint8_t[]){0x4D}, 1);
    st7701_send(0xB1, (const uint8_t[]){0x37}, 1);
    st7701_send(0xB2, (const uint8_t[]){0x87}, 1);
    st7701_send(0xB3, (const uint8_t[]){0x80}, 1);
    st7701_send(0xB5, (const uint8_t[]){0x4A}, 1);
    st7701_send(0xB7, (const uint8_t[]){0x85}, 1);
    st7701_send(0xB8, (const uint8_t[]){0x21}, 1);
    const uint8_t b9[] = {0x00,0x13};
    st7701_send(0xB9, b9, 2);
    st7701_send(0xC0, (const uint8_t[]){0x09}, 1);
    st7701_send(0xC1, (const uint8_t[]){0x78}, 1);
    st7701_send(0xC2, (const uint8_t[]){0x78}, 1);
    st7701_send(0xD0, (const uint8_t[]){0x88}, 1);

    const uint8_t e0[] = {0x80,0x00,0x02};
    st7701_send(0xE0, e0, 3);
    delay(100);

    const uint8_t e1[] = {0x0F,0xA0,0x00,0x00,0x10,0xA0,0x00,0x00,0x00,0x60,0x60};
    st7701_send(0xE1, e1, 11);
    const uint8_t e2[] = {0x30,0x30,0x60,0x60,0x45,0xA0,0x00,0x00,0x46,0xA0,0x00,0x00,0x00};
    st7701_send(0xE2, e2, 13);
    const uint8_t e3[] = {0x00,0x00,0x33,0x33};
    st7701_send(0xE3, e3, 4);
    const uint8_t e4[] = {0x44,0x44};
    st7701_send(0xE4, e4, 2);
    const uint8_t e5[] = {0x0F,0x4A,0xA0,0xA0,0x11,0x4A,0xA0,0xA0,0x13,0x4A,0xA0,0xA0,0x15,0x4A,0xA0,0xA0};
    st7701_send(0xE5, e5, 16);
    const uint8_t e6[] = {0x00,0x00,0x33,0x33};
    st7701_send(0xE6, e6, 4);
    const uint8_t e7[] = {0x44,0x44};
    st7701_send(0xE7, e7, 2);
    const uint8_t e8[] = {0x10,0x4A,0xA0,0xA0,0x12,0x4A,0xA0,0xA0,0x14,0x4A,0xA0,0xA0,0x16,0x4A,0xA0,0xA0};
    st7701_send(0xE8, e8, 16);
    const uint8_t eb[] = {0x02,0x00,0x4E,0x4E,0xEE,0x44,0x00};
    st7701_send(0xEB, eb, 7);
    const uint8_t ed[] = {0xFF,0xFF,0x04,0x56,0x72,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x27,0x65,0x40,0xFF,0xFF};
    st7701_send(0xED, ed, 16);
    const uint8_t ef[] = {0x08,0x08,0x08,0x40,0x3F,0x64};
    st7701_send(0xEF, ef, 6);

    const uint8_t c9[] = {0x77,0x01,0x00,0x00,0x13};
    st7701_send(0xFF, c9, 5);
    const uint8_t e8b[] = {0x00,0x0E};
    st7701_send(0xE8, e8b, 2);
    const uint8_t c10[] = {0x77,0x01,0x00,0x00,0x00};
    st7701_send(0xFF, c10, 5);

    spi_cmd(0x11);  // Exit sleep
    delay(120);

    const uint8_t c11[] = {0x77,0x01,0x00,0x00,0x13};
    st7701_send(0xFF, c11, 5);
    const uint8_t e8c[] = {0x00,0x0C};
    st7701_send(0xE8, e8c, 2);
    delay(10);
    const uint8_t e8d[] = {0x00,0x00};
    st7701_send(0xE8, e8d, 2);
    const uint8_t c12[] = {0x77,0x01,0x00,0x00,0x00};
    st7701_send(0xFF, c12, 5);

    st7701_send(0x3A, (const uint8_t[]){0x55}, 1);  // RGB565
    st7701_send(0x36, (const uint8_t[]){0x00}, 1);  // Normal orientation
    st7701_send(0x35, (const uint8_t[]){0x00}, 1);  // Tearing effect on

    spi_cmd(0x29);  // Display on
    delay(20);
}

// -----------------------------------------------------------------------------
// Backlight
// -----------------------------------------------------------------------------

static void backlight_init() {
    ledc_timer_config_t timer_conf = {};
    timer_conf.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_conf.duty_resolution = (ledc_timer_bit_t)LCD_BL_PWM_BITS;
    timer_conf.timer_num = LEDC_TIMER_3;
    timer_conf.freq_hz = LCD_BL_PWM_HZ;
    timer_conf.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t ch_conf = {};
    ch_conf.gpio_num = LCD_BL;
    ch_conf.speed_mode = LEDC_LOW_SPEED_MODE;
    ch_conf.channel = (ledc_channel_t)LCD_BL_PWM_CHANNEL;
    ch_conf.timer_sel = LEDC_TIMER_3;
    ch_conf.duty = 0;  // 0 = max brightness (inverted logic)
    ledc_channel_config(&ch_conf);
}

void backlight_set(uint8_t brightness) {
    // Inverted: duty 0 = brightest, 255 = off
    uint32_t duty = 0xFF - brightness;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)LCD_BL_PWM_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)LCD_BL_PWM_CHANNEL);
}

// -----------------------------------------------------------------------------
// RGB Panel + LVGL Display Driver
// -----------------------------------------------------------------------------

static void lvgl_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, color_p);
    lv_disp_flush_ready(drv);
}

static void rgb_panel_init() {
    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src = LCD_CLK_SRC_DEFAULT;
    cfg.timings.pclk_hz          = LCD_PIXEL_CLOCK_HZ;
    cfg.timings.h_res            = LCD_H_RES;
    cfg.timings.v_res            = LCD_V_RES;
    cfg.timings.hsync_pulse_width = LCD_HSYNC_PULSE;
    cfg.timings.hsync_back_porch  = LCD_HSYNC_BP;
    cfg.timings.hsync_front_porch = LCD_HSYNC_FP;
    cfg.timings.vsync_pulse_width = LCD_VSYNC_PULSE;
    cfg.timings.vsync_back_porch  = LCD_VSYNC_BP;
    cfg.timings.vsync_front_porch = LCD_VSYNC_FP;
    cfg.timings.flags.pclk_active_neg = true;

    cfg.data_width      = 16;
    cfg.bits_per_pixel  = 16;
    cfg.num_fbs         = 1;
    cfg.bounce_buffer_size_px = 10 * LCD_H_RES;
    cfg.psram_trans_align = 64;

    cfg.hsync_gpio_num  = LCD_HSYNC;
    cfg.vsync_gpio_num  = LCD_VSYNC;
    cfg.de_gpio_num     = LCD_DE;
    cfg.pclk_gpio_num   = LCD_PCLK;
    cfg.disp_gpio_num   = -1;

    // Data bus order: B-G-R (per Waveshare demo)
    cfg.data_gpio_nums[0]  = LCD_B0;
    cfg.data_gpio_nums[1]  = LCD_B1;
    cfg.data_gpio_nums[2]  = LCD_B2;
    cfg.data_gpio_nums[3]  = LCD_B3;
    cfg.data_gpio_nums[4]  = LCD_B4;
    cfg.data_gpio_nums[5]  = LCD_G0;
    cfg.data_gpio_nums[6]  = LCD_G1;
    cfg.data_gpio_nums[7]  = LCD_G2;
    cfg.data_gpio_nums[8]  = LCD_G3;
    cfg.data_gpio_nums[9]  = LCD_G4;
    cfg.data_gpio_nums[10] = LCD_G5;
    cfg.data_gpio_nums[11] = LCD_R0;
    cfg.data_gpio_nums[12] = LCD_R1;
    cfg.data_gpio_nums[13] = LCD_R2;
    cfg.data_gpio_nums[14] = LCD_R3;
    cfg.data_gpio_nums[15] = LCD_R4;

    cfg.flags.fb_in_psram = true;

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
}

static void lvgl_display_init() {
    lv_init();

    // Allocate draw buffers in PSRAM
    size_t buf_size = LCD_H_RES * LVGL_BUF_LINES;
    s_buf1 = (lv_color_t*)heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    s_buf2 = (lv_color_t*)heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!s_buf1 || !s_buf2) {
        Serial.println("[FATAL] PSRAM allocation failed — check PSRAM config");
        while (1) delay(1000);
    }

    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, buf_size);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res   = LCD_H_RES;
    s_disp_drv.ver_res   = LCD_V_RES;
    s_disp_drv.flush_cb  = lvgl_flush_cb;
    s_disp_drv.draw_buf  = &s_draw_buf;
    s_disp_drv.user_data = s_panel;
    lv_disp_drv_register(&s_disp_drv);
}

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== RSVP Reader ===");

    // GPIO setup for SPI init lines
    gpio_set_direction((gpio_num_t)LCD_RST,     GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)LCD_SPI_CS,  GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)LCD_SPI_SDA, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)LCD_SPI_SCL, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)LCD_SPI_CS,  1);
    gpio_set_level((gpio_num_t)LCD_SPI_SCL, 1);

    // Display init
    backlight_init();
    st7701_init();
    rgb_panel_init();
    lvgl_display_init();
    backlight_set(255);  // Full brightness
    Serial.println("[OK] Display ready");

    // SD card
    if (sd_init()) {
        Serial.println("[OK] SD card mounted");
    } else {
        Serial.println("[WARN] SD card not found — continuing without storage");
    }

    // IMU (tilt gestures)
    imu_init();
    Serial.println("[OK] IMU ready");

    // Bookmarks
    bookmark_init();
    Serial.println("[OK] Bookmarks loaded");

    // RSVP engine
    rsvp_init(300);

    // UI — create all screens
    ui_init();

    // Wire callbacks: IMU → engine/UI
    imu_set_callback(on_control_event);

    // Wire callbacks: RSVP engine → UI
    rsvp_set_word_callback(on_word_change);
    rsvp_set_state_callback(on_state_change);
    rsvp_set_chapter_callback(on_chapter_change);

    // Wire callbacks: UI → engine
    ui_set_file_callback(on_file_selected);
    ui_set_resume_callback(on_resume_choice);
    ui_set_menu_callback(on_menu_action);

    // Show file picker or resume last book
    const char* last_book = bookmark_last_book();
    if (last_book && sd_file_exists(last_book)) {
        bookmark_load(last_book, &last_chapter, &last_word, &last_wpm);
        ui_show_resume_prompt(last_book, last_chapter, last_word, last_wpm);
    } else {
        ui_show_file_picker();
    }

    Serial.println("[OK] RSVP Reader started\n");
}

// -----------------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------------

void loop() {
    // LVGL tick
    lv_tick_inc(LVGL_TICK_MS);
    lv_timer_handler();

    // RSVP engine tick
    rsvp_tick();

    // Poll controls
    imu_tick();

    // Auto-save bookmark while reading
    if (rsvp_is_playing()) {
        unsigned long now = millis();
        if (now - s_last_bookmark_ms >= BOOKMARK_INTERVAL_MS) {
            s_last_bookmark_ms = now;
            bookmark_save(rsvp_current_book(), rsvp_current_chapter(),
                         rsvp_current_word_idx(), rsvp_get_wpm());
        }
    }

    delay(LVGL_TICK_MS);
}

// -----------------------------------------------------------------------------
// Callback Implementations
// -----------------------------------------------------------------------------

static void open_book(const char* path, int chapter, int word_idx, int wpm) {
    // 1. Parse EPUB structure and extract all chapters to SD cache
    if (!epub_open(path)) {
        Serial.printf("[ERR] Failed to open EPUB: %s\n", path);
        ui_show_file_picker();
        return;
    }
    int extracted = epub_extract_all();
    Serial.printf("[OK] Extracted %d chapters from: %s\n", extracted, epub_title());

    // 2. Load into RSVP engine (reads the plaintext cache files)
    rsvp_open_book(path);
    if (chapter > 0) {
        for (int i = 0; i < chapter; i++) rsvp_next_chapter();
    }
    if (word_idx > 0) rsvp_seek_word(word_idx);
    if (wpm != rsvp_get_wpm()) rsvp_set_wpm(wpm);
    ui_show_reader();
    ui_update_chapter_info(epub_title(), rsvp_current_chapter(), rsvp_chapter_count());
    ui_update_wpm(rsvp_get_wpm());
    ui_update_progress(rsvp_current_word_idx(), rsvp_total_words());
}

static void on_control_event(ControlEvent evt) {
    switch (evt) {
        case EVT_PLAY_PAUSE:
            rsvp_toggle();
            break;
        case EVT_MENU:
            rsvp_pause();
            bookmark_save(rsvp_current_book(), rsvp_current_chapter(),
                         rsvp_current_word_idx(), rsvp_get_wpm());
            ui_show_menu();
            imu_set_menu_mode(true);
            imu_gestures_enable(false);
            break;
        case EVT_NEXT_CHAPTER:
            rsvp_next_chapter();
            break;
        case EVT_PREV_CHAPTER:
            rsvp_prev_chapter();
            break;
        case EVT_WPM_UP:
            rsvp_set_wpm(rsvp_get_wpm() + 25);
            ui_update_wpm(rsvp_get_wpm());
            break;
        case EVT_WPM_DOWN:
            rsvp_set_wpm(rsvp_get_wpm() - 25);
            ui_update_wpm(rsvp_get_wpm());
            break;
        default:
            break;
    }
}

static void on_word_change(const char* word, int orp_idx) {
    ui_update_word(word, orp_idx);
    ui_update_progress(rsvp_current_word_idx(), rsvp_total_words());
}

static void on_state_change(bool playing) {
    ui_update_play_state(playing);
}

static void on_chapter_change(int chapter, int total) {
    ui_update_chapter_info(epub_title(), chapter, total);
    ui_update_progress(rsvp_current_word_idx(), rsvp_total_words());
}

static void on_file_selected(const char* path) {
    open_book(path, 0, 0, 300);
}

static void on_resume_choice(bool resume) {
    const char* last = bookmark_last_book();
    if (resume && last) {
        int ch = 0, wi = 0, wpm = 300;
        bookmark_load(last, &ch, &wi, &wpm);
        open_book(last, ch, wi, wpm);
    } else {
        ui_show_file_picker();
    }
}

static void on_menu_action(int action) {
    imu_set_menu_mode(false);
    imu_gestures_enable(true);
    switch (action) {
        case 0: // Resume reading
            ui_show_reader();
            rsvp_play();
            break;
        case 1: // Jump to chapter (TODO: show chapter list)
            ui_show_reader();
            break;
        case 2: // Adjust WPM (TODO: show WPM slider)
            ui_show_reader();
            break;
        case 3: // Back to file picker
            rsvp_pause();
            bookmark_save(rsvp_current_book(), rsvp_current_chapter(),
                         rsvp_current_word_idx(), rsvp_get_wpm());
            ui_show_file_picker();
            break;
        case 4: // Settings
            ui_show_reader();
            break;
    }
}
