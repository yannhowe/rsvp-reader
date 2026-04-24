/*
 * ui_manager.cpp
 *
 * LVGL v8 UI for the ESP32-S3 RSVP e-reader.
 * Display: 320×820 portrait.
 *
 * All screens are pre-created in ui_init() and kept alive for the lifetime of
 * the firmware — only lv_scr_load_anim() switches between them.
 *
 * Thread safety: all LVGL calls must come from the main Arduino loop.
 */

#include "ui_manager.h"
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Display geometry
// ---------------------------------------------------------------------------
static const lv_coord_t DISPLAY_W   = 320;
static const lv_coord_t DISPLAY_H   = 820;
static const lv_coord_t ORP_X       = 160;   // horizontal anchor for pivot char

// ---------------------------------------------------------------------------
// Colors
// ---------------------------------------------------------------------------
#define CLR_BG          lv_color_hex(0x000000)
#define CLR_TEXT        lv_color_hex(0xFFFFFF)
#define CLR_ACCENT      lv_color_hex(0xFF4444)
#define CLR_BAR_BG      lv_color_hex(0x1A1A1A)
#define CLR_GUIDE       lv_color_hex(0x333333)
#define CLR_PROG_TRACK  lv_color_hex(0x333333)

// ---------------------------------------------------------------------------
// Fonts — prefer 36px for RSVP word, 28px for UI chrome
// ---------------------------------------------------------------------------
#if defined(LV_FONT_MONTSERRAT_36) && LV_FONT_MONTSERRAT_36
  #define FONT_RSVP   (&lv_font_montserrat_36)
#else
  #define FONT_RSVP   (&lv_font_montserrat_28)
#endif

#if defined(LV_FONT_MONTSERRAT_28) && LV_FONT_MONTSERRAT_28
  #define FONT_UI     (&lv_font_montserrat_28)
#elif defined(LV_FONT_MONTSERRAT_24) && LV_FONT_MONTSERRAT_24
  #define FONT_UI     (&lv_font_montserrat_24)
#else
  #define FONT_UI     (&lv_font_montserrat_16)
#endif

// ---------------------------------------------------------------------------
// Callback slots
// ---------------------------------------------------------------------------
static ui_file_selected_cb_t  s_file_cb   = nullptr;
static ui_resume_cb_t         s_resume_cb = nullptr;
static ui_menu_action_cb_t    s_menu_cb   = nullptr;

// ---------------------------------------------------------------------------
// Shared styles (initialised once in ui_init)
// ---------------------------------------------------------------------------
static lv_style_t s_style_screen;      // full-screen black background
static lv_style_t s_style_bar_bg;      // header / footer panel
static lv_style_t s_style_label_white; // standard white UI text
static lv_style_t s_style_label_red;   // accent / pivot text
static lv_style_t s_style_list;        // file-picker list body
static lv_style_t s_style_list_btn;    // unselected list button
static lv_style_t s_style_list_btn_sel;// selected list button
static lv_style_t s_style_prog_track;  // progress bar track
static lv_style_t s_style_prog_ind;    // progress bar indicator

// ---------------------------------------------------------------------------
// FILE PICKER screen
// ---------------------------------------------------------------------------
static lv_obj_t* s_scr_picker       = nullptr;
static lv_obj_t* s_picker_list      = nullptr;
static lv_obj_t* s_picker_footer    = nullptr;
static lv_obj_t* s_picker_footer_lbl= nullptr;

static char s_file_paths[32][256];   // copy of file paths for callbacks
static int  s_file_count = 0;

