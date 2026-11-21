// SPDX-License-Identifier: AGPL-3.0-or-later
#include "libslic3r/SLA/LayerDocument.hpp"
#include <catch2/catch_test_macros.hpp>
#include <thread>
using namespace Slic3r;
namespace {
std::vector<uint8_t> bytes(const sla::LayerDocument &doc,size_t layer) {
    const auto b=doc.encoded_layer(layer);const auto *p=static_cast<const uint8_t *>(b.buf);return {p,p+b.sz};
}
sla::LayerDocument fixture(sla::LayerMaskPrecision precision=sla::LayerMaskPrecision::Gray8) {
    std::vector<sla::EncodedRaster> images;
    for(size_t layer=0;layer<7;++layer) {
        std::vector<uint8_t> pixels(1024*1024);
        for(size_t i=0;i<pixels.size();++i) pixels[i]=sla::project_mask_value(uint8_t((i*37+i/1024*13+layer*17)%256),precision);
        images.push_back(sla::PNGRasterEncoder{}(pixels.data(),1024,1024,1));
    }
    using F=sla::LayerParameter;
    sla::FreshLayerJob job;job.producer=sla::FreshLayerProducer::ModelSlice;job.mask_precision=precision;
    job.geometry={1024,1024,100.,100.};job.globals.layer_height=.05;
    job.globals.normal={{F::Exposure,2.},{F::LiftDistance,5.},{F::LiftSpeed,60.},
        {F::LiftDistance2,0.},{F::LiftSpeed2,0.},{F::RetractDistance2,0.},{F::RetractSpeed,120.},
        {F::RetractSpeed2,0.},{F::WaitBefore,0.},{F::WaitAfter,0.},{F::WaitAfterLift,0.},
        {F::LightOff,0.},{F::LightPWM,255.}};
    job.globals.bottom=job.globals.normal;
    for(size_t i=0;i<images.size();++i){auto row=job.globals.normal;row[F::PositionZ]=.05*(i+1);job.rows.push_back(row);}
    return sla::LayerDocument::from_fresh_rasters(std::move(images),std::move(job));
}
std::vector<std::vector<uint8_t>> encoded(const sla::LayerDocument &doc) {
    std::vector<std::vector<uint8_t>> out;
    for(size_t i=0;i<doc.layer_count();++i)out.push_back(bytes(doc,i));
    return out;
}
// Independent literal disk at a single center, not production line/spans.
void disk(png::ImageGreyscale &mask,int cx,int cy,int r,uint8_t value) {
    for(int y=0;y<int(mask.rows);++y)for(int x=0;x<int(mask.cols);++x)
        if(int64_t(x-cx)*(x-cx)+int64_t(y-cy)*(y-cy)<=int64_t(r)*r)
            mask.buf[size_t(y)*mask.cols+size_t(x)]=value;
}
}
TEST_CASE("Range PNG batches preserve literal pixels palettes exact PNG and one history step", "[sla_paint_batches]") {
    for(auto precision:{sla::LayerMaskPrecision::Gray8,sla::LayerMaskPrecision::NativeGray7,
                       sla::LayerMaskPrecision::NativeGray4,sla::bitplane_mask_precision(3)}) {
        CAPTURE(int(precision));auto doc=fixture(precision);const auto before=encoded(doc);const auto revision=doc.revision();
        std::vector<std::vector<uint8_t>> expected=before;
        for(size_t i=1;i<=5;++i) {
            auto mask=doc.image(i);disk(mask,3,5,11,127);
            for(auto &p:mask.buf)p=sla::project_mask_value(p,precision);
            const auto png=sla::PNGRasterEncoder{}(mask.buf.data(),mask.cols,mask.rows,1);const auto *p=static_cast<const uint8_t *>(png.data());expected[i]={p,p+png.size()};
        }
        const auto caller=std::this_thread::get_id();std::vector<size_t> seen;
        REQUIRE(doc.paint_stroke(1,5,{{3,5}},11,127,sla::LayerMaskPlane::Main,[&](size_t done,size_t total){
            CHECK(std::this_thread::get_id()==caller);CHECK(total==5);seen.push_back(done);return true;
        }));
        CHECK(seen==std::vector<size_t>{0,1,2,3,4,5});CHECK(encoded(doc)==expected);CHECK(doc.dirty());
        REQUIRE(doc.undo());CHECK(encoded(doc)==before);CHECK(doc.revision()==revision);CHECK_FALSE(doc.can_undo());CHECK(doc.can_redo());
        REQUIRE(doc.redo());CHECK(encoded(doc)==expected);CHECK(doc.can_undo());CHECK_FALSE(doc.can_redo());
    }
}
TEST_CASE("Range PNG batches cancel every logical checkpoint without changing existing history", "[sla_paint_batches]") {
    auto source=fixture();source.invert(0);source.invert(6);REQUIRE(source.undo());
    const auto before=encoded(source);const auto revision=source.revision();
    for(size_t stop=0;stop<=5;++stop) {
        auto doc=source;size_t calls=0;
        CHECK_FALSE(doc.paint_stroke(1,5,{{3,5},{900,750}},3,0,sla::LayerMaskPlane::Main,[&](size_t done,size_t total){
            CHECK(done==calls++);CHECK(total==5);return done!=stop;
        }));
        CHECK(calls==stop+1);CHECK(encoded(doc)==before);CHECK(doc.revision()==revision);
        CHECK(doc.can_undo());CHECK(doc.can_redo());CHECK(doc.dirty());
    }
    auto invalid=source;CHECK_THROWS(invalid.paint_stroke(1,5,{{3,5},{1024,750}},3,0));
    CHECK(encoded(invalid)==before);CHECK(invalid.revision()==revision);CHECK(invalid.can_undo());CHECK(invalid.can_redo());
}
TEST_CASE("Range PNG batches report a corrupt future layer only after its cancellation checkpoint", "[sla_paint_batches]") {
    auto source=fixture();
    // Deliberate QA fault after validated construction: damage IEND CRC while
    // retaining a valid size header. No public mutator can create this input.
    const auto view=source.encoded_layer(3);auto *p=const_cast<uint8_t *>(static_cast<const uint8_t *>(view.buf));p[view.sz-1]^=1;
    const auto before=encoded(source);const auto revision=source.revision();
    for(size_t stop=0;stop<=3;++stop) {
        auto doc=source;size_t calls=0;
        CHECK_FALSE(doc.paint_stroke(0,4,{{3,5}},3,0,sla::LayerMaskPlane::Main,[&](size_t done,size_t total){
            CHECK(done==calls++);CHECK(total==5);return done!=stop;
        }));
        CHECK(calls==stop+1);CHECK(encoded(doc)==before);CHECK(doc.revision()==revision);CHECK_FALSE(doc.can_undo());
    }
    auto doc=source;std::vector<size_t> seen;
    CHECK_THROWS(doc.paint_stroke(0,4,{{3,5}},3,0,sla::LayerMaskPlane::Main,[&](size_t done,size_t){seen.push_back(done);return true;}));
    CHECK(seen==std::vector<size_t>{0,1,2,3});CHECK(encoded(doc)==before);CHECK(doc.revision()==revision);CHECK_FALSE(doc.can_undo());
}
TEST_CASE("Range brush rejects callback mutations instead of overwriting the newer document", "[sla_paint_generation]") {
    const auto source=fixture();
    for(size_t change_at:{size_t(0),size_t(1),size_t(4),size_t(5)}) {
        CAPTURE(change_at);auto doc=source,expected=source;expected.invert(0);
        const auto expected_png=encoded(expected);size_t calls=0;bool changed=false;
        CHECK_THROWS(doc.paint_stroke(0,4,{{3,5}},3,0,sla::LayerMaskPlane::Main,[&](size_t done,size_t){
            ++calls;if(done==change_at){doc.invert(0);changed=true;}return true;
        }));
        CHECK(changed);CHECK(calls==change_at+1);CHECK(encoded(doc)==expected_png);CHECK(doc.revision()==expected.revision());
        CHECK(doc.can_undo());CHECK_FALSE(doc.can_redo());REQUIRE(doc.undo());CHECK(encoded(doc)==encoded(source));CHECK_FALSE(doc.can_undo());
    }
}

