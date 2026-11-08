#include <array>
#include <libslic3r/CustomGCode.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <unordered_map>
#include <catch2/generators/catch_generators.hpp>
#include <cfenv>
#include <CGAL/FPU.h>
#include <random>
#include <numeric>
#include <cstdint>

#include "sla_test_utils.hpp"

#include <libslic3r/TriangleMeshSlicer.hpp>
#include <libslic3r/SLA/PrinterRaster.hpp>
#include <libslic3r/SLA/LayerPolygonComposition.hpp>
#include <libslic3r/SLA/SupportTreeMesher.hpp>
#include <libslic3r/BranchingTree/PointCloud.hpp>

namespace {

const char *const BELOW_PAD_TEST_OBJECTS[] = {
    "20mm_cube.obj",
    "V.obj",
};

const char *const AROUND_PAD_TEST_OBJECTS[] = {
    "20mm_cube.obj",
    "V.obj",
    "frog_legs.obj",
    "cube_with_concave_hole_enlarged.obj",
};

const char *const SUPPORT_TEST_MODELS[] = {
    "cube_with_concave_hole_enlarged_standing.obj",
    "A_upsidedown.obj",
    "extruder_idler.obj"
};

} // namespace

TEST_CASE("Flat pad geometry is valid", "[SLASupportGeneration]") {
    sla::PadConfig padcfg;
    
    // Disable wings
    padcfg.wall_height_mm = .0;
    
    for (auto &fname : BELOW_PAD_TEST_OBJECTS) test_pad(fname, padcfg);
}

TEST_CASE("WingedPadGeometryIsValid", "[SLASupportGeneration]") {
    sla::PadConfig padcfg;
    
    // Add some wings to the pad to test the cavity
    padcfg.wall_height_mm = 1.;
    
    for (auto &fname : BELOW_PAD_TEST_OBJECTS) test_pad(fname, padcfg);
}

TEST_CASE("FlatPadAroundObjectIsValid", "[SLASupportGeneration]") {
    sla::PadConfig padcfg;
    
    // Add some wings to the pad to test the cavity
    padcfg.wall_height_mm = 0.;
    // padcfg.embed_object.stick_stride_mm = 0.;
    padcfg.embed_object.enabled = true;
    padcfg.embed_object.everywhere = true;
    
    for (auto &fname : AROUND_PAD_TEST_OBJECTS) test_pad(fname, padcfg);
}

TEST_CASE("WingedPadAroundObjectIsValid", "[SLASupportGeneration]") {
    sla::PadConfig padcfg;
    
    // Add some wings to the pad to test the cavity
    padcfg.wall_height_mm = 1.;
    padcfg.embed_object.enabled = true;
    padcfg.embed_object.everywhere = true;
    
    for (auto &fname : AROUND_PAD_TEST_OBJECTS) test_pad(fname, padcfg);
}

TEST_CASE("DefaultSupports::ElevatedSupportGeometryIsValid", "[SLASupportGeneration]") {
    sla::SupportTreeConfig supportcfg;
    supportcfg.object_elevation_mm = 10.;
    
    for (auto fname : SUPPORT_TEST_MODELS) test_supports(fname, supportcfg);
}

TEST_CASE("DefaultSupports::FloorSupportGeometryIsValid", "[SLASupportGeneration]") {
    sla::SupportTreeConfig supportcfg;
    supportcfg.object_elevation_mm = 0;
    
    for (auto &fname: SUPPORT_TEST_MODELS) test_supports(fname, supportcfg);
}

TEST_CASE("DefaultSupports::ElevatedSupportsDoNotPierceModel", "[SLASupportGeneration]") {
    sla::SupportTreeConfig supportcfg;
    supportcfg.object_elevation_mm = 10.;

    for (auto fname : SUPPORT_TEST_MODELS)
        test_support_model_collision(fname, supportcfg);
}

TEST_CASE("DefaultSupports::FloorSupportsDoNotPierceModel", "[SLASupportGeneration]") {
    
    sla::SupportTreeConfig supportcfg;
    supportcfg.object_elevation_mm = 0;
    
    for (auto fname : SUPPORT_TEST_MODELS)
        test_support_model_collision(fname, supportcfg);
}

//TEST_CASE("BranchingSupports::ElevatedSupportGeometryIsValid", "[SLASupportGeneration][Branching]") {
//    sla::SupportTreeConfig supportcfg;
//    supportcfg.object_elevation_mm = 10.;
//    supportcfg.tree_type = sla::SupportTreeType::Branching;

//    for (auto fname : SUPPORT_TEST_MODELS) test_supports(fname, supportcfg);
//}

