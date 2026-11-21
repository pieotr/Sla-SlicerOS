// SPDX-License-Identifier: AGPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <boost/filesystem.hpp>
#include <fstream>
#include <cstdlib>
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/LayerDocument.hpp"
#include "libslic3r/SLA/LayerPreviewCache.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include <nlohmann/json.hpp>
#include "libslic3r/Format/AnycubicLayerLayout.hpp"
using namespace Slic3r;
namespace {
std::vector<uint8_t> read(const boost::filesystem::path &path) {
    std::ifstream in(path.string(),std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};
}
void write(const boost::filesystem::path &path,const std::vector<uint8_t> &bytes) {
    std::ofstream out(path.string(),std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char *>(bytes.data()),bytes.size());
}
void put(std::vector<uint8_t> &bytes,size_t at,uint32_t value) {
    for(unsigned n=0;n<4;++n)bytes.at(at+n)=uint8_t(value>>(8*n));
}
struct Directory {
    boost::filesystem::path path=boost::filesystem::unique_path("anycubic-subimages-%%%%");bool keep=false;
    ~Directory(){if(!keep){boost::system::error_code e;boost::filesystem::remove_all(path,e);}}
};
}
TEST_CASE("Main mask edits preserve independent Anycubic SUBIMGS and unknown fields", "[sla_reference][sla_anycubic_subimages][sla_anycubic_planes][sla_layer_preview_planes]")
{
    const std::string format=GENERATE("pwc","pm5s","m5sp","pm4n");
    const bool portrait=GENERATE(false,true),mixed=GENERATE(false,true);
    CAPTURE(format,portrait,mixed);
    DynamicPrintConfig cfg;cfg.apply(SLAFullPrintConfig{});cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format",format);cfg.set("sla_anycubic_version",std::string("518"));
    cfg.set("display_pixels_x",8);cfg.set("display_pixels_y",4);
    cfg.set("display_width",16.);cfg.set("display_height",4.);
    cfg.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[10]=85;pixels[20]=170;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),portrait?4:8,portrait?8:4,1);
    auto job=SLAPrint::create_raster_job(cfg,sla::LayerDocument::from_rasters({png,png,png}),{.1,.15,.2});
    Directory dir;
    if(const char *folder=std::getenv("SLA_ANYCUBIC_SUBIMAGES_FIXTURES")) {
        dir.path=boost::filesystem::path(folder)/(format+(portrait?"-portrait":"-landscape")+(mixed?"-mixed":"-independent"));dir.keep=true;
    }
    boost::filesystem::create_directories(dir.path);cfg.save((dir.path/"source.ini").string());
    const auto source=dir.path/("source."+format),saved=dir.path/("saved."+format),restored=dir.path/("restored."+format);
    job->export_print(source.string());auto bytes=read(source);
    // Build a valid independent plane, which UVtools' own encoder overwrites
    // with the main mask. Preserve opaque padding without inventing exposure.
    const AnycubicLayerLayout initial(bytes,format);
    std::vector<uint8_t> secondary(32,0);secondary[3]=255;secondary[21]=119;
    const auto payload=initial.encode(secondary);
    for(size_t i=0;i<3;++i) {
        const size_t sub=initial.subrecord(i);
        if(!mixed || i==1) {
            const auto address=uint32_t(bytes.size());bytes.insert(bytes.end(),payload.begin(),payload.end());
            put(bytes,sub,address);put(bytes,sub+4,uint32_t(payload.size()));put(bytes,sub+8,2);
        }
        for(size_t n=12;n<44;++n)bytes[sub+n]=uint8_t(32+i+n);
    }
    bytes.insert(bytes.end(),{'v','e','n','d','o','r'});write(source,bytes);
    const AnycubicLayerLayout before(bytes,format);
    auto loaded=sla::LayerDocument::load_archive(source.string());
    REQUIRE(loaded.mask_plane_count()==2);
    // A cached overlay must distinguish independent Main/SUBIMGS planes even
    // when both requests have identical document ownership and layer bounds.
    sla::LayerPreviewCache preview;
    using PreviewPlane=sla::LayerMaskPlane;
    preview.publish(sla::LayerPreviewCache::prepare(loaded,0,2,PreviewPlane::Main),
        loaded,0,2,PreviewPlane::Main);
    REQUIRE(preview.find(loaded,0,2,PreviewPlane::Main));
    CHECK(preview.find(loaded,0,2,PreviewPlane::Main)->buf==pixels);
    CHECK(preview.find(loaded,0,2,PreviewPlane::Secondary)==nullptr);
    preview.publish(sla::LayerPreviewCache::prepare(loaded,0,2,PreviewPlane::Secondary),
        loaded,0,2,PreviewPlane::Secondary);
    auto expected_overlay=secondary;
    if(mixed)for(size_t i=0;i<pixels.size();++i)expected_overlay[i]=std::max(pixels[i],secondary[i]);
    REQUIRE(preview.find(loaded,0,2,PreviewPlane::Secondary));
    CHECK(preview.find(loaded,0,2,PreviewPlane::Secondary)->buf==expected_overlay);
    CHECK(preview.find(loaded,0,2,PreviewPlane::Main)==nullptr);
    CHECK_FALSE(loaded.dirty()); CHECK_FALSE(loaded.can_undo());
    // Imported native documents have their own owner; the raster-job factory
    // rejects native tables rather than flattening their independent planes.
    const auto captured=loaded.snapshot();
    REQUIRE(captured.mask_plane_count()==2);
    CHECK(captured.archive_format()==loaded.archive_format());
    CHECK_FALSE(captured.can_undo());
    for(size_t i=0;i<loaded.layer_count();++i) {
        CHECK(captured.mask_is_shared(i)==loaded.mask_is_shared(i));
        CHECK(captured.layer_parameters(i)==loaded.layer_parameters(i));
        for(const auto plane:{sla::LayerMaskPlane::Main,sla::LayerMaskPlane::Secondary})
            CHECK(captured.image(i,plane).buf==loaded.image(i,plane).buf);
    }
    const auto captured_path=dir.path/("captured."+format);
    auto captured_copy=captured;
    captured_copy.save_archive(captured_path.string());
    CHECK(read(captured_path)==bytes);
    loaded.save_archive(restored.string());REQUIRE(read(restored)==bytes);
    loaded.invert(1);loaded.save_archive(saved.string());
    CHECK(captured.image(1).buf==pixels);
    const auto edited_capture=loaded.snapshot();
    CHECK_FALSE(edited_capture.can_undo());
    auto edited_copy=edited_capture;edited_copy.save_archive(captured_path.string());
    CHECK(read(captured_path)==read(saved));
    const auto output=read(saved);const AnycubicLayerLayout after(output,format);
    for(size_t i=0;i<3;++i) {
        const auto old=before.subrecord(i),at=after.subrecord(i);
        REQUIRE(std::vector<uint8_t>(bytes.begin()+old,bytes.begin()+old+44)==std::vector<uint8_t>(output.begin()+at,output.begin()+at+44));
        const auto address=before.integer(old),length=before.integer(old+4);
        REQUIRE(std::vector<uint8_t>(bytes.begin()+address,bytes.begin()+address+length)==std::vector<uint8_t>(output.begin()+address,output.begin()+address+length));
    }
    auto expected=pixels;for(auto &value:expected)value=uint8_t(255-value);
    REQUIRE(sla::LayerDocument::load_archive(saved.string()).image(1).buf==expected);
    REQUIRE(loaded.undo());loaded.save_archive(restored.string());REQUIRE(read(restored)==bytes);
    loaded.invert(1);REQUIRE_THROWS(loaded.save_archive(restored.string(),[](size_t,size_t){return false;}));REQUIRE(read(restored)==bytes);
    const auto invalid=dir.path/("invalid."+format);
    for(auto [address,length]:{std::pair<uint32_t,uint32_t>{0,4},{UINT32_MAX,4},{uint32_t(before.subtable),4}}) {
        auto bad=bytes;put(bad,before.subrecord(1),address);put(bad,before.subrecord(1)+4,length);write(invalid,bad);
        REQUIRE_THROWS(sla::LayerDocument::load_archive(invalid.string()));
    }
    // Counts within the image bounds must still agree with decoded pixels.
    for(uint32_t count:{1u,33u}) {
        auto bad=bytes;put(bad,before.subrecord(1)+8,count);write(invalid,bad);
        REQUIRE_THROWS(sla::LayerDocument::load_archive(invalid.string()));
    }
    // A valid address does not make an overlong secondary RLE run safe.
    auto bad=bytes;bad[before.integer(before.subrecord(1))]=0x0f;write(invalid,bad);
    REQUIRE_THROWS(sla::LayerDocument::load_archive(invalid.string()));
    boost::filesystem::remove(invalid);
    using Plane=sla::LayerMaskPlane;
    auto editor=sla::LayerDocument::load_archive(source.string());
    REQUIRE(editor.image(1,Plane::Secondary).buf==secondary);
    REQUIRE(editor.mask_is_shared(0)==mixed);
    REQUIRE_FALSE(editor.mask_is_shared(1));
    REQUIRE_THROWS(editor.image(0,static_cast<Plane>(2)));
    const auto secondary_path=dir.path/("secondary."+format);
    editor.invert(1,Plane::Secondary);editor.save_archive(secondary_path.string());
    const auto edited=read(secondary_path);const AnycubicLayerLayout edited_layout(edited,format);
    auto inverted=secondary;for(auto &value:inverted)value=uint8_t(255-value);
    for(size_t i=0;i<3;++i) REQUIRE(edited_layout.decode(i)==before.decode(i));
    REQUIRE(edited_layout.decode_subimage(1)==inverted);
    const auto sub=before.subrecord(1);
    // Compare the complete original prefix in one assertion, excluding only
    // the explicitly edited secondary address/length/count fields.
    auto preserved=edited;preserved.resize(bytes.size());
    std::copy(bytes.begin()+sub,bytes.begin()+sub+12,preserved.begin()+sub);
    REQUIRE(preserved==bytes);
    auto reopened=sla::LayerDocument::load_archive(secondary_path.string());
    REQUIRE(reopened.image(1,Plane::Secondary).buf==inverted);
    REQUIRE(reopened.image(1).buf==pixels);
    REQUIRE(editor.undo());REQUIRE(editor.image(1,Plane::Secondary).buf==secondary);
    editor.save_archive(restored.string());REQUIRE(read(restored)==bytes);
    REQUIRE(editor.redo());REQUIRE(editor.image(1,Plane::Secondary).buf==inverted);
    REQUIRE(editor.undo());
    // A cancelled range may not change either mask or history.
    REQUIRE_FALSE(editor.transform_layers(0,2,sla::LayerDocument::MaskOperation::Fill,17,
                                          [](size_t n,size_t){return n<1;},1,Plane::Secondary));
    REQUIRE(editor.image(1,Plane::Secondary).buf==secondary);
    REQUIRE_FALSE(editor.can_undo());
    REQUIRE(editor.transform_layers(0,2,sla::LayerDocument::MaskOperation::Fill,17,{},1,Plane::Secondary));
    const auto range_path=dir.path/("secondary-range."+format);editor.save_archive(range_path.string());
    auto range=sla::LayerDocument::load_archive(range_path.string());
    for(size_t i=0;i<3;++i) {
        REQUIRE(range.image(i,Plane::Secondary).buf==std::vector<uint8_t>(32,17));
        REQUIRE(range.image(i).buf==(mixed && i!=1?std::vector<uint8_t>(32,17):pixels));
        REQUIRE(range.mask_is_shared(i)==(mixed && i!=1));
    }
    REQUIRE(editor.undo());REQUIRE_FALSE(editor.can_undo());
    editor.save_archive(restored.string());REQUIRE(read(restored)==bytes);
    // Brushes, replacement and image export operate on the selected plane.
    editor.paint(1,0,0,0,255,Plane::Secondary);
    auto painted=secondary;painted[0]=255;REQUIRE(editor.image(1,Plane::Secondary).buf==painted);
    REQUIRE(editor.image(1).buf==pixels);
    const auto mask_path=dir.path/"secondary.png";
    REQUIRE(editor.export_images(mask_path.string(),"png",1,1,{},Plane::Secondary));
    REQUIRE(sla::LayerDocument::load_archive(mask_path.string()).image(0).buf==painted);
    editor.replace_image(1,sla::LayerDocument::load_archive(mask_path.string()).image(0),Plane::Secondary);
    REQUIRE(editor.undo());REQUIRE(editor.image(1,Plane::Secondary).buf==secondary);
    editor.threshold(1,100,Plane::Secondary);REQUIRE(editor.image(1).buf==pixels);
    REQUIRE(editor.undo());editor.save_archive(restored.string());REQUIRE(read(restored)==bytes);
    REQUIRE_THROWS(SLAPrint::create_raster_job(cfg,editor,{.1,.15,.2}));
    // The complete project schema must preserve imported documents.
    sla::ProjectRasterState state{editor,{.1,.15,.2},std::string(40,'a')};
    mz_zip_archive project{};REQUIRE(mz_zip_writer_init_heap(&project,0,0));
    REQUIRE_NOTHROW(sla::write_project_layers(project,state));
    REQUIRE(project.m_total_files>0);
    REQUIRE(mz_zip_writer_end(&project));
    auto single=sla::LayerDocument::from_rasters({png});REQUIRE(single.mask_plane_count()==1);
    REQUIRE_THROWS(single.image(0,Plane::Secondary));

}

