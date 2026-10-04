#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/SLA/CalibrationRaster.hpp"
#include "libslic3r/SLA/PCBExposure.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <cstdlib>
#include <boost/filesystem.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
using namespace Slic3r;
namespace {
const std::string prefix = "%FSLAX24Y24*%\n%MOMM*%\n";
double area(const sla::PCBArtwork &board) {
  double result = 0.;
  for (const auto &polygon : board.copper)
    result += polygon.area() * SCALING_FACTOR * SCALING_FACTOR;
  return result;
}
DynamicPrintConfig printer_config() {
  DynamicPrintConfig config;
  config.apply(SLAFullPrintConfig{});
  config.set("display_width", 40.);
  config.set("display_height", 30.);
  config.set("display_pixels_x", 400);
  config.set("display_pixels_y", 150);
  config.option("display_orientation")->setInt(sladoLandscape);
  config.set("display_mirror_x", false); config.set("display_mirror_y", false);
  config.set("layer_height", .05);
  config.set("initial_layer_height", .1);
  config.set("max_print_height", 50.);
  config.set("sla_archive_format", std::string("uvj"));
  return config;
}
} // namespace
TEST_CASE("Gerber standard apertures have physical sizes and ordered polarity",
          "[sla_pcb]") {
  SECTION("circle track and holed flash") {
    auto board =
        sla::parse_gerber(prefix + "%ADD10C,2*%D10*X0Y0D02*X100000Y0D01*M02*");
    REQUIRE(area(board) == Catch::Approx(20. + 3.141592653589793).margin(.03));
    auto holed =
        sla::parse_gerber(prefix + "%ADD10C,4X2*%D10*X100000Y100000D03*M02*");
    REQUIRE(area(holed) == Catch::Approx(3. * 3.141592653589793).margin(.05));
    REQUIRE(holed.copper.front().holes.size() == 1);
  }
  SECTION("rectangle and clear pad") {
    auto board = sla::parse_gerber(
        prefix + "%ADD10R,10X8*%%ADD11R,2X2*%D10*X100000Y100000D03*%LPC*%D11*"
                 "X100000Y100000D03*M02*");
    REQUIRE(area(board) == Catch::Approx(76.).margin(.001));
    REQUIRE(board.copper.front().holes.size() == 1);
    auto restored = sla::parse_gerber(
        prefix + "%ADD10R,10X8*%%ADD11R,2X2*%D10*X100000Y100000D03*%LPC*%D11*"
                 "X100000Y100000D03*%LPD*%X100000Y100000D03*M02*");
    REQUIRE(area(restored) == Catch::Approx(80.).margin(.001));
  }
  SECTION("obround and polygon") {
    auto board = sla::parse_gerber(prefix + "%ADD10O,6X2*%D10*X0Y0D03*M02*");
    REQUIRE(area(board) == Catch::Approx(8. + 3.141592653589793).margin(.03));
    auto square =
        sla::parse_gerber(prefix + "%ADD10P,4X4X45*%D10*X0Y0D03*M02*");
    REQUIRE(area(square) == Catch::Approx(8.).margin(.001));
  }
  SECTION("inch units and trailing zero suppression") {
    auto board = sla::parse_gerber(
        "%FSTAX24Y24*%%MOIN*%%ADD10R,0.1X0.2*%D10*X01Y02D03*M02*");
    const auto bounds = get_extents(board.copper);
    REQUIRE(unscaled(bounds.min.x()) == Catch::Approx(24.13).margin(1e-6));
    REQUIRE(unscaled(bounds.min.y()) == Catch::Approx(48.26).margin(1e-6));
    REQUIRE(area(board) == Catch::Approx(12.9032).margin(.001));
  }
}
TEST_CASE("Gerber regions and arcs preserve geometry", "[sla_pcb]") {
  auto rectangle = sla::parse_gerber(
      prefix + "G36*X0Y0D02*X100000Y0D01*Y50000D01*X0D01*Y0D01*G37*M02*");
  REQUIRE(area(rectangle) == Catch::Approx(50.).margin(.001));
  auto ring = sla::parse_gerber(
      prefix + "G75*G36*X50000Y0D02*G03X50000Y0I-50000J0D01*G37*M02*");
  REQUIRE(area(ring) == Catch::Approx(25. * 3.141592653589793).margin(.16));
  auto track = sla::parse_gerber(
      prefix + "%ADD10C,1*%D10*G75*X50000Y0D02*G03X0Y50000I-50000J0D01*M02*");
  REQUIRE(area(track) > 8.4);
  REQUIRE(area(track) < 8.8);
}
TEST_CASE("Unsupported or invalid PCB commands cannot silently change artwork",
          "[sla_pcb]") {
  for (const std::string command :
       {"G91*", "%SRX2Y2I1J1*%", "%AMTEST*1,1,2,0,0*%", "G74*", "%LMX*%"})
    REQUIRE_THROWS(sla::parse_gerber(prefix + command + "M02*"));
  REQUIRE_THROWS(sla::parse_gerber(prefix + "%ADD10R,2X2*%D10*X0Y0D03*"));
  REQUIRE_THROWS(sla::parse_gerber(prefix + "%ADD10C,-1*%D10*X0Y0D03*M02*"));
  REQUIRE_THROWS(
      sla::parse_gerber(prefix + "%ADD10C,2X1*%D10*X0Y0D02*X10000Y0D01*M02*"));
  REQUIRE_THROWS(sla::parse_gerber(prefix + "G36*X0Y0D02*G37*M02*"));
  REQUIRE_THROWS(
      sla::parse_gerber(prefix + "%ADD10C,1*%D10*X0Y0D03*M02*X0Y0D03*"));
}
TEST_CASE("Excellon drill subtraction is transactional and unit aware",
          "[sla_pcb]") {
  auto board =
      sla::parse_gerber(prefix + "%ADD10R,10X8*%D10*X100000Y100000D03*M02*");
  const double before = area(board);
  REQUIRE_THROWS(sla::subtract_excellon(
      board, "M48\nMETRIC,LZ\nT1C2\n%\nT1\nX10.0Y10.0\nG01X11Y11\nM30\n"));
  REQUIRE(area(board) == before);
  sla::subtract_excellon(
      board, "M48\nMETRIC,LZ,000.000\nT1C2\n%\nT1\nX010000Y010000\nM30\n");
  REQUIRE(area(board) == Catch::Approx(before - 3.141592653589793).margin(.03));
  REQUIRE(board.copper.front().holes.size() == 1);
}
TEST_CASE("PCB placement uses physical LCD axes before portrait rotation",
          "[sla_pcb]") {
  const bool portrait = GENERATE(false, true);
  auto config = printer_config();
  config.option("display_orientation")
      ->setInt(portrait ? sladoPortrait : sladoLandscape);
  SLAPrinterConfig printer;
  printer.apply(config, true);
  auto board = sla::parse_gerber(prefix + "%ADD10R,10X8*%D10*X0Y0D03*M02*");
  auto document = sla::rasterize_pcb(board, printer);
  auto image = document.image(0);
  REQUIRE(image.cols == size_t(portrait ? 150 : 400));
  REQUIRE(image.rows == size_t(portrait ? 400 : 150));
  REQUIRE(std::count(image.buf.begin(), image.buf.end(), uint8_t(255)) == 4000);
  sla::PCBPlacement placement;
  placement.offset_x = 30.;
  REQUIRE_THROWS(sla::rasterize_pcb(board, printer, placement));
  printer.display_width.value = 5.;
  REQUIRE_THROWS(sla::rasterize_pcb(board, printer));
}
TEST_CASE("Multiple PCB files retain common physical registration", "[sla_pcb]") {
  const bool portrait = GENERATE(false,true);
  const bool mirror = GENERATE(false,true);
  auto config = printer_config();
  config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
  config.set("display_mirror_x",mirror);
  SLAPrinterConfig printer;printer.apply(config,true);
  const auto left = sla::parse_gerber(prefix+"%ADD10R,4X2*%D10*X50000Y50000D03*M02*");
  const auto right = sla::parse_gerber(prefix+"%ADD10R,4X2*%D10*X150000Y90000D03*M02*");
  sla::PCBPlacement placement;placement.mirror_x=mirror;
  auto separate = sla::rasterize_pcb_layers({left,right},printer,false,placement);
  auto merged = sla::rasterize_pcb_layers({left,right},printer,true,placement);
  REQUIRE(separate.layer_count()==2);REQUIRE(merged.layer_count()==1);
  const auto first=separate.image(0),second=separate.image(1),together=merged.image(0);
  REQUIRE(first.cols==size_t(portrait?150:400));
  REQUIRE(first.rows==size_t(portrait?400:150));
  REQUIRE(std::count(first.buf.begin(),first.buf.end(),uint8_t(255))==400);
  REQUIRE(std::count(second.buf.begin(),second.buf.end(),uint8_t(255))==400);
  size_t overlap=0;
  bool equal_union=true;
  for(size_t i=0;i<together.buf.size();++i) {
    overlap+=first.buf[i]!=0 && second.buf[i]!=0;
    equal_union &= together.buf[i]==std::max(first.buf[i],second.buf[i]);
  }
  REQUIRE(overlap==0);REQUIRE(equal_union);
  // Independent centering would place both rectangles at the same location.
  auto independent=sla::rasterize_pcb(left,printer,placement);
  REQUIRE(independent.image(0).buf!=first.buf);
  sla::PCBPlacement origin;origin.center=false;origin.margin=0.;
  auto at_origin=sla::rasterize_pcb_layers({left,right},printer,false,origin);
  REQUIRE(at_origin.image(0).buf!=first.buf);
  REQUIRE_THROWS(sla::rasterize_pcb_layers({},printer,false));
  REQUIRE_THROWS(sla::rasterize_pcb_layers({left,{}},printer,true));
  placement.offset_x=100.;
  REQUIRE_THROWS(sla::rasterize_pcb_layers({left,right},printer,false,placement));
  bool cancelled=false;
  REQUIRE_THROWS(sla::rasterize_pcb_layers({left,right},printer,true,{},[&]{cancelled=true;throw RuntimeError("cancelled");}));
  REQUIRE(cancelled);
}
TEST_CASE("Native calibration jobs have dimensions grayscale and exact Z",
          "[sla_calibration]") {
  auto config = printer_config();
  sla::CalibrationParameters parameters;
  parameters.size_x = 5.;
  parameters.size_y = 4.;
  parameters.height = .2;
  parameters.margin = 1.;
  auto job = sla::create_calibration(config, parameters);
  REQUIRE(job->finished());
  REQUIRE(job->objects().empty());
  REQUIRE(job->layer_document()->layer_count() == 4);
  const auto blocks = job->layer_document()->image(0);
  REQUIRE(std::count(blocks.buf.begin(), blocks.buf.end(), uint8_t(255)) ==
          9000);
  for (size_t i = 1; i < 4; ++i)
    REQUIRE(job->layer_document()->image(i).buf == blocks.buf);
  parameters.pattern = sla::CalibrationPattern::GrayscalePatches;
  parameters.grayscale_steps = 4;
  parameters.grayscale_first = 0;
  parameters.grayscale_last = 255;
  job = sla::create_calibration(config, parameters);
  auto image = job->layer_document()->image(0);
  // Unequal pixel pitches put some horizontal edges at half pixels. Check
  // patch interiors; edge antialiasing can equal another patch's gray value.
  REQUIRE(image.get(87, 170) == 0);
  REQUIRE(image.get(87, 230) == 85);
  REQUIRE(image.get(62, 170) == 170);
  REQUIRE(image.get(62, 230) == 255);
  parameters.pattern = sla::CalibrationPattern::LiftArea;
  job = sla::create_calibration(config, parameters);
  image = job->layer_document()->image(0);
  REQUIRE(std::count(image.buf.begin(), image.buf.end(), uint8_t(255)) == 1000);
  parameters.height = .21;
  REQUIRE_THROWS(sla::create_calibration(config, parameters));
  parameters.height = .2;
  parameters.size_x = 50.;
  REQUIRE_THROWS(sla::create_calibration(config, parameters));
}