//TEST_CASE("BranchingSupports::FloorSupportGeometryIsValid", "[SLASupportGeneration][Branching]") {
//    sla::SupportTreeConfig supportcfg;
//    supportcfg.object_elevation_mm = 0;
//    supportcfg.tree_type = sla::SupportTreeType::Branching;

//    for (auto &fname: SUPPORT_TEST_MODELS) test_supports(fname, supportcfg);
//}


TEST_CASE("BranchingSupports::ElevatedSupportsDoNotPierceModel", "[SLASupportGeneration][Branching]") {

    sla::SupportTreeConfig supportcfg;
    supportcfg.object_elevation_mm = 10.;
    supportcfg.tree_type = sla::SupportTreeType::Branching;

    for (auto fname : SUPPORT_TEST_MODELS)
        test_support_model_collision(fname, supportcfg);
}

TEST_CASE("BranchingSupports::FloorSupportsDoNotPierceModel", "[SLASupportGeneration][Branching]") {

    sla::SupportTreeConfig supportcfg;
    supportcfg.object_elevation_mm = 0;
    supportcfg.tree_type = sla::SupportTreeType::Branching;

    for (auto fname : SUPPORT_TEST_MODELS)
        test_support_model_collision(fname, supportcfg);
}

TEST_CASE("InitializedRasterShouldBeNONEmpty", "[SLARasterOutput]") {
    // Default Prusa SL1 display parameters
    sla::Resolution res{2560, 1440};
    sla::PixelDim   pixdim{120. / res.width_px, 68. / res.height_px};
    
    sla::RasterGrayscaleAAGammaPower raster(res, pixdim, {}, 1.);
    REQUIRE(raster.resolution().width_px == res.width_px);
    REQUIRE(raster.resolution().height_px == res.height_px);
    REQUIRE(raster.pixel_dimensions().w_mm == Approx(pixdim.w_mm));
    REQUIRE(raster.pixel_dimensions().h_mm == Approx(pixdim.h_mm));
}

TEST_CASE("MirroringShouldBeCorrect", "[SLARasterOutput]") {
    sla::RasterBase::TMirroring mirrorings[] = {sla::RasterBase::NoMirror,
                                                sla::RasterBase::MirrorX,
                                                sla::RasterBase::MirrorY,
                                                sla::RasterBase::MirrorXY};

    sla::RasterBase::Orientation orientations[] =
        {sla::RasterBase::roLandscape, sla::RasterBase::roPortrait};
    
    for (auto orientation : orientations)
        for (auto &mirror : mirrorings)
            check_raster_transformations(orientation, mirror);
}


TEST_CASE("RasterizedPolygonAreaShouldMatch", "[SLARasterOutput]") {
    double disp_w = 120., disp_h = 68.;
    sla::Resolution res{2560, 1440};
    sla::PixelDim pixdim{disp_w / res.width_px, disp_h / res.height_px};
    
    double gamma = 1.;
    sla::RasterGrayscaleAAGammaPower raster(res, pixdim, {}, gamma);
    auto bb = BoundingBox({0, 0}, {scaled(disp_w), scaled(disp_h)});
    
    ExPolygon poly = square_with_hole(10.);
    poly.translate(bb.center().x(), bb.center().y());
    raster.draw(poly);
    
    double a = poly.area() / (scaled<double>(1.) * scaled(1.));
    double ra = raster_white_area(raster);
    double diff = std::abs(a - ra);
    
    REQUIRE(diff <= predict_error(poly, pixdim));
    
    raster.clear();
    poly = square_with_hole(60.);
    poly.translate(bb.center().x(), bb.center().y());
    raster.draw(poly);
    
    a = poly.area() / (scaled<double>(1.) * scaled(1.));
    ra = raster_white_area(raster);
    diff = std::abs(a - ra);
    
    REQUIRE(diff <= predict_error(poly, pixdim));
    
    sla::RasterGrayscaleAA raster0(res, pixdim, {}, [](double) { return 0.; });
    REQUIRE(raster_pxsum(raster0) == 0);
    
    raster0.draw(poly);
    ra = raster_white_area(raster);
    REQUIRE(raster_pxsum(raster0) == 0);
}


TEST_CASE("halfcone test", "[halfcone]") {
    sla::DiffBridge br{Vec3d{1., 1., 1.}, Vec3d{10., 10., 10.}, 0.25, 0.5};

    indexed_triangle_set m = sla::get_mesh(br, 45);

    its_merge_vertices(m);
    its_write_obj(m, "Halfcone.obj");
}

