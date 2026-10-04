// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <istream>
#include <stdexcept>

namespace Slic3r { namespace sla {
// UVtools' CXDLP checksum: v2 is byte XOR; v3/v4 use the reflected
// polynomial with initial CRC 0 and no final XOR (unlike PNG/zlib CRC).
inline uint32_t creality_checksum_update(uint32_t checksum, uint8_t byte, unsigned version)
{
    if(version==2) return checksum^byte;
    static constexpr auto table=[] {
        std::array<uint32_t,256> values{};
        for(uint32_t i=0;i<256;++i) {
            uint32_t value=i;
            for(unsigned bit=0;bit<8;++bit)value=(value&1)?(value>>1)^0xedb88320u:value>>1;
            values[i]=value;
        }
        return values;
    }();
    return table[uint8_t(checksum)^byte]^(checksum>>8);
}
inline uint32_t creality_checksum(const uint8_t *bytes, size_t size, unsigned version)
{
    uint32_t checksum=0;
    for(size_t i=0;i<size;++i)checksum=creality_checksum_update(checksum,bytes[i],version);
    return checksum;
}
inline uint32_t creality_checksum(std::istream &input, unsigned version)
{
    std::array<char,65536> chunk{};
    uint32_t checksum=0;
    while(input) {
        input.read(chunk.data(),chunk.size());
        for(std::streamsize i=0;i<input.gcount();++i)
            checksum=creality_checksum_update(checksum,uint8_t(chunk[size_t(i)]),version);
    }
    if(!input.eof())throw std::runtime_error("Cannot read Creality checksum input");
    return checksum;
}
}} // namespace Slic3r::sla
