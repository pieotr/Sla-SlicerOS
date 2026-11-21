// SPDX-License-Identifier: AGPL-3.0-or-later
#include "libslic3r/SLA/LayerDocument.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
using namespace Slic3r;
namespace {
// Literal pre-optimization raster rule, not the span implementation.
void literal_brush(png::ImageGreyscale &img,int x0,int y0,int x1,int y1,unsigned radius,uint8_t value)
{
    const int r=int(radius),dx=std::abs(x1-x0),sx=x0<x1?1:-1;
    const int dy=-std::abs(y1-y0),sy=y0<y1?1:-1;
    int error=dx+dy;
    for (;;) {
        for(int y=std::max(0,y0-r);y<=std::min(int(img.rows)-1,y0+r);++y)
            for(int x=std::max(0,x0-r);x<=std::min(int(img.cols)-1,x0+r);++x)
                if(int64_t(x-x0)*(x-x0)+int64_t(y-y0)*(y-y0)<=int64_t(r)*r)
                    img.buf[size_t(y)*img.cols+size_t(x)]=value;
        if(x0==x1 && y0==y1)break;
        const int twice=2*error;
        if(twice>=dy){error+=dy;x0+=sx;}
        if(twice<=dx){error+=dx;y0+=sy;}
    }
}
png::ImageGreyscale patterned_mask(size_t cols,size_t rows)
{
    png::ImageGreyscale img;img.cols=cols;img.rows=rows;img.buf.resize(cols*rows);
    for(size_t i=0;i<img.buf.size();++i)img.buf[i]=uint8_t((i*37+i/cols*13+11)%256);
    return img;
}
}
TEST_CASE("Circular brush matches literal disks in all line directions and borders", "[sla_brush_spans]")
{
    const auto original=patterned_mask(5,4);
    for(int a=0;a<20;++a)for(int b=0;b<20;++b)
        for(unsigned radius:{0u,1u,2u,3u,4u,5u,8u,9u,63u,64u,4096u})
            for(uint8_t value:{uint8_t(0),uint8_t(1),uint8_t(127),uint8_t(255)}) {
                CAPTURE(a,b,radius,value);
                auto actual=original,expected=original;
                literal_brush(expected,a%5,a/5,b%5,b/5,radius,value);
                sla::LayerDocument::draw_line(actual,a%5,a/5,b%5,b/5,radius,value);
                REQUIRE(actual.buf==expected.buf);
            }
}
TEST_CASE("Circular brush preserves polyline gaps and maximum coordinate boundaries", "[sla_brush_spans]")
{
    auto actual=patterned_mask(15,13),expected=actual;
    const auto original=actual.buf;
    const std::array<sla::LayerDocument::BrushPoint,6> points{{{2,2},{12,2},{12,10},{2,10},{2,2},{12,2}}};
    auto previous=points.front();
    for(const auto &point:points) {
        literal_brush(expected,previous.x,previous.y,point.x,point.y,1,0);
        sla::LayerDocument::draw_line(actual,previous.x,previous.y,point.x,point.y,1,0);
        previous=point;
    }
    CHECK(actual.buf==expected.buf);CHECK(actual.buf[6*15+7]==original[6*15+7]);
    for(bool vertical:{false,true})for(unsigned radius:{0u,1u,3u,64u,4096u}) {
        CAPTURE(vertical,radius);
        auto a=patterned_mask(vertical?1:32768,vertical?32768:1),b=a;
        const int x0=vertical?0:32760,y0=vertical?32760:0;
        const int x1=vertical?0:32767,y1=vertical?32767:0;
        literal_brush(b,x0,y0,x1,y1,radius,127);
        sla::LayerDocument::draw_line(a,x0,y0,x1,y1,radius,127);
        CHECK(a.buf==b.buf);
    }
    auto one=patterned_mask(1,1);sla::LayerDocument::draw_line(one,0,0,0,0,4096,255);CHECK(one.buf[0]==255);
}
TEST_CASE("Circular brush rejects invalid inputs before changing pixels", "[sla_brush_spans]")
{
    const auto original=patterned_mask(5,4);
    for(const auto &points:std::array<std::array<int,4>,6>{{{{-1,0,1,1}},{{0,-1,1,1}},{{0,0,5,0}},{{0,0,0,4}},{{0,0,-1,0}},{{0,0,0,-1}}}}) {
        auto mask=original;
        REQUIRE_THROWS(sla::LayerDocument::draw_line(mask,points[0],points[1],points[2],points[3],1,255));CHECK(mask.buf==original.buf);
    }
    auto mask=original;REQUIRE_THROWS(sla::LayerDocument::draw_line(mask,0,0,1,1,4097,255));CHECK(mask.buf==original.buf);
    mask.buf.pop_back();const auto shortened=mask.buf;REQUIRE_THROWS(sla::LayerDocument::draw_line(mask,0,0,1,1,1,255));CHECK(mask.buf==shortened);
    mask=patterned_mask(32769,1);const auto too_wide=mask.buf;REQUIRE_THROWS(sla::LayerDocument::draw_line(mask,0,0,1,0,1,255));CHECK(mask.buf==too_wide);
    mask=patterned_mask(0,0);REQUIRE_THROWS(sla::LayerDocument::draw_line(mask,0,0,0,0,1,255));CHECK(mask.buf.empty());
}
TEST_CASE("Circular brush preserves wide disk boundaries and shallow diagonals", "[sla_brush_spans]")
{
    const auto original=patterned_mask(137,139);
    const std::array<std::array<int,4>,9> lines{{{{68,69,68,69}},{{0,0,136,138}},{{136,0,0,138}},{{0,69,136,70}},{{136,69,0,68}},{{68,0,69,138}},{{69,138,68,0}},{{0,0,136,1}},{{136,138,0,137}}}};
    for(const auto &line:lines)for(unsigned radius:{0u,1u,3u,7u,16u,63u,64u,65u,127u}) {
        CAPTURE(line[0],line[1],line[2],line[3],radius);
        auto actual=original,expected=original;
        literal_brush(expected,line[0],line[1],line[2],line[3],radius,127);
        sla::LayerDocument::draw_line(actual,line[0],line[1],line[2],line[3],radius,127);
        REQUIRE(actual.buf==expected.buf);
    }
}