// ---------------------------------------------------------------------------
// READER screen
// ---------------------------------------------------------------------------
static lv_obj_t* s_scr_reader       = nullptr;
// Top bar
static lv_obj_t* s_reader_topbar    = nullptr;
static lv_obj_t* s_reader_title_lbl = nullptr;
static lv_obj_t* s_reader_chap_lbl  = nullptr;
// RSVP word area
static lv_obj_t* s_word_area        = nullptr;
static lv_obj_t* s_guide_line       = nullptr;
static lv_obj_t* s_lbl_before       = nullptr;
static lv_obj_t* s_lbl_pivot        = nullptr;
static lv_obj_t* s_lbl_after        = nullptr;
// Bottom controls
static lv_obj_t* s_prog_bar         = nullptr;
static lv_obj_t* s_prog_pct_lbl     = nullptr;
static lv_obj_t* s_wpm_lbl          = nullptr;
static lv_obj_t* s_play_lbl         = nullptr;

// ---------------------------------------------------------------------------
// MENU OVERLAY screen
// ---------------------------------------------------------------------------
static lv_obj_t* s_scr_menu         = nullptr;
static lv_obj_t* s_menu_list        = nullptr;

// ---------------------------------------------------------------------------
// RESUME PROMPT screen
// ---------------------------------------------------------------------------
static lv_obj_t* s_scr_resume       = nullptr;
static lv_obj_t* s_resume_msg_lbl   = nullptr;

// ============================================================================
// Forward declarations of internal helpers
// ============================================================================
static void build_style_screen(lv_style_t* st);
static void build_style_bar_bg(lv_style_t* st);
static void build_style_label_white(lv_style_t* st);
static void build_style_label_red(lv_style_t* st);
static void build_style_list(lv_style_t* st);
static void build_style_list_btn(lv_style_t* st);
static void build_style_list_btn_sel(lv_style_t* st);
static void build_style_prog_track(lv_style_t* st);
static void build_style_prog_ind(lv_style_t* st);

static void build_screen_picker();
static void build_screen_reader();
static void build_screen_menu();
static void build_screen_resume();

static void cb_file_btn(lv_event_t* e);
static void cb_menu_btn(lv_event_t* e);
static void cb_resume_yes(lv_event_t* e);
static void cb_resume_no(lv_event_t* e);

static lv_obj_t* make_panel(lv_obj_t* parent,
                             lv_coord_t x, lv_coord_t y,
                             lv_coord_t w, lv_coord_t h);
static lv_obj_t* make_label(lv_obj_t* parent, lv_style_t* style,
                             const lv_font_t* font,
                             lv_coord_t x, lv_coord_t y,
                             lv_coord_t w, lv_coord_t h,
                             lv_text_align_t align,
                             const char* text);

// ============================================================================
// Style builders
// ============================================================================

static void build_style_screen(lv_style_t* st) {
    lv_style_init(st);
    lv_style_set_bg_opa(st, LV_OPA_COVER);
    lv_style_set_bg_color(st, CLR_BG);
    lv_style_set_border_width(st, 0);
    lv_style_set_pad_all(st, 0);
}

static void build_style_bar_bg(lv_style_t* st) {
    lv_style_init(st);
    lv_style_set_bg_opa(st, LV_OPA_COVER);
    lv_style_set_bg_color(st, CLR_BAR_BG);
    lv_style_set_border_width(st, 0);
    lv_style_set_radius(st, 0);
    lv_style_set_pad_all(st, 0);
}

static void build_style_label_white(lv_style_t* st) {
    lv_style_init(st);
    lv_style_set_text_color(st, CLR_TEXT);
    lv_style_set_bg_opa(st, LV_OPA_TRANSP);
    lv_style_set_border_width(st, 0);
}

static void build_style_label_red(lv_style_t* st) {
    lv_style_init(st);
    lv_style_set_text_color(st, CLR_ACCENT);
    lv_style_set_bg_opa(st, LV_OPA_TRANSP);
    lv_style_set_border_width(st, 0);
}

static void build_style_list(lv_style_t* st) {
    lv_style_init(st);
    lv_style_set_bg_opa(st, LV_OPA_COVER);
    lv_style_set_bg_color(st, CLR_BG);
    lv_style_set_border_width(st, 0);
    lv_style_set_radius(st, 0);
    lv_style_set_pad_all(st, 0);
    lv_style_set_pad_row(st, 0);
}