TEST_CASE(
    "PCB cancellation and drill zero conventions preserve physical locations",
    "[sla_pcb]") {
  const auto source = prefix + "%ADD10R,10X8*%D10*X100000Y100000D03*M02*";
  unsigned calls = 0;
  REQUIRE_THROWS(sla::parse_gerber(source, .005, [&] {
    if (++calls == 4)
      throw std::runtime_error("cancel");
  }));
  auto board = sla::parse_gerber(source);
  const auto before = area(board);
  REQUIRE_THROWS(sla::subtract_excellon(
      board, "M48\nMETRIC,LZ\nT1C2\n%\nT1\nX010000Y010000\nM30\n", .005,
      [] { throw std::runtime_error("cancel"); }));
  REQUIRE(area(board) == before);
  for (const std::string mode : {"LZ", "TZ"}) {
    auto copy = board;
    const std::string xy = mode == "LZ" ? "X010Y010" : "X10000Y10000";
    sla::subtract_excellon(copy, "M48\n;FILE_FORMAT=3:3\nMETRIC," + mode +
                                     "\nT1C2\n%\nT1\n" + xy + "\nM30\n");
    REQUIRE(area(copy) ==
            Catch::Approx(before - 3.141592653589793).margin(.03));
  }
  REQUIRE_THROWS(sla::parse_gerber(prefix + "%ADD10C,1*%D10.1*X0Y0D03*M02*"));
}
TEST_CASE("PCB manual mirroring and printer transforms act once", "[sla_pcb]") {
  const bool portrait = GENERATE(false, true);
  const bool mirror_x = GENERATE(false, true), mirror_y = GENERATE(false, true);
  auto config = printer_config();
  config.option("display_orientation")
      ->setInt(portrait ? sladoPortrait : sladoLandscape);
  config.set("display_mirror_x", mirror_x);
  config.set("display_mirror_y", mirror_y);
  SLAPrinterConfig printer;
  printer.apply(config, true);
  auto board = sla::parse_gerber(
      prefix + "%ADD10R,10X8*%%ADD11R,2X2*%D10*X100000Y100000D03*%LPC*%D11*"
               "X120000Y110000D03*M02*");
  sla::PCBPlacement placement;
  for (bool manual : {false, true}) {
    placement.mirror_x = manual;
    const auto mask = sla::rasterize_pcb(board, printer, placement).image(0);
    double x = (manual ? 18. : 22.) * 10., y = 16. * 5.;
    if (portrait) {
      std::swap(x, y);
      if (!mirror_x)
        x = mask.cols - x;
      if (!mirror_y)
        y = mask.rows - y;
    } else {
      if (mirror_x)
        x = mask.cols - x;
      if (!mirror_y)
        y = mask.rows - y;
    }
    REQUIRE(mask.get(size_t(y), size_t(x)) == 0);
    REQUIRE(mask.get(mask.rows / 2, mask.cols / 2) == 255);
  }
}
TEST_CASE("Gerber aperture macros evaluate parameters rotations and local clear shapes", "[sla_pcb]") {
    SECTION("circle and assigned inner diameter") {
        auto board = sla::parse_gerber(prefix + "%AMRING*0 A local ring*$3=$1/2*1,1,$1,0,0*1,0,$3,0,0*%%ADD10RING,4X1*%D10*X100000Y100000D03*M02*");
        REQUIRE(area(board) == Catch::Approx(3. * 3.141592653589793).margin(.05));
        REQUIRE(board.copper.front().holes.size() == 1);
    }
    SECTION("center rectangle expression and rotation") {
        auto board = sla::parse_gerber(prefix + "%AMRECT*21,1,($1+2)x2,$2,0,0,90*%%ADD10RECT,2X2*%D10*X0Y0D03*M02*");
        const auto bounds = get_extents(board.copper);
        REQUIRE(unscaled(bounds.size().x()) == Catch::Approx(2.).margin(1e-6));
        REQUIRE(unscaled(bounds.size().y()) == Catch::Approx(8.).margin(1e-6));
        REQUIRE(area(board) == Catch::Approx(16.).margin(.001));
    }
    SECTION("vector line uses square ends") {
        auto board = sla::parse_gerber(prefix + "%AMVECTOR*20,1,2,0,0,10,0,0*%%ADD10VECTOR*%D10*X0Y0D03*M02*");
        REQUIRE(area(board) == Catch::Approx(20.).margin(.001));
    }
    SECTION("closed outline and regular polygon") {
        auto triangle = sla::parse_gerber(prefix + "%AMTRI*4,1,3,0,0,4,0,0,4,0,0,0*%%ADD10TRI*%D10*X0Y0D03*M02*");
        REQUIRE(area(triangle) == Catch::Approx(8.).margin(.001));
        auto square = sla::parse_gerber(prefix + "%AMPOLY*5,1,4,0,0,4,45*%%ADD10POLY*%D10*X0Y0D03*M02*");
        REQUIRE(area(square) == Catch::Approx(8.).margin(.001));
    }
    SECTION("macro hole does not erase copper from an earlier flash") {
        auto board = sla::parse_gerber(prefix + "%AMRING*1,1,4,0,0*1,0,2,0,0*%%ADD10C,1*%%ADD11RING*%D10*X0Y0D03*D11*X0Y0D03*M02*");
        REQUIRE(area(board) == Catch::Approx(3.25 * 3.141592653589793).margin(.05));
        REQUIRE(board.copper.size() == 2);
    }
    SECTION("unsupported or malformed expressions fail") {
        REQUIRE_THROWS(sla::parse_gerber(prefix + "%AMBAD*1,1,1/0,0,0*%%ADD10BAD*%D10*X0Y0D03*M02*"));
        REQUIRE_THROWS(sla::parse_gerber(prefix + "%AMBAD*6,1,2,3,4*%%ADD10BAD*%D10*X0Y0D03*M02*"));
        REQUIRE_THROWS(sla::parse_gerber(prefix + "%AMBAD*4,1,3,0,0,4,0,0,4,1,1,0*%%ADD10BAD*%D10*X0Y0D03*M02*"));
        REQUIRE_THROWS(sla::parse_gerber(prefix + "%AMCIRCLE*1,1,2,0,0*%%ADD10CIRCLE*%D10*X0Y0D02*X10000Y10000D01*M02*"));
    }
}

