#ifndef RSVP_ENGINE_H
#define RSVP_ENGINE_H

// =============================================================================
// RSVP Engine — Speed-Reading Core
// =============================================================================
// Manages word timing, ORP (Optimal Recognition Point) calculation,
// and reading state for the RSVP e-reader.
//
// State machine:
//   IDLE    → no book loaded, ready to receive one
//   LOADED  → book loaded, chapter text cached on SD
//   PLAYING → displaying words at timed intervals
//   PAUSED  → word visible but not advancing
//
// Call rsvp_tick() from loop() on every iteration.
// =============================================================================

#include <Arduino.h>

// -----------------------------------------------------------------------------
// Callbacks
// -----------------------------------------------------------------------------

// Called whenever the current word advances (or is seeked to).
// word     — pointer to the current word string (valid until next tick)
// orp_idx  — 0-based character index of the Optimal Recognition Point
typedef void (*rsvp_word_cb_t)(const char* word, int orp_idx);

// Called when playback state changes (play/pause).
typedef void (*rsvp_state_cb_t)(bool playing);

// Called when the chapter changes.
// chapter — 0-based chapter index
// total   — total chapter count in the current book
typedef void (*rsvp_chapter_cb_t)(int chapter, int total);

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

// Initialize the engine at the given WPM (clamped 100–1000).
void rsvp_init(int wpm);

// Open an EPUB file (delegates extraction to epub_parser; this module
// reads the resulting plaintext files).
void rsvp_open_book(const char* epub_path);

// Return the path of the currently open EPUB (or "" if none).
const char* rsvp_current_book();

// -----------------------------------------------------------------------------
// Chapter management
// -----------------------------------------------------------------------------

// Load a specific plaintext chapter file directly (e.g. "/tmp/ch_000.txt").
void rsvp_load_chapter(const char* txt_path);

void rsvp_next_chapter();
void rsvp_prev_chapter();
int  rsvp_current_chapter();   // 0-based index
int  rsvp_chapter_count();

// -----------------------------------------------------------------------------
// Playback control
// -----------------------------------------------------------------------------

void rsvp_play();
void rsvp_pause();
void rsvp_toggle();
bool rsvp_is_playing();

// Advance internal state; call from loop() on every iteration.
void rsvp_tick();

// -----------------------------------------------------------------------------
// Word access
// -----------------------------------------------------------------------------

// Pointer to the current word string. Valid until the next rsvp_tick() call
// that advances the word, or until a seek/chapter change.
const char* rsvp_current_word();

// 0-based character index of the Optimal Recognition Point in the current word.
int rsvp_orp_index();

void rsvp_seek_word(int word_idx);
int  rsvp_current_word_idx();
int  rsvp_total_words();

// -----------------------------------------------------------------------------
// WPM
// -----------------------------------------------------------------------------

void rsvp_set_wpm(int wpm);   // clamped 100–1000
int  rsvp_get_wpm();

// -----------------------------------------------------------------------------
// Callback registration
// -----------------------------------------------------------------------------

void rsvp_set_word_callback(rsvp_word_cb_t cb);
void rsvp_set_state_callback(rsvp_state_cb_t cb);
void rsvp_set_chapter_callback(rsvp_chapter_cb_t cb);

#endif // RSVP_ENGINE_H