static void build_style_list_btn(lv_style_t* st) {
    lv_style_init(st);
    lv_style_set_bg_opa(st, LV_OPA_COVER);
    lv_style_set_bg_color(st, CLR_BG);
    lv_style_set_text_color(st, CLR_TEXT);
    lv_style_set_text_font(st, FONT_UI);
    lv_style_set_border_width(st, 0);
    lv_style_set_radius(st, 0);
    lv_style_set_pad_left(st, 12);
    lv_style_set_pad_right(st, 12);
    lv_style_set_pad_top(st, 14);
    lv_style_set_pad_bottom(st, 14);
}

static void build_style_list_btn_sel(lv_style_t* st) {
    lv_style_init(st);
    lv_style_set_bg_opa(st, LV_OPA_COVER);
    lv_style_set_bg_color(st, CLR_BG);
    lv_style_set_text_color(st, CLR_ACCENT);
}

static void build_style_prog_track(lv_style_t* st) {
    lv_style_init(st);
    lv_style_set_bg_opa(st, LV_OPA_COVER);
    lv_style_set_bg_color(st, CLR_PROG_TRACK);
    lv_style_set_border_width(st, 0);
    lv_style_set_radius(st, 4);
}

static void build_style_prog_ind(lv_style_t* st) {
    lv_style_init(st);
    lv_style_set_bg_opa(st, LV_OPA_COVER);
    lv_style_set_bg_color(st, CLR_ACCENT);
    lv_style_set_radius(st, 4);
}

// ============================================================================
// Generic widget helpers
// ============================================================================

