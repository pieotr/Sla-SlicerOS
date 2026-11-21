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
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "sla_archive_test_config.hpp"

using namespace Slic3r;
namespace {
uint32_t word(const std::vector<uint8_t> &data, size_t at)
{
    return uint32_t(data.at(at)) | uint32_t(data.at(at+1))<<8 |
           uint32_t(data.at(at+2))<<16 | uint32_t(data.at(at+3))<<24;
}
float scalar(const std::vector<uint8_t> &data, size_t at)
{
    const auto bits = word(data, at); float value; std::memcpy(&value, &bits, 4); return value;
}
std::vector<uint8_t> read(const boost::filesystem::path &path)
{
    std::ifstream stream(path.string(), std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
struct Directory {
    boost::filesystem::path path = boost::filesystem::unique_path("ctb-modern-editor-%%%%");
    bool keep = false;
    ~Directory() { if (!keep) { boost::system::error_code e; boost::filesystem::remove_all(path,e); } }
};
}

TEST_CASE("Modern CTB mask editing preserves both layer definitions and physical parameters", "[sla_reference][sla_ctb_modern_editor]")
{
    const std::string format = GENERATE("ctb_v3", "ctb_v4", "ctb_v5", "ctb_gktwo");
    const bool portrait = GENERATE(false, true);
    const bool seeded = GENERATE(false, true);
    CAPTURE(format, portrait, seeded);
    SLAFullPrintConfig base; Test::configure_archive_fixture(base, format);
    DynamicPrintConfig cfg; cfg.apply(base);
    cfg.set("display_pixels_x",8); cfg.set("display_pixels_y",4);
    cfg.set("display_width",16.); cfg.set("display_height",8.);
    cfg.option("display_orientation")->setInt(portrait ? sladoPortrait : sladoLandscape);
    cfg.set("printer_model",std::string("CTB test — drukarka"));
    if (format == "ctb_v3") {
        for (const bool bottom : {false,true})
            for (const char *key : {"lift_height2", "retract_height2", "wait_after_cure", "wait_after_lift"})
                cfg.set(std::string(bottom ? "sla_bottom_" : "sla_")+key,0.);
    }
    std::vector<uint8_t> pixels(32,0);
    for (auto [at,value] : {std::pair<size_t,uint8_t>{0,255},{9,32},{10,64},{11,128},{17,192},{19,85}}) pixels[at]=value;
    const auto png = sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1);
    const auto document = sla::LayerDocument::from_rasters({png,png,png});
    auto job = SLAPrint::create_raster_job(cfg,document,{.05,.13,.2});
    Directory dir;
    if (const char *folder = std::getenv("SLA_CTB_MODERN_EDITOR_FIXTURES")) {
        dir.path=boost::filesystem::path(folder)/(format+(portrait?"-portrait":"-landscape")+(seeded?"-seeded":"-plain"));dir.keep=true;
    }
    boost::filesystem::create_directories(dir.path);
    cfg.save((dir.path/"source.ini").string());
    const std::string suffix = get_archive_entry(format.c_str())->ext;
    const auto source=dir.path/("source."+suffix), saved=dir.path/("saved."+suffix);
    job->export_print(source.string());
    { std::ofstream out(source.string(),std::ios::binary|std::ios::app);out << "vendor bytes preserved"; }
    if (!seeded) {
        auto plain = read(source);
        const uint32_t increment = word(plain,100)*0x2d83cdacU+0xd8a83423U;
        for (uint32_t layer=0; layer<3; ++layer) {
            const size_t at=word(plain,64)+36*layer;
            uint32_t key=(layer*0x1e1530cdU+0xec3d47cdU)*increment;
            for (size_t n=0; n<word(plain,at+16); ++n) {
                plain.at(word(plain,at+12)+n)^=uint8_t(key>>(8*(n%4)));
                if (n%4==3) key+=increment;
            }
        }
        std::fill(plain.begin()+100,plain.begin()+104,0);
        std::ofstream out(source.string(),std::ios::binary|std::ios::trunc);
        out.write(reinterpret_cast<const char *>(plain.data()),plain.size());
    }
    const auto original=read(source);
    auto loaded=sla::LayerDocument::load_archive(source.string());
    REQUIRE(loaded.archive_format()==format);REQUIRE(loaded.layer_count()==3);
    for (auto &value:pixels) value=value<2?0:uint8_t(value|1);
    for (size_t i=0;i<3;++i) REQUIRE(loaded.image(i).buf==pixels);
    loaded.save_archive(saved.string());REQUIRE(read(saved)==original);
    loaded.invert(1);
    auto expected=pixels;
    for (auto &value:expected) { value=255-value;value=value<2?0:uint8_t(value|1); }
    REQUIRE(loaded.image(1).buf==expected);
    loaded.save_archive(saved.string());
    auto edited=sla::LayerDocument::load_archive(saved.string());
    REQUIRE(edited.archive_format()==format);
    REQUIRE(edited.image(0).buf==pixels);REQUIRE(edited.image(1).buf==expected);REQUIRE(edited.image(2).buf==pixels);
    const auto written=read(saved);const size_t table=word(original,64);
    REQUIRE(word(written,0)==word(original,0));REQUIRE(word(written,4)==word(original,4));
    REQUIRE(word(written,100)==word(original,100));
    for (size_t i=0;i<3;++i) {
        const size_t record=table+36*i, old_extended=word(original,record+12)-84, extended=word(written,record+12)-84;
        // Parameters are expected from the original raw schema, not from the new reader.
        REQUIRE(std::vector<uint8_t>(written.begin()+extended+40,written.begin()+extended+84)==
                std::vector<uint8_t>(original.begin()+old_extended+40,original.begin()+old_extended+84));
        for (size_t field:{0u,4u,8u,12u,16u,20u,24u}) REQUIRE(word(written,extended+field)==word(written,record+field));
        REQUIRE(word(written,extended+36)==84+word(written,record+16));
        if (i!=1) REQUIRE(extended==old_extended);
    }
    const auto old_count=std::count_if(pixels.begin(),pixels.end(),[](uint8_t p){return p!=0;});
    const auto new_count=std::count_if(expected.begin(),expected.end(),[](uint8_t p){return p!=0;});
    const double volume=(old_count*.12+new_count*.08)*16.*8./32./1000.;
    REQUIRE(scalar(written,word(written,84)+20)==Catch::Approx(volume));
    const auto restored=dir.path/("restored."+suffix);
    REQUIRE(loaded.undo());loaded.save_archive(restored.string());REQUIRE(read(restored)==original);
    const auto before=read(restored);
    loaded.invert(1);
    REQUIRE_THROWS(loaded.save_archive(restored.string(),[](size_t,size_t){return false;}));
    REQUIRE(read(restored)==before);REQUIRE(loaded.undo());
}

TEST_CASE("Modern CTB rejects corrupt extended records and addressed metadata", "[sla_reference][sla_ctb_modern_editor]")
{
    SLAFullPrintConfig base; Test::configure_archive_fixture(base,"ctb_v5");
    DynamicPrintConfig cfg;cfg.apply(base);cfg.set("display_pixels_x",8);cfg.set("display_pixels_y",4);
    const std::vector<uint8_t> pixels(32,255);
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    auto job=SLAPrint::create_raster_job(cfg,sla::LayerDocument::from_rasters({png,png}),{.05,.1});
    Directory dir;boost::filesystem::create_directory(dir.path);
    const auto source=dir.path/"source.v5.ctb",invalid=dir.path/"invalid.v5.ctb";
    job->export_print(source.string());const auto bytes=read(source);
    const size_t table=word(bytes,64),slicer=word(bytes,104),v4=word(bytes,slicer+64),resin=word(bytes,v4+80);
    const size_t extended=word(bytes,table+12)-84;
    for (auto [at,value] : {std::pair<size_t,uint32_t>{table+12,40},{table+12,uint32_t(resin)},
            {table+12,UINT32_MAX},{table+16,UINT32_MAX},{table+20,1},{slicer+64,0},
            {v4+72,UINT32_MAX},{resin+8,uint32_t(table)},{extended+40,0x7fc00000},{extended,0x3e6b851f}}) {
        CAPTURE(at,value);auto bad=bytes;
        for (unsigned n=0;n<4;++n) bad.at(at+n)=uint8_t(value>>(8*n));
        { std::ofstream out(invalid.string(),std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char *>(bad.data()),bad.size()); }
        REQUIRE_THROWS(sla::LayerDocument::load_archive(invalid.string()));
    }
    REQUIRE_THROWS(sla::LayerDocument::load_archive(source.string(),[](size_t,size_t){return false;}));
    // A misleading filename must not override the detected schema/version.
    const auto mislabeled=dir.path/"misleading.v3.ctb";
    boost::filesystem::copy_file(source,mislabeled);
    REQUIRE(sla::LayerDocument::load_archive(mislabeled.string()).archive_format()=="ctb_v5");
    // Saving: printers and Chitubox name every CTB version plain ".ctb"; an edited v5 document must save
    // under that name, while a name claiming a different variant is still refused.
    auto document=sla::LayerDocument::load_archive(source.string());
    const auto plain=dir.path/"plain.ctb",wrong=dir.path/"wrong.v3.ctb";
    REQUIRE_NOTHROW(document.save_archive(plain.string()));
    REQUIRE(sla::LayerDocument::load_archive(plain.string()).archive_format()=="ctb_v5");
    REQUIRE(read(plain)==bytes);
    REQUIRE_THROWS(document.save_archive(wrong.string()));
    REQUIRE_FALSE(boost::filesystem::exists(wrong));
}