// The baseline heights identify the originating slice, independently of edited Z.
TEST_CASE("Project document roundtrip preserves native parameters and mask relationships", "[sla_layers][sla_project_document]")
{
    const bool parameters=GENERATE(false,true),independent=GENERATE(false,true);
    CAPTURE(parameters,independent);
    Directory dir;boost::filesystem::create_directories(dir.path);
    DynamicPrintConfig cfg;cfg.apply(SLAFullPrintConfig{});cfg.option("printer_technology")->setInt(ptSLA);
    cfg.set("sla_archive_format",std::string("pwc"));cfg.set("sla_anycubic_version",std::string("518"));
    cfg.set("display_pixels_x",8);cfg.set("display_pixels_y",4);cfg.set("display_width",16.);cfg.set("display_height",4.);
    cfg.option("display_orientation")->setInt(sladoLandscape);
    std::vector<uint8_t> pixels(32,0);pixels[1]=255;pixels[20]=170;
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),8,4,1);
    auto job=SLAPrint::create_raster_job(cfg,sla::LayerDocument::from_rasters({png,png,png}),{.1,.15,.2});
    const auto source=dir.path/"source.pwc",expected=dir.path/"expected.pwc",actual=dir.path/"actual.pwc",project=dir.path/"project.3mf";
    job->export_print(source.string());auto bytes=read(source);const AnycubicLayerLayout layout(bytes,"pwc");
    if(independent) {
        auto secondary=pixels;secondary[3]=119;
        const auto payload=layout.encode(secondary);const auto offset=uint32_t(bytes.size());
        bytes.insert(bytes.end(),payload.begin(),payload.end());
        put(bytes,layout.subrecord(1),offset);put(bytes,layout.subrecord(1)+4,uint32_t(payload.size()));
        put(bytes,layout.subrecord(1)+8,3);
    }
    bytes.insert(bytes.end(),{'o','p','a','q','u','e'});write(source,bytes);
    auto document=sla::LayerDocument::load_archive(source.string());
    document.paint(1,0,0,0,255,sla::LayerMaskPlane::Secondary);
    if(parameters)document.set_layer_parameters(1,{{sla::LayerParameter::Exposure,7.25},{sla::LayerParameter::PositionZ,.16}});
    sla::ProjectRasterState state{document,{.1,.15,.2},std::string(40,'a')};
    struct Writer {
        mz_zip_archive archive{};
        ~Writer(){if(archive.m_pState)close_zip_writer(&archive);}
    } writer;
    REQUIRE(open_zip_writer(&writer.archive,project.string()));
    REQUIRE_NOTHROW(sla::write_project_layers(writer.archive,state));
    REQUIRE(mz_zip_writer_finalize_archive(&writer.archive));
    REQUIRE(close_zip_writer(&writer.archive));
    auto restored=sla::read_project_layers(project.string());REQUIRE(restored.has_value());
    REQUIRE(restored->positions_z==state.positions_z);
    REQUIRE(restored->document.archive_format()==document.archive_format());
    REQUIRE_FALSE(restored->document.dirty());REQUIRE_FALSE(restored->document.can_undo());
    REQUIRE(restored->document.has_parameter_edits()==parameters);
    // A project cannot change how the immutable native source links its planes.
    const auto entries=read_zipper_archive(project.string(),{"metadata/sla/"},{}).entries;
    const auto manifest=std::find_if(entries.begin(),entries.end(),[](const auto &entry){return entry.fname=="metadata/sla/layers.json";});
    REQUIRE(manifest!=entries.end());auto metadata=nlohmann::json::parse(manifest->buf);
    const auto main_index=metadata["document"]["main"][1].get<size_t>();
    std::string clone_name;std::vector<uint8_t> clone;
    if(independent)metadata["document"]["secondary"][1]=main_index;
    else {
        const auto index=metadata["document"]["entries"].size();auto number=std::to_string(main_index);
        const auto original_name="metadata/sla/document/"+std::string(8-number.size(),'0')+number+".bin";
        const auto original=std::find_if(entries.begin(),entries.end(),[&](const auto &entry){return entry.fname==original_name;});
        REQUIRE(original!=entries.end());clone=original->buf;number=std::to_string(index);
        clone_name="metadata/sla/document/"+std::string(8-number.size(),'0')+number+".bin";
        metadata["document"]["entries"].push_back("cloned-mask");metadata["document"]["secondary"][1]=index;
    }
    const auto changed_sharing=dir.path/"invalid-sharing.3mf";
    {Zipper zip(changed_sharing.string());
        for(const auto &entry:entries) {
            if(entry.fname==manifest->fname){zip.add_entry(entry.fname);zip<<metadata.dump();}
            else zip.add_entry(entry.fname,entry.buf.data(),entry.buf.size());
        }
        if(!clone_name.empty())zip.add_entry(clone_name,clone.data(),clone.size());
        zip.finalize();
    }
    REQUIRE_THROWS(sla::read_project_layers(changed_sharing.string()));

    for(size_t i=0;i<3;++i) {
        REQUIRE(restored->document.image(i).buf==document.image(i).buf);
        REQUIRE(restored->document.image(i,sla::LayerMaskPlane::Secondary).buf==document.image(i,sla::LayerMaskPlane::Secondary).buf);
        REQUIRE(restored->document.mask_is_shared(i)==document.mask_is_shared(i));
        REQUIRE(restored->document.layer_parameters(i)==document.layer_parameters(i));
    }
    document.save_archive(expected.string());restored->document.save_archive(actual.string());
    REQUIRE(read(actual)==read(expected)); // includes opaque native trailer, parameters and both planes
    restored->document.paint(0,0,0,0,17,sla::LayerMaskPlane::Secondary);
    REQUIRE(restored->document.image(0).get(0,0)==17); // shared relationship survives restart
    REQUIRE(restored->document.undo());
    REQUIRE(restored->document.image(0).buf==document.image(0).buf);
}
