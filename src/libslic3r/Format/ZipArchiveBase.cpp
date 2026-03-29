///|/ Copyright (c) Prusa Research 2023 Tomáš Mészáros @tamasmeszaros, Pavel Mikuš @Godrak
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "ZipArchiveBase.hpp"
#include <fstream>
#include <cstring>
#include <boost/crc.hpp>
#include <cassert>

namespace Slic3r::sla {

// CRC32 table for zip files
static uint32_t crc32_table[256];
static bool crc32_table_initialized = false;

static void init_crc32_table() {
    if (crc32_table_initialized) return;
    
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_initialized = true;
}

// ZIP CRC32 implementation
uint32_t ZipArchiveBase::crc32(const std::vector<uint8_t> &data) {
    init_crc32_table();
    
    uint32_t c = 0xFFFFFFFF;
    for (uint8_t byte : data) {
        c = crc32_table[(c ^ byte) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFF;
}

// Convert time_t to DOS time format
uint16_t ZipArchiveBase::dos_time(std::time_t t) {
    struct tm *timeinfo = std::localtime(&t);
    uint16_t dos_time = (timeinfo->tm_hour << 11) |
                        (timeinfo->tm_min << 5) |
                        (timeinfo->tm_sec >> 1);
    return dos_time;
}

// Convert time_t to DOS date format
uint16_t ZipArchiveBase::dos_date(std::time_t t) {
    struct tm *timeinfo = std::localtime(&t);
    uint16_t dos_date = ((timeinfo->tm_year + 1900 - 1980) << 9) |
                        ((timeinfo->tm_mon + 1) << 5) |
                        (timeinfo->tm_mday);
    return dos_date;
}

// Write local file header
void ZipLocalFileHeader::write(std::vector<uint8_t> &data) const {
    auto write_u32 = [&data](uint32_t v) {
        data.push_back((v) & 0xFF);
        data.push_back((v >> 8) & 0xFF);
        data.push_back((v >> 16) & 0xFF);
        data.push_back((v >> 24) & 0xFF);
    };
    auto write_u16 = [&data](uint16_t v) {
        data.push_back((v) & 0xFF);
        data.push_back((v >> 8) & 0xFF);
    };
    
    write_u32(signature);
    write_u16(version_needed);
    write_u16(flags);
    write_u16(compression);
    write_u16(mod_time);
    write_u16(mod_date);
    write_u32(crc32);
    write_u32(compressed_size);
    write_u32(uncompressed_size);
    write_u16(filename_length);
    write_u16(extra_length);
}

// Write central directory header
void ZipCentralDirectoryHeader::write(std::vector<uint8_t> &data) const {
    auto write_u32 = [&data](uint32_t v) {
        data.push_back((v) & 0xFF);
        data.push_back((v >> 8) & 0xFF);
        data.push_back((v >> 16) & 0xFF);
        data.push_back((v >> 24) & 0xFF);
    };
    auto write_u16 = [&data](uint16_t v) {
        data.push_back((v) & 0xFF);
        data.push_back((v >> 8) & 0xFF);
    };
    
    write_u32(signature);
    write_u16(version_made);
    write_u16(version_needed);
    write_u16(flags);
    write_u16(compression);
    write_u16(mod_time);
    write_u16(mod_date);
    write_u32(crc32);
    write_u32(compressed_size);
    write_u32(uncompressed_size);
    write_u16(filename_length);
    write_u16(extra_length);
    write_u16(comment_length);
    write_u16(disk_number);
    write_u16(internal_attr);
    write_u32(external_attr);
    write_u32(local_header_offset);
}

// Write end of central directory
void ZipEndOfCentralDirectory::write(std::vector<uint8_t> &data) const {
    auto write_u32 = [&data](uint32_t v) {
        data.push_back((v) & 0xFF);
        data.push_back((v >> 8) & 0xFF);
        data.push_back((v >> 16) & 0xFF);
        data.push_back((v >> 24) & 0xFF);
    };
    auto write_u16 = [&data](uint16_t v) {
        data.push_back((v) & 0xFF);
        data.push_back((v >> 8) & 0xFF);
    };
    
    write_u32(signature);
    write_u16(disk_number);
    write_u16(disk_with_central);
    write_u16(num_entries_disk);
    write_u16(num_entries_total);
    write_u32(central_dir_size);
    write_u32(central_dir_offset);
    write_u16(comment_length);
}

// Calculate CRC32 for entry
void ZipEntry::calculate_crc32() {
    crc32 = compute_crc32(data);
}

uint32_t ZipEntry::compute_crc32(const std::vector<uint8_t> &data) {
    return ZipArchiveBase::crc32(data);
}

// Add entry to ZIP
void ZipArchiveBase::add_entry(const std::string &filename, const std::vector<uint8_t> &data) {
    ZipEntry entry;
    entry.filename = filename;
    entry.data = data;
    entry.calculate_crc32();
    
    // Set DOS time/date
    std::time_t now = std::time(nullptr);
    entry.mod_time = dos_time(now);
    entry.mod_date = dos_date(now);
    
    m_entries.push_back(entry);
}

// Add text file
void ZipArchiveBase::add_text_file(const std::string &filename, const std::string &content) {
    std::vector<uint8_t> data(content.begin(), content.end());
    add_entry(filename, data);
}

// Add binary file
void ZipArchiveBase::add_binary_file(const std::string &filename, const std::vector<uint8_t> &data) {
    add_entry(filename, data);
}

// Create ZIP file
void ZipArchiveBase::create_zip_file(const std::string &filepath) {
    std::vector<uint8_t> zip_data;
    std::vector<ZipCentralDirectoryHeader> central_headers;
    
    // Write all local file headers and file data
    for (const auto &entry : m_entries) {
        size_t local_header_offset = zip_data.size();
        
        // Build local file header
        ZipLocalFileHeader local_header;
        local_header.mod_time = entry.mod_time;
        local_header.mod_date = entry.mod_date;
        local_header.crc32 = entry.crc32;
        local_header.uncompressed_size = entry.data.size();
        local_header.compressed_size = entry.data.size();
        local_header.filename_length = entry.filename.length();
        
        // Write local header
        local_header.write(zip_data);
        
        // Write filename
        zip_data.insert(zip_data.end(), entry.filename.begin(), entry.filename.end());
        
        // Write file data
        zip_data.insert(zip_data.end(), entry.data.begin(), entry.data.end());
        
        // Build central directory header
        ZipCentralDirectoryHeader central_header;
        central_header.mod_time = entry.mod_time;
        central_header.mod_date = entry.mod_date;
        central_header.crc32 = entry.crc32;
        central_header.uncompressed_size = entry.data.size();
        central_header.compressed_size = entry.data.size();
        central_header.filename_length = entry.filename.length();
        central_header.local_header_offset = local_header_offset;
        
        central_headers.push_back(central_header);
    }
    
    // Calculate central directory offset
    size_t central_dir_offset = zip_data.size();
    
    // Write central directory
    std::vector<uint8_t> central_dir_data;
    for (size_t i = 0; i < m_entries.size(); i++) {
        central_headers[i].write(central_dir_data);
        central_dir_data.insert(central_dir_data.end(), 
                               m_entries[i].filename.begin(), 
                               m_entries[i].filename.end());
    }
    
    // Write end of central directory
    ZipEndOfCentralDirectory eocd;
    eocd.num_entries_disk = m_entries.size();
    eocd.num_entries_total = m_entries.size();
    eocd.central_dir_size = central_dir_data.size();
    eocd.central_dir_offset = central_dir_offset;
    
    std::vector<uint8_t> eocd_data;
    eocd.write(eocd_data);
    
    // Combine all parts
    zip_data.insert(zip_data.end(), central_dir_data.begin(), central_dir_data.end());
    zip_data.insert(zip_data.end(), eocd_data.begin(), eocd_data.end());
    
    // Write to file
    std::ofstream file(filepath, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open file: " + filepath);
    }
    file.write(reinterpret_cast<const char*>(zip_data.data()), zip_data.size());
    file.close();
}

} // namespace Slic3r::sla
