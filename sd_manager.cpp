#include "sd_manager.h"
#include "user_config.h"
#include <SD_MMC.h>
#include <FS.h>
#include <algorithm>

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static bool s_mounted = false;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Case-insensitive check for ".epub" suffix.
static bool is_epub(const String& name) {
    if (name.length() < 5) return false;
    String suffix = name.substring(name.length() - 5);
    suffix.toLowerCase();
    return suffix == ".epub";
}

// Recursive directory scanner; results appended to `out`.
static void scan_dir(const char* path, int depth, std::vector<String>& out) {
    if (depth > 3) return;
    if (out.size() >= 100) return;

    File dir = SD_MMC.open(path);
    if (!dir || !dir.isDirectory()) return;

    File entry = dir.openNextFile();
    while (entry && out.size() < 100) {
        if (entry.isDirectory()) {
            scan_dir(entry.path(), depth + 1, out);
        } else {
            String name = String(entry.name());
            if (is_epub(name)) {
                out.push_back(String(entry.path()));
            }
        }
        entry = dir.openNextFile();
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool sd_init() {
    s_mounted = false;

    // Configure SDMMC pins for the Waveshare ESP32-S3-LCD-3.16 board.
    // The default ESP32-S3 SDMMC pins differ from this board's wiring.
    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);

    // Attempt 4-wire SDMMC.
    if (SD_MMC.begin()) {
        uint8_t cardType = SD_MMC.cardType();
        if (cardType != CARD_NONE) {
            s_mounted = true;
            Serial.print("[SD] Card type: ");
            switch (cardType) {
                case CARD_MMC:  Serial.println("MMC");   break;
                case CARD_SD:   Serial.println("SD");    break;
                case CARD_SDHC: Serial.println("SDHC");  break;
                default:        Serial.println("Unknown"); break;
            }
            Serial.printf("[SD] Size: %llu MB\n",
                          SD_MMC.cardSize() / (1024ULL * 1024ULL));
            return true;
        }
        // Card detected but type is NONE — end and retry as 1-wire.
        SD_MMC.end();
    }

    // Fallback: 1-wire (DAT0-only) mode.
    Serial.println("[SD] 4-wire init failed, trying 1-wire mode...");
    if (SD_MMC.begin("/sdcard", true)) {
        uint8_t cardType = SD_MMC.cardType();
        if (cardType != CARD_NONE) {
            s_mounted = true;
            Serial.print("[SD] (1-wire) Card type: ");
            switch (cardType) {
                case CARD_MMC:  Serial.println("MMC");   break;
                case CARD_SD:   Serial.println("SD");    break;
                case CARD_SDHC: Serial.println("SDHC");  break;
                default:        Serial.println("Unknown"); break;
            }
            Serial.printf("[SD] Size: %llu MB\n",
                          SD_MMC.cardSize() / (1024ULL * 1024ULL));
            return true;
        }
        SD_MMC.end();
    }

    Serial.println("[SD] Init failed — no card or unsupported type.");
    return false;
}

bool sd_is_mounted() {
    return s_mounted;
}

std::vector<String> sd_list_epubs(const char* dir) {
    std::vector<String> results;
    if (!s_mounted) return results;

    scan_dir(dir, 1, results);
    std::sort(results.begin(), results.end());
    return results;
}

bool sd_file_exists(const char* path) {
    if (!s_mounted) return false;
    return SD_MMC.exists(path);
}

size_t sd_file_size(const char* path) {
    if (!s_mounted) return 0;
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return 0;
    size_t sz = f.size();
    f.close();
    return sz;
}

String sd_format_size(size_t bytes) {
    if (bytes >= 1024UL * 1024UL * 1024UL) {
        float gb = bytes / (1024.0f * 1024.0f * 1024.0f);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f GB", gb);
        return String(buf);
    } else if (bytes >= 1024UL * 1024UL) {
        float mb = bytes / (1024.0f * 1024.0f);
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f MB", mb);
        return String(buf);
    } else if (bytes >= 1024UL) {
        float kb = bytes / 1024.0f;
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f KB", kb);
        return String(buf);
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u B", (unsigned)bytes);
        return String(buf);
    }
}

bool sd_mkdir(const char* path) {
    if (!s_mounted) return false;
    if (SD_MMC.exists(path)) return true;

    // Walk the path and create each component that is missing.
    String p = String(path);
    // Normalize: strip trailing slash.
    while (p.length() > 1 && p.endsWith("/")) {
        p.remove(p.length() - 1);
    }

    // Collect segments to create from deepest missing ancestor downward.
    std::vector<String> to_create;
    String cur = p;
    while (cur.length() > 1) {
        if (SD_MMC.exists(cur.c_str())) break;
        to_create.push_back(cur);
        int last = cur.lastIndexOf('/');
        if (last <= 0) break;
        cur = cur.substring(0, last);
    }

    // Create from shallowest to deepest.
    for (int i = (int)to_create.size() - 1; i >= 0; i--) {
        if (!SD_MMC.mkdir(to_create[i].c_str())) {
            Serial.printf("[SD] mkdir failed: %s\n", to_create[i].c_str());
            return false;
        }
    }
    return true;
}

uint64_t sd_total_bytes() {
    if (!s_mounted) return 0;
    return SD_MMC.totalBytes();
}

uint64_t sd_free_bytes() {
    if (!s_mounted) return 0;
    return SD_MMC.totalBytes() - SD_MMC.usedBytes();
}

bool sd_remove_file(const char* path) {
    if (!s_mounted) return false;
    return SD_MMC.remove(path);
}