TEST_CASE("Wide range brush cancellation preserves existing history at every checkpoint", "[sla_brush_spans]")
{
    const auto image=patterned_mask(137,139);
    const auto png=sla::PNGRasterEncoder{}(image.buf.data(),image.cols,image.rows,1);
    auto source=sla::LayerDocument::from_rasters({png,png,png,png,png,png});
    source.invert(0);source.invert(5);REQUIRE(source.undo());
    const auto revision=source.revision();REQUIRE(source.can_undo());REQUIRE(source.can_redo());REQUIRE(source.dirty());
    const auto encoded=[](const sla::LayerDocument &doc,size_t layer){
        const auto view=doc.encoded_layer(layer);const auto *bytes=static_cast<const uint8_t *>(view.buf);
        return std::vector<uint8_t>(bytes,bytes+view.sz);
    };
    std::vector<std::vector<uint8_t>> before;
    for(size_t i=0;i<source.layer_count();++i)before.push_back(encoded(source,i));
    const std::vector<sla::LayerDocument::BrushPoint> points{{20,20},{100,110}};
    for(size_t stop=0;stop<=4;++stop) {
        CAPTURE(stop);
        size_t callbacks=0;
        CHECK_FALSE(source.paint_stroke(1,4,points,64,127,sla::LayerMaskPlane::Main,[&](size_t done,size_t total){
            CHECK(total==4);CHECK(done==callbacks);++callbacks;return done!=stop;
        }));
        CHECK(callbacks==stop+1);CHECK(source.revision()==revision);
        CHECK(source.can_undo());CHECK(source.can_redo());CHECK(source.dirty());
        for(size_t i=0;i<source.layer_count();++i)CHECK(encoded(source,i)==before[i]);
    }
    CHECK_THROWS(source.paint_stroke(1,4,{{20,20},{137,110}},64,127));
    CHECK(source.revision()==revision);CHECK(source.can_redo());
    for(size_t i=0;i<source.layer_count();++i)CHECK(encoded(source,i)==before[i]);
    std::vector<std::vector<uint8_t>> expected;
    for(size_t i=0;i<source.layer_count();++i) {
        auto pixels=source.image(i);
        if(i>=1 && i<=4)literal_brush(pixels,20,20,100,110,64,127);
        expected.push_back(std::move(pixels.buf));
    }
    REQUIRE(source.paint_stroke(1,4,points,64,127));CHECK_FALSE(source.can_redo());
    for(size_t i=0;i<source.layer_count();++i)CHECK(source.image(i).buf==expected[i]);
    REQUIRE(source.undo());CHECK(source.revision()==revision);CHECK(source.can_undo());CHECK(source.can_redo());
    for(size_t i=0;i<source.layer_count();++i)CHECK(encoded(source,i)==before[i]);
    REQUIRE(source.redo());for(size_t i=0;i<source.layer_count();++i)CHECK(source.image(i).buf==expected[i]);
}
