// =============================================================================
// epub_parser.cpp — EPUB file parser implementation
// =============================================================================
// Parses EPUB (ZIP) files from SD card. Extracts chapter XHTML to plaintext
// cached at /tmp/ch_XXX.txt.
//
// Design notes:
//   - No dynamic allocation during parsing; static buffers throughout.
//   - XML/XHTML parsing is a simple character-by-character state machine,
//     not a full XML parser. Handles the common subset found in EPUB files.
//   - miniz is used for ZIP decompression (single-header, compiled separately).
// =============================================================================

#include "epub_parser.h"

#include <Arduino.h>
#include <SD_MMC.h>
#include <FS.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "lib/miniz/miniz.h"

// =============================================================================
// Internal state
// =============================================================================

// One chapter entry from the spine + manifest
struct ChapterEntry {
    char idref[128];          // idref attribute from <itemref>
    char href[256];           // href from manifest (relative to OPF dir)
    char out_path[64];        // /tmp/ch_XXX.txt  (empty = not extracted)
    bool extracted;
};

static mz_zip_archive      s_zip;
static bool                s_zip_open       = false;

static char                s_title[EPUB_MAX_META_LEN];
static char                s_author[EPUB_MAX_META_LEN];
static char                s_opf_dir[256];   // directory part of OPF path

static ChapterEntry        s_chapters[EPUB_MAX_CHAPTERS];
static int                 s_chapter_count  = 0;

// Scratch buffer for in-memory file content (reused for every file we read)
static char                s_xhtml_buf[EPUB_MAX_CHAPTER_SIZE];

// Small scratch buffer for XML parsing passes
static char                s_scratch[1024];

// =============================================================================
// Utility helpers
// =============================================================================

