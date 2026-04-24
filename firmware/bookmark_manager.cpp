#include "bookmark_manager.h"
#include <Preferences.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static Preferences prefs;
static bool        prefs_open        = false;

// Static buffers — max path length for EPUB files
static const int   MAX_PATH_LEN      = 256;
static char        s_last_book[MAX_PATH_LEN];  // cached last-read path

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// FNV-1a 32-bit hash of a null-terminated string.
static uint32_t fnv1a_32(const char* str) {
    uint32_t hash = 0x811c9dc5u;
    while (*str) {
        hash ^= (uint8_t)(*str++);
        hash *= 0x01000193u;
    }
    return hash;
}

// Build the key prefix for a given epub_path, e.g. "bk_a3f2c801".
// out must be at least 12 bytes.
static void make_key_prefix(const char* epub_path, char* out) {
    uint32_t h = fnv1a_32(epub_path);
    snprintf(out, 12, "bk_%08x", (unsigned)h);
}

// Ensure Preferences is open; if NVS is corrupt, clear and re-open.
static bool ensure_open() {
    if (prefs_open) return true;

    if (!prefs.begin("rsvp", false)) {
        // NVS namespace may be corrupted — clear and retry
        prefs.clear();
        prefs.end();
        if (!prefs.begin("rsvp", false)) {
            return false;
        }
    }
    prefs_open = true;
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void bookmark_init() {
    s_last_book[0] = '\0';

    if (!ensure_open()) {
        // Could not open NVS at all — degrade gracefully
        return;
    }

    // Pre-load last-read book path into the static buffer
    String stored = prefs.getString("last_book", "");
    if (stored.length() > 0 && stored.length() < MAX_PATH_LEN) {
        strncpy(s_last_book, stored.c_str(), MAX_PATH_LEN - 1);
        s_last_book[MAX_PATH_LEN - 1] = '\0';
    }
}

void bookmark_save(const char* epub_path, int chapter, int word_idx, int wpm) {
    if (!epub_path || !ensure_open()) return;

    char prefix[12];
    make_key_prefix(epub_path, prefix);

    // Key strings: prefix + suffix (NVS key max is 15 chars)
    char k_ch[16], k_wi[16], k_wpm[16], k_path[16];
    snprintf(k_ch,   sizeof(k_ch),   "%s_ch",  prefix);
    snprintf(k_wi,   sizeof(k_wi),   "%s_wi",  prefix);
    snprintf(k_wpm,  sizeof(k_wpm),  "%s_wm",  prefix);  // "wm" to stay ≤15
    snprintf(k_path, sizeof(k_path), "%s_pt",  prefix);  // "pt" to stay ≤15

    // Check for hash collision: if a different book already occupies this slot,
    // refuse to overwrite it (data loss prevention).
    String existing_path = prefs.getString(k_path, "");
    if (existing_path.length() > 0 && strcmp(existing_path.c_str(), epub_path) != 0) {
        Serial.printf("[bookmark] Hash collision for '%s' — slot occupied by '%s', skipping save\n",
                      epub_path, existing_path.c_str());
        return;
    }

    prefs.putInt(k_ch,  chapter);
    prefs.putInt(k_wi,  word_idx);
    prefs.putInt(k_wpm, wpm);
    prefs.putString(k_path, epub_path);

    // Update last-read book
    prefs.putString("last_book", epub_path);
    strncpy(s_last_book, epub_path, MAX_PATH_LEN - 1);
    s_last_book[MAX_PATH_LEN - 1] = '\0';
}

bool bookmark_load(const char* epub_path, int* chapter, int* word_idx, int* wpm) {
    if (!epub_path || !chapter || !word_idx || !wpm) return false;
    if (!ensure_open()) return false;

    char prefix[12];
    make_key_prefix(epub_path, prefix);

    char k_ch[16], k_wi[16], k_wpm[16], k_path[16];
    snprintf(k_ch,   sizeof(k_ch),   "%s_ch",  prefix);
    snprintf(k_wi,   sizeof(k_wi),   "%s_wi",  prefix);
    snprintf(k_wpm,  sizeof(k_wpm),  "%s_wm",  prefix);
    snprintf(k_path, sizeof(k_path), "%s_pt",  prefix);

    // Verify the stored path matches (collision check)
    String stored_path = prefs.getString(k_path, "");
    if (stored_path.length() == 0) return false;               // no bookmark
    if (strcmp(stored_path.c_str(), epub_path) != 0) return false; // hash collision

    // Sentinel: chapter -1 means no valid bookmark was ever written
    int ch = prefs.getInt(k_ch, -1);
    if (ch < 0) return false;

    *chapter  = ch;
    *word_idx = prefs.getInt(k_wi,  0);
    *wpm      = prefs.getInt(k_wpm, 250);  // sensible default WPM

    return true;
}

const char* bookmark_last_book() {
    if (s_last_book[0] == '\0') return nullptr;
    return s_last_book;
}

void bookmark_delete(const char* epub_path) {
    if (!epub_path || !ensure_open()) return;

    char prefix[12];
    make_key_prefix(epub_path, prefix);

    char k_ch[16], k_wi[16], k_wpm[16], k_path[16];
    snprintf(k_ch,   sizeof(k_ch),   "%s_ch",  prefix);
    snprintf(k_wi,   sizeof(k_wi),   "%s_wi",  prefix);
    snprintf(k_wpm,  sizeof(k_wpm),  "%s_wm",  prefix);
    snprintf(k_path, sizeof(k_path), "%s_pt",  prefix);

    prefs.remove(k_ch);
    prefs.remove(k_wi);
    prefs.remove(k_wpm);
    prefs.remove(k_path);

    // If this was the last-read book, clear that pointer too
    if (strcmp(s_last_book, epub_path) == 0) {
        s_last_book[0] = '\0';
        prefs.remove("last_book");
    }
}
