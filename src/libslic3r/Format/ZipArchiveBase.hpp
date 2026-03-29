///|/ Copyright (c) Prusa Research 2023 Tomáš Mészáros @tamasmeszaros, Pavel Mikuš @Godrak
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <ctime>

namespace Slic3r::sla {

// ZIP file structure components
struct ZipLocalFileHeader {
    static constexpr uint32_t SIGNATURE = 0x04034b50; // "PK\x03\x04"
    uint32_t signature = SIGNATURE;
    uint16_t version_needed = 0x0014;      // Version 2.0
    uint16_t flags = 0;                    // No flags
    uint16_t compression = 0;              // 0 = stored (no compression)
    uint16_t mod_time = 0;
    uint16_t mod_date = 0;
    uint32_t crc32 = 0;
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
    uint16_t filename_length = 0;
    uint16_t extra_length = 0;
    
    void write(std::vector<uint8_t> &data) const;
    static ZipLocalFileHeader read(const std::vector<uint8_t> &data, size_t &offset);
};

struct ZipCentralDirectoryHeader {
    static constexpr uint32_t SIGNATURE = 0x02014b50; // "PK\x01\x02"
    uint32_t signature = SIGNATURE;
    uint16_t version_made = 0x0014;        // Version 2.0
    uint16_t version_needed = 0x0014;
    uint16_t flags = 0;
    uint16_t compression = 0;
    uint16_t mod_time = 0;
    uint16_t mod_date = 0;
    uint32_t crc32 = 0;
    uint32_t compressed_size = 0;
    uint32_t uncompressed_size = 0;
    uint16_t filename_length = 0;
    uint16_t extra_length = 0;
    uint16_t comment_length = 0;
    uint16_t disk_number = 0;
    uint16_t internal_attr = 0;
    uint32_t external_attr = 0;
    uint32_t local_header_offset = 0;
    
    void write(std::vector<uint8_t> &data) const;
};

struct ZipEndOfCentralDirectory {
    static constexpr uint32_t SIGNATURE = 0x06054b50; // "PK\x05\x06"
    uint32_t signature = SIGNATURE;
    uint16_t disk_number = 0;
    uint16_t disk_with_central = 0;
    uint16_t num_entries_disk = 0;
    uint16_t num_entries_total = 0;
    uint32_t central_dir_size = 0;
    uint32_t central_dir_offset = 0;
    uint16_t comment_length = 0;
    
    void write(std::vector<uint8_t> &data) const;
};

// ZIP file entry
struct ZipEntry {
    std::string filename;
    std::vector<uint8_t> data;
    uint32_t crc32 = 0;
    uint16_t mod_time = 0;
    uint16_t mod_date = 0;
    
    void calculate_crc32();
    
    // Static CRC32 calculator
    static uint32_t compute_crc32(const std::vector<uint8_t> &data);
};

// Base ZIP archive creator
class ZipArchiveBase {
protected:
    std::vector<ZipEntry> m_entries;
    
    // ZIP creation
    void add_entry(const std::string &filename, const std::vector<uint8_t> &data);
    void write_zip_file(const std::string &filepath);
    
public:
    virtual ~ZipArchiveBase() = default;
    
    // Utility functions
    static uint32_t crc32(const std::vector<uint8_t> &data);
    static uint16_t dos_time(std::time_t t);
    static uint16_t dos_date(std::time_t t);
    
    // Add a text file  
    void add_text_file(const std::string &filename, const std::string &content);
    
    // Add binary file
    void add_binary_file(const std::string &filename, const std::vector<uint8_t> &data);
    
    // Clear all entries
    void clear_entries() { m_entries.clear(); }
    
    // Get file data
    const std::vector<ZipEntry> &get_entries() const { return m_entries; }
    
    // Create ZIP file
    void create_zip_file(const std::string &filepath);
};

} // namespace Slic3r::sla
