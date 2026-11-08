// SPDX-License-Identifier: AGPL-3.0-or-later
// Fixed public device protocol from local UVtools CTBEncryptedFile.cs and
// CryptExtensions.cs. These bytes are not user credentials. No external
// crypto process, network, OpenSSL or managed runtime is required.
#ifndef SLIC3R_CTB_ENCRYPTED_CRYPTO_HPP
#define SLIC3R_CTB_ENCRYPTED_CRYPTO_HPP
#include "libslic3r/Exception.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>
#include <functional>

namespace Slic3r::ctb_encrypted {
inline constexpr std::array<uint8_t,32> protocol_key{
    0xd0,0x5b,0x8e,0x33,0x71,0xde,0x3d,0x1a,0xe5,0x4f,0x22,0xdd,0xdf,0x5b,0xfd,0x94,
    0xab,0x5d,0x64,0x3a,0x9d,0x7e,0xbf,0xaf,0x42,0x03,0xf3,0x10,0xd8,0x52,0x2a,0xea};
inline constexpr std::array<uint8_t,16> protocol_iv{
    0x0f,0x01,0x0a,0x05,0x05,0x0b,0x06,0x07,0x08,0x06,0x0a,0x0c,0x0c,0x0d,0x09,0x0f};

class AES256 {
    std::array<uint8_t,240> m_round_keys{};
    std::array<uint8_t,256> m_sub{},m_inverse{};
    using Block=std::array<uint8_t,16>;
    static uint8_t multiply(uint8_t a,uint8_t b) {
        uint8_t value=0;
        while(b) {if(b&1)value^=a;a=uint8_t((a<<1)^((a&0x80)?0x1b:0));b>>=1;}
        return value;
    }
    static uint8_t rotate_byte(uint8_t value,unsigned count) {
        return uint8_t((unsigned(value)<<count)|(unsigned(value)>>(8-count)));
    }
    void add_key(Block &block,unsigned round) const {
        for(unsigned i=0;i<16;++i)block[i]^=m_round_keys[16*round+i];
    }
    void substitute(Block &block,bool inverse) const {
        const auto &table=inverse?m_inverse:m_sub;
        for(auto &byte:block)byte=table[byte];
    }
    static void shift_rows(Block &block,bool inverse) {
        const Block old=block;
        for(unsigned column=0;column<4;++column)for(unsigned row=0;row<4;++row)
            block[4*column+row]=old[4*((column+(inverse?4-row:row))%4)+row];
    }
    static void mix_columns(Block &block,bool inverse) {
        constexpr uint8_t forward[4]{2,3,1,1},backward[4]{14,11,13,9};
        const auto *matrix=inverse?backward:forward;
        for(unsigned column=0;column<4;++column) {
            const Block old=block;
            for(unsigned row=0;row<4;++row) {
                uint8_t value=0;
                for(unsigned n=0;n<4;++n)value^=multiply(old[4*column+n],matrix[(n+4-row)%4]);
                block[4*column+row]=value;
            }
        }
    }
public:
    AES256() {
        // Generate both substitution tables from the AES affine transform.
        for(unsigned value=0;value<256;++value) {
            uint8_t inverse=0;
            if(value) {
                inverse=1;uint8_t power=uint8_t(value);unsigned exponent=254;
                while(exponent){if(exponent&1)inverse=multiply(inverse,power);power=multiply(power,power);exponent>>=1;}
            }
            const auto substituted=uint8_t(inverse^rotate_byte(inverse,1)^rotate_byte(inverse,2)^rotate_byte(inverse,3)^rotate_byte(inverse,4)^0x63);
            m_sub[value]=substituted;m_inverse[substituted]=uint8_t(value);
        }
        std::copy(protocol_key.begin(),protocol_key.end(),m_round_keys.begin());
        uint8_t round_constant=1;
        for(unsigned offset=32;offset<m_round_keys.size();offset+=4) {
            std::array<uint8_t,4> word{};std::copy_n(m_round_keys.begin()+offset-4,4,word.begin());
            if(offset%32==0) {
                std::rotate(word.begin(),word.begin()+1,word.end());
                for(auto &byte:word)byte=m_sub[byte];
                word[0]^=round_constant;round_constant=multiply(round_constant,2);
            } else if(offset%32==16)for(auto &byte:word)byte=m_sub[byte];
            for(unsigned n=0;n<4;++n)m_round_keys[offset+n]=m_round_keys[offset+n-32]^word[n];
        }
    }
    Block encrypt(Block block) const {
        add_key(block,0);
        for(unsigned round=1;round<14;++round) {
            substitute(block,false);shift_rows(block,false);mix_columns(block,false);add_key(block,round);
        }
        substitute(block,false);shift_rows(block,false);add_key(block,14);return block;
    }
    Block decrypt(Block block) const {
        add_key(block,14);
        for(unsigned round=13;round>0;--round) {
            shift_rows(block,true);substitute(block,true);add_key(block,round);mix_columns(block,true);
        }
        shift_rows(block,true);substitute(block,true);add_key(block,0);return block;
    }
};

inline std::vector<uint8_t> aes_cbc(std::vector<uint8_t> bytes,bool encrypt, const std::function<void()> &check={}) {
    if(bytes.size()%16)throw ExportError("CTB encrypted AES blocks require 16-byte alignment");
    static const AES256 cipher;
    auto previous=protocol_iv;
    for(size_t offset=0;offset<bytes.size();offset+=16) {
        if (check && offset%65536==0) check();
        std::array<uint8_t,16> block;std::copy_n(bytes.begin()+offset,16,block.begin());
        if(encrypt) {
            for(unsigned n=0;n<16;++n)block[n]^=previous[n];
            block=cipher.encrypt(block);previous=block;
        } else {
            const auto ciphertext=block;block=cipher.decrypt(block);
            for(unsigned n=0;n<16;++n)block[n]^=previous[n];
            previous=ciphertext;
        }
        std::copy(block.begin(),block.end(),bytes.begin()+offset);
    }
    return bytes;
}

// The signature hashes exactly eight LITTLE-ENDIAN checksum bytes. A single
// SHA-256 block is sufficient; this helper deliberately has no general hash API.
inline std::array<uint8_t,32> checksum_digest(uint64_t checksum) {
    constexpr uint32_t constants[64]{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::array<uint32_t,64> words{};
    for(unsigned byte=0;byte<8;++byte)words[byte/4]|=uint32_t(uint8_t(checksum>>(8*byte)))<<(24-8*(byte%4));
    words[2]=0x80000000;words[15]=64;
    auto rotate=[](uint32_t value,unsigned count){return(value>>count)|(value<<(32-count));};
    for(unsigned n=16;n<64;++n) {
        const uint32_t a=words[n-15],b=words[n-2];
        words[n]=words[n-16]+(rotate(a,7)^rotate(a,18)^(a>>3))+words[n-7]+(rotate(b,17)^rotate(b,19)^(b>>10));
    }
    std::array<uint32_t,8> hash{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    auto state=hash;
    for(unsigned round=0;round<64;++round) {
        const auto a=state[0],b=state[1],c=state[2],e=state[4],f=state[5],g=state[6];
        const uint32_t first=state[7]+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+constants[round]+words[round];
        const uint32_t second=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
        for(unsigned n=7;n>0;--n)state[n]=state[n-1];
        state[4]+=first;state[0]=first+second;
    }
    std::array<uint8_t,32> bytes{};
    for(unsigned word=0;word<8;++word) {
        hash[word]+=state[word];for(unsigned n=0;n<4;++n)bytes[4*word+n]=uint8_t(hash[word]>>(24-8*n));
    }
    return bytes;
}
inline std::vector<uint8_t> signature(uint64_t checksum) {
    const auto hash=checksum_digest(checksum);return aes_cbc({hash.begin(),hash.end()},true);
}
inline void xor_layer(std::vector<uint8_t> &bytes,uint32_t index,uint32_t seed=0xEFBEADDE, const std::function<void()> &check={}) {
    if(!seed)return;
    const uint32_t increment=seed*0x2d83cdacU+0xd8a83423U;
    uint32_t key=(index*0x1e1530cdU+0xec3d47cdU)*increment;
    for(size_t n=0;n<bytes.size();++n) {
        if(check && n%65536==0)check();
        bytes[n]^=uint8_t(key>>(8*(n%4)));if(n%4==3)key+=increment;
    }
}
}
#endif