// Copy up to dst_size-1 chars, always NUL-terminate
static void safe_copy(char* dst, const char* src, size_t dst_size) {
    if (!dst || !src || dst_size == 0) return;
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

// Trim leading/trailing ASCII whitespace in-place
static void trim_inplace(char* s) {
    if (!s) return;
    // Trim leading
    char* start = s;
    while (*start && (uint8_t)*start <= ' ') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    // Trim trailing
    int len = strlen(s);
    while (len > 0 && (uint8_t)s[len - 1] <= ' ') s[--len] = '\0';
}

// Case-insensitive prefix check
static bool istr_starts(const char* s, const char* prefix) {
    while (*prefix) {
        if (tolower((uint8_t)*s) != tolower((uint8_t)*prefix)) return false;
        s++; prefix++;
    }
    return true;
}

// Extract attribute value from a tag string: attr="value" or attr='value'
// tag_buf: the buffered tag content (without < >)
// attr:    attribute name (e.g. "full-path")
// out:     destination buffer
// out_len: size of destination buffer
// Returns true if found
static bool extract_attr(const char* tag_buf, const char* attr,
                         char* out, size_t out_len) {
    if (!tag_buf || !attr || !out || out_len == 0) return false;
    out[0] = '\0';

    const char* p = tag_buf;
    size_t attr_len = strlen(attr);

    while (*p) {
        // Skip whitespace
        while (*p && (uint8_t)*p <= ' ') p++;
        if (!*p) break;

        // Check if this token matches the attribute name
        if (strncasecmp(p, attr, attr_len) == 0) {
            const char* q = p + attr_len;
            while (*q && (uint8_t)*q <= ' ') q++;
            if (*q == '=') {
                q++;
                while (*q && (uint8_t)*q <= ' ') q++;
                char delim = 0;
                if (*q == '"' || *q == '\'') { delim = *q; q++; }
                size_t n = 0;
                while (*q && n < out_len - 1) {
                    if (delim && *q == delim) break;
                    if (!delim && ((uint8_t)*q <= ' ' || *q == '>')) break;
                    out[n++] = *q++;
                }
                out[n] = '\0';
                return n > 0;
            }
        }

        // Advance past this attribute token (skip to next whitespace or =)
        while (*p && (uint8_t)*p > ' ' && *p != '=') p++;
        if (*p == '=') {
            p++;
            if (*p == '"' || *p == '\'') {
                char d = *p++;
                while (*p && *p != d) p++;
                if (*p) p++;
            } else {
                while (*p && (uint8_t)*p > ' ') p++;
            }
        }
    }
    return false;
}

// Get directory portion of a path (everything up to and including last '/')
// Result is stored in out[out_len]. Empty string if no slash.
static void path_dir(const char* path, char* out, size_t out_len) {
    if (!path || !out || out_len == 0) return;
    const char* last_slash = strrchr(path, '/');
    if (!last_slash) {
        out[0] = '\0';
        return;
    }
    size_t n = (size_t)(last_slash - path + 1);  // include the slash
    if (n >= out_len) n = out_len - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

// Join OPF dir + relative href, normalising simple "../" sequences.
// Result written into out[out_len].
static void resolve_href(const char* base_dir, const char* href,
                         char* out, size_t out_len) {
    if (!base_dir || !href || !out || out_len == 0) return;

    // Absolute path: use directly
    if (href[0] == '/') {
        safe_copy(out, href, out_len);
        return;
    }

    char tmp[512];
    // Concatenate base_dir + href
    size_t dir_len = strlen(base_dir);
    if (dir_len + strlen(href) + 1 >= sizeof(tmp)) {
        // Fallback: just use href
        safe_copy(out, href, out_len);
        return;
    }
    memcpy(tmp, base_dir, dir_len);
    strcpy(tmp + dir_len, href);

    // Normalise: remove "seg/../" sequences
    char resolved[512];
    resolved[0] = '\0';
    char* r = resolved;
    char* src = tmp;

    while (*src) {
        // Copy a path segment
        char seg[256];
        int seg_len = 0;
        while (*src && *src != '/') {
            if (seg_len < (int)sizeof(seg) - 1) seg[seg_len++] = *src;
            src++;
        }
        seg[seg_len] = '\0';

        if (strcmp(seg, "..") == 0) {
            // Remove last segment from resolved
            if (r > resolved) r--;         // step back over trailing '/'
            while (r > resolved && *(r-1) != '/') r--;
        } else if (strcmp(seg, ".") != 0) {
            size_t written = r - resolved;
            if (written + seg_len + 2 < sizeof(resolved)) {
                memcpy(r, seg, seg_len);
                r += seg_len;
                if (*src == '/') { *r++ = '/'; }
            }
        }

        if (*src == '/') src++;  // skip separator
    }
    *r = '\0';
    safe_copy(out, resolved, out_len);
}

// =============================================================================
// ZIP helpers (thin wrappers over miniz)
// =============================================================================

// Extract a named file from the open ZIP into s_xhtml_buf.
// Returns actual size written, or 0 on error.
// NUL-terminates the buffer.
static size_t zip_extract_to_buf(const char* filename) {
    size_t out_size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(
                     &s_zip, filename, &out_size, 0);
    if (!data) {
        Serial.print("[epub] ZIP extract failed: ");
        Serial.println(filename);
        return 0;
    }
    if (out_size >= EPUB_MAX_CHAPTER_SIZE) {
        Serial.print("[epub] File too large (truncating): ");
        Serial.println(filename);
        out_size = EPUB_MAX_CHAPTER_SIZE - 1;
    }
    memcpy(s_xhtml_buf, data, out_size);
    s_xhtml_buf[out_size] = '\0';
    mz_free(data);
    return out_size;
}

// =============================================================================
// container.xml parser
// =============================================================================
// Scans for <rootfile full-path="..."/> and extracts the full-path value.

static bool parse_container_xml(char* opf_path_out, size_t out_len) {
    if (zip_extract_to_buf("META-INF/container.xml") == 0) return false;

    const char* p = s_xhtml_buf;
    while (*p) {
        // Look for "rootfile"
        const char* found = strstr(p, "rootfile");
        if (!found) break;
        p = found + 8;

        // Scan backwards to find the opening '<' of this tag
        const char* tag_start = found;
        while (tag_start > s_xhtml_buf && *tag_start != '<') tag_start--;

        // Find the closing '>'
        const char* tag_end = p;
        while (*tag_end && *tag_end != '>') tag_end++;

        // Copy tag content into scratch (excluding < and >)
        size_t tag_len = (size_t)(tag_end - tag_start - 1);
        if (tag_len >= sizeof(s_scratch)) tag_len = sizeof(s_scratch) - 1;
        memcpy(s_scratch, tag_start + 1, tag_len);
        s_scratch[tag_len] = '\0';

        if (extract_attr(s_scratch, "full-path", opf_path_out, out_len)) {
            return true;
        }
        p = tag_end + 1;
    }
    Serial.println("[epub] container.xml: rootfile full-path not found");
    return false;
}

// =============================================================================
// OPF parser
// =============================================================================
// Two-pass approach:
//   Pass 1: extract <dc:title>, <dc:creator>
//   Pass 2: build manifest ID→href map, then walk spine to build chapter list

// Manifest entry (temporary, used during OPF parsing only)
#define MANIFEST_MAX 300
struct ManifestItem {
    char id[128];
    char href[256];
};
static ManifestItem s_manifest[MANIFEST_MAX];
static int          s_manifest_count;

// Simple linear search in manifest
static const char* manifest_href_for_id(const char* id) {
    for (int i = 0; i < s_manifest_count; i++) {
        if (strcmp(s_manifest[i].id, id) == 0) return s_manifest[i].href;
    }
    return nullptr;
}

// Extract text content between two tags.
// e.g. for "<dc:title>Foo Bar</dc:title>", tag_open="<dc:title>", returns "Foo Bar"
static bool extract_between_tags(const char* haystack,
                                 const char* open_tag, const char* close_tag,
                                 char* out, size_t out_len) {
    const char* start = strstr(haystack, open_tag);
    if (!start) return false;
    start += strlen(open_tag);
    const char* end = strstr(start, close_tag);
    if (!end) return false;
    size_t len = (size_t)(end - start);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    trim_inplace(out);
    return true;
}

// Parse a single manifest <item> tag and populate a ManifestItem.
// tag_buf: content between < and > (without < >)
static bool parse_manifest_item(const char* tag_buf, ManifestItem* item) {
    return extract_attr(tag_buf, "id",   item->id,   sizeof(item->id)) &&
           extract_attr(tag_buf, "href", item->href, sizeof(item->href));
}

// Walk the OPF text and populate s_manifest[] and s_chapters[].
static void parse_opf(const char* opf_text) {
    s_manifest_count = 0;
    s_chapter_count  = 0;

    // ----- Pass 1: Metadata -----
    extract_between_tags(opf_text, "<dc:title>",   "</dc:title>",
                         s_title,  sizeof(s_title));
    // Try with namespace prefix variants
    if (s_title[0] == '\0') {
        extract_between_tags(opf_text, "<DC:title>",   "</DC:title>",
                             s_title, sizeof(s_title));
    }
    extract_between_tags(opf_text, "<dc:creator>", "</dc:creator>",
                         s_author, sizeof(s_author));
    if (s_author[0] == '\0') {
        extract_between_tags(opf_text, "<dc:creator ",  "</dc:creator>",
                             s_author, sizeof(s_author));
        // If it picked up attributes, strip them
        char* gt = strchr(s_author, '>');
        if (gt) memmove(s_author, gt + 1, strlen(gt));
        trim_inplace(s_author);
    }

    // ----- Pass 2: Manifest -----
    // Find <manifest> ... </manifest> block
    const char* manifest_start = strstr(opf_text, "<manifest");
    const char* manifest_end   = strstr(opf_text, "</manifest>");
    if (!manifest_start || !manifest_end) {
        Serial.println("[epub] OPF: no <manifest> found");
        return;
    }

    const char* p = manifest_start;
    while (p < manifest_end && s_manifest_count < MANIFEST_MAX) {
        const char* item_tag = strstr(p, "<item ");
        if (!item_tag || item_tag >= manifest_end) break;

        const char* tag_end = strchr(item_tag, '>');
        if (!tag_end) break;

        size_t tag_len = (size_t)(tag_end - item_tag - 1);
        if (tag_len < sizeof(s_scratch)) {
            memcpy(s_scratch, item_tag + 1, tag_len);
            s_scratch[tag_len] = '\0';
            ManifestItem* mi = &s_manifest[s_manifest_count];
            if (parse_manifest_item(s_scratch, mi)) {
                s_manifest_count++;
            }
        }
        p = tag_end + 1;
    }

    // ----- Pass 3: Spine -----
    const char* spine_start = strstr(opf_text, "<spine");
    const char* spine_end   = strstr(opf_text, "</spine>");
    if (!spine_start || !spine_end) {
        Serial.println("[epub] OPF: no <spine> found");
        return;
    }

    p = spine_start;
    while (p < spine_end && s_chapter_count < EPUB_MAX_CHAPTERS) {
        const char* ref_tag = strstr(p, "<itemref");
        if (!ref_tag || ref_tag >= spine_end) break;

        const char* tag_end = strchr(ref_tag, '>');
        if (!tag_end) break;

        size_t tag_len = (size_t)(tag_end - ref_tag - 1);
        if (tag_len < sizeof(s_scratch)) {
            memcpy(s_scratch, ref_tag + 1, tag_len);
            s_scratch[tag_len] = '\0';

            char idref[128] = "";
            if (extract_attr(s_scratch, "idref", idref, sizeof(idref))) {
                const char* href = manifest_href_for_id(idref);
                if (href) {
                    ChapterEntry* ch = &s_chapters[s_chapter_count];
                    safe_copy(ch->idref, idref, sizeof(ch->idref));

                    // Resolve href relative to OPF directory
                    char full_href[512];
                    resolve_href(s_opf_dir, href, full_href, sizeof(full_href));
                    safe_copy(ch->href, full_href, sizeof(ch->href));

                    snprintf(ch->out_path, sizeof(ch->out_path),
                             EPUB_TMP_PATH_FMT, s_chapter_count);
                    ch->extracted = false;
                    s_chapter_count++;
                }
            }
        }
        p = tag_end + 1;
    }
}

// =============================================================================
// XHTML → plaintext state machine
// =============================================================================

typedef enum {
    ST_NORMAL,
    ST_IN_TAG,
    ST_IN_SCRIPT,
    ST_IN_STYLE,
    ST_IN_ENTITY
} XhtmlState;

// Tag names that produce a newline on close / self-close
static bool tag_is_block_break(const char* tag_name) {
    // tag_name is lowercased
    return (strcmp(tag_name, "br")   == 0 ||
            strcmp(tag_name, "/p")   == 0 ||
            strcmp(tag_name, "/div") == 0 ||
            strcmp(tag_name, "/h1")  == 0 ||
            strcmp(tag_name, "/h2")  == 0 ||
            strcmp(tag_name, "/h3")  == 0 ||
            strcmp(tag_name, "/h4")  == 0 ||
            strcmp(tag_name, "/h5")  == 0 ||
            strcmp(tag_name, "/h6")  == 0 ||
            strcmp(tag_name, "/li")  == 0 ||
            strcmp(tag_name, "/tr")  == 0 ||
            strcmp(tag_name, "/blockquote") == 0);
}

// Write plaintext output for one XHTML buffer.
// in_buf  : NUL-terminated XHTML source
// in_size : byte count (not counting NUL)
// out_f   : open SD File to write into
// Returns approximate word count.
static int xhtml_to_text(const char* in_buf, size_t in_size, File& out_f) {
    XhtmlState state = ST_NORMAL;

    char tag_buf[256];   // accumulates current tag
    int  tag_len = 0;

    char entity_buf[16];
    int  entity_len = 0;

    // Whitespace collapsing: track what we last emitted
    // 0=nothing, 1=space, 2=newline
    int  last_emit = 0;

    int  word_count = 0;
    bool in_word = false;

    // Skip-until markers for script/style
    const char* skip_script = "</script";
    const char* skip_style  = "</style";

    const char* p   = in_buf;
    const char* end = in_buf + in_size;

    auto emit_char = [&](char c) {
        out_f.write((const uint8_t*)&c, 1);
        if (c > ' ') {
            if (!in_word) { word_count++; in_word = true; }
            last_emit = 0;
        } else {
            in_word = false;
        }
    };

    auto emit_space = [&]() {
        if (last_emit == 0) {
            out_f.write((const uint8_t*)" ", 1);
            last_emit = 1;
            in_word   = false;
        }
    };

    auto emit_newline = [&]() {
        if (last_emit != 2) {
            out_f.write((const uint8_t*)"\n", 1);
            last_emit = 2;
            in_word   = false;
        }
    };

    while (p < end) {
        char c = *p++;

        switch (state) {

        // ----------------------------------------------------------------
        case ST_NORMAL:
            if (c == '<') {
                state   = ST_IN_TAG;
                tag_len = 0;
            } else if (c == '&') {
                state      = ST_IN_ENTITY;
                entity_len = 0;
            } else if ((uint8_t)c <= ' ') {
                emit_space();
            } else {
                emit_char(c);
            }
            break;

        // ----------------------------------------------------------------
        case ST_IN_TAG: {
            if (c == '>') {
                tag_buf[tag_len] = '\0';

                // Extract tag name (first token, lowercase)
                char tag_name[64] = "";
                int n = 0;
                const char* tp = tag_buf;
                // Skip any leading whitespace
                while (*tp && (uint8_t)*tp <= ' ') tp++;
                // '/' may be first char for closing tags
                if (*tp == '/') { tag_name[n++] = '/'; tp++; }
                while (*tp && (uint8_t)*tp > ' ' && *tp != '/' && *tp != '>' && n < 62) {
                    tag_name[n++] = tolower((uint8_t)*tp);
                    tp++;
                }
                tag_name[n] = '\0';

                // Decide next state / what to emit
                if (strcmp(tag_name, "script") == 0) {
                    state = ST_IN_SCRIPT;
                } else if (strcmp(tag_name, "style") == 0) {
                    state = ST_IN_STYLE;
                } else if (tag_is_block_break(tag_name)) {
                    emit_newline();
                    state = ST_NORMAL;
                } else {
                    // Inline tag → emit space separator
                    emit_space();
                    state = ST_NORMAL;
                }
            } else {
                // Buffer the tag content (capped)
                if (tag_len < (int)sizeof(tag_buf) - 1) {
                    tag_buf[tag_len++] = c;
                }
            }
            break;
        }

        // ----------------------------------------------------------------
        case ST_IN_SCRIPT:
            // Skip until </script
            if (c == '<') {
                // peek ahead
                if ((size_t)(end - p) >= 7 &&
                    strncasecmp(p, "/script", 7) == 0) {
                    // skip to closing '>'
                    while (p < end && *p != '>') p++;
                    if (p < end) p++;  // skip '>'
                    state = ST_NORMAL;
                }
            }
            break;

        // ----------------------------------------------------------------
        case ST_IN_STYLE:
            if (c == '<') {
                if ((size_t)(end - p) >= 6 &&
                    strncasecmp(p, "/style", 6) == 0) {
                    while (p < end && *p != '>') p++;
                    if (p < end) p++;
                    state = ST_NORMAL;
                }
            }
            break;

        // ----------------------------------------------------------------
        case ST_IN_ENTITY: {
            if (c == ';') {
                entity_buf[entity_len] = '\0';
                // Decode named entities
                char decoded = 0;
                if      (strcmp(entity_buf, "amp")  == 0) decoded = '&';
                else if (strcmp(entity_buf, "lt")   == 0) decoded = '<';
                else if (strcmp(entity_buf, "gt")   == 0) decoded = '>';
                else if (strcmp(entity_buf, "nbsp") == 0) decoded = ' ';
                else if (strcmp(entity_buf, "quot") == 0) decoded = '"';
                else if (strcmp(entity_buf, "apos") == 0) decoded = '\'';
                else if (strcmp(entity_buf, "mdash") == 0) decoded = '-';
                else if (strcmp(entity_buf, "ndash") == 0) decoded = '-';
                else if (strcmp(entity_buf, "lsquo") == 0) decoded = '\'';
                else if (strcmp(entity_buf, "rsquo") == 0) decoded = '\'';
                else if (strcmp(entity_buf, "ldquo") == 0) decoded = '"';
                else if (strcmp(entity_buf, "rdquo") == 0) decoded = '"';
                else if (strcmp(entity_buf, "hellip") == 0) { /* ... */ decoded = '.'; }
                else if (entity_buf[0] == '#') {
                    // Numeric: &#NNN; or &#xHH;
                    long code = 0;
                    if (entity_buf[1] == 'x' || entity_buf[1] == 'X') {
                        code = strtol(entity_buf + 2, nullptr, 16);
                    } else {
                        code = strtol(entity_buf + 1, nullptr, 10);
                    }
                    // Only output printable ASCII range
                    if (code >= 32 && code < 127) decoded = (char)code;
                    else if (code == 0x2019 || code == 0x2018) decoded = '\'';
                    else if (code == 0x201C || code == 0x201D) decoded = '"';
                    else if (code == 0x2013 || code == 0x2014) decoded = '-';
                    else if (code == 0x2026) decoded = '.';
                    else if (code == 0xA0)  decoded = ' ';
                    else decoded = 0;  // skip
                }

                if (decoded == ' ') emit_space();
                else if (decoded != 0) emit_char(decoded);
                // Unknown entity: skip silently

                state = ST_NORMAL;
            } else if (entity_len < (int)sizeof(entity_buf) - 1) {
                entity_buf[entity_len++] = c;
            } else {
                // Buffer overflow — abandon entity, emit '&' and resume
                state = ST_NORMAL;
            }
            break;
        }

        } // switch
    } // while

    // Ensure file ends with a newline
    if (last_emit != 2) {
        out_f.write((const uint8_t*)"\n", 1);
    }

    return word_count;
}

// =============================================================================
// Public API implementation
// =============================================================================

bool epub_open(const char* epub_path) {
    epub_close();  // reset any previous state

    // Ensure tmp directory exists
    if (!SD_MMC.exists(EPUB_TMP_DIR)) {
        SD_MMC.mkdir(EPUB_TMP_DIR);
    }

    memset(&s_zip,     0, sizeof(s_zip));
    memset(s_title,    0, sizeof(s_title));
    memset(s_author,   0, sizeof(s_author));
    memset(s_opf_dir,  0, sizeof(s_opf_dir));
    memset(s_chapters, 0, sizeof(s_chapters));
    s_chapter_count  = 0;
    s_manifest_count = 0;

    // Open the ZIP (EPUB).
    // miniz uses fopen() internally, which needs the VFS mount prefix.
    // SD_MMC mounts at "/sdcard", so prefix the path if not already absolute.
    char vfs_path[300];
    if (strncmp(epub_path, "/sdcard", 7) == 0) {
        strncpy(vfs_path, epub_path, sizeof(vfs_path) - 1);
    } else {
        snprintf(vfs_path, sizeof(vfs_path), "/sdcard%s", epub_path);
    }
    vfs_path[sizeof(vfs_path) - 1] = '\0';

    if (!mz_zip_reader_init_file(&s_zip, vfs_path, 0)) {
        Serial.print("[epub] Cannot open ZIP: ");
        Serial.println(vfs_path);
        return false;
    }
    s_zip_open = true;

    // Step 1: parse container.xml → find OPF path
    char opf_path[256] = "";
    if (!parse_container_xml(opf_path, sizeof(opf_path))) {
        epub_close();
        return false;
    }
    Serial.print("[epub] OPF path: ");
    Serial.println(opf_path);

    // Derive OPF directory for resolving relative hrefs
    path_dir(opf_path, s_opf_dir, sizeof(s_opf_dir));

    // Step 2: read OPF file
    if (zip_extract_to_buf(opf_path) == 0) {
        Serial.print("[epub] Cannot read OPF: ");
        Serial.println(opf_path);
        epub_close();
        return false;
    }

    // Step 3: parse OPF
    parse_opf(s_xhtml_buf);

    Serial.print("[epub] Title: ");    Serial.println(s_title);
    Serial.print("[epub] Author: ");   Serial.println(s_author);
    Serial.print("[epub] Chapters: "); Serial.println(s_chapter_count);

    return s_chapter_count > 0;
}

const char* epub_title() {
    return s_title;
}

const char* epub_author() {
    return s_author;
}

int epub_chapter_count() {
    return s_chapter_count;
}

const char* epub_chapter_name(int idx) {
    if (idx < 0 || idx >= s_chapter_count) return "";
    return s_chapters[idx].idref;
}

bool epub_extract_chapter(int chapter_idx) {
    if (!s_zip_open) return false;
    if (chapter_idx < 0 || chapter_idx >= s_chapter_count) return false;

    ChapterEntry* ch = &s_chapters[chapter_idx];

    // Extract XHTML from ZIP into s_xhtml_buf
    size_t raw_size = zip_extract_to_buf(ch->href);
    if (raw_size == 0) {
        Serial.print("[epub] Cannot extract chapter XHTML: ");
        Serial.println(ch->href);
        return false;
    }

    // Open output file on SD
    File out_f = SD_MMC.open(ch->out_path, FILE_WRITE);
    if (!out_f) {
        Serial.print("[epub] Cannot open output file: ");
        Serial.println(ch->out_path);
        return false;
    }

    int words = xhtml_to_text(s_xhtml_buf, raw_size, out_f);
    out_f.close();

    ch->extracted = true;

    Serial.print("[epub] Extracted chapter ");
    Serial.print(chapter_idx);
    Serial.print(" (");
    Serial.print(words);
    Serial.print(" words) → ");
    Serial.println(ch->out_path);

    return true;
}

int epub_extract_all() {
    int count = 0;
    for (int i = 0; i < s_chapter_count; i++) {
        if (epub_extract_chapter(i)) count++;
    }
    return count;
}

const char* epub_chapter_path(int chapter_idx) {
    if (chapter_idx < 0 || chapter_idx >= s_chapter_count) return nullptr;
    if (!s_chapters[chapter_idx].extracted) return nullptr;
    return s_chapters[chapter_idx].out_path;
}

int epub_chapter_word_count(int chapter_idx) {
    if (chapter_idx < 0 || chapter_idx >= s_chapter_count) return -1;
    if (!s_chapters[chapter_idx].extracted) return -1;

    File f = SD_MMC.open(s_chapters[chapter_idx].out_path, FILE_READ);
    if (!f) return -1;

    int  count   = 0;
    bool in_word = false;

    while (f.available()) {
        int b = f.read();
        if (b < 0) break;
        char c = (char)b;
        if ((uint8_t)c > ' ') {
            if (!in_word) { count++; in_word = true; }
        } else {
            in_word = false;
        }
    }
    f.close();
    return count;
}

void epub_close() {
    if (s_zip_open) {
        mz_zip_reader_end(&s_zip);
        s_zip_open = false;
    }
    memset(s_title,    0, sizeof(s_title));
    memset(s_author,   0, sizeof(s_author));
    memset(s_opf_dir,  0, sizeof(s_opf_dir));
    memset(s_chapters, 0, sizeof(s_chapters));
    s_chapter_count  = 0;
    s_manifest_count = 0;
}