TEST_CASE("Zero-dimension macro primitives are transparent", "[sla_pcb]") {
    auto board = sla::parse_gerber(prefix + "%AMOPTIONAL*1,1,4,0,0*1,0,$1,0,0*21,1,0,4,0,0,0*%%ADD10OPTIONAL,0*%D10*X0Y0D03*M02*");
    REQUIRE(area(board) == Catch::Approx(4. * 3.141592653589793).margin(.05));
    REQUIRE(board.copper.front().holes.empty());
}

TEST_CASE("Moire and thermal Gerber macros keep rings gaps rotation and local polarity", "[sla_pcb][sla_pcb_thermal]") {
    SECTION("moire concentric rings") {
        auto board = sla::parse_gerber(prefix + "%AMTARGET*6,0,0,10,1,1,2,0,0,0*%%ADD10TARGET*%D10*X0Y0D03*M02*");
        REQUIRE(board.copper.size() == 2);
        REQUIRE(area(board) == Catch::Approx(14. * 3.141592653589793).margin(.15));
        for (const auto &ring : board.copper) REQUIRE(ring.holes.size() == 1);
    }
    SECTION("moire crosshair and rotated offset centre") {
        auto board = sla::parse_gerber(prefix + "%AMTARGET*6,2,3,10,1,1,0,1,12,90*%%ADD10TARGET*%D10*X0Y0D03*M02*");
        const auto bounds = get_extents(board.copper);
        REQUIRE(area(board) == Catch::Approx(23.).margin(.001));
        REQUIRE(unscaled(bounds.center().x()) == Catch::Approx(-3.).margin(1e-6));
        REQUIRE(unscaled(bounds.center().y()) == Catch::Approx(2.).margin(1e-6));
    }
    SECTION("thermal has four disconnected quadrants") {
        auto board = sla::parse_gerber(prefix + "%AMTHERMAL*7,0,0,10,6,1,0*%%ADD10THERMAL*%D10*X0Y0D03*M02*");
        REQUIRE(board.copper.size() == 4);
        REQUIRE(area(board) < 16. * 3.141592653589793);
        REQUIRE(area(board) > 40.);
        auto mask = sla::rasterize_pcb(board, [&] { SLAPrinterConfig p; p.apply(printer_config(), true); return p; }()).image(0);
        REQUIRE(mask.get(mask.rows/2, mask.cols/2) == 0);
        REQUIRE(mask.get(mask.rows/2, mask.cols/2+40) == 0);
    }
    SECTION("local thermal hole does not erase previous copper") {
        auto board = sla::parse_gerber(prefix + "%AMTHERMAL*1,1,2,0,0*7,0,0,10,6,1,45*%%ADD10THERMAL*%D10*X0Y0D03*M02*");
        REQUIRE(board.copper.size() == 5);
        REQUIRE(area(board) > 43.);
    }
    SECTION("invalid parameters are rejected") {
        for (const auto &primitive : {"7,0,0,10,12,1,0", "7,0,0,10,6,-1,0", "6,0,0,10,0,1,2,0,0,0",
             "6,0,0,10,1,1,2.5,0,0,0", "6,0,0,10,1,1,100000,0,0,0"})
            REQUIRE_THROWS(sla::parse_gerber(prefix + "%AMBAD*" + primitive + "*%%ADD10BAD*%D10*X0Y0D03*M02*"));
    }
}

