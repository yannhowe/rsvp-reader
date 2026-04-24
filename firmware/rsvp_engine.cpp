// =============================================================================
// RSVP Engine — Speed-Reading Core
// =============================================================================
// C-style module — all state is file-static.
// No dynamic allocation during playback; everything is pre-allocated.
// =============================================================================

#include "rsvp_engine.h"
#include <SD_MMC.h>
#include <string.h>
#include <ctype.h>

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

#define WORD_BUF_COUNT   50      // ring-buffer capacity (number of words)
#define WORD_MAX_LEN     64      // max characters per word (including NUL)
#define REFILL_THRESHOLD 25      // refill when this many slots have been consumed
#define CHAPTER_MAX      64      // maximum chapters per book
#define WPM_MIN          100
#define WPM_MAX          1000

// Timing multipliers (stored as fixed-point * 100 to avoid float math)
// Applied by computing: interval = base * num / den
// Using floats is fine on ESP32 (hardware FPU), kept as literals for clarity.
#define PUNCT_COMMA_MULT   1.5f   // comma, semicolon
#define PUNCT_PERIOD_MULT  2.0f   // period, exclamation, question
#define LONG_WORD_MULT     1.25f  // word length > 10
#define SHORT_WORD_MULT    0.75f  // word length <= 3

// -----------------------------------------------------------------------------
// State machine
// -----------------------------------------------------------------------------

typedef enum {
    STATE_IDLE = 0,
    STATE_LOADED,
    STATE_PLAYING,
    STATE_PAUSED,
} RsvpState;

// -----------------------------------------------------------------------------
// Internal state
// -----------------------------------------------------------------------------

// -- Playback --
static RsvpState    s_state       = STATE_IDLE;
static int          s_wpm         = 300;
static unsigned long s_last_ms    = 0;   // millis() at last word advance
static unsigned long s_interval   = 200; // ms until next advance

// -- Word ring buffer --
// words[i] holds a NUL-terminated string of at most WORD_MAX_LEN-1 chars.
static char s_words[WORD_BUF_COUNT][WORD_MAX_LEN];
static int  s_buf_head  = 0;  // index of current (front) word in ring
static int  s_buf_count = 0;  // number of valid words currently buffered

// -- File read position --
// We keep the SD File open during playback and refill from it.
static File s_file;
static bool s_file_open    = false;
static bool s_file_eof     = false;

// Leftover partial token from the last SD read block (may span read calls).
static char s_leftover[WORD_MAX_LEN];
static int  s_leftover_len = 0;

// -- Chapter / book --
static char s_book_path[256];
static char s_chapter_paths[CHAPTER_MAX][64]; // "/tmp/ch_XXX.txt"
static int  s_chapter_count  = 0;
static int  s_current_chapter = 0;

// -- Word position tracking --
// We track absolute word index relative to the start of the current chapter.
// Words already consumed from the ring buffer are counted in s_word_base.
static int s_word_base    = 0;  // words consumed before current buffer
static int s_total_words  = 0;  // estimated total (grows as we read; final on EOF)

// -- Callbacks --
static rsvp_word_cb_t    s_word_cb    = nullptr;
static rsvp_state_cb_t   s_state_cb   = nullptr;
static rsvp_chapter_cb_t s_chapter_cb = nullptr;

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------

static void     _buf_refill();
static bool     _buf_read_next_word(char* out, int max_len);
static void     _advance_word();
static unsigned long _calc_interval(const char* word);
static int      _calc_orp(const char* word);
static void     _fire_word_cb();
static void     _open_chapter_file(const char* path);
static void     _close_chapter_file();
static void     _set_state(RsvpState new_state);

// -----------------------------------------------------------------------------
// ORP calculation
// -----------------------------------------------------------------------------
// Returns 0-based character index of the Optimal Recognition Point.
//
//  Length   ORP index
//  1        0
//  2–5      1
//  6–9      2
//  10–13    3
//  14+      4

static int _calc_orp(const char* word) {
    if (!word || word[0] == '\0') return 0;
    int len = (int)strlen(word);
    if (len <= 1)  return 0;
    if (len <= 5)  return 1;
    if (len <= 9)  return 2;
    if (len <= 13) return 3;
    return 4;
}

// -----------------------------------------------------------------------------
// Interval calculation
// -----------------------------------------------------------------------------
// Base: 60000 / WPM ms per word.
// Multipliers are applied based on the last character of the word and length.
// Multiple multipliers can stack (e.g. a long word ending in '.' gets both).

