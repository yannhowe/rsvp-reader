// =============================================================================
// epub_parser.h — EPUB file parser for RSVP Reader
// =============================================================================
// Parses EPUB files (ZIP archives) from SD card. Extracts chapter text as
// plaintext files cached at /tmp/ch_XXX.txt (SD_MMC paths, no VFS prefix).
//
// EPUB structure:
//   META-INF/container.xml  → path to .opf file
//   .opf file               → spine (reading order) + manifest (file paths)
//   chapter XHTML files     → stripped to plaintext
// =============================================================================

#ifndef EPUB_PARSER_H
#define EPUB_PARSER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// Limits
// -----------------------------------------------------------------------------

#define EPUB_MAX_CHAPTERS       200
#define EPUB_MAX_META_LEN       128
#define EPUB_MAX_CHAPTER_SIZE   (256 * 1024)   // 256 KB per chapter XHTML
#define EPUB_TMP_DIR            "/tmp"
#define EPUB_TMP_PATH_FMT       "/tmp/ch_%03d.txt"

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

// Open an EPUB file from SD card. Reads container.xml → OPF → spine/manifest.
// Must be called before any other epub_* function.
// Returns true on success.
bool epub_open(const char* epub_path);

// Title from <dc:title> in OPF metadata. Empty string if not found.
const char* epub_title();

// Author from <dc:creator> in OPF metadata. Empty string if not found.
const char* epub_author();

// Number of chapters (spine items) found in the OPF.
int epub_chapter_count();

// Display name for chapter at index. Returns the idref from the spine
// (or href basename if no better name is available). Empty string on bad index.
const char* epub_chapter_name(int idx);

// Extract chapter text to /tmp/ch_XXX.txt on SD card.
// Reads the XHTML from the ZIP, strips tags via state machine, writes plaintext.
// Returns true on success.
bool epub_extract_chapter(int chapter_idx);

// Extract all chapters in spine order. Returns the number successfully extracted.
int epub_extract_all();

// Path to the extracted plaintext file for a chapter.
// Returns NULL if the chapter has not been extracted yet.
const char* epub_chapter_path(int chapter_idx);

// Count words in an already-extracted chapter file.
// A "word" is any whitespace-delimited token.
// Returns -1 if the chapter has not been extracted.
int epub_chapter_word_count(int chapter_idx);

// Close the EPUB and free all resources.
void epub_close();

#ifdef __cplusplus
}
#endif

#endif // EPUB_PARSER_H