TEST_CASE("Excellon G85 slots use the tool diameter and preserve artwork on errors", "[sla_pcb][sla_pcb_slots]") {
    auto board = sla::parse_gerber(prefix + "%ADD10R,20X10*%D10*X100000Y50000D03*M02*");
    const std::string header = "M48\nMETRIC,TZ\nT1C2\n%\nT1\n";
    SECTION("inline slot endpoints") {
        sla::subtract_excellon(board, header + "X2.0Y5.0G85X8.0Y5.0\nM30\n", .001);
        REQUIRE(area(board) == Catch::Approx(200. - 12. - 3.141592653589793).margin(.02));
        REQUIRE(board.copper.front().holes.size() == 1);
    }
    SECTION("modal start and diagonal endpoint") {
        // The initial drill hit is entirely inside the subsequent slot.
        sla::subtract_excellon(board, header + "X3.0Y3.0\nG85X7.0Y6.0\nM30\n", .001);
        REQUIRE(area(board) == Catch::Approx(200. - 10. - 3.141592653589793).margin(.02));
    }
    SECTION("zero length slot is a drill hit") {
        sla::subtract_excellon(board, header + "X5.0Y5.0G85X5.0Y5.0\nM30\n", .001);
        REQUIRE(area(board) == Catch::Approx(200. - 3.141592653589793).margin(.02));
    }
    SECTION("malformed slots never partially subtract") {
        for (const auto &command : {"X2Y5G85", "X2Y5G85X8G85Y6", "X2Y5G85X8G01Y6"}) {
            REQUIRE_THROWS(sla::subtract_excellon(board, header + "X3.0Y3.0\n" + command + "\nM30\n"));
            REQUIRE(area(board) == Catch::Approx(200.));
        }
    }
}

