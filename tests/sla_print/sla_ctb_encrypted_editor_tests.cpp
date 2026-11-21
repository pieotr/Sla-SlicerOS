// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_approx.hpp>
#include <boost/filesystem.hpp>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/Format/CTBEncryptedCrypto.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "sla_archive_test_config.hpp"
using namespace Slic3r;
namespace {
uint32_t word(const std::vector<uint8_t> &data,size_t at) {
    return uint32_t(data.at(at))|uint32_t(data.at(at+1))<<8|uint32_t(data.at(at+2))<<16|uint32_t(data.at(at+3))<<24;
}
void put(std::vector<uint8_t> &data,size_t at,uint32_t value) {
    for (unsigned n=0;n<4;++n) data.at(at+n)=uint8_t(value>>(8*n));
}
float scalar(const std::vector<uint8_t> &data,size_t at) {
    const auto bits=word(data,at);float value;std::memcpy(&value,&bits,4);return value;
}
std::vector<uint8_t> read(const boost::filesystem::path &path) {
    std::ifstream in(path.string(),std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};
}
void write(const boost::filesystem::path &path,const std::vector<uint8_t> &data) {
    std::ofstream out(path.string(),std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char *>(data.data()),data.size());
}
std::vector<uint8_t> settings(const std::vector<uint8_t> &data) {
    return ctb_encrypted::aes_cbc({data.begin()+word(data,8),data.begin()+word(data,8)+word(data,4)},false);
}
struct Directory {
    boost::filesystem::path path=boost::filesystem::unique_path("ctb-encrypted-editor-%%%%");bool keep=false;
    ~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}
};
}
TEST_CASE("Encrypted CTB editing preserves signed settings and optional AES mask ranges", "[sla_reference][sla_ctb_encrypted_editor]")
{
    const std::string format=GENERATE("ctb_encrypted","ctb_encrypted_v4");
    const bool portrait=GENERATE(false,true),seeded=GENERATE(false,true),aes=GENERATE(false,true);
    CAPTURE(format,portrait,seeded,aes);
    SLAFullPrintConfig base;Test::configure_archive_fixture(base,format);
    DynamicPrintConfig cfg;cfg.apply(base);
    cfg.set("display_pixels_x",32);cfg.set("display_pixels_y",16);
    cfg.set("display_width",16.);cfg.set("display_height",8.);
    cfg.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    cfg.set("printer_model",std::string("Encrypted test — drukarka"));
    std::vector<uint8_t> pixels(512);for(size_t i=0;i<pixels.size();++i)pixels[i]=uint8_t(i*3);
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),portrait?16:32,portrait?32:16,1);
    auto job=SLAPrint::create_raster_job(cfg,sla::LayerDocument::from_rasters({png,png,png}),{.05,.13,.2});
    Directory dir;
    if(const char *folder=std::getenv("SLA_CTB_ENCRYPTED_EDITOR_FIXTURES")) {
        dir.path=boost::filesystem::path(folder)/(format+(portrait?"-portrait":"-landscape")+(seeded?"-seeded":"-plain")+(aes?"-aes":"-xor"));dir.keep=true;
    }
    boost::filesystem::create_directories(dir.path);cfg.save((dir.path/"source.ini").string());
    const std::string suffix=get_archive_entry(format.c_str())->ext;
    const auto source=dir.path/("source."+suffix),saved=dir.path/("saved."+suffix),restored=dir.path/("restored."+suffix);
    job->export_print(source.string());
    auto bytes=read(source),plain=settings(bytes);
    for(uint32_t i=0;i<3;++i) {
        const size_t record=word(bytes,word(plain,8)+16*i),address=word(bytes,record+16),length=word(bytes,record+24);
        REQUIRE(length>=48);
        if(!seeded) {
            std::vector<uint8_t> payload(bytes.begin()+address,bytes.begin()+address+length);
            ctb_encrypted::xor_layer(payload,i,word(plain,128));std::copy(payload.begin(),payload.end(),bytes.begin()+address);
        }
        if(aes) {
            const auto encrypted=ctb_encrypted::aes_cbc({bytes.begin()+address+16,bytes.begin()+address+48},true);
            std::copy(encrypted.begin(),encrypted.end(),bytes.begin()+address+16);put(bytes,record+32,16);put(bytes,record+36,32);
        }
    }
    if(!seeded) put(plain,128,0);
    // The signature signs this arbitrary 64-bit checksum, not masks/settings.
    put(plain,0,0x12345678);put(plain,4,0x89abcdef);
    const auto encrypted=ctb_encrypted::aes_cbc(plain,true),signature=ctb_encrypted::signature(0x89abcdef12345678ULL);
    std::copy(encrypted.begin(),encrypted.end(),bytes.begin()+word(bytes,8));
    std::copy(signature.begin(),signature.end(),bytes.begin()+word(bytes,24));
    bytes.insert(bytes.end(),{'v','e','n','d','o','r'});write(source,bytes);
    auto loaded=sla::LayerDocument::load_archive(source.string());REQUIRE(loaded.archive_format()==format);
    for(auto &p:pixels)p=p<2?0:uint8_t(p|1);
    for(size_t i=0;i<3;++i)REQUIRE(loaded.image(i).buf==pixels);
    loaded.save_archive(saved.string());REQUIRE(read(saved)==bytes);
    loaded.invert(1);auto expected=pixels;for(auto &p:expected){p=255-p;p=p<2?0:uint8_t(p|1);}
    REQUIRE(loaded.image(1).buf==expected);loaded.save_archive(saved.string());
    const auto written=read(saved),updated=settings(written);
    REQUIRE(updated.size()==plain.size());
    for(size_t i=0;i<plain.size();++i)if(i<104||i>=116)REQUIRE(updated[i]==plain[i]);
    REQUIRE(std::equal(signature.begin(),signature.end(),written.begin()+word(written,24)));
    REQUIRE(word(written,24)>word(bytes,24));
    const auto old_count=std::count_if(pixels.begin(),pixels.end(),[](uint8_t p){return p!=0;});
    const auto new_count=std::count_if(expected.begin(),expected.end(),[](uint8_t p){return p!=0;});
    const double volume=(old_count*.12+new_count*.08)*16.*8./512./1000.;
    REQUIRE(scalar(updated,104)==Catch::Approx(volume));
    REQUIRE(scalar(updated,108)==Catch::Approx(scalar(plain,108)*volume/scalar(plain,104)));
    REQUIRE(scalar(updated,112)==Catch::Approx(scalar(plain,112)*volume/scalar(plain,104)));
    for(size_t i=0;i<3;++i) {
        const auto old_record=word(bytes,word(plain,8)+16*i),new_record=word(written,word(updated,8)+16*i);
        REQUIRE(std::vector<uint8_t>(bytes.begin()+old_record+40,bytes.begin()+old_record+88)==std::vector<uint8_t>(written.begin()+new_record+40,written.begin()+new_record+88));
        REQUIRE(word(written,new_record+16)==new_record+88);
        if(i!=1)REQUIRE(old_record==new_record);
        if(aes)REQUIRE(word(written,new_record+36)==32);
    }
    auto edited=sla::LayerDocument::load_archive(saved.string());
    REQUIRE(edited.image(0).buf==pixels);REQUIRE(edited.image(1).buf==expected);REQUIRE(edited.image(2).buf==pixels);
    REQUIRE(loaded.undo());loaded.save_archive(restored.string());REQUIRE(read(restored)==bytes);
    loaded.invert(1);REQUIRE_THROWS(loaded.save_archive(restored.string(),[](size_t,size_t){return false;}));
    REQUIRE(read(restored)==bytes);REQUIRE(loaded.undo());
    // This 131-byte RLE is small and unaligned, but long enough for the old
    // partial AES range. Encrypting it would make original UVtools clone an
    // adjacent image; a canonical unencrypted fallback must retain this mask.
    auto small_image=loaded.image(1);std::fill(small_image.buf.begin()+128,small_image.buf.end(),255);
    loaded.replace_image(1,small_image);const auto small=dir.path/("small."+suffix);
    loaded.save_archive(small.string());const auto small_bytes=read(small),small_settings=settings(small_bytes);
    const auto small_record=word(small_bytes,word(small_settings,8)+16);
    REQUIRE(word(small_bytes,small_record+36)==0);
    REQUIRE(sla::LayerDocument::load_archive(small.string()).image(1).buf==small_image.buf);
}

