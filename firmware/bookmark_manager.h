#pragma once
#include <Arduino.h>

// Initialize NVS namespace — call once at startup
void bookmark_init();

// Save reading position for a book.
// Called periodically (~30s) and on pause/menu. Fast (~1ms NVS write).
void bookmark_save(const char* epub_path, int chapter, int word_idx, int wpm);

// Load reading position for a book.
// Returns true if a valid bookmark exists for epub_path.
// On return, *chapter, *word_idx, and *wpm are populated.
bool bookmark_load(const char* epub_path, int* chapter, int* word_idx, int* wpm);

// Return the full path of the last-read book, or NULL if none recorded.
// Points to an internal static buffer — do not free or modify.
const char* bookmark_last_book();

// Delete the bookmark for a book (e.g. when the user finishes or removes a book).
void bookmark_delete(const char* epub_path);