TEST_CASE("Test concurrency")
{
    std::vector<double> vals = grid(0., 100., 10.);

    double ref = std::accumulate(vals.begin(), vals.end(), 0.);

    double s = execution::accumulate(ex_tbb, vals.begin(), vals.end(), 0.);

    REQUIRE(s == Approx(ref));
}

TEST_CASE("Raster masks are independent of worker floating-point rounding", "[sla_raster_rounding]") {
    struct RestoreEnvironment {
        std::fenv_t state;
        RestoreEnvironment(){std::fegetenv(&state);}
        ~RestoreEnvironment(){std::fesetenv(&state);}
    } restore;
    // Pixel pitch is 1536 coordinate units (a non-binary ratio). The edges sit exactly halfway
    // between AGG subpixels; upward/downward rounding must not change AA.
    ExPolygon poly(Polygon({Point(30723,30723),Point(122883,30723),
        Point(122883,92163),Point(30723,92163)}));
    const auto render=[&] {
        auto raster=sla::create_raster_grayscale_aa({128,96},{.001536,.001536},1.);
        raster->draw(poly);
        return raster->encode([](const void *data,size_t width,size_t height,size_t components) {
            const auto *bytes=static_cast<const uint8_t *>(data);
            return sla::EncodedRaster(std::vector<uint8_t>(bytes,bytes+width*height*components),"raw");
        });
    };
    REQUIRE(std::fesetround(FE_TONEAREST)==0);const auto reference=render();
    const auto *base=static_cast<const uint8_t *>(reference.data());
    const std::vector<uint8_t> expected(base,base+reference.size());
    REQUIRE(expected.size()==128*96);
    REQUIRE(std::count_if(expected.begin(),expected.end(),[](uint8_t value){return value!=0;})>0);
    for(const auto rounding:{FE_TONEAREST,FE_UPWARD,FE_DOWNWARD}) {
        REQUIRE(std::fesetround(rounding)==0);
        const auto actual=render();const auto *bytes=static_cast<const uint8_t *>(actual.data());
        const bool identical=std::vector<uint8_t>(bytes,bytes+actual.size())==expected;
        CHECK(identical);
        CHECK(std::fegetround()==rounding);
    }
}

