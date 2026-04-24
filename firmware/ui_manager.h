#pragma once

#include <Arduino.h>
#include <stddef.h>
#include "imu_controls.h"  // for Orientation enum

// Initialize all UI screens (call once in setup())
void ui_init();

// Screen navigation
void ui_show_file_picker();
void ui_show_reader();
void ui_show_menu();
void ui_show_resume_prompt(const char* book_path, int chapter, int word, int wpm);

// Reader screen updates
void ui_update_word(const char* word, int orp_index);
void ui_update_progress(int current_word, int total_words);
void ui_update_wpm(int wpm);
void ui_update_play_state(bool playing);
void ui_update_chapter_info(const char* title, int chapter, int total_chapters);

// Handle orientation change — repositions LVGL elements
void ui_set_orientation(Orientation orient);

// File picker
void ui_set_file_list(const char** files, int count, const size_t* sizes);

// Callbacks (set by main sketch to connect UI events to engine)
typedef void (*ui_file_selected_cb_t)(const char* path);
typedef void (*ui_resume_cb_t)(bool resume);        // true=resume, false=start new
typedef void (*ui_menu_action_cb_t)(int action);    // 0=resume, 1=chapter, 2=wpm, 3=picker, 4=settings

void ui_set_file_callback(ui_file_selected_cb_t cb);
void ui_set_resume_callback(ui_resume_cb_t cb);
void ui_set_menu_callback(ui_menu_action_cb_t cb);

// Update the text of a menu button at runtime (0-indexed).
// Call before ui_show_menu() to reflect dynamic state (e.g. "WiFi: ON").
void ui_set_menu_label(int index, const char* text);