// SPDX-License-Identifier: AGPL-3.0-or-later
// Included after the accepted fixture and contracts for source-identity checks.
TEST_CASE("Range brush rejects callback rollback even when its revision is unchanged", "[sla_paint_generation_extra]") {
    const auto source=fixture();
    for(size_t change_at:{size_t(1),size_t(5)}) {
        CAPTURE(change_at);auto doc=source,expected=source;expected.invert(0);REQUIRE(expected.undo());
        size_t calls=0;
        CHECK_THROWS(doc.paint_stroke(0,4,{{3,5}},3,0,sla::LayerMaskPlane::Main,[&](size_t done,size_t){
            ++calls;if(done==change_at){doc.invert(0);doc.undo();}return true;
        }));
        CHECK(calls==change_at+1);CHECK(encoded(doc)==encoded(expected));CHECK(doc.revision()==source.revision());
        CHECK_FALSE(doc.can_undo());CHECK(doc.can_redo());
    }
}
TEST_CASE("Range brush rejects callback replacement with matching revision and pixels", "[sla_paint_generation_extra]") {
    const auto source=fixture();
    for(size_t change_at:{size_t(1),size_t(5)}) {
        CAPTURE(change_at);auto doc=source;size_t calls=0;
        CHECK_THROWS(doc.paint_stroke(0,4,{{3,5}},3,0,sla::LayerMaskPlane::Main,[&](size_t done,size_t){
            ++calls;if(done==change_at)doc=source;return true;
        }));
        CHECK(calls==change_at+1);CHECK(encoded(doc)==encoded(source));CHECK(doc.revision()==source.revision());
        CHECK_FALSE(doc.can_undo());CHECK_FALSE(doc.can_redo());
    }
}