static unsigned long _calc_interval(const char* word) {
    float base = 60000.0f / (float)s_wpm;
    float mult = 1.0f;

    if (!word || word[0] == '\0') return (unsigned long)base;

    int len = (int)strlen(word);
    char last = word[len - 1];

    // Punctuation multiplier
    if (last == '.' || last == '!' || last == '?') {
        mult *= PUNCT_PERIOD_MULT;
    } else if (last == ',' || last == ';') {
        mult *= PUNCT_COMMA_MULT;
    }

    // Length multiplier
    if (len > 10) {
        mult *= LONG_WORD_MULT;
    } else if (len <= 3) {
        mult *= SHORT_WORD_MULT;
    }

    return (unsigned long)(base * mult + 0.5f);
}

// -----------------------------------------------------------------------------
// File / buffer helpers
// -----------------------------------------------------------------------------

static void _open_chapter_file(const char* path) {
    _close_chapter_file();

    s_file = SD_MMC.open(path, FILE_READ);
    if (!s_file) {
        Serial.printf("[RSVP] Failed to open chapter file: %s\n", path);
        s_file_open = false;
        s_file_eof  = true;
        return;
    }
    s_file_open = true;
    s_file_eof  = false;
    s_leftover_len = 0;
    s_leftover[0]  = '\0';
}

static void _close_chapter_file() {
    if (s_file_open && s_file) {
        s_file.close();
    }
    s_file_open = false;
    s_file_eof  = false;
}

// Read the next whitespace-delimited token from the open SD file into `out`.
// Returns true if a token was produced, false on EOF / error.
// Handles tokens that span read-block boundaries via s_leftover.
static bool _buf_read_next_word(char* out, int max_len) {
    // We read one byte at a time for simplicity (SD buffering handles perf).
    // This avoids a scratch buffer and is cache-friendly on Arduino SD libs.
    int out_pos = 0;

    // Drain any leftover partial token from a previous call first.
    if (s_leftover_len > 0) {
        int copy = (s_leftover_len < max_len - 1) ? s_leftover_len : max_len - 1;
        memcpy(out, s_leftover, copy);
        out_pos = copy;
        s_leftover_len = 0;
        s_leftover[0]  = '\0';
    }

    while (true) {
        if (!s_file_open || s_file_eof) break;

        int b = s_file.read();
        if (b < 0) {
            // EOF
            s_file_eof = true;
            break;
        }

        char c = (char)b;

        if (isspace((unsigned char)c)) {
            // Whitespace: if we have accumulated chars, token is complete.
            if (out_pos > 0) break;
            // Otherwise skip leading whitespace.
        } else {
            if (out_pos < max_len - 1) {
                out[out_pos++] = c;
            }
            // If the token is already at max length, keep reading until
            // whitespace to consume the rest (drop overflow silently).
        }
    }

    if (out_pos == 0) return false;
    out[out_pos] = '\0';
    return true;
}

// Fill empty slots in the ring buffer from the SD file.
// Called when the buffer drops to REFILL_THRESHOLD or below.
static void _buf_refill() {
    if (!s_file_open || s_file_eof) return;

    while (s_buf_count < WORD_BUF_COUNT) {
        int slot = (s_buf_head + s_buf_count) % WORD_BUF_COUNT;
        if (_buf_read_next_word(s_words[slot], WORD_MAX_LEN)) {
            s_buf_count++;
            s_total_words = s_word_base + s_buf_count;
        } else {
            break; // EOF or error
        }
    }

    // Update total_words estimate now we know how far we've read.
    // If EOF, this is exact; otherwise it's a lower-bound.
    if (s_file_eof) {
        s_total_words = s_word_base + s_buf_count;
    }
}

// -----------------------------------------------------------------------------
// Internal state helpers
// -----------------------------------------------------------------------------

static void _set_state(RsvpState new_state) {
    if (s_state == new_state) return;
    s_state = new_state;
    bool playing = (new_state == STATE_PLAYING);
    if (s_state_cb) s_state_cb(playing);
}

static void _fire_word_cb() {
    if (!s_word_cb || s_buf_count == 0) return;
    const char* w = s_words[s_buf_head];
    s_word_cb(w, _calc_orp(w));
}