TEST_CASE("Native calibration rejects an unknown archive ID", "[sla_calibration]") {
    auto config = printer_config(); config.set("sla_archive_format",std::string("missing-format"));
    sla::CalibrationParameters parameters;parameters.size_x=5.;parameters.size_y=4.;
    REQUIRE_THROWS(sla::create_calibration(config,parameters));
}

TEST_CASE("Benchmark repeated native calibration masks", "[.sla_calibration_benchmark]") {
    using Clock = std::chrono::steady_clock;
    auto config = printer_config();
    config.set("display_width",192.); config.set("display_height",108.);
    config.set("display_pixels_x",3840); config.set("display_pixels_y",2160);
    sla::CalibrationParameters parameters;
    parameters.size_x=5.;parameters.size_y=4.;parameters.height=50.;parameters.margin=2.;
    const auto start=Clock::now(); auto job=sla::create_calibration(config,parameters);const auto generated=Clock::now();
    REQUIRE(job->layer_document()->layer_count()==1000);
    REQUIRE(job->print_statistics().objects_used_material==Catch::Approx(9000.).margin(.001));
    const char *requested=std::getenv("SLA_CALIBRATION_BENCHMARK_PATH");
    const auto path=requested?boost::filesystem::path(requested):boost::filesystem::absolute(boost::filesystem::unique_path("calibration-benchmark-%%%%.uvj"));
    job->export_layer_document(*job->layer_document(),"uvj",path.string(),{},"calibration-benchmark");
    const auto exported=Clock::now();const auto bytes=boost::filesystem::file_size(path);
    REQUIRE(bytes>0);
    std::cout << "CALIBRATION_BENCHMARK {\"width\":3840,\"height\":2160,\"layers\":1000,\"generation_ms\":"
        <<std::chrono::duration<double,std::milli>(generated-start).count()<<",\"export_ms\":"
        <<std::chrono::duration<double,std::milli>(exported-generated).count()<<",\"archive_bytes\":"<<bytes<<"}\n";
    if(!requested) boost::filesystem::remove(path);
}

TEST_CASE("Raster jobs canonicalize the case of known format IDs", "[sla_calibration]") {
    auto config=printer_config();config.set("sla_archive_format",std::string("UVJ"));
    sla::CalibrationParameters parameters;parameters.size_x=5.;parameters.size_y=4.;parameters.height=.1;
    auto job=sla::create_calibration(config,parameters);
    REQUIRE(job->printer_config().sla_archive_format.value=="uvj");
}
