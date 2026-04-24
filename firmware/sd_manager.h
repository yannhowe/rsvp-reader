#ifndef SD_MANAGER_H
#define SD_MANAGER_H

#include <Arduino.h>
#include <vector>

// Initialize SD card via SDMMC 4-wire.
// Falls back to 1-wire mode if 4-wire fails.
// Returns true if mounted successfully.
bool sd_init();

// Returns true if the SD card is currently mounted.
bool sd_is_mounted();

// List all .epub files under `dir` (recursive, max depth 3).
// Returns full paths (e.g. "/books/mybook.epub"), sorted alphabetically.
// Maximum 100 results.
std::vector<String> sd_list_epubs(const char* dir = "/");

// Returns true if the file at `path` exists on the SD card.
bool sd_file_exists(const char* path);

// Returns the size of the file at `path` in bytes, or 0 on error.
size_t sd_file_size(const char* path);

// Formats a byte count for human-readable display (e.g. "1.2 MB", "340 KB").
String sd_format_size(size_t bytes);

// Ensures the directory at `path` exists, creating it (and parents) if needed.
// Returns true on success.
bool sd_mkdir(const char* path);

// Returns total card capacity in bytes.
uint64_t sd_total_bytes();

// Returns free space on the card in bytes.
uint64_t sd_free_bytes();

// Delete a file from the SD card. Returns true on success.
bool sd_remove_file(const char* path);

#endif // SD_MANAGER_H