// Advance to the next word in the ring buffer.
static void _advance_word() {
    if (s_buf_count == 0) return;

    // Move head forward, freeing the consumed slot.
    s_buf_head  = (s_buf_head + 1) % WORD_BUF_COUNT;
    s_buf_count--;
    s_word_base++;

    // Refill if we've consumed half the buffer.
    if (s_buf_count <= REFILL_THRESHOLD) {
        _buf_refill();
    }

    // Reached end of chapter?
    if (s_buf_count == 0 && s_file_eof) {
        // Auto-advance to next chapter if available.
        if (s_current_chapter + 1 < s_chapter_count) {
            rsvp_next_chapter();
        } else {
            // End of book.
            _set_state(STATE_LOADED);
            Serial.println("[RSVP] End of book reached.");
        }
        return;
    }

    // Schedule next word.
    s_interval = _calc_interval(s_words[s_buf_head]);
    _fire_word_cb();
}

// -----------------------------------------------------------------------------
// Public API — Lifecycle
// -----------------------------------------------------------------------------

void rsvp_init(int wpm) {
    s_state           = STATE_IDLE;
    s_buf_head        = 0;
    s_buf_count       = 0;
    s_file_open       = false;
    s_file_eof        = false;
    s_leftover_len    = 0;
    s_word_base       = 0;
    s_total_words     = 0;
    s_chapter_count   = 0;
    s_current_chapter = 0;
    s_book_path[0]    = '\0';
    s_last_ms         = 0;
    s_word_cb         = nullptr;
    s_state_cb        = nullptr;
    s_chapter_cb      = nullptr;

    rsvp_set_wpm(wpm);
    Serial.printf("[RSVP] Engine initialized at %d WPM\n", s_wpm);
}

// Open a book: the epub_parser is expected to have already extracted chapter
// plaintext files to /tmp/ch_000.txt … /tmp/ch_NNN.txt on the SD card.
// This function discovers those files and loads chapter 0.
void rsvp_open_book(const char* epub_path) {
    if (!epub_path || epub_path[0] == '\0') return;

    strncpy(s_book_path, epub_path, sizeof(s_book_path) - 1);
    s_book_path[sizeof(s_book_path) - 1] = '\0';

    // Discover chapter files: /tmp/ch_000.txt, /tmp/ch_001.txt, …
    s_chapter_count = 0;
    for (int i = 0; i < CHAPTER_MAX; i++) {
        snprintf(s_chapter_paths[i], sizeof(s_chapter_paths[i]),
                 "/tmp/ch_%03d.txt", i);
        if (!SD_MMC.exists(s_chapter_paths[i])) break;
        s_chapter_count++;
    }

    if (s_chapter_count == 0) {
        Serial.printf("[RSVP] No chapter files found for: %s\n", epub_path);
        _set_state(STATE_IDLE);
        return;
    }

    Serial.printf("[RSVP] Book opened: %s (%d chapters)\n",
                  epub_path, s_chapter_count);

    s_current_chapter = 0;
    rsvp_load_chapter(s_chapter_paths[0]);

    if (s_chapter_cb) s_chapter_cb(s_current_chapter, s_chapter_count);
}

const char* rsvp_current_book() {
    return s_book_path;
}

// -----------------------------------------------------------------------------
// Public API — Chapter management
// -----------------------------------------------------------------------------

void rsvp_load_chapter(const char* txt_path) {
    bool was_playing = (s_state == STATE_PLAYING);
    _set_state(STATE_IDLE);  // pause callbacks during reload

    // Reset buffer state.
    s_buf_head     = 0;
    s_buf_count    = 0;
    s_word_base    = 0;
    s_total_words  = 0;

    _open_chapter_file(txt_path);
    _buf_refill();

    if (s_buf_count > 0) {
        _set_state(was_playing ? STATE_PLAYING : STATE_LOADED);
        s_interval = _calc_interval(s_words[s_buf_head]);
        s_last_ms  = millis();
        _fire_word_cb();
        Serial.printf("[RSVP] Chapter loaded: %s (%d words buffered)\n",
                      txt_path, s_buf_count);
    } else {
        Serial.printf("[RSVP] Chapter empty or unreadable: %s\n", txt_path);
        _set_state(STATE_IDLE);
    }
}

void rsvp_next_chapter() {
    if (s_chapter_count == 0) return;
    if (s_current_chapter + 1 >= s_chapter_count) return;

    bool was_playing = (s_state == STATE_PLAYING);
    s_current_chapter++;
    rsvp_load_chapter(s_chapter_paths[s_current_chapter]);
    if (was_playing) _set_state(STATE_PLAYING);
    if (s_chapter_cb) s_chapter_cb(s_current_chapter, s_chapter_count);
}