TEST_CASE("Printer raster computes pixel pitch independently of worker rounding", "[sla_printer_pitch_rounding]") {
    struct RestoreEnvironment {
        std::fenv_t state;
        RestoreEnvironment() { std::fegetenv(&state); }
        ~RestoreEnvironment() { std::fesetenv(&state); }
    } restore;
    const bool portrait = GENERATE(false, true);
    const bool mirror_x = GENERATE(false, true);
    const bool mirror_y = GENERATE(false, true);
    const double gamma = GENERATE(0., 1., 1.7);
    CAPTURE(portrait, mirror_x, mirror_y, gamma);
    SLAPrinterConfig config;
    config.display_width.value = .192;
    config.display_height.value = .142848;
    config.display_pixels_x.value = 125;
    config.display_pixels_y.value = 93;
    config.gamma_correction.value = gamma;
    config.display_orientation.value = portrait ? sladoPortrait : sladoLandscape;
    config.display_mirror_x.value = mirror_x;
    config.display_mirror_y.value = mirror_y;
    ExPolygon poly(Polygon({Point(30723,30723),Point(122883,30723),
        Point(122883,92163),Point(30723,92163)}));
    auto render = [&] {
        auto raster = sla::create_printer_raster(config);
        raster->draw(poly);
        return raster->encode([](const void *data, size_t width, size_t height, size_t components) {
            const auto *bytes = static_cast<const uint8_t *>(data);
            return sla::EncodedRaster(std::vector<uint8_t>(bytes, bytes + width * height * components), "raw");
        });
    };
    REQUIRE(std::fesetround(FE_TONEAREST) == 0);
    const auto expected = render();
    CHECK(expected.size() == 125 * 93);
    const auto *pixels = static_cast<const uint8_t *>(expected.data());
    CHECK(std::count_if(pixels, pixels + expected.size(), [](uint8_t value) { return value != 0; }) > 0);
    for (const auto rounding : {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        REQUIRE(std::fesetround(rounding) == 0);
        const auto actual = render();
        CHECK(actual.size() == expected.size());
        const bool same = actual.size() == expected.size() &&
            std::memcmp(actual.data(), expected.data(), actual.size()) == 0;
        CHECK(same);
        CHECK(std::fegetround() == rounding);
        auto invalid = config;
        invalid.display_pixels_x.value = 0;
        CHECK_THROWS(sla::create_printer_raster(invalid));
        CHECK(std::fegetround() == rounding);
    }
}

TEST_CASE("SLA composition is independent of geometry worker rounding", "[sla_composition_rounding]") {
    struct RestoreEnvironment {
        std::fenv_t state;
        RestoreEnvironment() { std::fegetenv(&state); }
        ~RestoreEnvironment() { std::fesetenv(&state); }
    } restore;
    const ExPolygons models{ExPolygon(Polygon({Point(0,0), Point(6,1), Point(0,5)}))};
    const ExPolygons supports{ExPolygon(Polygon({Point(3,-1), Point(4,-1), Point(4,6), Point(3,6)}))};
    REQUIRE(std::fesetround(FE_TONEAREST) == 0);
    const auto reference = sla::compose_layer_polygons(models, supports);
    REQUIRE_FALSE(reference.polygons.empty());
    for (const auto mode : {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD}) {
        REQUIRE(std::fesetround(mode) == 0);
        const auto actual = sla::compose_layer_polygons(models, supports);
        const bool same = actual.polygons == reference.polygons;
        CHECK(same);
        CHECK(actual.model_area == reference.model_area);
        CHECK(actual.support_area == reference.support_area);
        CHECK(std::fegetround() == mode);
    }
}

TEST_CASE("Raster restores CGAL SSE rounding independently of the x87 mode", "[sla_mixed_rounding]") {
    struct RestoreEnvironment {
        std::fenv_t state;
        RestoreEnvironment() { std::fegetenv(&state); }
        ~RestoreEnvironment() { std::fesetenv(&state); }
    } restore;
    SLAPrinterConfig config;
    config.display_width.value = .192; config.display_height.value = .142848;
    config.display_pixels_x.value = 125; config.display_pixels_y.value = 93;
    config.gamma_correction.value = 1.;
    ExPolygon poly(Polygon({Point(30723,30723),Point(122883,30723),
        Point(122883,92163),Point(30723,92163)}));
    auto render = [&] {
        auto raster = sla::create_printer_raster(config);
        raster->draw(poly);
        return raster->encode([](const void *data, size_t width, size_t height, size_t components) {
            const auto *bytes = static_cast<const uint8_t *>(data);
            return sla::EncodedRaster(std::vector<uint8_t>(bytes, bytes + width * height * components), "raw");
        });
    };
    REQUIRE(std::fesetround(FE_TONEAREST) == 0);
    const auto reference = render();
    for (const auto mode : {CGAL_FE_UPWARD, CGAL_FE_DOWNWARD}) {
        REQUIRE(std::fesetround(FE_TONEAREST) == 0);
        CGAL::FPU_set_cw(mode);
        const auto x87_mode = std::fegetround();
        const auto cgal_mode = CGAL::FPU_get_cw();
        CAPTURE(x87_mode, cgal_mode);
        const auto actual = render();
        const bool same = actual.size() == reference.size() &&
            std::memcmp(actual.data(), reference.data(), actual.size()) == 0;
        CHECK(same);
        CHECK(std::fegetround() == x87_mode);
        CHECK(CGAL::FPU_get_cw() == cgal_mode);
    }
}

TEST_CASE("Large SLA contour areas preserve the caller-independent geometry contract", "[sla_composition_rounding]") {
    struct RestoreEnvironment {
        std::fenv_t state;
        RestoreEnvironment() {std::fegetenv(&state);}
        ~RestoreEnvironment() {std::fesetenv(&state);}
    } restore;
    const ExPolygons models{ExPolygon(Polygon({Point(100000001,100000003),
        Point(200000011,100000019),Point(100000023,200000033)}))};
    REQUIRE(std::fesetround(FE_TONEAREST)==0);
    const auto reference=sla::compose_layer_polygons(models,{});
    for(const auto mode:{FE_UPWARD,FE_DOWNWARD}) {
        REQUIRE(std::fesetround(mode)==0);
        const auto actual=sla::compose_layer_polygons(models,{});
        CHECK(actual.model_area==reference.model_area);
        CHECK(std::fegetround()==mode);
    }
    for(const auto mode:{CGAL_FE_UPWARD,CGAL_FE_DOWNWARD}) {
        REQUIRE(std::fesetround(FE_TONEAREST)==0);CGAL::FPU_set_cw(mode);
        const auto actual=sla::compose_layer_polygons(models,{});
        CHECK(actual.model_area==reference.model_area);
        CHECK(CGAL::FPU_get_cw()==static_cast<CGAL::FPU_CW_t>(mode));
    }
}

TEST_CASE("SLA composition canonicalizes equivalent contour traversal orders", "[sla_composition_rounding]") {
    ExPolygon frame(Polygon({Point(100000001,100000003),Point(200000011,100000019),
        Point(200000031,200000033),Point(100000023,200000033)}));
    frame.holes.emplace_back(Polygon({Point(120000000,120000000),Point(120000000,140000000),
        Point(140000000,140000000),Point(140000000,120000000)}));
    frame.holes.emplace_back(Polygon({Point(160000000,160000000),Point(160000000,180000000),
        Point(180000000,180000000),Point(180000000,160000000)}));
    const ExPolygons models{frame,ExPolygon(Polygon({Point(180000000,130000000),
        Point(210000000,130000000),Point(190000000,160000000)}))};
    const ExPolygons supports{ExPolygon(Polygon({Point(90000000,140000000),Point(110000000,140000000),
        Point(100000000,170000000)})),ExPolygon(Polygon({Point(150000000,90000000),
        Point(170000000,110000000),Point(140000000,120000000)}))};
    const auto reference=sla::compose_layer_polygons(models,supports);
    for(size_t offset=0;offset<4;++offset) {
        auto a=models,b=supports;
        const auto rotate=[offset](Polygon &ring) {
            std::rotate(ring.points.begin(),ring.points.begin()+offset%ring.size(),ring.points.end());
        };
        for(auto *set:{&a,&b}) {
            std::reverse(set->begin(),set->end());
            for(auto &polygon:*set) {
                rotate(polygon.contour);for(auto &hole:polygon.holes)rotate(hole);
                std::reverse(polygon.holes.begin(),polygon.holes.end());
            }
        }
        const auto actual=sla::compose_layer_polygons(a,b);
        CHECK(actual.polygons==reference.polygons);
        CHECK(actual.model_area==reference.model_area);CHECK(actual.support_area==reference.support_area);
    }
}

TEST_CASE("SLA contour order remains stable at a repeated minimum vertex", "[sla_composition_rounding]") {
    // Two lobes touch at the same lexicographic minimum. Choosing the first
    // minimum alone does not define an invariant start for this ring.
    const Polygon touching({Point(100000000,100000000),Point(102000000,101000000),
        Point(101000000,102000000),Point(100000000,100000000),
        Point(101000000,98000000),Point(102000000,99000000)});
    const auto reference=sla::compose_layer_polygons({ExPolygon(touching)},{});
    REQUIRE(reference.model_area==3000000000000.);
    for(size_t offset=0;offset<touching.size();++offset) {
        auto rotated=touching;
        std::rotate(rotated.points.begin(),rotated.points.begin()+offset,rotated.points.end());
        const auto actual=sla::compose_layer_polygons({ExPolygon(rotated)},{});
        CHECK(actual.polygons==reference.polygons);
        CHECK(actual.model_area==reference.model_area);
    }
}

TEST_CASE("Legacy 3MF colorprint migration preserves metadata without an FFF emitter", "[3mf][sla_legacy_colorprint]") {
    DynamicPrintConfig legacy;
    const std::vector<double> heights{.2,.4,.6,.8,1.,1.2,1.4,1.6};
    legacy.set_key_value("colorprint_heights",new ConfigOptionFloats(heights));
    CustomGCode::Info migrated;
    CustomGCode::update_custom_gcode_per_print_z_from_config(migrated,&legacy);
    REQUIRE(migrated.gcodes.size()==heights.size());
    CHECK(migrated.mode==CustomGCode::SingleExtruder);
    CHECK_FALSE(legacy.has("colorprint_heights"));
    // Stored project colors are user-visible metadata; preserve the historical
    // first-color offset and cycle rather than inspecting an internal palette.
    const std::array<const char *,8> expected{"#E67E22","#F1C40F","#27AE60","#1ABC9C",
        "#2980B9","#9B59B6","#C0392B","#E67E22"};
    for(size_t i=0;i<heights.size();++i) {
        CHECK(migrated.gcodes[i].print_z==heights[i]);
        CHECK(migrated.gcodes[i].type==CustomGCode::ColorChange);
        CHECK(migrated.gcodes[i].extruder==1);
        CHECK(migrated.gcodes[i].color==expected[i]);
        CHECK(migrated.gcodes[i].extra.empty());
    }
    const auto preserved=migrated;
    legacy.set_key_value("colorprint_heights",new ConfigOptionFloats(std::vector<double>{9.}));
    CustomGCode::update_custom_gcode_per_print_z_from_config(migrated,&legacy);
    CHECK(migrated==preserved); // Existing modern metadata wins over legacy heights.
    CHECK_FALSE(legacy.has("colorprint_heights"));
    CustomGCode::update_custom_gcode_per_print_z_from_config(migrated,&legacy);
    CHECK(migrated==preserved); // Repeated import conversion is idempotent.
}
