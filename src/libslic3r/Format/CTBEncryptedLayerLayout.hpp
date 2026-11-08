// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "CTBEncryptedCrypto.hpp"
#include "ChituboxRasterEncoder.hpp"
#include "libslic3r/SLA/LayerProgress.hpp"
#include <cmath>
#include <cstring>

namespace Slic3r::sla {
// CTBEncryptedFile is a distinct addressed schema, not ordinary ChituboxFile.
// Settings use AES-CBC; mask data uses XOR followed by optional partial AES.
struct CTBEncryptedLayerLayout {
    struct Range { size_t begin,end; };
    const std::vector<uint8_t> &bytes;
    std::vector<uint8_t> settings;
    uint32_t version,columns,rows,count,seed,table,settings_offset,signature_offset;
    std::vector<Range> metadata;
    static uint32_t integer(const std::vector<uint8_t> &data,size_t at) {
        if (at>data.size() || data.size()-at<4) throw RuntimeError("Truncated CTBEncrypted field");
        return uint32_t(data[at])|uint32_t(data[at+1])<<8|uint32_t(data[at+2])<<16|uint32_t(data[at+3])<<24;
    }
    uint32_t integer(size_t at) const { return integer(bytes,at); }
    uint32_t setting(size_t at) const { return integer(settings,at); }
    static float number(const std::vector<uint8_t> &data,size_t at) {
        const auto bits=integer(data,at);float value;std::memcpy(&value,&bits,4);return value;
    }
    float number(size_t at) const { return number(bytes,at); }
    float parameter(size_t at) const { return number(settings,at); }
    void require(size_t at,uint64_t length) const {
        if (at>bytes.size() || length>bytes.size()-at) throw RuntimeError("CTBEncrypted section outside file");
    }
    void section(size_t at,uint64_t length) { require(at,length);metadata.push_back({at,at+size_t(length)}); }
    size_t record(size_t layer) const { return integer(table+16*layer); }
    explicit CTBEncryptedLayerLayout(const std::vector<uint8_t> &source,const std::function<void()> &check={}) : bytes(source) {
        if (check) check();
        section(0,48);
        version=integer(16);
        if (integer(0)!=0x12fd0107 || (version!=4 && version!=5)) throw RuntimeError("Unsupported CTBEncrypted schema");
        const auto settings_size=integer(4);settings_offset=integer(8);
        if (settings_size<288 || settings_size%16 || settings_size>(1u<<20)) throw RuntimeError("Invalid CTBEncrypted settings size");
        section(settings_offset,settings_size);
        settings=ctb_encrypted::aes_cbc({bytes.begin()+settings_offset,bytes.begin()+settings_offset+settings_size},false,check);
        signature_offset=integer(24);
        if (integer(20)!=32) throw RuntimeError("Invalid CTBEncrypted signature size");
        section(signature_offset,32);
        const uint64_t checksum=setting(0)|(uint64_t(setting(4))<<32);
        const auto signature=ctb_encrypted::signature(checksum);
        if (!std::equal(signature.begin(),signature.end(),bytes.begin()+signature_offset)) throw RuntimeError("CTBEncrypted signature mismatch");
        // Keep the original protocol trailer and optional v5 prefix when moving
        // the signature to the end after appended replacement layer records.
        section(signature_offset+32,4);
        if (version==5) {
            if (signature_offset<8) throw RuntimeError("Truncated CTBEncrypted v5 trailer");
            section(signature_offset-8,8);
        }
        columns=setting(56);rows=setting(60);count=setting(64);seed=setting(128);table=setting(8);
        if (!columns || !rows || columns>32768 || rows>32768 || uint64_t(columns)*rows>(uint64_t(128)<<20) || !count || count>100000)
            throw RuntimeError("CTBEncrypted dimensions or layer count exceed editor limits");
        for (size_t at:{12u,16u,20u}) if (!std::isfinite(parameter(at)) || parameter(at)<=0) throw RuntimeError("Invalid CTBEncrypted geometry");
        section(table,uint64_t(count)*16);
        if (setting(164)) section(setting(160),setting(164));
        if (setting(268)) section(setting(264),setting(268));
        if (version==5 && setting(276)) {
            const auto resin=setting(276);section(resin,40);
            for (auto [address,length]:{std::pair<size_t,size_t>{8,28},{16,12},{24,20}})
                if (integer(resin+length)) section(integer(resin+address),integer(resin+length));
        }
        for (size_t field:{68u,72u}) {
            const auto at=setting(field);if(!at)continue;section(at,16);
            const auto w=integer(at),h=integer(at+4),data=integer(at+8),length=integer(at+12);
            if (!w || !h || w>4096 || h>4096) throw RuntimeError("Invalid CTBEncrypted preview dimensions");
            section(data,length);uint64_t pixels=0;size_t next_check=data;
            for (size_t cursor=data;cursor<size_t(data)+length;) {
                if (check && cursor>=next_check) {check();next_check=cursor+65536;}
                if (size_t(data)+length-cursor<2) throw RuntimeError("Truncated CTBEncrypted preview");
                const auto color=uint16_t(bytes[cursor])|(uint16_t(bytes[cursor+1])<<8);cursor+=2;unsigned run=1;
                if(color&32) {
                    if (size_t(data)+length-cursor<2) throw RuntimeError("Truncated CTBEncrypted preview run");
                    run+=bytes[cursor]|((bytes[cursor+1]&15)<<8);cursor+=2;
                }
                pixels+=run;if(pixels>uint64_t(w)*h)throw RuntimeError("CTBEncrypted preview exceeds dimensions");
            }
            if(pixels!=uint64_t(w)*h)throw RuntimeError("Incomplete CTBEncrypted preview");
        }
        std::vector<Range> payloads;
        for(size_t i=0;i<count;++i) {
            if(check && i%256==0)check();
            const auto pointer=table+16*i,at=record(i);
            if(integer(pointer+4) || integer(pointer+8)!=88)throw RuntimeError("Unsupported CTBEncrypted layer page or record size");
            section(at,88);
            const auto address=integer(at+16),length=integer(at+24),aes_at=integer(at+32),aes_size=integer(at+36);
            if(integer(at)!=88 || integer(at+20) || address!=at+88 || !length)throw RuntimeError("Invalid CTBEncrypted mask address or record size");
            if(aes_size && (aes_size%16 || aes_at>length || aes_size>length-aes_at))throw RuntimeError("Invalid CTBEncrypted AES range");
            require(address,length);payloads.push_back({address,size_t(address)+length});
            for(size_t field:{4u,8u,12u,40u,44u,48u,52u,56u,60u,64u,68u,72u,76u,80u})
                if(!std::isfinite(number(at+field)) || number(at+field)<0)throw RuntimeError("Invalid CTBEncrypted layer parameter");
        }
        auto less=[](const Range &a,const Range &b){return a.begin<b.begin || (a.begin==b.begin && a.end<b.end);};
        std::sort(metadata.begin(),metadata.end(),less);
        auto verify_disjoint=[](const std::vector<Range> &ranges) {
            for(size_t i=1;i<ranges.size();++i)
                if(ranges[i].begin<ranges[i-1].end && (ranges[i].begin!=ranges[i-1].begin || ranges[i].end!=ranges[i-1].end))
                    throw RuntimeError("CTBEncrypted sections overlap");
        };
        verify_disjoint(metadata);std::sort(payloads.begin(),payloads.end(),less);verify_disjoint(payloads);
        for(const auto &range:payloads) {
            const auto at=std::lower_bound(metadata.begin(),metadata.end(),range.begin,[](const Range &r,size_t begin){return r.end<=begin;});
            if(at!=metadata.end() && at->begin<range.end)throw RuntimeError("CTBEncrypted mask overlaps metadata");
        }
    }
    std::vector<uint8_t> decode(size_t layer,const std::function<void()> &check={}) const {
        if(layer>=count)throw RuntimeError("CTBEncrypted layer index outside document");
        if(check)check();
        const auto at=record(layer);const auto address=integer(at+16),length=integer(at+24);
        std::vector<uint8_t> payload(bytes.begin()+address,bytes.begin()+address+length);
        if(const auto size=integer(at+36)) {
            const auto offset=integer(at+32);
            const auto decrypted=ctb_encrypted::aes_cbc({payload.begin()+offset,payload.begin()+offset+size},false,check);
            std::copy(decrypted.begin(),decrypted.end(),payload.begin()+offset);
        }
        ctb_encrypted::xor_layer(payload,uint32_t(layer),seed,check);
        return chitubox::decode_gray7(payload,size_t(columns)*rows,check);
    }
};
}
