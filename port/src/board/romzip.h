// Extracting the ROM files from MAME zip files (e.g. 720.zip) placed in the ROM folder.
// Only the 720 Degrees files are extracted: names starting with "136047-" and
// "720-eeprom.bin"; files already present are left alone.  Self-contained: a small zip
// directory reader and a deflate decoder (RFC 1951), no external library.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// extracts missing ROM files from every *.zip in 'dir'; returns the number written.
// Problems with individual archives are appended to 'log'.
int extract_rom_zips(const std::string &dir, std::string &log);

// true if 'dir' holds the ROM files or a zip that contains them
bool rom_folder_usable(const std::string &dir);

// decompress a raw deflate stream; false on corrupt input
bool inflate_raw(const uint8_t *in, size_t n, std::vector<uint8_t> &out, size_t expected_size);