void rsvp_prev_chapter() {
    if (s_chapter_count == 0) return;
    if (s_current_chapter == 0) return;

    bool was_playing = (s_state == STATE_PLAYING);
    s_current_chapter--;
    rsvp_load_chapter(s_chapter_paths[s_current_chapter]);
    if (was_playing) _set_state(STATE_PLAYING);
    if (s_chapter_cb) s_chapter_cb(s_current_chapter, s_chapter_count);
}

int rsvp_current_chapter() {
    return s_current_chapter;
}

int rsvp_chapter_count() {
    return s_chapter_count;
}

// -----------------------------------------------------------------------------
// Public API — Playback control
// -----------------------------------------------------------------------------

void rsvp_play() {
    if (s_state == STATE_IDLE) return;
    if (s_state == STATE_PLAYING) return;
    s_last_ms = millis();  // reset timer so we don't immediately skip
    _set_state(STATE_PLAYING);
}

void rsvp_pause() {
    if (s_state != STATE_PLAYING) return;
    _set_state(STATE_PAUSED);
}

void rsvp_toggle() {
    if (s_state == STATE_PLAYING) {
        rsvp_pause();
    } else if (s_state == STATE_PAUSED || s_state == STATE_LOADED) {
        rsvp_play();
    }
}

bool rsvp_is_playing() {
    return s_state == STATE_PLAYING;
}

// Must be called from loop() on every iteration.
void rsvp_tick() {
    if (s_state != STATE_PLAYING) return;
    if (s_buf_count == 0) return;

    unsigned long now = millis();
    if (now - s_last_ms >= s_interval) {
        s_last_ms = now;
        _advance_word();
    }
}

// -----------------------------------------------------------------------------
// Public API — Word access
// -----------------------------------------------------------------------------

const char* rsvp_current_word() {
    if (s_buf_count == 0) return "";
    return s_words[s_buf_head];
}

int rsvp_orp_index() {
    if (s_buf_count == 0) return 0;
    return _calc_orp(s_words[s_buf_head]);
}

void rsvp_seek_word(int word_idx) {
    if (word_idx < 0) word_idx = 0;
    if (s_state == STATE_IDLE) return;

    bool was_playing = (s_state == STATE_PLAYING);
    _set_state(STATE_PAUSED);

    // Re-open the current chapter file and scan forward to word_idx.
    // We do a fast scan: read and discard words until we reach the target.
    _close_chapter_file();
    s_buf_head    = 0;
    s_buf_count   = 0;
    s_word_base   = 0;
    s_total_words = 0;
    s_leftover_len = 0;

    _open_chapter_file(s_chapter_paths[s_current_chapter]);

    // Discard words before the target index.
    char discard[WORD_MAX_LEN];
    for (int i = 0; i < word_idx; i++) {
        if (!_buf_read_next_word(discard, WORD_MAX_LEN)) break;
    }
    s_word_base = word_idx;

    // Fill the ring buffer starting at target.
    _buf_refill();

    if (s_buf_count > 0) {
        s_interval = _calc_interval(s_words[s_buf_head]);
        s_last_ms  = millis();
        _fire_word_cb();
    }

    if (was_playing) _set_state(STATE_PLAYING);
    else             _set_state(STATE_PAUSED);
}

int rsvp_current_word_idx() {
    return s_word_base;
}

int rsvp_total_words() {
    return s_total_words;
}

// -----------------------------------------------------------------------------
// Public API — WPM
// -----------------------------------------------------------------------------

void rsvp_set_wpm(int wpm) {
    if (wpm < WPM_MIN) wpm = WPM_MIN;
    if (wpm > WPM_MAX) wpm = WPM_MAX;
    s_wpm = wpm;
    // Recalculate interval for the current word immediately.
    if (s_buf_count > 0) {
        s_interval = _calc_interval(s_words[s_buf_head]);
    }
}

int rsvp_get_wpm() {
    return s_wpm;
}

// -----------------------------------------------------------------------------
// Public API — Callbacks
// -----------------------------------------------------------------------------

void rsvp_set_word_callback(rsvp_word_cb_t cb) {
    s_word_cb = cb;
}

void rsvp_set_state_callback(rsvp_state_cb_t cb) {
    s_state_cb = cb;
}

void rsvp_set_chapter_callback(rsvp_chapter_cb_t cb) {
    s_chapter_cb = cb;
}