static lv_obj_t* make_panel(lv_obj_t* parent,
                             lv_coord_t x, lv_coord_t y,
                             lv_coord_t w, lv_coord_t h) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_add_style(obj, &s_style_bar_bg, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t* make_label(lv_obj_t* parent, lv_style_t* style,
                             const lv_font_t* font,
                             lv_coord_t x, lv_coord_t y,
                             lv_coord_t w, lv_coord_t h,
                             lv_text_align_t align,
                             const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_size(lbl, w, h);
    if (style) lv_obj_add_style(lbl, style, 0);
    if (font)  lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_align(lbl, align, 0);
    lv_label_set_text(lbl, text);
    return lbl;
}

// ============================================================================
// Screen: File Picker
// ============================================================================

static void build_screen_picker() {
    s_scr_picker = lv_obj_create(NULL);
    lv_obj_add_style(s_scr_picker, &s_style_screen, 0);
    lv_obj_clear_flag(s_scr_picker, LV_OBJ_FLAG_SCROLLABLE);

    // Header
    lv_obj_t* header = make_panel(s_scr_picker, 0, 0, DISPLAY_W, 50);
    make_label(header, &s_style_label_white, FONT_UI,
               0, 0, DISPLAY_W, 50,
               LV_TEXT_ALIGN_CENTER, "RSVP Reader");

    // Scrollable list (between header and footer)
    s_picker_list = lv_list_create(s_scr_picker);
    lv_obj_set_pos(s_picker_list, 0, 50);
    lv_obj_set_size(s_picker_list, DISPLAY_W, DISPLAY_H - 100); // 720px
    lv_obj_add_style(s_picker_list, &s_style_list, 0);
    lv_obj_set_style_bg_color(s_picker_list, CLR_BG, 0);
    lv_obj_set_style_bg_opa(s_picker_list, LV_OPA_COVER, 0);

    // Footer
    s_picker_footer = make_panel(s_scr_picker, 0, DISPLAY_H - 50, DISPLAY_W, 50);
    s_picker_footer_lbl = make_label(s_picker_footer, &s_style_label_white, FONT_UI,
                                     0, 0, DISPLAY_W, 50,
                                     LV_TEXT_ALIGN_CENTER, "No books found");
}

// ============================================================================
// Screen: Reader
// ============================================================================

static void build_screen_reader() {
    s_scr_reader = lv_obj_create(NULL);
    lv_obj_add_style(s_scr_reader, &s_style_screen, 0);
    lv_obj_clear_flag(s_scr_reader, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Top bar (y=0..40) ----
    s_reader_topbar = make_panel(s_scr_reader, 0, 0, DISPLAY_W, 80);
    s_reader_title_lbl = make_label(s_reader_topbar, &s_style_label_white, FONT_UI,
                                    0, 4, DISPLAY_W, 36,
                                    LV_TEXT_ALIGN_CENTER, "");
    s_reader_chap_lbl = make_label(s_reader_topbar, &s_style_label_white, FONT_UI,
                                   0, 44, DISPLAY_W, 28,
                                   LV_TEXT_ALIGN_CENTER, "");

    // ---- Word area (y=80..740) — 660px tall ----
    s_word_area = lv_obj_create(s_scr_reader);
    lv_obj_set_pos(s_word_area, 0, 80);
    lv_obj_set_size(s_word_area, DISPLAY_W, 660);
    lv_obj_add_style(s_word_area, &s_style_screen, 0);
    lv_obj_clear_flag(s_word_area, LV_OBJ_FLAG_SCROLLABLE);

    // Vertical ORP guide line (1px wide, full height of word area)
    s_guide_line = lv_obj_create(s_word_area);
    lv_obj_set_pos(s_guide_line, ORP_X, 0);
    lv_obj_set_size(s_guide_line, 1, 660);
    lv_obj_set_style_bg_opa(s_guide_line, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_guide_line, CLR_GUIDE, 0);
    lv_obj_set_style_border_width(s_guide_line, 0, 0);
    lv_obj_clear_flag(s_guide_line, LV_OBJ_FLAG_SCROLLABLE);

    // RSVP labels sit in a horizontal band centred vertically in word area.
    // Word area height = 660px; labels sit at y = (660 - font_height) / 2 ≈ 297
    // We use a generous height so descenders are not clipped.
    const lv_coord_t WORD_Y    = 280;   // top of label row within word_area
    const lv_coord_t WORD_H    = 80;    // label height (covers 36px font + padding)
    const lv_coord_t HALF_SIDE = ORP_X; // available width to the left/right

    // before: occupies [0 .. ORP_X), right-aligned
    s_lbl_before = lv_label_create(s_word_area);
    lv_obj_set_pos(s_lbl_before, 0, WORD_Y);
    lv_obj_set_size(s_lbl_before, HALF_SIDE, WORD_H);
    lv_obj_add_style(s_lbl_before, &s_style_label_white, 0);
    lv_obj_set_style_text_font(s_lbl_before, FONT_RSVP, 0);
    lv_obj_set_style_text_align(s_lbl_before, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_lbl_before, "");

    // pivot: single char centred at ORP_X.  Give it a wide bounding box so
    // it can be truly centred — we position it straddling ORP_X symmetrically.
    const lv_coord_t PIVOT_W = 60;
    s_lbl_pivot = lv_label_create(s_word_area);
    lv_obj_set_pos(s_lbl_pivot, ORP_X - PIVOT_W / 2, WORD_Y);
    lv_obj_set_size(s_lbl_pivot, PIVOT_W, WORD_H);
    lv_obj_add_style(s_lbl_pivot, &s_style_label_red, 0);
    lv_obj_set_style_text_font(s_lbl_pivot, FONT_RSVP, 0);
    lv_obj_set_style_text_align(s_lbl_pivot, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lbl_pivot, "");

    // after: occupies [ORP_X .. DISPLAY_W), left-aligned
    s_lbl_after = lv_label_create(s_word_area);
    lv_obj_set_pos(s_lbl_after, ORP_X, WORD_Y);
    lv_obj_set_size(s_lbl_after, HALF_SIDE, WORD_H);
    lv_obj_add_style(s_lbl_after, &s_style_label_white, 0);
    lv_obj_set_style_text_font(s_lbl_after, FONT_RSVP, 0);
    lv_obj_set_style_text_align(s_lbl_after, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(s_lbl_after, "");

    // ---- Progress bar (y=740..780) ----
    lv_obj_t* prog_panel = make_panel(s_scr_reader, 0, 740, DISPLAY_W, 40);

    s_prog_bar = lv_bar_create(prog_panel);
    lv_obj_set_pos(s_prog_bar, 8, 10);
    lv_obj_set_size(s_prog_bar, 240, 18);
    lv_obj_add_style(s_prog_bar, &s_style_prog_track, LV_PART_MAIN);
    lv_obj_add_style(s_prog_bar, &s_style_prog_ind,   LV_PART_INDICATOR);
    lv_bar_set_range(s_prog_bar, 0, 1000);  // use permille for precision
    lv_bar_set_value(s_prog_bar, 0, LV_ANIM_OFF);

    s_prog_pct_lbl = make_label(prog_panel, &s_style_label_white, FONT_UI,
                                256, 0, 60, 40,
                                LV_TEXT_ALIGN_RIGHT, "0%");

    // ---- Status bar (y=780..820) ----
    lv_obj_t* status_panel = make_panel(s_scr_reader, 0, 780, DISPLAY_W, 40);

    s_wpm_lbl  = make_label(status_panel, &s_style_label_white, FONT_UI,
                             8, 0, 120, 40,
                             LV_TEXT_ALIGN_LEFT, "300 WPM");
    s_play_lbl = make_label(status_panel, &s_style_label_white, FONT_UI,
                             DISPLAY_W / 2, 0, 150, 40,
                             LV_TEXT_ALIGN_LEFT, "\xe2\x96\xb6 Playing");
}

// ============================================================================
// Screen: Menu Overlay
// ============================================================================

static void build_screen_menu() {
    // Semi-transparent overlay: create a new screen with a dark, semi-opaque bg.
    s_scr_menu = lv_obj_create(NULL);
    lv_obj_set_style_bg_opa(s_scr_menu, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(s_scr_menu, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_scr_menu, 0, 0);
    lv_obj_set_style_pad_all(s_scr_menu, 0, 0);
    lv_obj_clear_flag(s_scr_menu, LV_OBJ_FLAG_SCROLLABLE);

    // Centre panel
    const lv_coord_t PANEL_W = 280;
    const lv_coord_t PANEL_H = 380;
    const lv_coord_t PANEL_X = (DISPLAY_W - PANEL_W) / 2;
    const lv_coord_t PANEL_Y = (DISPLAY_H - PANEL_H) / 2;

    lv_obj_t* panel = lv_obj_create(s_scr_menu);
    lv_obj_set_pos(panel, PANEL_X, PANEL_Y);
    lv_obj_set_size(panel, PANEL_W, PANEL_H);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(panel, CLR_BAR_BG, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    static const char* menu_labels[] = {
        "Resume",
        "Jump to Chapter",
        "Adjust WPM",
        "File Picker",
        "Settings",
        nullptr
    };

    s_menu_list = lv_list_create(panel);
    lv_obj_set_pos(s_menu_list, 0, 0);
    lv_obj_set_size(s_menu_list, PANEL_W, PANEL_H);
    lv_obj_add_style(s_menu_list, &s_style_list, 0);
    lv_obj_set_style_bg_color(s_menu_list, CLR_BAR_BG, 0);
    lv_obj_set_style_bg_opa(s_menu_list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_menu_list, 8, 0);

    for (int i = 0; menu_labels[i] != nullptr; ++i) {
        lv_obj_t* btn = lv_list_add_btn(s_menu_list, nullptr, menu_labels[i]);
        lv_obj_add_style(btn, &s_style_list_btn, 0);
        lv_obj_add_style(btn, &s_style_list_btn_sel, LV_STATE_FOCUSED);
        lv_obj_add_style(btn, &s_style_list_btn_sel, LV_STATE_PRESSED);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, cb_menu_btn, LV_EVENT_CLICKED, nullptr);
        lv_obj_set_size(btn, PANEL_W, 70);
    }
}

// ============================================================================
// Screen: Resume Prompt
// ============================================================================

static void build_screen_resume() {
    s_scr_resume = lv_obj_create(NULL);
    lv_obj_add_style(s_scr_resume, &s_style_screen, 0);
    lv_obj_clear_flag(s_scr_resume, LV_OBJ_FLAG_SCROLLABLE);

    // Centre card
    const lv_coord_t CARD_W = 290;
    const lv_coord_t CARD_H = 300;
    const lv_coord_t CARD_X = (DISPLAY_W - CARD_W) / 2;
    const lv_coord_t CARD_Y = (DISPLAY_H - CARD_H) / 2;

    lv_obj_t* card = lv_obj_create(s_scr_resume);
    lv_obj_set_pos(card, CARD_X, CARD_Y);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, CLR_BAR_BG, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_resume_msg_lbl = lv_label_create(card);
    lv_obj_set_pos(s_resume_msg_lbl, 0, 0);
    lv_obj_set_size(s_resume_msg_lbl, CARD_W - 32, 200);
    lv_obj_add_style(s_resume_msg_lbl, &s_style_label_white, 0);
    lv_obj_set_style_text_font(s_resume_msg_lbl, FONT_UI, 0);
    lv_obj_set_style_text_align(s_resume_msg_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_resume_msg_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_resume_msg_lbl, "Resume reading?");

    // "Resume" button
    lv_obj_t* btn_yes = lv_btn_create(card);
    lv_obj_set_pos(btn_yes, 0, 210);
    lv_obj_set_size(btn_yes, CARD_W - 32, 50);
    lv_obj_set_style_bg_color(btn_yes, CLR_ACCENT, 0);
    lv_obj_set_style_bg_opa(btn_yes, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_yes, 0, 0);
    lv_obj_set_style_radius(btn_yes, 6, 0);
    lv_obj_add_event_cb(btn_yes, cb_resume_yes, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl_yes = lv_label_create(btn_yes);
    lv_obj_add_style(lbl_yes, &s_style_label_white, 0);
    lv_obj_set_style_text_font(lbl_yes, FONT_UI, 0);
    lv_label_set_text(lbl_yes, "Resume");
    lv_obj_center(lbl_yes);

    // "Start Over" button
    lv_obj_t* btn_no = lv_btn_create(card);
    lv_obj_set_pos(btn_no, 0, 270);
    lv_obj_set_size(btn_no, CARD_W - 32, 50);
    lv_obj_set_style_bg_color(btn_no, CLR_GUIDE, 0);
    lv_obj_set_style_bg_opa(btn_no, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_no, 0, 0);
    lv_obj_set_style_radius(btn_no, 6, 0);
    lv_obj_add_event_cb(btn_no, cb_resume_no, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl_no = lv_label_create(btn_no);
    lv_obj_add_style(lbl_no, &s_style_label_white, 0);
    lv_obj_set_style_text_font(lbl_no, FONT_UI, 0);
    lv_label_set_text(lbl_no, "Start Over");
    lv_obj_center(lbl_no);
}

// ============================================================================
// Event callbacks
// ============================================================================

static void cb_file_btn(lv_event_t* e) {
    lv_obj_t* btn  = lv_event_get_target(e);
    int       idx  = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (s_file_cb && idx >= 0 && idx < s_file_count) {
        s_file_cb(s_file_paths[idx]);
    }
}

static void cb_menu_btn(lv_event_t* e) {
    lv_obj_t* btn    = lv_event_get_target(e);
    int       action = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (s_menu_cb) {
        s_menu_cb(action);
    }
}

static void cb_resume_yes(lv_event_t* /*e*/) {
    if (s_resume_cb) s_resume_cb(true);
}

static void cb_resume_no(lv_event_t* /*e*/) {
    if (s_resume_cb) s_resume_cb(false);
}

// ============================================================================
// Public API implementation
// ============================================================================

void ui_init() {
    // Build shared styles
    build_style_screen(&s_style_screen);
    build_style_bar_bg(&s_style_bar_bg);
    build_style_label_white(&s_style_label_white);
    build_style_label_red(&s_style_label_red);
    build_style_list(&s_style_list);
    build_style_list_btn(&s_style_list_btn);
    build_style_list_btn_sel(&s_style_list_btn_sel);
    build_style_prog_track(&s_style_prog_track);
    build_style_prog_ind(&s_style_prog_ind);

    // Pre-create all screens
    build_screen_picker();
    build_screen_reader();
    build_screen_menu();
    build_screen_resume();
}

// ---------------------------------------------------------------------------
// Screen navigation
// ---------------------------------------------------------------------------

void ui_show_file_picker() {
    lv_scr_load_anim(s_scr_picker, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
}

void ui_show_reader() {
    lv_scr_load_anim(s_scr_reader, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
}

void ui_show_menu() {
    lv_scr_load_anim(s_scr_menu, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
}

void ui_show_resume_prompt(const char* book_path, int chapter, int word, int wpm) {
    char buf[256];
    // Extract filename from path for brevity
    const char* fname = strrchr(book_path, '/');
    fname = fname ? fname + 1 : book_path;

    snprintf(buf, sizeof(buf),
             "Resume reading?\n\n%s\n\nChapter %d, word %d\n%d WPM",
             fname, chapter + 1, word, wpm);
    lv_label_set_text(s_resume_msg_lbl, buf);
    lv_scr_load_anim(s_scr_resume, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
}

// ---------------------------------------------------------------------------
// Reader screen updates
// ---------------------------------------------------------------------------

void ui_update_word(const char* word, int orp_index) {
    if (!word || word[0] == '\0') {
        lv_label_set_text(s_lbl_before, "");
        lv_label_set_text(s_lbl_pivot,  "");
        lv_label_set_text(s_lbl_after,  "");
        return;
    }

    int len = (int)strlen(word);

    // Clamp orp_index to valid range
    if (orp_index < 0)   orp_index = 0;
    if (orp_index >= len) orp_index = len - 1;

    // Build "before" string (chars 0 .. orp_index-1)
    static char before_buf[128];
    static char pivot_buf[8];
    static char after_buf[128];

    int before_len = orp_index;
    if (before_len >= (int)sizeof(before_buf))
        before_len = (int)sizeof(before_buf) - 1;

    if (before_len > 0) {
        memcpy(before_buf, word, before_len);
        before_buf[before_len] = '\0';
    } else {
        before_buf[0] = '\0';
    }

    // Build pivot char (single UTF-8 code point at orp_index)
    // For ASCII this is always 1 byte; for multi-byte UTF-8 we copy up to 4 bytes.
    {
        int pi = 0;
        unsigned char c = (unsigned char)word[orp_index];
        int cp_len = 1;
        if      ((c & 0x80) == 0x00) cp_len = 1;
        else if ((c & 0xE0) == 0xC0) cp_len = 2;
        else if ((c & 0xF0) == 0xE0) cp_len = 3;
        else if ((c & 0xF8) == 0xF0) cp_len = 4;

        if (orp_index + cp_len > len) cp_len = len - orp_index;
        if (cp_len > (int)sizeof(pivot_buf) - 1) cp_len = (int)sizeof(pivot_buf) - 1;

        memcpy(pivot_buf, word + orp_index, cp_len);
        pivot_buf[cp_len] = '\0';
        pi = cp_len; // suppress unused warning

        // "after" starts right after the pivot code point
        int after_start = orp_index + cp_len;
        int after_len   = len - after_start;
        if (after_len >= (int)sizeof(after_buf))
            after_len = (int)sizeof(after_buf) - 1;
        if (after_len > 0) {
            memcpy(after_buf, word + after_start, after_len);
            after_buf[after_len] = '\0';
        } else {
            after_buf[0] = '\0';
        }
        (void)pi;
    }

    lv_label_set_text(s_lbl_before, before_buf);
    lv_label_set_text(s_lbl_pivot,  pivot_buf);
    lv_label_set_text(s_lbl_after,  after_buf);
}

void ui_update_progress(int current_word, int total_words) {
    if (total_words <= 0) {
        lv_bar_set_value(s_prog_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(s_prog_pct_lbl, "0%");
        return;
    }
    int permille = (int)((long)current_word * 1000 / total_words);
    if (permille > 1000) permille = 1000;
    lv_bar_set_value(s_prog_bar, permille, LV_ANIM_OFF);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", permille / 10);
    lv_label_set_text(s_prog_pct_lbl, buf);
}

void ui_update_wpm(int wpm) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d WPM", wpm);
    lv_label_set_text(s_wpm_lbl, buf);
}

void ui_update_play_state(bool playing) {
    // UTF-8 for ▶ = E2 96 B6 ; UTF-8 for ⏸ (pause) = E2 8F B8
    lv_label_set_text(s_play_lbl,
                      playing ? "\xe2\x96\xb6 Playing"
                               : "\xe2\x8f\xb8 Paused");
}

void ui_update_chapter_info(const char* title, int chapter, int total_chapters) {
    lv_label_set_text(s_reader_title_lbl, title ? title : "");

    char buf[64];
    snprintf(buf, sizeof(buf), "Chapter %d of %d", chapter + 1, total_chapters);
    lv_label_set_text(s_reader_chap_lbl, buf);
}

// ---------------------------------------------------------------------------
// File picker population
// ---------------------------------------------------------------------------

void ui_set_file_list(const char** files, int count, const size_t* sizes) {
    // Remove old items
    lv_obj_clean(s_picker_list);

    s_file_count = 0;

    for (int i = 0; i < count && i < 32; ++i) {
        if (!files[i]) continue;

        // Copy path for callback
        strncpy(s_file_paths[s_file_count], files[i], 255);
        s_file_paths[s_file_count][255] = '\0';

        // Build display label: "filename   1.2 MB"
        char display[320];
        const char* fname = strrchr(files[i], '/');
        fname = fname ? fname + 1 : files[i];

        if (sizes) {
            size_t sz = sizes[i];
            if (sz >= 1024 * 1024) {
                snprintf(display, sizeof(display), "%s  %.1f MB",
                         fname, (double)sz / (1024.0 * 1024.0));
            } else {
                snprintf(display, sizeof(display), "%s  %zu KB",
                         fname, sz / 1024);
            }
        } else {
            snprintf(display, sizeof(display), "%s", fname);
        }

        lv_obj_t* btn = lv_list_add_btn(s_picker_list, nullptr, display);
        lv_obj_add_style(btn, &s_style_list_btn, 0);
        lv_obj_add_style(btn, &s_style_list_btn_sel, LV_STATE_FOCUSED);
        lv_obj_add_style(btn, &s_style_list_btn_sel, LV_STATE_PRESSED);
        lv_obj_set_user_data(btn, (void*)(intptr_t)s_file_count);
        lv_obj_add_event_cb(btn, cb_file_btn, LV_EVENT_CLICKED, nullptr);
        lv_obj_set_size(btn, DISPLAY_W, 60);

        s_file_count++;
    }

    // Update footer
    char footer[64];
    snprintf(footer, sizeof(footer),
             "%d book%s found  |  tap to open",
             s_file_count, s_file_count == 1 ? "" : "s");
    lv_label_set_text(s_picker_footer_lbl, footer);
}

// ---------------------------------------------------------------------------
// Callback registration
// ---------------------------------------------------------------------------

void ui_set_file_callback(ui_file_selected_cb_t cb)  { s_file_cb   = cb; }
void ui_set_resume_callback(ui_resume_cb_t cb)         { s_resume_cb = cb; }
void ui_set_menu_callback(ui_menu_action_cb_t cb)      { s_menu_cb   = cb; }