TEST_CASE("Encrypted CTB rejects signature damage and unsafe AES/table ranges", "[sla_reference][sla_ctb_encrypted_editor]")
{
    SLAFullPrintConfig base;Test::configure_archive_fixture(base,"ctb_encrypted");
    DynamicPrintConfig cfg;cfg.apply(base);cfg.set("display_pixels_x",32);cfg.set("display_pixels_y",16);
    const std::vector<uint8_t> pixels(512,255);
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),32,16,1);
    auto job=SLAPrint::create_raster_job(cfg,sla::LayerDocument::from_rasters({png,png}),{.05,.1});
    Directory dir;boost::filesystem::create_directory(dir.path);
    const auto source=dir.path/"source.encrypted.ctb",invalid=dir.path/"invalid.encrypted.ctb";
    job->export_print(source.string());const auto original=read(source),plain=settings(original);
    const size_t pointer=word(plain,8),record=word(original,pointer);
    for(auto [at,value]:{std::pair<size_t,uint32_t>{4,287},{24,UINT32_MAX},{pointer+4,1},{pointer,0},
            {record+16,0},{record+24,UINT32_MAX},{record+36,15},{record+36,32},{record+40,0x7fc00000}}) {
        CAPTURE(at,value);auto bad=original;put(bad,at,value);write(invalid,bad);
        REQUIRE_THROWS(sla::LayerDocument::load_archive(invalid.string()));
    }
    auto bad=original;bad.at(word(original,24))^=1;write(invalid,bad);
    REQUIRE_THROWS(sla::LayerDocument::load_archive(invalid.string()));
    bad=original;auto changed=plain;put(changed,8,word(original,24));
    const auto encrypted=ctb_encrypted::aes_cbc(changed,true);
    std::copy(encrypted.begin(),encrypted.end(),bad.begin()+word(original,8));write(invalid,bad);
    REQUIRE_THROWS(sla::LayerDocument::load_archive(invalid.string()));
    REQUIRE_THROWS(sla::LayerDocument::load_archive(source.string(),[](size_t,size_t){return false;}));
}

TEST_CASE("AES mask cancellation observes blocks without changing CBC output", "[sla_reference][sla_ctb_encrypted_editor]")
{
    const std::vector<uint8_t> bytes(131072,42);
    unsigned checks=0;
    REQUIRE_THROWS(ctb_encrypted::aes_cbc(bytes,true,[&]{if(++checks==2)throw std::runtime_error("cancel");}));
    REQUIRE(checks==2);
    const auto encrypted=ctb_encrypted::aes_cbc(bytes,true);
    REQUIRE(ctb_encrypted::aes_cbc(encrypted,false)==bytes);
}
