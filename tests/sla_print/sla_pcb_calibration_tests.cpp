#include "sla_archive_test_config.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/SLA/CalibrationRaster.hpp"
#include "libslic3r/SLA/PCBExposure.hpp"
#include "libslic3r/SLA/ProjectedPNGMaskEncoder.hpp"
#include "libslic3r/SLA/ProjectArchive.hpp"
#include "libslic3r/SLA/GeneratedLayerOwnership.hpp"
#include "libslic3r/SLA/ExportRequest.hpp"
#include "libslic3r/SLA/SlicedResultSession.hpp"
#include "libslic3r/SLA/ExportFormatPreference.hpp"
#include "libslic3r/SLA/ResinBottle.hpp"
#include "libslic3r/Zipper.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include "libslic3r/Format/AnycubicSLA.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <fstream>
#include <limits>
#include <cfenv>
#include <cstdlib>
#include <cstring>
#include <boost/filesystem.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
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

TEST_CASE("Stationary PCB projects retain local exposure, registration and oriented masks", "[sla_pcb][sla_pcb_project]") {
  const bool portrait=GENERATE(false,true), merge=GENERATE(false,true), invert=GENERATE(false,true);
  auto config=printer_config();config.set("sla_archive_format","uvj");
  config.set("display_width",100.);config.set("display_height",60.);
  config.set("display_pixels_x",80);config.set("display_pixels_y",40);
  config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
  const auto original_exposure=config.opt_serialize("exposure_time");
  const auto original_lift=config.opt_serialize("sla_lift_height");
  const std::vector<sla::PCBArtwork> boards{
    sla::parse_gerber(prefix+"%ADD10R,10X8*%D10*X100000Y100000D03*M02*"),
    sla::parse_gerber(prefix+"%ADD10R,4X2*%D10*X220000Y100000D03*M02*")};
  std::unique_ptr<SLAPrint> job;
  REQUIRE_NOTHROW(job=sla::create_pcb_exposure_job(config,boards,merge,{},30.,invert));
  REQUIRE(job->is_raster_job());REQUIRE(job->finished());
  REQUIRE(job->layer_document()->layer_count()==(merge?1:2));
  CHECK(job->material_config().exposure_time.value==30.);
  CHECK(job->material_config().initial_exposure_time.value==30.);
  CHECK(job->material_config().sla_lift_height.value==0.);
  CHECK(job->material_config().sla_bottom_lift_height.value==0.);
  CHECK(job->material_config().sla_wait_before_cure.value==0.);
  CHECK(job->material_config().sla_light_pwm.value==255);
  CHECK(config.opt_serialize("exposure_time")==original_exposure);
  CHECK(config.opt_serialize("sla_lift_height")==original_lift);
  SLAPrinterConfig printer;printer.apply(config,true);printer.gamma_correction.value=1.;
  auto expected=sla::rasterize_pcb_layers(boards,printer,merge);
  if(invert)expected.transform_layers(0,expected.layer_count()-1,sla::LayerDocument::MaskOperation::Invert);
  for(size_t i=0;i<expected.layer_count();++i){
    const auto image=job->layer_document()->image(i);
    CHECK(image.cols==(portrait?40:80));CHECK(image.rows==(portrait?80:40));
    CHECK(image.buf==expected.image(i).buf);
  }
  auto snapshot=sla::capture_generated_project(*job,*job->layer_document());
  REQUIRE(snapshot.settings.has_value());CHECK(snapshot.settings->material.values.at("exposure_time")=="30");
  const auto restored=sla::restore_generated_project(snapshot);
  CHECK(restored->material_config().sla_lift_height.value==0.);
  for(size_t i=0;i<expected.layer_count();++i)CHECK(restored->layer_document()->image(i).buf==expected.image(i).buf);
  CHECK_THROWS(sla::create_pcb_exposure_job(config,boards,merge,{},0.));
  CHECK_THROWS(sla::create_pcb_exposure_job(config,boards,merge,{},std::numeric_limits<double>::infinity()));
  CHECK_THROWS(sla::create_pcb_exposure_job(config,boards,merge,{},86401.));
  CHECK_THROWS(sla::create_pcb_exposure_job(config,boards,merge,{},30.,false,[]{throw RuntimeError("test cancellation");}));
}

// Actual producer entrypoints: ownership must describe their final overrides,
// rather than a hand-created descriptor that bypasses the production path.
TEST_CASE("Calibration producer owns final rows and exact source positions",
          "[sla_generated_owned]") {
  using F = sla::LayerParameter;
  const auto format = GENERATE("SL1", "SL1S", "cxdlpv4", "ctb_v4", "ctb_v5",
                               "ctb_encrypted_v4", "ctb_encrypted");
  const bool portrait = GENERATE(false, true);
  auto config = printer_config(); config.set("sla_archive_format", format);
  config.set("printer_model", "GENERIC");
  config.option("display_orientation")->setInt(portrait ? sladoPortrait : sladoLandscape);
  config.set("sla_bottom_layers", 10); config.set("sla_transition_layers", 20);
  config.set("exposure_time", 2.12); config.set("initial_exposure_time", 5.12);
  for (const auto prefix : {"sla_", "sla_bottom_"})
    for (const auto key : {"lift_height", "lift_height2", "retract_height2",
                          "wait_before_cure", "wait_after_cure", "wait_after_lift"})
      config.set(std::string(prefix)+key, 0.);
  const auto original = config;
  sla::CalibrationParameters parameters;
  parameters.size_x=2.; parameters.size_y=2.; parameters.margin=1.; parameters.height=.15;
  auto job = sla::create_calibration(config, parameters);
  CHECK(config.diff(original).empty());
  const auto &document = *job->layer_document();
  REQUIRE(document.fresh_job().has_value());
  const auto &fresh = *document.fresh_job();
  CHECK(fresh.producer == sla::FreshLayerProducer::Calibration);
  CHECK(fresh.globals.bottom_layers == 3); CHECK(fresh.globals.transition_layers == 0);
  CHECK(fresh.globals.layer_height == .05);
  CHECK(fresh.geometry.columns == size_t(portrait ? 150 : 400));
  CHECK(fresh.geometry.rows == size_t(portrait ? 400 : 150));
  CHECK(fresh.geometry.width_mm == (portrait ? 30. : 40.));
  CHECK(fresh.geometry.height_mm == (portrait ? 40. : 30.));
  REQUIRE(fresh.rows.size() == 3);
  for (size_t i=0; i<3; ++i) {
    CHECK(fresh.rows[i].at(F::PositionZ) == (i+1)*.05);
    CHECK(fresh.rows[i].at(F::Exposure) == 5.12);
    CHECK(fresh.rows[i].size() == 14);
    CHECK(document.image(i).buf == document.image(0).buf);
  }
  CHECK(job->print_statistics().time_basis == SLAPrintStatistics::TimeBasis::OwnedNeutralSchedule);
  CHECK(job->print_statistics().estimated_print_time == 3*5.12);
  const auto snapshot=sla::capture_generated_project(*job,document);
  CHECK(snapshot.positions_z == std::vector<double>{.05,2*.05,3*.05});
  const auto restored=sla::restore_generated_project(snapshot);
  REQUIRE(restored->layer_document()->fresh_job().has_value());
  CHECK(restored->layer_document()->fresh_job()->rows == fresh.rows);
  CHECK(restored->print_statistics().time_basis == SLAPrintStatistics::TimeBasis::OwnedNeutralSchedule);
}

TEST_CASE("PCB producer owns its stationary final override without changing presets",
          "[sla_generated_owned]") {
  using F=sla::LayerParameter;
  const auto format=GENERATE("SL1", "SL1S", "cxdlpv4", "ctb_v4", "ctb_v5",
                             "ctb_encrypted_v4", "ctb_encrypted");
  const bool portrait=GENERATE(false,true), merge=GENERATE(false,true), invert=GENERATE(false,true);
  auto config=printer_config(); config.set("sla_archive_format",format); config.set("printer_model","GENERIC");
  config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
  config.set("sla_bottom_layers",10);config.set("sla_transition_layers",20);
  config.set("sla_lift_height",7.25);config.set("sla_wait_before_cure",4.75);
  const auto original=config;
  const auto left=sla::parse_gerber(prefix+"%ADD10R,4X2*%D10*X50000Y50000D03*M02*");
  const auto right=sla::parse_gerber(prefix+"%ADD10R,4X2*%D10*X150000Y90000D03*M02*");
  auto job=sla::create_pcb_exposure_job(config,{left,right},merge,{},30.12,invert);
  CHECK(config.diff(original).empty());
  const auto &document=*job->layer_document(); REQUIRE(document.fresh_job().has_value());
  const auto &fresh=*document.fresh_job();
  CHECK(fresh.producer==sla::FreshLayerProducer::PCB);
  CHECK(fresh.globals.bottom_layers==0);CHECK(fresh.globals.transition_layers==0);
  REQUIRE(fresh.rows.size()==size_t(merge?1:2));
  SLAPrinterConfig printer;printer.apply(config,true);printer.gamma_correction.value=1.;
  auto expected=sla::rasterize_pcb_layers({left,right},printer,merge);
  if(invert) expected.transform_layers(0,expected.layer_count()-1,sla::LayerDocument::MaskOperation::Invert);
  for(size_t i=0;i<fresh.rows.size();++i) {
    const auto &row=fresh.rows[i]; CHECK(row.at(F::PositionZ)==(i+1)*.05);
    CHECK(row.at(F::Exposure)==30.12); CHECK(row.at(F::LightPWM)==255.);
    for(const auto field:{F::LiftDistance,F::LiftDistance2,F::RetractDistance2,
                          F::WaitBefore,F::WaitAfter,F::WaitAfterLift,F::LightOff}) CHECK(row.at(field)==0.);
    CHECK(document.image(i).buf==expected.image(i).buf);
  }
  CHECK(job->print_statistics().time_basis==SLAPrintStatistics::TimeBasis::OwnedNeutralSchedule);
  CHECK(job->print_statistics().estimated_print_time==30.12*fresh.rows.size());
  const auto snapshot=sla::capture_generated_project(*job,document);
  const auto restored=sla::restore_generated_project(snapshot);
  REQUIRE(restored->layer_document()->fresh_job().has_value());
  CHECK(restored->layer_document()->fresh_job()->rows==fresh.rows);
  CHECK(restored->layer_document()->fresh_job()->producer==sla::FreshLayerProducer::PCB);
  for(size_t i=0;i<fresh.rows.size();++i) CHECK(restored->layer_document()->image(i).buf==document.image(i).buf);
}

TEST_CASE("Explicit raster production preserves source doubles and rejects ownership replacement",
          "[sla_generated_owned_guards]") {
  using F=sla::LayerParameter;
  auto config=printer_config();config.set("sla_archive_format","SL1");
  config.set("sla_bottom_layers",0);config.set("sla_transition_layers",0);
  std::vector<uint8_t> pixels(400*150,0);pixels[10]=255;
  auto masks=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(pixels.data(),400,150,1)});
  const std::vector<double> positions{.0500004};
  auto unowned=SLAPrint::create_raster_job(config,masks,positions);
  CHECK_FALSE(unowned->layer_document()->fresh_job());
  auto owned=SLAPrint::create_raster_job(config,masks,positions,sla::FreshLayerProducer::Calibration);
  REQUIRE(owned->layer_document()->fresh_job());
  CHECK(owned->layer_document()->fresh_job()->rows[0].at(F::PositionZ)==positions[0]);
  CHECK(owned->layer_document()->revision()==0);
  CHECK(owned->layer_document()->parameter_capabilities().size()==14);
  CHECK(sla::capture_generated_project(*owned,*owned->layer_document()).positions_z==positions);
  CHECK_THROWS(SLAPrint::create_raster_job(config,*owned->layer_document(),positions,sla::FreshLayerProducer::PCB));
  CHECK_THROWS(SLAPrint::create_raster_job(config,masks,positions,static_cast<sla::FreshLayerProducer>(99)));
  auto still_owned=SLAPrint::create_raster_job(config,*owned->layer_document(),positions);
  CHECK(still_owned->layer_document()->fresh_job()->rows==owned->layer_document()->fresh_job()->rows);
  for(const auto format:{"uvj","UVJ","genericzip"}) {
    config.set("sla_archive_format",format);
    sla::CalibrationParameters parameters;parameters.size_x=2.;parameters.size_y=2.;parameters.margin=1.;parameters.height=.1;
    auto generated=sla::create_calibration(config,parameters);
    if(std::string(format)=="genericzip")CHECK_FALSE(generated->layer_document()->fresh_job());
    else {
      REQUIRE(generated->layer_document()->fresh_job());
      CHECK(generated->layer_document()->fresh_job()->producer==sla::FreshLayerProducer::Calibration);
    }
  }
  config.set("sla_archive_format","sl1");
  sla::CalibrationParameters parameters;parameters.size_x=2.;parameters.size_y=2.;parameters.margin=1.;parameters.height=.1;
  CHECK(sla::create_calibration(config,parameters)->layer_document()->fresh_job().has_value());
}

TEST_CASE("Actual PCB and calibration producers export through both preserving routes after restart",
          "[sla_generated_owned_export]") {
  const auto format=GENERATE("SL1","SL1S","cxdlpv4","ctb_v4","ctb_v5","ctb_encrypted_v4","ctb_encrypted");
  const bool portrait=GENERATE(false,true),pcb=GENERATE(false,true);
  CAPTURE(format,portrait,pcb);
  auto config=printer_config();config.set("sla_archive_format",format);config.set("printer_model","GENERIC");
  config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
  config.set("exposure_time",2.12);config.set("initial_exposure_time",5.12);
  config.set("sla_bottom_layers",10);config.set("sla_transition_layers",20);
  config.set("material_density",1.5);config.set("bottle_cost",100.);config.set("bottle_volume",500.);
  for(const auto prefix:{"sla_","sla_bottom_"}) {
    for(const auto key:{"lift_height","lift_height2","retract_height2","wait_before_cure","wait_after_cure","wait_after_lift",
                        "lift_speed","lift_speed2","retract_speed","retract_speed2"})
      config.set(std::string(prefix)+key,0.);
    config.set(std::string(prefix)+"light_pwm",255);
  }
  std::unique_ptr<SLAPrint> job;
  if(pcb) {
    // The producer must override inactive speeds too: bottom0 has no row
    // from which the CX reference can reconstruct virtual bottom defaults.
    config.set("sla_bottom_retract_speed",120.);config.set("sla_lift_speed",180.);
    const auto left=sla::parse_gerber(prefix+"%ADD10R,4X2*%D10*X50000Y50000D03*M02*");
    const auto right=sla::parse_gerber(prefix+"%ADD10R,4X2*%D10*X150000Y90000D03*M02*");
    job=sla::create_pcb_exposure_job(config,{left,right},false,{},30.12);
  } else {
    // CX reference virtual bottom uses fallback100/80 for zero speeds.
    // Select representable source intent explicitly, rather than rewriting it.
    for(const auto prefix:{"sla_","sla_bottom_"}) {
      config.set(std::string(prefix)+"retract_speed",100.);
      config.set(std::string(prefix)+"retract_speed2",80.);
    }
    sla::CalibrationParameters parameters;parameters.size_x=2.;parameters.size_y=2.;parameters.margin=1.;parameters.height=.15;
    job=sla::create_calibration(config,parameters);
  }
  const auto &document=*job->layer_document();REQUIRE(document.fresh_job());
  for(size_t i=0;i<document.layer_count();++i) {
    const auto image=document.image(i);
    CHECK(std::count(image.buf.begin(),image.buf.end(),uint8_t(255))==(pcb?400:1800));
  }
  const auto captured=sla::capture_generated_project(*job,document);
  const char *requested=std::getenv("SLA_GENERATED_OWNED_FIXTURES");
  const auto base=requested?boost::filesystem::path(requested):boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("generated-owned-%%%%");
  const auto folder=base/(std::string(format)+"-"+(portrait?"portrait":"landscape")+"-"+(pcb?"pcb":"calibration"));
  boost::filesystem::create_directories(folder);
  const auto project=folder/"fresh.sla-project";
  sla::save_layer_project(project.string(),captured);
  const auto read=sla::read_project_layers(project.string());REQUIRE(read);
  auto restored=sla::restore_generated_project(*read);
  REQUIRE(restored->layer_document()->fresh_job());
  CHECK(restored->layer_document()->fresh_job()->rows==document.fresh_job()->rows);
  const auto extension=std::string(format)=="SL1"?"sl1":std::string(format)=="SL1S"?"sl1s":std::string(format)=="cxdlpv4"?"cxdlpv4":"ctb";
  for(bool restart:{false,true}) {
    const auto &source=restart?*restored:*job;
    for(bool direct:{false,true}) {
      const auto path=folder/((std::string(restart?"restart":"original")+"-"+(direct?"print":"document"))+"."+extension);
      if(direct) {
        // export_print mutates only its encoding cache; source intent stays owned.
        auto &mutable_source=restart?*restored:*job;
        REQUIRE_NOTHROW(mutable_source.export_print(path.string(),"generated-owned"));
      } else REQUIRE_NOTHROW(source.export_layer_document(*source.layer_document(),format,path.string(),{},"generated-owned"));
      CHECK(boost::filesystem::file_size(path)>0);
      CHECK(source.layer_document()->fresh_job()->rows==document.fresh_job()->rows);
    }
  }
  if(!requested) boost::filesystem::remove_all(base);
}

TEST_CASE("Lift calibration tapers its final masks and preserves the generation recipe", "[sla_calibration_taper]") {
  auto config=printer_config();config.set("sla_bottom_layers",3);
  const auto format=GENERATE("uvj","pwc");config.set("sla_archive_format",format);
  const bool portrait=GENERATE(false,true);config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
  sla::CalibrationParameters p;p.pattern=sla::CalibrationPattern::LiftArea;
  p.size_x=20;p.size_y=10;p.height=1;p.lift_taper_step=10;p.lift_taper_min=10;
  auto job=sla::create_calibration(config,p);REQUIRE(job->layer_document()->layer_count()==20);
  auto area_at=[&](size_t index){const auto image=job->layer_document()->image(index);return std::count_if(image.buf.begin(),image.buf.end(),[](auto v){return v!=0;});};
  const auto base=area_at(0);REQUIRE(base>0);
  for(size_t i=1;i<11;++i)CHECK(area_at(i)==base);
  for(size_t i=11;i<20;++i){CHECK(area_at(i)>0);CHECK(area_at(i)<area_at(i-1));}
  const auto path=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("lift-taper-%%%%.sla-project");
  sla::save_layer_project(path.string(),sla::capture_generated_project(*job,*job->layer_document()));
  const auto restored=sla::read_project_layers(path.string());REQUIRE(restored);REQUIRE(restored->calibration_recipe);
  CHECK(restored->calibration_recipe->parameters.lift_taper_step==10);
  CHECK(restored->calibration_recipe->parameters.lift_taper_min==10);
  auto reopened=sla::restore_generated_project(*restored);CHECK(reopened->layer_document()->image(19).buf==job->layer_document()->image(19).buf);
  boost::filesystem::remove(path);
  if (const char *fixtures=std::getenv("SLA_LIFT_TAPER_FIXTURES")) {
    const auto folder=boost::filesystem::path(fixtures);boost::filesystem::create_directories(folder);
    const auto name=std::string(format)+(portrait?"-portrait":"-landscape");
    sla::save_layer_project((folder/(name+".sla-project")).string(),sla::capture_generated_project(*job,*job->layer_document()));
    job->export_print((folder/(name+"."+format)).string(),"lift-taper");
  }
  p.height=.2;CHECK_THROWS_AS(sla::create_calibration(config,p),RuntimeError);
}
TEST_CASE("Lift size planning rejects invalid ranges and protects bottom layers", "[sla_calibration_taper]") {
  auto config=printer_config();config.set("sla_bottom_layers",3);
  sla::CalibrationParameters p;p.pattern=sla::CalibrationPattern::LiftArea;
  p.lift_taper_step=10;p.lift_taper_min=10;
  auto plan=sla::calibration_lift_plan(config,p);
  CHECK(plan.base_layers==11);
  CHECK(plan.factors==std::vector<unsigned>{90,80,70,60,50,40,30,20,10});
  p.height=.6;CHECK(sla::calibration_lift_plan(config,p).base_layers==3);
  p.height=.55;CHECK_THROWS_AS(sla::calibration_lift_plan(config,p),RuntimeError);
  p.height=1;p.lift_taper_step=30;p.lift_taper_min=20;
  CHECK(sla::calibration_lift_plan(config,p).factors==std::vector<unsigned>{70,40});
  p.lift_taper_min=80;CHECK_THROWS_AS(sla::calibration_lift_plan(config,p),RuntimeError);
  p.lift_taper_step=100;CHECK_THROWS_AS(sla::calibration_lift_plan(config,p),RuntimeError);
  p.lift_taper_step=10;p.lift_taper_min=0;CHECK_THROWS_AS(sla::calibration_lift_plan(config,p),RuntimeError);
  p.lift_taper_min=100;CHECK_THROWS_AS(sla::calibration_lift_plan(config,p),RuntimeError);
  p.lift_taper_min=10;p.pattern=sla::CalibrationPattern::XYZBlocks;
  CHECK_THROWS_AS(sla::create_calibration(config,p),RuntimeError);
  p.pattern=sla::CalibrationPattern::LiftArea;p.lift_taper_step=0;p.height=.1;
  CHECK(sla::calibration_lift_plan(config,p).base_layers==2);
  CHECK(sla::calibration_lift_plan(config,p).factors.empty());
  CHECK_NOTHROW(sla::create_calibration(config,p)); // Preserve legacy constant masks.
}

TEST_CASE("Lift taper project schema rejects malformed or downgraded provenance", "[sla_calibration_taper]") {
  auto config=printer_config();config.set("sla_bottom_layers",3);
  sla::CalibrationParameters p;p.pattern=sla::CalibrationPattern::LiftArea;p.lift_taper_step=10;
  auto job=sla::create_calibration(config,p);
  const auto folder=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("lift-schema-%%%%");
  boost::filesystem::create_directories(folder);
  const auto good=folder/"good.sla-project";
  sla::save_layer_project(good.string(),sla::capture_generated_project(*job,*job->layer_document()));
  const auto original=read_zipper_archive(good.string(),{"metadata/sla/"},{});
  using Json=nlohmann::json;
  const std::vector<std::function<void(Json &)>> mutations{
    [](auto &j){j["version"]=1;}, [](auto &j){j["version"]=3;},
    [](auto &j){j.erase("lift_taper_step");}, [](auto &j){j.erase("lift_taper_min");},
    [](auto &j){j["lift_taper_step"]=-1;}, [](auto &j){j["lift_taper_step"]=100;},
    [](auto &j){j["lift_taper_step"]=1.5;}, [](auto &j){j["lift_taper_step"]=true;},
    [](auto &j){j["lift_taper_min"]=0;}, [](auto &j){j["lift_taper_min"]=100;},
    [](auto &j){j["lift_taper_min"]=95;}, [](auto &j){j["pattern"]=0;}
  };
  for(const auto &mutate:mutations){
    auto entries=original.entries;
    auto manifest=std::find_if(entries.begin(),entries.end(),[](const auto &e){return e.fname=="metadata/sla/layers.json";});
    REQUIRE(manifest!=entries.end());auto metadata=Json::parse(manifest->buf);
    REQUIRE(metadata["calibration_recipe"]["version"]==2);
    mutate(metadata["calibration_recipe"]);const auto text=metadata.dump();manifest->buf.assign(text.begin(),text.end());
    const auto bad=folder/"bad.sla-project";
    {Zipper zip(bad.string());for(const auto &entry:entries)zip.add_entry(entry.fname,entry.buf.data(),entry.buf.size());zip.finalize();}
    CHECK_THROWS(sla::read_project_layers(bad.string()));
    REQUIRE(sla::read_project_layers(good.string()));
  }
  boost::filesystem::remove_all(folder);
}
TEST_CASE("Grayscale calibration refuses patches indistinguishable in its source palette",
          "[sla_calibration_palette]") {
  auto config = printer_config();
  config.set("sla_archive_format", "pwc"); config.set("sla_anycubic_version", "516");
  sla::CalibrationParameters parameters;
  parameters.pattern = sla::CalibrationPattern::GrayscalePatches;
  parameters.size_x = parameters.size_y = 2.; parameters.margin = 1.; parameters.height = .1;
  parameters.grayscale_steps = 16; parameters.grayscale_first = 32; parameters.grayscale_last = 33;
  const auto original = config;
  CHECK_THROWS_AS(sla::create_calibration(config, parameters), RuntimeError);
  CHECK(config.diff(original).empty());
  parameters.grayscale_first = 0; parameters.grayscale_last = 255;
  CHECK_NOTHROW(sla::create_calibration(config, parameters));
}

TEST_CASE("Calibration projects persist the original generation recipe",
          "[sla_calibration_recipe]") {
  auto config = printer_config();
  const auto format = GENERATE("uvj", "pwc");
  const auto version = GENERATE("516", "515");
  config.set("sla_archive_format", format); config.set("sla_anycubic_version", version);
  const auto pattern = GENERATE(sla::CalibrationPattern::GrayscalePatches,
      sla::CalibrationPattern::XYZBlocks, sla::CalibrationPattern::LiftArea);
  sla::CalibrationParameters parameters;
  parameters.pattern = pattern;
  parameters.size_x = parameters.size_y = 2.; parameters.margin = 1.; parameters.height = .1;
  parameters.grayscale_steps = 14; parameters.grayscale_first = 32;
  auto job = sla::create_calibration(config, parameters);
  REQUIRE(job->calibration_recipe());
  auto edited = *job->layer_document(); edited.paint(0, 1, 1, 1, 255);
  job->replace_layer_document(edited);
  const auto path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("calibration-recipe-%%%%.sla-project");
  sla::save_layer_project(path.string(), sla::capture_generated_project(*job, *job->layer_document()));
  mz_zip_archive archive{};
  REQUIRE(mz_zip_reader_init_file(&archive, path.string().c_str(), 0));
  size_t size = 0;
  void *payload = mz_zip_reader_extract_file_to_heap(&archive, "Metadata/SLA/layers.json", &size, 0);
  REQUIRE(payload);
  const std::string metadata(static_cast<const char *>(payload), size);
  mz_free(payload); mz_zip_reader_end(&archive);
  const auto read = sla::read_project_layers(path.string()); REQUIRE(read);
  REQUIRE(read->calibration_recipe);
  const auto restored = sla::restore_generated_project(*read);
  REQUIRE(restored->calibration_recipe());
  const auto &recipe = *restored->calibration_recipe();
  CHECK(recipe.parameters.pattern == pattern);
  CHECK(recipe.parameters.size_x == parameters.size_x);
  CHECK(recipe.parameters.height == parameters.height);
  CHECK(recipe.parameters.grayscale_first == parameters.grayscale_first);
  CHECK(recipe.requested == job->calibration_recipe()->requested);
  CHECK(recipe.effective == job->calibration_recipe()->effective);
  CHECK(restored->layer_document()->image(0).buf == edited.image(0).buf);
  auto invalid = *read;
  invalid.origin = sla::ProjectLayerOrigin::ImportedArchive;
  CHECK_THROWS(sla::restore_generated_project(invalid));
  auto bad_recipe = std::make_shared<sla::CalibrationRecipe>(recipe);
  bad_recipe->parameters.pattern = static_cast<sla::CalibrationPattern>(3);
  CHECK_THROWS(restored->set_calibration_recipe(bad_recipe));
  CHECK(restored->calibration_recipe()->parameters.pattern == pattern);
  boost::filesystem::remove(path);
  CHECK(metadata.find("\"calibration_recipe\"") != std::string::npos);
}

TEST_CASE("Calibration recipe rejects malformed projects and preserves published files",
          "[sla_calibration_recipe_hostile]") {
  auto config = printer_config(); config.set("sla_archive_format", "pwc");
  sla::CalibrationParameters parameters;
  parameters.pattern = sla::CalibrationPattern::GrayscalePatches;
  parameters.size_x = parameters.size_y = 2.; parameters.margin = 1.; parameters.height = .1;
  parameters.grayscale_steps = 14; parameters.grayscale_first = 32;
  auto job = sla::create_calibration(config, parameters);
  auto document = *job->layer_document(); document.paint(0, 1, 1, 1, 255);
  job->replace_layer_document(document);
  const auto source_pixels = document.image(0).buf;
  const auto state = sla::capture_generated_project(*job, document);
  const char *requested = std::getenv("SLA_CALIBRATION_RECIPE_HOSTILE_FIXTURES");
  const auto folder = requested ? boost::filesystem::path(requested) :
      boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("recipe-hostile-%%%%");
  boost::filesystem::create_directories(folder);
  const auto good = folder / "valid.sla-project";
  sla::save_layer_project(good.string(), state);
  const auto original = read_zipper_archive(good.string(), {"metadata/sla/"}, {});
  auto bytes = [](const boost::filesystem::path &path) {
    std::ifstream in(path.string(), std::ios::binary);
    return std::vector<char>{std::istreambuf_iterator<char>(in), {}};
  };
  const auto published = bytes(good);
  using Json = nlohmann::json;
  const std::vector<std::pair<std::string, std::function<void(Json &)>>> mutations{
    {"missing-recipe", [](auto &j) { j.erase("calibration_recipe"); }},
    {"downgrade", [](auto &j) { j["version"] = 5; }},
    {"schema", [](auto &j) { j["calibration_recipe"]["version"] = 2; }},
    {"fractional-schema", [](auto &j) { j["calibration_recipe"]["version"] = 1.0; }},
    {"missing-field", [](auto &j) { j["calibration_recipe"].erase("size_x"); }},
    {"extra-field", [](auto &j) { j["calibration_recipe"]["other"] = 1; }},
    {"unknown-pattern", [](auto &j) { j["calibration_recipe"]["pattern"] = 3; }},
    {"negative-pattern", [](auto &j) { j["calibration_recipe"]["pattern"] = -1; }},
    {"fractional-pattern", [](auto &j) { j["calibration_recipe"]["pattern"] = 1.5; }},
    {"unknown-precision", [](auto &j) { j["calibration_recipe"]["precision"] = 19; }},
    {"wrong-producer", [](auto &j) { j["document"]["fresh_job"]["producer"] = 3; }},
    {"wrong-origin", [](auto &j) { j["source_kind"] = "imported_archive"; }},
    {"missing-settings", [](auto &j) { j.erase("settings"); }},
    {"zero-size", [](auto &j) { j["calibration_recipe"]["size_x"] = 0; }},
    {"negative-margin", [](auto &j) { j["calibration_recipe"]["margin"] = -1; }},
    {"string-dimension", [](auto &j) { j["calibration_recipe"]["height"] = "0.1"; }},
    {"null-dimension", [](auto &j) { j["calibration_recipe"]["height"] = nullptr; }},
    {"huge-dimension", [](auto &j) { j["calibration_recipe"]["height"] = 1e200; }},
    {"steps-overflow", [](auto &j) { j["calibration_recipe"]["grayscale_steps"] = 257; }},
    {"steps-zero", [](auto &j) { j["calibration_recipe"]["grayscale_steps"] = 0; }},
    {"byte-overflow", [](auto &j) { j["calibration_recipe"]["grayscale_first"] = 256; }},
    {"byte-negative", [](auto &j) { j["calibration_recipe"]["effective"][0] = -1; }},
    {"byte-fractional", [](auto &j) { j["calibration_recipe"]["effective"][0] = 34.5; }},
    {"byte-bool", [](auto &j) { j["calibration_recipe"]["effective"][0] = true; }},
    {"levels-not-array", [](auto &j) { j["calibration_recipe"]["requested"] = "32"; }},
    {"short-levels", [](auto &j) { j["calibration_recipe"]["effective"].erase(0); }},
    {"long-levels", [](auto &j) { j["calibration_recipe"]["requested"] = Json::array(); for (int i=0;i<257;++i) j["calibration_recipe"]["requested"].push_back(i); }},
    {"wrong-requested", [](auto &j) { j["calibration_recipe"]["requested"][0] = 33; }},
    {"wrong-effective", [](auto &j) { j["calibration_recipe"]["effective"][0] = 35; }},
    {"duplicate-levels", [](auto &j) { j["calibration_recipe"]["effective"][1] = 34; }}
  };
  for (const auto &[name, mutate] : mutations) {
    CAPTURE(name);
    auto entries = original.entries;
    auto manifest = std::find_if(entries.begin(), entries.end(), [](const auto &e) { return e.fname == "metadata/sla/layers.json"; });
    REQUIRE(manifest != entries.end());
    auto metadata = Json::parse(manifest->buf); mutate(metadata);
    const auto text = metadata.dump(); manifest->buf.assign(text.begin(), text.end());
    const auto damaged = folder / (name + ".sla-project");
    { Zipper zip(damaged.string()); for (const auto &entry : entries) zip.add_entry(entry.fname, entry.buf.data(), entry.buf.size()); zip.finalize(); }
    CHECK_THROWS(sla::read_project_layers(damaged.string()));
    CHECK(bytes(good) == published);
    CHECK(job->layer_document()->image(0).buf == source_pixels);
    CHECK(job->layer_document()->revision() == document.revision());
  }
  auto invalid = state;
  auto recipe = std::make_shared<sla::CalibrationRecipe>(*state.calibration_recipe);
  recipe->parameters.height = std::numeric_limits<double>::infinity();
  invalid.calibration_recipe = recipe;
  CHECK_THROWS(sla::save_layer_project(good.string(), invalid));
  CHECK(bytes(good) == published);
  const auto valid = sla::read_project_layers(good.string()); REQUIRE(valid);
  CHECK(valid->document.image(0).buf == source_pixels);
  if (!requested) boost::filesystem::remove_all(folder);
}

TEST_CASE("Calibration grayscale plan reports requested and effective dose levels",
          "[sla_calibration_palette]") {
  auto config = printer_config(); config.set("sla_archive_format", "pwc");
  sla::CalibrationParameters parameters; parameters.grayscale_first = 32;
  auto plan = sla::calibration_grayscale_plan(config, parameters);
  CHECK(plan.requested.front() == 32); CHECK(plan.effective.front() == 34);
  CHECK(plan.has_duplicates()); CHECK(plan.distinct_levels == 14);
  parameters.grayscale_first = 0;
  plan = sla::calibration_grayscale_plan(config, parameters);
  CHECK_FALSE(plan.has_duplicates()); CHECK(plan.changed_levels == 0);
  for (unsigned i = 0; i < 16; ++i) CHECK(plan.effective[i] == i * 17);
  parameters.grayscale_steps = 256;
  plan = sla::calibration_grayscale_plan(config, parameters);
  CHECK(plan.distinct_levels == 16);
  for (unsigned i = 0; i < 256; ++i) CHECK(plan.effective[i] == (i >> 4) * 17);
  config.set("sla_archive_format", "ctb_v4");
  plan = sla::calibration_grayscale_plan(config, parameters);
  CHECK(plan.distinct_levels == 128); CHECK(plan.has_duplicates());
  config.set("sla_archive_format", "uvj");
  plan = sla::calibration_grayscale_plan(config, parameters);
  CHECK(plan.distinct_levels == 256); CHECK_FALSE(plan.has_duplicates());
  CHECK(plan.requested == plan.effective);
  // Legacy PWC source masks are still Gray8; their eventual export limitations
  // must not be advertised as owned source precision.
  config.set("sla_archive_format", "pwc"); config.set("sla_anycubic_version", "515");
  plan = sla::calibration_grayscale_plan(config, parameters);
  CHECK(plan.precision == sla::LayerMaskPrecision::Gray8);
  CHECK(plan.requested == plan.effective);
}

TEST_CASE("PWC generated jobs preserve native parameters and masks through restart",
          "[sla_pwc_generated_export]") {
  using F = sla::LayerParameter;
  const auto version = GENERATE("516", "517", "518");
  const bool portrait = GENERATE(false, true);
  const int kind = GENERATE(0, 1, 2); // XYZ, grayscale, stationary PCB.
  CAPTURE(version, portrait, kind);
  auto config = printer_config();
  config.set("sla_archive_format", "pwc");
  config.set("sla_anycubic_version", version);
  config.option("display_orientation")->setInt(portrait ? sladoPortrait : sladoLandscape);
  config.set("exposure_time", 2.12); config.set("initial_exposure_time", 5.12);
  config.set("sla_bottom_layers", 1); config.set("sla_transition_layers", 0);
  for (const auto group : {"sla_", "sla_bottom_"}) {
    for (const auto field : {"lift_height", "lift_height2", "retract_height2",
                            "wait_before_cure", "wait_after_cure", "wait_after_lift",
                            "lift_speed", "lift_speed2", "retract_speed", "retract_speed2"})
      config.set(std::string(group) + field, 0.);
    config.set(std::string(group) + "light_pwm", 255);
  }
  // Exercise both native EXTRA groups and two-stage travel. PCB must override
  // this entire moving schedule locally without changing the caller's preset.
  config.set("sla_lift_height", 1.75); config.set("sla_lift_height2", .5);
  config.set("sla_lift_speed", 150.); config.set("sla_lift_speed2", 75.);
  config.set("sla_retract_speed", 120.); config.set("sla_retract_speed2", 90.);
  config.set("sla_bottom_lift_height", 1.25); config.set("sla_bottom_lift_height2", .75);
  config.set("sla_bottom_lift_speed", 120.); config.set("sla_bottom_lift_speed2", 60.);
  config.set("sla_bottom_retract_speed", 90.); config.set("sla_bottom_retract_speed2", 120.);
  config.set("sla_wait_before_cure", .125); config.set("sla_bottom_wait_before_cure", .125);
  const auto original_config = config;
  std::unique_ptr<SLAPrint> job;
  if (kind == 2) {
    const auto left = sla::parse_gerber(prefix + "%ADD10R,4X2*%D10*X50000Y50000D03*M02*");
    const auto right = sla::parse_gerber(prefix + "%ADD10R,4X2*%D10*X150000Y90000D03*M02*");
    job = sla::create_pcb_exposure_job(config, {left, right}, false, {}, 30.12, true);
  } else {
    sla::CalibrationParameters parameters;
    parameters.size_x = 2.; parameters.size_y = 2.; parameters.margin = 1.; parameters.height = .15;
    if (kind == 1) {
      parameters.pattern = sla::CalibrationPattern::GrayscalePatches;
      parameters.grayscale_steps = 16;
      parameters.grayscale_first = 0; parameters.grayscale_last = 255;
    }
    job = sla::create_calibration(config, parameters);
  }
  CHECK(config.diff(original_config).empty());
  const auto &document = *job->layer_document();
  REQUIRE(document.fresh_job());
  const auto original_job = *document.fresh_job();
  CHECK(original_job.producer == (kind == 2 ? sla::FreshLayerProducer::PCB : sla::FreshLayerProducer::Calibration));
  CHECK(original_job.mask_precision == sla::LayerMaskPrecision::NativeGray4);
  std::vector<std::vector<uint8_t>> original_images;
  for (size_t i = 0; i < document.layer_count(); ++i) {
    const auto image = document.image(i);
    CHECK(std::all_of(image.buf.begin(), image.buf.end(), [](uint8_t v) { return v % 17 == 0; }));
    CHECK(original_job.rows[i].at(F::Exposure) == (kind == 2 ? 30.12 : i ? 2.12 : 5.12));
    original_images.push_back(image.buf);
  }
  const char *requested = std::getenv("SLA_PWC_GENERATED_FIXTURES");
  const auto base = requested ? boost::filesystem::path(requested) :
      boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("pwc-generated-%%%%");
  const auto folder = base / (std::string(version) + "-" + (portrait ? "portrait" : "landscape") + "-" + std::to_string(kind));
  boost::filesystem::create_directories(folder);
  sla::save_layer_project((folder / "fresh.sla-project").string(), sla::capture_generated_project(*job, document));
  const auto read = sla::read_project_layers((folder / "fresh.sla-project").string());
  REQUIRE(read);
  auto restored = sla::restore_generated_project(*read);
  for (bool restart : {false, true}) {
    auto &source = restart ? *restored : *job;
    for (bool direct : {false, true}) {
      const auto path = folder / (std::string(restart ? "restart" : "original") + "-" + (direct ? "print" : "document") + ".pwc");
      if (direct) REQUIRE_NOTHROW(source.export_print(path.string()));
      else REQUIRE_NOTHROW(source.export_layer_document(*source.layer_document(), "pwc", path.string()));
      CHECK(source.layer_document()->fresh_job()->rows == original_job.rows);
      CHECK(source.layer_document()->fresh_job()->mask_precision == original_job.mask_precision);
      for (size_t i = 0; i < original_images.size(); ++i)
        CHECK(source.layer_document()->image(i).buf == original_images[i]);
    }
  }
  if (!requested) boost::filesystem::remove_all(base);
}

TEST_CASE("All modern Anycubic producers own native parameters and palettes",
          "[sla_anycubic_generated_export]") {
  using F = sla::LayerParameter;
  const auto format = GENERATE("pwmx","pwmo","pwms","pmsq","dlp","pwma","pm3","pm3m",
      "pwmb","dl2p","pmx2","pm3r","pm3n","pm5","px6s","pm5s","m5sp","pm4n","pwc");
  const auto number = GENERATE_COPY(Catch::Generators::from_range(anycubic_available_versions(format)));
  if (number < 516) {
    CHECK_FALSE(sla::generated_layer_ownership(format, sla::FreshLayerProducer::Calibration, std::to_string(number)));
    return;
  }
  const auto version = std::to_string(number);
  const bool portrait = GENERATE(false, true);
  const int kind = GENERATE(0, 1, 2); // XYZ, grayscale, stationary PCB.
  CAPTURE(format, version, portrait, kind);
  auto config = printer_config();
  config.set("sla_archive_format", format);
  config.set("sla_anycubic_version", version);
  config.option("display_orientation")->setInt(portrait ? sladoPortrait : sladoLandscape);
  config.set("exposure_time", 2.12); config.set("initial_exposure_time", 5.12);
  config.set("sla_bottom_layers", 1); config.set("sla_transition_layers", 0);
  for (const auto group : {"sla_", "sla_bottom_"}) {
    for (const auto field : {"lift_height", "lift_height2", "retract_height2",
                            "wait_before_cure", "wait_after_cure", "wait_after_lift",
                            "lift_speed", "lift_speed2", "retract_speed", "retract_speed2"})
      config.set(std::string(group) + field, 0.);
    config.set(std::string(group) + "light_pwm", 255);
  }
  // Exercise both native EXTRA groups and two-stage travel. PCB must override
  // this entire moving schedule locally without changing the caller's preset.
  config.set("sla_lift_height", 1.75); config.set("sla_lift_height2", .5);
  config.set("sla_lift_speed", 150.); config.set("sla_lift_speed2", 75.);
  config.set("sla_retract_speed", 120.); config.set("sla_retract_speed2", 90.);
  config.set("sla_bottom_lift_height", 1.25); config.set("sla_bottom_lift_height2", .75);
  config.set("sla_bottom_lift_speed", 120.); config.set("sla_bottom_lift_speed2", 60.);
  config.set("sla_bottom_retract_speed", 90.); config.set("sla_bottom_retract_speed2", 120.);
  config.set("sla_wait_before_cure", .125); config.set("sla_bottom_wait_before_cure", .125);
  const auto original_config = config;
  std::unique_ptr<SLAPrint> job;
  if (kind == 2) {
    const auto left = sla::parse_gerber(prefix + "%ADD10R,4X2*%D10*X50000Y50000D03*M02*");
    const auto right = sla::parse_gerber(prefix + "%ADD10R,4X2*%D10*X150000Y90000D03*M02*");
    job = sla::create_pcb_exposure_job(config, {left, right}, false, {}, 30.12, true);
  } else {
    sla::CalibrationParameters parameters;
    parameters.size_x = 2.; parameters.size_y = 2.; parameters.margin = 1.; parameters.height = .15;
    if (kind == 1) {
      parameters.pattern = sla::CalibrationPattern::GrayscalePatches;
      parameters.grayscale_steps = 16;
      parameters.grayscale_first = 0; parameters.grayscale_last = 255;
    }
    job = sla::create_calibration(config, parameters);
  }
  CHECK(config.diff(original_config).empty());
  const auto &document = *job->layer_document();
  REQUIRE(document.fresh_job());
  const auto original_job = *document.fresh_job();
  CHECK(original_job.producer == (kind == 2 ? sla::FreshLayerProducer::PCB : sla::FreshLayerProducer::Calibration));
  CHECK(original_job.mask_precision == sla::LayerMaskPrecision::NativeGray4);
  std::vector<std::vector<uint8_t>> original_images;
  for (size_t i = 0; i < document.layer_count(); ++i) {
    const auto image = document.image(i);
    CHECK(std::all_of(image.buf.begin(), image.buf.end(), [](uint8_t v) { return v % 17 == 0; }));
    CHECK(original_job.rows[i].at(F::Exposure) == (kind == 2 ? 30.12 : i ? 2.12 : 5.12));
    original_images.push_back(image.buf);
  }
  const char *requested = std::getenv("SLA_ANYCUBIC_GENERATED_FIXTURES");
  const auto base = requested ? boost::filesystem::path(requested) :
      boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("pwc-generated-%%%%");
  const auto folder = base / (std::string(format) + "-" + version + "-" + (portrait ? "portrait" : "landscape") + "-" + std::to_string(kind));
  boost::filesystem::create_directories(folder);
  sla::save_layer_project((folder / "fresh.sla-project").string(), sla::capture_generated_project(*job, document));
  const auto read = sla::read_project_layers((folder / "fresh.sla-project").string());
  REQUIRE(read);
  auto restored = sla::restore_generated_project(*read);
  for (bool restart : {false, true}) {
    auto &source = restart ? *restored : *job;
    for (bool direct : {false, true}) {
      const auto path = folder / (std::string(restart ? "restart" : "original") + "-" + (direct ? "print" : "document") + "." + format);
      if (direct) REQUIRE_NOTHROW(source.export_print(path.string()));
      else REQUIRE_NOTHROW(source.export_layer_document(*source.layer_document(), format, path.string()));
      CHECK(source.layer_document()->fresh_job()->rows == original_job.rows);
      CHECK(source.layer_document()->fresh_job()->mask_precision == original_job.mask_precision);
      for (size_t i = 0; i < original_images.size(); ++i)
        CHECK(source.layer_document()->image(i).buf == original_images[i]);
    }
  }
  if (!requested) boost::filesystem::remove_all(base);
}

TEST_CASE("Calibration wiring retains active two-stage typed motion and phase overrides",
          "[sla_generated_owned_guards]") {
  using F=sla::LayerParameter;
  auto config=printer_config();config.set("sla_archive_format","SL1");
  config.set("sla_bottom_layers",1);config.set("sla_transition_layers",20);
  config.set("exposure_time",3.25);config.set("initial_exposure_time",9.5);
  const std::map<F,std::pair<std::string,double>> fields{
    {F::LiftDistance,{"lift_height",3.123456789}}, {F::LiftDistance2,{"lift_height2",1.25}},
    {F::LiftSpeed,{"lift_speed",60.}}, {F::LiftSpeed2,{"lift_speed2",180.}},
    {F::RetractDistance2,{"retract_height2",.75}}, {F::RetractSpeed,{"retract_speed",150.}},
    {F::RetractSpeed2,{"retract_speed2",240.}}, {F::WaitBefore,{"wait_before_cure",.12}},
    {F::WaitAfter,{"wait_after_cure",.34}}, {F::WaitAfterLift,{"wait_after_lift",.56}}};
  for(const auto prefix:{"sla_","sla_bottom_"})
    for(const auto &[field,keyvalue]:fields)config.set(std::string(prefix)+keyvalue.first,keyvalue.second);
  config.set("sla_light_pwm",180);config.set("sla_bottom_light_pwm",129);
  const auto original=config;
  sla::CalibrationParameters parameters;parameters.size_x=2.;parameters.size_y=2.;parameters.margin=1.;parameters.height=.15;
  const auto job=sla::create_calibration(config,parameters);REQUIRE(job->layer_document()->fresh_job());
  const auto &fresh=*job->layer_document()->fresh_job();CHECK(config.diff(original).empty());
  CHECK(fresh.globals.bottom_layers==1);CHECK(fresh.globals.transition_layers==0);
  for(size_t i=0;i<3;++i) {
    for(const auto &[field,keyvalue]:fields) CHECK(fresh.rows[i].at(field)==keyvalue.second);
    CHECK(fresh.rows[i].at(F::Exposure)==(i?3.25:9.5));
    CHECK(fresh.rows[i].at(F::LightPWM)==(i?180.:129.));
  }
  const double motor=3.123456789+1.25/3.+.75/4.+(3.123456789+1.25-.75)/2.5;
  const double expected_time=9.5+2*3.25+3*(motor+.12+.34+.56);
  CHECK(job->print_statistics().estimated_print_time==Catch::Approx(expected_time).epsilon(0).margin(1e-12));
}

TEST_CASE("Fresh material export refuses positive underflow and overflowing native resin density atomically",
          "[sla_generated_owned_material_guard]") {
  const auto format=GENERATE("ctb_v4","ctb_v5","ctb_encrypted_v4","ctb_encrypted");
  const int scenario=GENERATE(0,1,2);
  const bool direct=GENERATE(false,true);
  CAPTURE(format,scenario,direct);
  auto config=printer_config();config.set("sla_archive_format",format);config.set("printer_model","GENERIC");
  config.set("exposure_time",30.12);config.set("initial_exposure_time",30.12);
  config.set("sla_bottom_layers",0);config.set("sla_transition_layers",0);
  config.set("material_density",scenario==0?1e-300:scenario==2?1e300:1.5);
  config.set("bottle_cost",scenario==1?1e-300:100.);config.set("bottle_volume",500.);
  for(const auto p:{"sla_","sla_bottom_"}) {
    for(const auto key:{"lift_height","lift_height2","retract_height2","wait_before_cure","wait_after_cure","wait_after_lift",
                        "lift_speed","lift_speed2","retract_speed","retract_speed2"}) config.set(std::string(p)+key,0.);
    config.set(std::string(p)+"light_pwm",255);
  }
  // Zero occupancy isolates native density overflow from material-total guards.
  std::vector<uint8_t> pixels(400*150,0);
  if(scenario!=2) std::fill(pixels.begin()+100,pixels.begin()+500,uint8_t(255));
  auto source=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(pixels.data(),400,150,1)});
  if(scenario==2) {
    // The existing constructor already rejects overflowing material profiles.
    // This is a regression of that guard, not a newly discovered export bug.
    CHECK_THROWS_AS(SLAPrint::create_raster_job(config,source,{.05},sla::FreshLayerProducer::PCB),RuntimeError);
    CHECK(source.revision()==0);CHECK(source.image(0).buf==pixels);
    CHECK(config.opt_float("material_density")==1e300);
    return;
  }
  auto job=SLAPrint::create_raster_job(config,source,{.05},sla::FreshLayerProducer::PCB);
  const auto &document=*job->layer_document();const auto descriptor=*document.fresh_job();
  const auto source_png=document.image(0).buf;const auto revision=document.revision();
  const auto folder=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("fresh-material-guard-%%%%");
  boost::filesystem::create_directories(folder);const auto target=folder/"existing.ctb";
  const std::string sentinel="keep previous valid export";
  {std::ofstream stream(target.string(),std::ios::binary);stream<<sentinel;}
  CHECK_THROWS_AS(direct?job->export_layer_document(document,format,target.string()):
      job->export_print(target.string(),ThumbnailsList{}),RuntimeError);
  std::ifstream stream(target.string(),std::ios::binary);
  CHECK(std::string((std::istreambuf_iterator<char>(stream)),{})==sentinel);
  CHECK(document.revision()==revision);CHECK(document.image(0).buf==source_png);
  CHECK(document.fresh_job()->rows==descriptor.rows);
  CHECK(document.fresh_job()->geometry.width_mm==descriptor.geometry.width_mm);
  CHECK(document.fresh_job()->geometry.height_mm==descriptor.geometry.height_mm);
  CHECK(std::distance(boost::filesystem::directory_iterator(folder),boost::filesystem::directory_iterator())==1);
  boost::filesystem::remove_all(folder);
}

TEST_CASE("Anycubic auto producer uses the registry's actual default version",
          "[sla_anycubic_producer_version]") {
  const auto selection = GENERATE(std::make_pair("pwmx", 1), std::make_pair("pwma", 515),
      std::make_pair("dl2p", 517), std::make_pair("pm5s", 518), std::make_pair("pwc", 516));
  const auto producer = GENERATE(sla::FreshLayerProducer::ModelSlice,
      sla::FreshLayerProducer::Calibration, sla::FreshLayerProducer::PCB);
  CAPTURE(selection.first, selection.second);
  CHECK(sla::generated_anycubic_version(selection.first) == selection.second);
  const auto owned = sla::generated_layer_ownership(selection.first, producer);
  if (selection.second >= 516) {
    CHECK(owned == producer);
    CHECK(sla::generated_mask_precision(selection.first) == sla::LayerMaskPrecision::NativeGray4);
  } else CHECK_FALSE(owned);
  CHECK_THROWS(sla::generated_layer_ownership(selection.first, producer, "999"));
}

TEST_CASE("Initial PWC ownership requires a preserving native version", "[sla_pwc_initial_version]") {
  const auto producer = GENERATE(sla::FreshLayerProducer::ModelSlice,
      sla::FreshLayerProducer::Calibration, sla::FreshLayerProducer::PCB);
  for (const char *version : {"auto", "516", "517", "518"}) {
    CAPTURE(version);
    CHECK(sla::generated_layer_ownership("pwc", producer, version) == producer);
  }
  // Legacy writers retain their existing path until their neutral contracts
  // are implemented. Selecting an unsupported version must not adopt a job.
  for (const char *version : {"1", "515"}) {
    CAPTURE(version);
    CHECK_FALSE(sla::generated_layer_ownership("pwc", producer, version));
  }
  for (const char *version : {"0", "519", "516x", "+516"}) {
    CAPTURE(version);
    CHECK_THROWS_AS(sla::generated_layer_ownership("pwc", producer, version), std::invalid_argument);
  }
}

TEST_CASE("Export format drafts and persisted printer preferences have distinct ownership",
          "[sla_export_preference_draft]") {
  CHECK(sla::preferred_export_format("pwc","pm5s","pwc") == "pm5s");
  CHECK(sla::preferred_export_format("pwc","pwc","uvj") == "uvj");
  CHECK(sla::preferred_export_format("pwc","pwc","") == "pwc");
  CHECK(sla::preferred_export_format("pwc","pwc","unknown") == "pwc");
  CHECK(sla::preferred_export_format("pwc","pwc","pm5s","517") == "pwc");
  CHECK(sla::preferred_export_format("pwc","pwc","pm5s","518") == "pm5s");
  CHECK(sla::preferred_export_format("pwc","pwc","UVJ","517") == "uvj");
}

TEST_CASE("Modern Anycubic rejects invalid shared waits before geometry work",
          "[sla_anycubic_wait_preflight]") {
  const auto format = GENERATE("pwc", "dl2p", "pm5s");
  const double wait = GENERATE(-.125, std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN(), 1e300, std::numeric_limits<double>::denorm_min());
  CAPTURE(format, wait);
  auto config = printer_config(); config.set("sla_archive_format", format);
  config.set("supports_enable", false); config.set("pad_enable", false);
  config.set("sla_wait_before_cure", wait); config.set("sla_bottom_wait_before_cure", wait);
  Model model; auto *object = model.add_object("Invalid shared wait", "", make_cube(4., 2., .15));
  object->add_instance()->set_offset(Vec3d(5., 5., 0.));
  SLAPrint print; print.apply(model, config);
  const auto message = print.validate();
  CHECK_FALSE(message.empty());
  CHECK(message.find("sla_wait_before_cure") != std::string::npos);
  CHECK_FALSE(print.layer_document());
}

TEST_CASE("Modern Anycubic reports unrepresentable settings before slicing",
          "[sla_anycubic_early_preflight]") {
  const auto format = GENERATE("pwc", "dl2p", "pm5s");
  const auto key = GENERATE("sla_wait_after_cure", "sla_bottom_wait_after_lift",
      "sla_retract_height2", "sla_bottom_light_pwm", "sla_bottom_wait_before_cure");
  CAPTURE(format, key);
  auto config = printer_config(); config.set("sla_archive_format", format);
  config.set("supports_enable", false); config.set("pad_enable", false);
  config.set("sla_wait_before_cure", 0.); config.set("sla_bottom_wait_before_cure", 0.);
  Model model; auto *object = model.add_object("Early preflight", "", make_cube(4., 2., .15));
  object->add_instance()->set_offset(Vec3d(5., 5., 0.));
  SLAPrint print; print.apply(model, config); REQUIRE(print.validate().empty());
  if (std::string(key) == "sla_bottom_light_pwm") config.set(key, 128);
  else config.set(key, .125);
  print.apply(model, config);
  const auto message = print.validate();
  CHECK_FALSE(message.empty());
  CHECK(message.find(key) != std::string::npos);
  CHECK_FALSE(print.layer_document());
}

TEST_CASE("Every modern Anycubic model export preserves edits and refreshed settings",
          "[sla_anycubic_model_export]") {
  using F = sla::LayerParameter;
  const auto format = GENERATE("pwmx","pwmo","pwms","pmsq","dlp","pwma","pm3","pm3m",
      "pwmb","dl2p","pmx2","pm3r","pm3n","pm5","px6s","pm5s","m5sp","pm4n","pwc");
  const auto version = GENERATE_COPY(Catch::Generators::from_range(anycubic_available_versions(format)));
  if (version < 516) return; // Legacy producers are covered separately.
  const bool portrait = GENERATE(false, true);
  CAPTURE(format, version, portrait);
  auto config = printer_config(); config.set("sla_archive_format", format);
  config.set("sla_anycubic_version", std::to_string(version));
  config.option("display_orientation")->setInt(portrait ? sladoPortrait : sladoLandscape);
  config.set("supports_enable", false); config.set("pad_enable", false); config.set("support_object_elevation", 0.);
  config.set("initial_layer_height", .05); config.set("exposure_time", 2.12); config.set("initial_exposure_time", 5.12);
  config.set("sla_bottom_layers", 1); config.set("sla_transition_layers", 0); config.set("gamma_correction", 1.);
  for (const auto group : {"sla_", "sla_bottom_"}) {
    for (const auto field : {"lift_height","lift_height2","retract_height2","wait_before_cure","wait_after_cure","wait_after_lift",
                            "lift_speed","lift_speed2","retract_speed","retract_speed2"})
      config.set(std::string(group) + field, 0.);
    config.set(std::string(group) + "light_pwm", 255);
  }
  Model model; auto *object = model.add_object("Anycubic model matrix", "", make_cube(4., 2., .15));
  object->add_instance()->set_offset(Vec3d(5., 5., 0.));
  SLAPrint print; print.apply(model, config); REQUIRE(print.validate().empty()); print.process();
  REQUIRE(print.layer_document()); REQUIRE(print.layer_document()->fresh_job());
  CHECK(print.layer_document()->fresh_job()->parameter_origin == sla::FreshLayerParameterOrigin::AutomaticModel);
  CHECK(print.layer_document()->fresh_job()->mask_precision == sla::LayerMaskPrecision::NativeGray4);
  auto edited = *print.layer_document(); edited.paint(0, 1, 1, 1, 255);
  const auto pixels = edited.image(0).buf;
  const char *requested = std::getenv("SLA_ANYCUBIC_MODEL_FIXTURES");
  const auto base = requested ? boost::filesystem::path(requested) :
      boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("any-model-%%%%");
  const auto folder = base / (std::string(format) + "-" + std::to_string(version) + "-" + (portrait ? "portrait" : "landscape"));
  boost::filesystem::create_directories(folder);
  auto initial = sla::capture_project_layers(model, config, print); initial.document = edited;
  sla::save_layer_project((folder / "initial.sla-project").string(), initial);
  print.export_layer_document(edited, format, (folder / (std::string("unapplied.") + format)).string());
  CHECK(print.layer_document()->image(0).buf != pixels);
  print.replace_layer_document(edited);
  print.export_print((folder / (std::string("initial.") + format)).string());
  config.set("exposure_time", 3.12); config.set("sla_bottom_layers", 0); config.set("sla_transition_layers", 1);
  config.set("sla_wait_before_cure", .12); config.set("sla_bottom_wait_before_cure", .12);
  config.set("sla_lift_height", 1.25); config.set("sla_lift_speed", 60.); config.set("sla_retract_speed", 120.);
  print.apply(model, config); print.process();
  CHECK(print.layer_document()->image(0).buf == pixels);
  CHECK(print.layer_document()->fresh_job()->rows.back().at(F::Exposure) == 3.12);
  auto history = *print.layer_document(); REQUIRE(history.undo()); REQUIRE(history.redo());
  CHECK(history.image(0).buf == pixels);
  sla::save_layer_project((folder / "resumed.sla-project").string(), sla::capture_project_layers(model, config, print));
  const auto read = sla::read_project_layers((folder / "resumed.sla-project").string()); REQUIRE(read);
  SLAPrint restored; restored.apply(model, config);
  restored.queue_project_layers(std::make_shared<const sla::ProjectRasterState>(*read)); restored.process();
  CHECK(restored.layer_document()->fresh_job()->rows == read->document.fresh_job()->rows);
  CHECK(restored.layer_document()->image(0).buf == pixels);
  restored.export_print((folder / (std::string("resumed-print.") + format)).string());
  restored.export_layer_document(*restored.layer_document(), format, (folder / (std::string("resumed-document.") + format)).string());
  if (!requested) boost::filesystem::remove_all(base);
}

TEST_CASE("Resin bottle dependencies preserve physical volume and refuse invalid arithmetic", "[sla_resin_bottle]") {
  const auto zero=sla::derive_bottle_setting("material_density",1000,1.3,0);
  REQUIRE(zero);CHECK(std::string(zero->key)=="bottle_weight");CHECK(zero->value==0);
  const auto density=sla::derive_bottle_setting("material_density",1000,1.3,2);
  REQUIRE(density);CHECK(density->value==2);
  const auto volume=sla::derive_bottle_setting("bottle_volume",500,1.3,2);
  REQUIRE(volume);CHECK(volume->value==1);
  const auto weight=sla::derive_bottle_setting("bottle_weight",1000,1,2);
  REQUIRE(weight);CHECK(std::string(weight->key)=="bottle_volume");CHECK(weight->value==500);
  CHECK_FALSE(sla::derive_bottle_setting("bottle_weight",1000,1,0));
  CHECK_FALSE(sla::derive_bottle_setting("bottle_volume",1e308,1,1e308));
  CHECK_FALSE(sla::derive_bottle_setting("bottle_weight",1000,1e308,1e-308));
  CHECK_FALSE(sla::derive_bottle_setting("material_density",1000,1,std::numeric_limits<double>::quiet_NaN()));
  CHECK_FALSE(sla::derive_bottle_setting("bottle_volume",-1,1,1));
}
TEST_CASE("SLA filename templates preserve decimal tails and explicit archive suffixes", "[sla_material_statistics][sla_filename]") {
  const auto pair=GENERATE(std::make_pair("part16.8","part16.8.uvj"),
      std::make_pair("part.v1.68","part.v1.68.uvj"),std::make_pair("part.uvj","part.uvj"),
      std::make_pair("part.CTB","part.CTB"),std::make_pair("part","part.uvj"));
  CAPTURE(pair.first);auto config=printer_config();config.set("output_filename_format",pair.first);
  SLAPrint print;print.apply(Model{},config);
  CHECK(print.output_filename()==pair.second);
}
TEST_CASE("Material pricing updates finished edited model statistics without reslicing", "[sla_material_statistics]") {
  const std::string key=GENERATE("material_density","bottle_cost","bottle_volume","zero_volume");
  CAPTURE(key);auto config=printer_config();
  config.set("supports_enable",false);config.set("pad_enable",false);config.set("support_object_elevation",0.);
  config.set("initial_layer_height",.05);config.set("material_density",1.);config.set("bottle_cost",50.);config.set("bottle_volume",1000.);
  Model model;auto *object=model.add_object("pricing cube","",make_cube(4.,2.,.15));
  object->add_instance()->set_offset(Vec3d(5.,5.,0.));
  SLAPrint print;print.apply(model,config);REQUIRE(print.validate().empty());print.process();
  REQUIRE(print.layer_document());auto edited=*print.layer_document();edited.paint(0,1,1,1,255);
  print.replace_layer_document(edited);
  const auto *document=print.layer_document();const auto pixels=document->image(0).buf;
  const auto before=print.print_statistics();const auto revision=document->revision();
  const double ml=before.objects_used_material/1000.;REQUIRE(ml>0);
  if(key=="material_density")config.set(key,2.);
  else if(key=="bottle_cost")config.set(key,100.);
  else config.set("bottle_volume",key=="zero_volume"?0.:500.);
  print.apply(model,config);
  REQUIRE(print.finished());REQUIRE(print.layer_document()==document);
  const double expected_weight=ml*config.opt_float("material_density");
  const double expected_cost=key=="zero_volume"?0.:ml*config.opt_float("bottle_cost")/config.opt_float("bottle_volume");
  CHECK(print.print_statistics().total_weight==Catch::Approx(expected_weight));
  CHECK(print.print_statistics().total_cost==Catch::Approx(expected_cost));
  CHECK(print.print_statistics().objects_used_material==before.objects_used_material);
  CHECK(print.print_statistics().estimated_print_time==before.estimated_print_time);
  CHECK(print.layer_document()->revision()==revision);
  CHECK(bool(print.layer_document()->image(0).buf==pixels));CHECK(print.layer_document()->can_undo());
  print.process();
  CHECK(print.print_statistics().total_weight==Catch::Approx(expected_weight));
  CHECK(print.print_statistics().total_cost==Catch::Approx(expected_cost));
}
TEST_CASE("Actual sliced model parameter edits survive presets and project restore", "[sla_model_parameter_edits]") {
  const bool portrait=GENERATE(false,true);
  auto config=printer_config();config.set("sla_archive_format","uvj");
  config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
  config.set("supports_enable",false);config.set("pad_enable",false);config.set("support_object_elevation",0.);
  config.set("initial_layer_height",.05);config.set("exposure_time",2.12);config.set("initial_exposure_time",5.12);
  config.set("sla_bottom_layers",1);config.set("sla_transition_layers",0);config.set("gamma_correction",1.);
  for(const char *prefix:{"sla_","sla_bottom_"}) {
    for(const char *key:{"lift_height","lift_height2","retract_height2","wait_before_cure","wait_after_cure","wait_after_lift",
                         "lift_speed","lift_speed2","retract_speed","retract_speed2"}) config.set(std::string(prefix)+key,0.);
    config.set(std::string(prefix)+"light_pwm",255);
  }
  Model model;auto *object=model.add_object("Parameter edits cube","",make_cube(4.,2.,.15));
  object->add_instance()->set_offset(Vec3d(5.,5.,0.));
  SLAPrint print;print.apply(model,config);REQUIRE(print.validate().empty());print.process();
  REQUIRE(print.layer_document());REQUIRE(print.layer_document()->layer_count()==3);
  auto edited=*print.layer_document();using F=sla::LayerParameter;
  const auto original=edited.fresh_job()->rows;
  std::vector<std::vector<uint8_t>> masks;for(size_t i=0;i<3;++i)masks.push_back(edited.image(i).buf);
  edited.set_layer_parameters(1,{{F::Exposure,7.12},{F::WaitBefore,.5}});
  CHECK(edited.fresh_job()->parameter_origin==sla::FreshLayerParameterOrigin::Explicit);
  print.replace_layer_document(edited);
  CHECK(print.print_statistics().estimated_print_time==Catch::Approx(14.86));
  config.set("exposure_time",9.12);print.apply(model,config);print.process();
  REQUIRE(print.layer_document()->fresh_job());CHECK(print.layer_document()->fresh_job()->rows==edited.fresh_job()->rows);
  for(size_t i=0;i<3;++i)CHECK(print.layer_document()->image(i).buf==masks[i]);
  auto history=*print.layer_document();REQUIRE(history.undo());CHECK(history.fresh_job()->rows==original);
  CHECK(history.fresh_job()->parameter_origin==sla::FreshLayerParameterOrigin::AutomaticModel);
  REQUIRE(history.redo());CHECK(history.fresh_job()->rows==edited.fresh_job()->rows);
  const char *fixtures=std::getenv("SLA_MODEL_PARAMETER_FIXTURES");
  const auto folder=fixtures?boost::filesystem::path(fixtures)/(portrait?"portrait":"landscape"):
      boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("model-parameters-%%%%");
  boost::filesystem::create_directories(folder);
  sla::save_layer_project((folder/"model.sla-project").string(),sla::capture_project_layers(model,config,print));
  const auto saved=sla::read_project_layers((folder/"model.sla-project").string());REQUIRE(saved);
  SLAPrint restored;restored.apply(model,config);
  unsigned owner_stop_calls=0;restored.set_cancel_callback([&]{++owner_stop_calls;});
  restored.queue_project_layers(std::make_shared<const sla::ProjectRasterState>(*saved));
  CHECK(owner_stop_calls==1); // Selecting saved masks stops the previous worker on the UI thread.
  const auto stops_before_process=owner_stop_calls;
  restored.process();
  CHECK(owner_stop_calls==stops_before_process); // Adoption inside process must not stop its own worker.
  restored.set_cancel_callback([]{});
  REQUIRE(restored.layer_document()->fresh_job());CHECK(restored.layer_document()->fresh_job()->rows==edited.fresh_job()->rows);
  CHECK(restored.layer_document()->fresh_job()->parameter_origin==sla::FreshLayerParameterOrigin::Explicit);
  CHECK(restored.print_statistics().estimated_print_time==Catch::Approx(14.86));
  for(size_t i=0;i<3;++i)CHECK(restored.layer_document()->image(i).buf==masks[i]);
  restored.export_print((folder/"restored.uvj").string(),"model-parameters");
  if(!fixtures)boost::filesystem::remove_all(folder);
}

TEST_CASE("Actual model slices own their final parameter intent", "[sla_model_owned]") {
  const auto format=GENERATE("SL1","SL1S","cxdlpv4","ctb_v4","ctb_v5","ctb_encrypted_v4","ctb_encrypted","cws","uvj","pwc","dl2p","pm5s");
  const bool portrait=GENERATE(false,true);CAPTURE(format,portrait);
  auto config=printer_config();config.set("sla_archive_format",format);config.set("printer_model","GENERIC");
  config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
  config.set("supports_enable",false);config.set("pad_enable",false);config.set("support_object_elevation",0.);
  config.set("initial_layer_height",.05);config.set("exposure_time",2.12);config.set("initial_exposure_time",5.12);
  config.set("sla_bottom_layers",1);config.set("sla_transition_layers",0);config.set("gamma_correction",1.);
  for(const auto p:{"sla_","sla_bottom_"}) {
    for(const auto key:{"lift_height","lift_height2","retract_height2","wait_before_cure","wait_after_cure","wait_after_lift",
                        "lift_speed","lift_speed2","retract_speed","retract_speed2"}) config.set(std::string(p)+key,0.);
    config.set(std::string(p)+"light_pwm",255);
  }
  if(std::string(format)=="cws") {
    // No peel does not remove the physical Z advance; its feed must be set.
    config.set("sla_lift_speed",60.);config.set("sla_bottom_lift_speed",60.);
  }
  Model model;auto *object=model.add_object("Model ownership cube","",make_cube(4.,2.,.15));
  object->add_instance()->set_offset(Vec3d(5.,5.,0.));
  SLAPrint print;print.apply(model,config);REQUIRE(print.validate().empty());print.process();
  REQUIRE(print.layer_document());REQUIRE(print.layer_document()->fresh_job());
  const auto &fresh=*print.layer_document()->fresh_job();
  if(std::string(format)=="pwc" || std::string(format)=="dl2p" || std::string(format)=="pm5s") {
    CHECK(fresh.mask_precision==sla::LayerMaskPrecision::NativeGray4);
    for(size_t i=0;i<print.layer_document()->layer_count();++i) {
      const auto image=print.layer_document()->image(i);
      CHECK(std::all_of(image.buf.begin(),image.buf.end(),[](uint8_t value){return value%17==0;}));
    }
  }
  CHECK(fresh.producer==sla::FreshLayerProducer::ModelSlice);
  CHECK(fresh.globals.bottom_layers==1);CHECK(fresh.globals.transition_layers==0);
  REQUIRE(fresh.rows.size()==print.print_layers().size());
  for(size_t i=0;i<fresh.rows.size();++i) {
    CHECK(fresh.rows[i].at(sla::LayerParameter::PositionZ)==double(print.print_layers()[i].level())*SCALING_FACTOR);
    CHECK(fresh.rows[i].at(sla::LayerParameter::Exposure)==(i?2.12:5.12));
  }
  CHECK(print.print_statistics().time_basis==SLAPrintStatistics::TimeBasis::OwnedNeutralSchedule);
  CHECK(fresh.parameter_origin==sla::FreshLayerParameterOrigin::AutomaticModel);
  const bool pwc=std::string(format)=="pwc";
  const char *requested=std::getenv(pwc?"SLA_PWC_MODEL_FIXTURES":"SLA_CWS_MODEL_FIXTURES");
  const bool keep=requested && (std::string(format)=="cws" || pwc);
  const auto folder=keep?boost::filesystem::path(requested)/(portrait?"portrait":"landscape"):
      boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("model-owned-%%%%");
  boost::filesystem::create_directories(folder);
  auto edited=*print.layer_document();edited.paint(0,1,1,1,255);
  if(std::string(format)=="cws") {
    print.export_layer_document(edited,"cws",(folder/"unapplied.cws").string());
    CHECK(print.layer_document()->image(0).buf!=edited.image(0).buf);
  }
  if(pwc) {
    print.export_layer_document(edited,"pwc",(folder/"unapplied-516.pwc").string());
    CHECK(print.layer_document()->image(0).buf!=edited.image(0).buf);
  }
  print.replace_layer_document(edited);
  if(std::string(format)=="cws")print.export_print((folder/"initial.cws").string());
  if(pwc)print.export_print((folder/"initial-516.pwc").string());
  const auto edited_pixels=edited.image(0).buf;const auto edit_revision=edited.revision();
  config.set("exposure_time",3.12);print.apply(model,config);print.process();
  REQUIRE(print.layer_document()->fresh_job());
  CHECK(print.layer_document()->fresh_job()->rows.back().at(sla::LayerParameter::Exposure)==3.12);
  CHECK(print.layer_document()->image(0).buf==edited_pixels);
  CHECK(print.layer_document()->revision()==edit_revision);
  auto history=*print.layer_document();REQUIRE(history.undo());REQUIRE(history.redo());
  CHECK(history.image(0).buf==edited_pixels);
  config.set("sla_bottom_layers",0);config.set("sla_transition_layers",1);
  config.set("sla_wait_before_cure",.12);config.set("sla_lift_height",1.25);
  if(pwc)config.set("sla_bottom_wait_before_cure",.12); // One native shared wait.
  config.set("sla_lift_speed",60.);config.set("sla_retract_speed",120.);
  print.apply(model,config);print.process();
  CHECK(print.layer_document()->fresh_job()->globals.bottom_layers==0);
  CHECK(print.layer_document()->fresh_job()->globals.transition_layers==1);
  CHECK(print.layer_document()->fresh_job()->rows.back().at(sla::LayerParameter::WaitBefore)==.12);
  CHECK(print.layer_document()->fresh_job()->rows.back().at(sla::LayerParameter::LiftDistance)==1.25);
  CHECK(print.layer_document()->image(0).buf==edited_pixels);
  CHECK(print.layer_document()->revision()==edit_revision);
  const auto state=sla::capture_project_layers(model,config,print);
  const auto file=folder/"model.sla-project";
  sla::save_layer_project(file.string(),state);const auto read=sla::read_project_layers(file.string());REQUIRE(read);
  REQUIRE(read->document.fresh_job());
  CHECK(read->document.fresh_job()->parameter_origin==sla::FreshLayerParameterOrigin::AutomaticModel);
  SLAPrint restored;restored.apply(model,config);
  restored.queue_project_layers(std::make_shared<const sla::ProjectRasterState>(*read));restored.process();
  CHECK(restored.layer_document()->image(0).buf==edited_pixels);
  if(pwc) {
    for(const char *version:{"516","517","518"}) {
      config.set("sla_anycubic_version",std::string(version));restored.apply(model,config);restored.process();
      restored.export_print((folder/(std::string("resumed-")+version+".pwc")).string());
      CHECK(restored.layer_document()->fresh_job()->mask_precision==sla::LayerMaskPrecision::NativeGray4);
      CHECK(restored.layer_document()->image(0).buf==edited_pixels);
    }
  }
  config.set("exposure_time",4.12);restored.apply(model,config);restored.process();
  CHECK(restored.layer_document()->fresh_job()->rows.back().at(sla::LayerParameter::Exposure)==4.12);
  CHECK(restored.layer_document()->image(0).buf==edited_pixels);
  if(std::string(format)=="cws")restored.export_print((folder/"resumed.cws").string());
  // Explicitly authored ModelSlice is not permission to overwrite its rows.
  auto explicit_job=*restored.layer_document()->fresh_job();
  explicit_job.parameter_origin=sla::FreshLayerParameterOrigin::Explicit;
  explicit_job.rows.back()[sla::LayerParameter::Exposure]=7.12;
  std::vector<sla::EncodedRaster> explicit_masks;
  for(size_t i=0;i<restored.layer_document()->layer_count();++i) {
    const auto bytes=restored.layer_document()->encoded_layer(i);const auto *begin=static_cast<const uint8_t *>(bytes.buf);
    explicit_masks.emplace_back(std::vector<uint8_t>(begin,begin+bytes.sz),"png");
  }
  restored.replace_layer_document(sla::LayerDocument::from_fresh_rasters(std::move(explicit_masks),explicit_job));
  config.set("exposure_time",5.12);restored.apply(model,config);restored.process();
  CHECK(restored.layer_document()->fresh_job()->parameter_origin==sla::FreshLayerParameterOrigin::Explicit);
  CHECK(restored.layer_document()->fresh_job()->rows==explicit_job.rows);
  CHECK(restored.layer_document()->image(0).buf==edited_pixels);
  if(std::string(format)=="cws")restored.export_print((folder/"explicit.cws").string());
  if(!keep)boost::filesystem::remove_all(folder);
}

TEST_CASE("Anycubic ZIP calibration reports its native four bit dose palette before generation",
          "[sla_anycubic_zip_palette]") {
  const std::string format=GENERATE("pm4u","pm7","pm7m","pwsz");
  CAPTURE(format);auto config=printer_config();config.set("sla_archive_format",format);
  sla::CalibrationParameters parameters;parameters.pattern=sla::CalibrationPattern::GrayscalePatches;
  parameters.size_x=parameters.size_y=2.;parameters.margin=1.;parameters.height=.1;
  parameters.grayscale_steps=2;parameters.grayscale_first=1;parameters.grayscale_last=15;
  const auto duplicate=sla::calibration_grayscale_plan(config,parameters);
  CHECK(duplicate.precision==sla::LayerMaskPrecision::NativeGray4);
  CHECK(bool(duplicate.requested==std::vector<uint8_t>{1,15}));
  CHECK(bool(duplicate.effective==std::vector<uint8_t>{0,0}));
  CHECK(duplicate.distinct_levels==1);CHECK(duplicate.changed_levels==2);CHECK(duplicate.has_duplicates());
  CHECK_THROWS(sla::create_calibration(config,parameters));
  parameters.grayscale_first=16;parameters.grayscale_last=47;
  const auto positive=sla::calibration_grayscale_plan(config,parameters);
  CHECK(positive.precision==sla::LayerMaskPrecision::NativeGray4);
  CHECK(bool(positive.requested==std::vector<uint8_t>{16,47}));
  CHECK(bool(positive.effective==std::vector<uint8_t>{17,34}));
  CHECK(positive.distinct_levels==2);CHECK(positive.changed_levels==2);CHECK_FALSE(positive.has_duplicates());
  // The fixed PW0 alphabet comes from unchanged UVtools DecodePW0, not the
  // selected project helper. All256 values exercise both sides of each bucket.
  constexpr std::array<uint8_t,16> wire_levels{0,17,34,51,68,85,102,119,136,153,170,187,204,221,238,255};
  const auto precision=sla::generated_mask_precision(format);
  for(unsigned i=0;i<256;++i) {
    CAPTURE(i);CHECK(sla::project_mask_value(uint8_t(i),precision)==wire_levels[i/16]);
  }
  // Pixel capability must not manufacture a missing owned parameter writer.
  CHECK_FALSE(sla::generated_layer_ownership(format,sla::FreshLayerProducer::Calibration));
}

TEST_CASE("Actual Anycubic ZIP calibration pixels and recipe agree with PW0 after project restart",
          "[sla_anycubic_zip_palette]") {
  const std::string format=GENERATE("pm4u","pm7","pm7m","pwsz");
  const bool portrait=GENERATE(false,true);CAPTURE(format,portrait);
  auto config=printer_config();config.set("sla_archive_format",format);
  config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
  config.set("exposure_time",3.12);config.set("initial_exposure_time",9.12);
  config.set("sla_bottom_layers",1);config.set("sla_transition_layers",0);
  for(const bool bottom:{false,true}) {
    const std::string prefix=bottom?"sla_bottom_":"sla_";
    config.set(prefix+"lift_height",bottom?7.:5.);config.set(prefix+"lift_height2",0.);
    config.set(prefix+"lift_speed",bottom?60.:90.);config.set(prefix+"lift_speed2",180.);
    config.set(prefix+"retract_speed",bottom?120.:150.);config.set(prefix+"retract_speed2",240.);
    config.set(prefix+"retract_height2",0.);config.set(prefix+"wait_before_cure",.7);
    config.set(prefix+"wait_after_cure",0.);config.set(prefix+"wait_after_lift",0.);config.set(prefix+"light_pwm",255);
  }
  sla::CalibrationParameters parameters;parameters.pattern=sla::CalibrationPattern::GrayscalePatches;
  parameters.size_x=parameters.size_y=2.;parameters.margin=1.;parameters.height=.15;
  parameters.grayscale_steps=2;parameters.grayscale_first=16;parameters.grayscale_last=47;
  auto job=sla::create_calibration(config,parameters);
  REQUIRE(job->layer_document());CHECK_FALSE(job->layer_document()->fresh_job());
  REQUIRE(job->calibration_recipe());
  CHECK(job->calibration_recipe()->precision==sla::LayerMaskPrecision::NativeGray4);
  CHECK(bool(job->calibration_recipe()->requested==std::vector<uint8_t>{16,47}));
  CHECK(bool(job->calibration_recipe()->effective==std::vector<uint8_t>{17,34}));
  const char *fixtures=std::getenv("SLA_ANYCUBIC_ZIP_PALETTE_FIXTURES");
  const auto folder=fixtures?boost::filesystem::path(fixtures)/(format+(portrait?"-portrait":"-landscape")):
    boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("zip-palette-%%%%");
  struct Cleanup {boost::filesystem::path folder;bool keep;~Cleanup(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(folder,error);}}} cleanup{folder,fixtures!=nullptr};
  boost::filesystem::create_directories(folder);
  auto export_and_check=[&](const std::string &name) {
    const auto file=folder/(name+"."+format);job->export_print(file.string());
    const auto archive=read_zipper_archive(file.string(),{""},{});
    auto entry=[&](const std::string &path)->const std::vector<uint8_t>& {
      const auto found=std::find_if(archive.entries.begin(),archive.entries.end(),[&](const auto &e){return e.fname==path;});
      REQUIRE(found!=archive.entries.end());return found->buf;
    };
    const auto settings=nlohmann::json::parse(entry("anycubic_photon_resins.pwsp"));
    const auto &machine=settings.at("machine_type");CHECK(machine.at("key_suffix")==format);
    CHECK(machine.at("res_x")==unsigned(portrait?150:400));CHECK(machine.at("res_y")==unsigned(portrait?400:150));
    const auto &resin=settings.at("machine_extern").at("user_resins").at(0);
    CHECK(resin.at("slicepara").at("bott_layers")==1);CHECK(resin.at("slice_extpara").at("transition_layercount")==0);
    CHECK(resin.at("slicepara").at("bott_time").get<double>()==Catch::Approx(9.12));
    CHECK(resin.at("slicepara").at("exposure_time").get<double>()==Catch::Approx(3.12));
    CHECK(resin.at("slicepara").at("off_time").get<double>()==Catch::Approx(.7));
    const auto controller=nlohmann::json::parse(entry("layers_controller.conf"));
    REQUIRE(controller.at("count")==job->layer_document()->layer_count());
    for(size_t i=0;i<job->layer_document()->layer_count();++i) {
      CAPTURE(name,i);const auto source=job->layer_document()->image(i);
      CHECK(source.cols==size_t(portrait?150:400));CHECK(source.rows==size_t(portrait?400:150));
      // Independent bounded wire parser: the gray nibble selects one literal
      // decoded level; black/white have12bit counts, other levels4bit counts.
      constexpr std::array<uint8_t,16> levels{0,17,34,51,68,85,102,119,136,153,170,187,204,221,238,255};
      const auto &wire=entry("layer_images/layer_"+std::to_string(i)+".pw0img");
      std::vector<uint8_t> decoded;
      for(size_t offset=0;offset<wire.size();) {
        const auto token=wire[offset++];const unsigned bucket=token/16;size_t count=token%16;
        if(bucket==0 || bucket==15){REQUIRE(offset<wire.size());count=count*256+wire[offset++];}
        REQUIRE(count>0);REQUIRE(count<=source.buf.size()-decoded.size());
        decoded.insert(decoded.end(),count,levels[bucket]);
      }
      REQUIRE(decoded.size()==source.buf.size());CHECK(bool(decoded==source.buf));
      CHECK(std::all_of(source.buf.begin(),source.buf.end(),[&](uint8_t pixel){return std::find(levels.begin(),levels.end(),pixel)!=levels.end();}));
      const auto &row=controller.at("paras").at(i);
      CHECK(row.at("exposure_time").get<double>()==Catch::Approx(i?3.12:9.12));
      CHECK(row.at("layer_thickness").get<double>()==Catch::Approx(.05));
      CHECK(row.at("zup_height").get<double>()==Catch::Approx(i?5.:7.));
    }
  };
  const auto project=folder/"calibration.sla-project";
  sla::save_layer_project(project.string(),sla::capture_generated_project(*job,*job->layer_document()));
  export_and_check("calibration");
  const auto state=sla::read_project_layers(project.string());REQUIRE(state);
  auto restarted=sla::restore_generated_project(*state);
  CHECK_FALSE(restarted->layer_document()->fresh_job());
  REQUIRE(restarted->calibration_recipe());CHECK(bool(restarted->calibration_recipe()->effective==std::vector<uint8_t>{17,34}));
  for(size_t i=0;i<job->layer_document()->layer_count();++i)CHECK(bool(restarted->layer_document()->image(i).buf==job->layer_document()->image(i).buf));
  job=std::move(restarted);export_and_check("restart");
  // Explicit source projection exercises all256 inputs without relying on
  // writer-side loss. Palette capability remains independent of parameters.
  auto edited=*job->layer_document();auto image=edited.image(0);REQUIRE(image.buf.size()>=256);
  const auto precision=sla::generated_mask_precision(format);
  for(unsigned i=0;i<256;++i)image.buf[i]=sla::project_mask_value(uint8_t(i),precision);
  edited.replace_image(0,image);job->replace_layer_document(edited);
  export_and_check("palette");
  sla::save_layer_project((folder/"palette.sla-project").string(),sla::capture_generated_project(*job,*job->layer_document()));
  if(fixtures) {
    nlohmann::json expected{{"format",format},{"portrait",portrait},{"requested",{16,47}},{"effective",{17,34}},
      {"normal_exposure",3.12},{"bottom_exposure",9.12},{"bottom_count",1},{"transition_count",0},
      {"width_mm",portrait?30.:40.},{"height_mm",portrait?40.:30.}};
    std::ofstream out((folder/"expected.json").string());out<<expected.dump(2);
  }
}

TEST_CASE("Legacy native palette provenance survives target changes without inventing parameters",
          "[sla_anycubic_zip_palette]") {
  auto config=printer_config();config.set("sla_archive_format","pm7");
  sla::CalibrationParameters parameters;parameters.pattern=sla::CalibrationPattern::GrayscalePatches;
  parameters.size_x=parameters.size_y=2.;parameters.margin=1.;parameters.height=.1;
  parameters.grayscale_steps=2;parameters.grayscale_first=16;parameters.grayscale_last=47;
  auto job=sla::create_calibration(config,parameters);
  auto edited=*job->layer_document();edited.invert(0);job->replace_layer_document(edited);
  const auto state=sla::capture_generated_project(*job,*job->layer_document());
  REQUIRE_FALSE(state.document.fresh_job());REQUIRE(state.calibration_recipe);
  const auto pixels=state.document.image(0).buf;
  const auto folder=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("zip-provenance-%%%%");
  boost::filesystem::create_directories(folder);
  struct Cleanup {boost::filesystem::path folder;~Cleanup(){boost::system::error_code error;boost::filesystem::remove_all(folder,error);}} cleanup{folder};
  for(const char *target:{"uvj","SL1"}) {
    auto changed=state;auto settings=sla::restore_project_settings(*changed.settings);
    settings.set("sla_archive_format",target);changed.settings=sla::capture_project_settings(settings);
    const auto path=folder/(std::string(target)+".sla-project");
    sla::save_layer_project(path.string(),changed);const auto loaded=sla::read_project_layers(path.string());REQUIRE(loaded);
    REQUIRE(loaded->calibration_recipe);CHECK(loaded->calibration_recipe->precision==sla::LayerMaskPrecision::NativeGray4);
    CHECK(bool(loaded->calibration_recipe->requested==std::vector<uint8_t>{16,47}));
    CHECK(bool(loaded->calibration_recipe->effective==std::vector<uint8_t>{17,34}));
    CHECK_FALSE(loaded->document.fresh_job());CHECK(bool(loaded->document.image(0).buf==pixels));
    auto restarted=sla::restore_generated_project(*loaded);
    CHECK_FALSE(restarted->layer_document()->fresh_job());CHECK(bool(restarted->layer_document()->image(0).buf==pixels));
  }
  const auto keep=folder/"keep.sla-project";
  auto rejects_without_publication=[&](const sla::ProjectRasterState &invalid) {
    {std::ofstream out(keep.string());out<<"protected output";}
    CHECK_THROWS(sla::save_layer_project(keep.string(),invalid));
    std::ifstream in(keep.string());const std::string bytes{std::istreambuf_iterator<char>(in),{}};
    CHECK(bytes=="protected output");CHECK(bool(state.document.image(0).buf==pixels));
  };
  for(const auto precision:{sla::LayerMaskPrecision::NativeGray7,sla::bitplane_mask_precision(2),sla::bitplane_mask_precision(16)}) {
    auto invalid=state;auto recipe=std::make_shared<sla::CalibrationRecipe>(*state.calibration_recipe);
    // A self-valid XYZ recipe ensures this tests source-precision domain,
    // rather than merely invalid grayscale requested/effective arithmetic.
    recipe->parameters.pattern=sla::CalibrationPattern::XYZBlocks;recipe->requested.clear();recipe->effective.clear();recipe->precision=precision;
    REQUIRE_NOTHROW(recipe->validate());invalid.calibration_recipe=recipe;rejects_without_publication(invalid);
  }
  {auto invalid=state;invalid.settings.reset();rejects_without_publication(invalid);}
  {auto invalid=state;invalid.origin=sla::ProjectLayerOrigin::ImportedArchive;invalid.positions_z.clear();rejects_without_publication(invalid);}
  {auto invalid=state;auto recipe=std::make_shared<sla::CalibrationRecipe>(*state.calibration_recipe);
    recipe->effective[1]=17;invalid.calibration_recipe=recipe;rejects_without_publication(invalid);}
  // Existing owned source identity/precision checks remain strictly enforced.
  config.set("sla_archive_format","uvj");parameters.pattern=sla::CalibrationPattern::XYZBlocks;
  auto owned=sla::create_calibration(config,parameters);const auto owned_state=sla::capture_generated_project(*owned,*owned->layer_document());
  REQUIRE(owned_state.document.fresh_job());
  {auto invalid=owned_state;auto recipe=std::make_shared<sla::CalibrationRecipe>(*owned_state.calibration_recipe);
    recipe->precision=sla::LayerMaskPrecision::NativeGray4;REQUIRE_NOTHROW(recipe->validate());
    invalid.calibration_recipe=recipe;rejects_without_publication(invalid);}
  {auto invalid=owned_state;auto fresh=*owned_state.document.fresh_job();fresh.producer=sla::FreshLayerProducer::PCB;
    std::vector<sla::EncodedRaster> masks;
    for(size_t i=0;i<owned_state.document.layer_count();++i) {
      const auto bytes=owned_state.document.encoded_layer(i);const auto *first=static_cast<const uint8_t *>(bytes.buf);
      masks.emplace_back(std::vector<uint8_t>(first,first+bytes.sz),"png");
    }
    invalid.document=sla::LayerDocument::from_fresh_rasters(std::move(masks),std::move(fresh));rejects_without_publication(invalid);}
}

TEST_CASE("Anycubic ZIP stores native reference time instead of an integer schedule without setup",
          "[sla_anycubic_zip_time]") {
  const std::string format=GENERATE("pm4u","pm7","pm7m","pwsz");
  const bool portrait=GENERATE(false,true);
  // Baseline8cases plus stationary, dominant wait, fractional two-stage,
  // zero-bottom and all-bottom. The new-edge literals must also be confirmed
  // by unchanged FullOpen on these exact emitted files before final acceptance.
  const int variant=GENERATE(0,1,2,3,4,5);CAPTURE(format,portrait,variant);
  constexpr std::array<double,6> expected_seconds{338.62,319.56,435.36,332.68,327.45,360.96};
  auto config=printer_config();config.set("sla_archive_format",format);
  config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
  config.set("exposure_time",variant==3?3.135:3.12);config.set("initial_exposure_time",variant==3?9.125:9.12);
  config.set("sla_bottom_layers",variant==4?0:variant==5?3:1);config.set("sla_transition_layers",0);
  for(const bool bottom:{false,true}) {
    const std::string prefix=bottom?"sla_bottom_":"sla_";
    const bool stationary=variant==1,fractional=variant==3;
    config.set(prefix+"lift_height",stationary?0.:fractional?(bottom?4.125:3.125):(bottom?7.:5.));
    config.set(prefix+"lift_height2",fractional?(bottom?1.125:2.125):0.);
    config.set(prefix+"lift_speed",stationary?0.:(bottom?60.:90.)+(fractional?.125:0.));
    config.set(prefix+"lift_speed2",stationary?0.:180.+(fractional?.125:0.));
    config.set(prefix+"retract_speed",stationary?0.:(bottom?120.:150.)+(fractional?.125:0.));
    config.set(prefix+"retract_speed2",stationary?0.:240.+(fractional?.125:0.));
    config.set(prefix+"retract_height2",0.);
    config.set(prefix+"wait_before_cure",variant==2?20.:fractional?.125:.7);
    config.set(prefix+"wait_after_cure",0.);config.set(prefix+"wait_after_lift",0.);config.set(prefix+"light_pwm",255);
  }
  sla::CalibrationParameters parameters;parameters.pattern=sla::CalibrationPattern::GrayscalePatches;
  parameters.size_x=parameters.size_y=2.;parameters.margin=1.;parameters.height=.15;
  parameters.grayscale_steps=2;parameters.grayscale_first=16;parameters.grayscale_last=47;
  auto job=sla::create_calibration(config,parameters);REQUIRE(job->layer_document());
  REQUIRE(job->layer_document()->layer_count()==3);CHECK_FALSE(job->layer_document()->fresh_job());
  const char *fixtures=std::getenv("SLA_ANYCUBIC_ZIP_TIME_FIXTURES");
  const auto folder=fixtures?boost::filesystem::path(fixtures)/(format+(portrait?"-portrait-":"-landscape-")+std::to_string(variant)):
    boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("zip-time-%%%%");
  struct Cleanup {boost::filesystem::path folder;bool keep;~Cleanup(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(folder,error);}}} cleanup{folder,fixtures!=nullptr};
  boost::filesystem::create_directories(folder);const auto path=folder/("calibration."+format);
  job->export_print(path.string());
  const auto archive=read_zipper_archive(path.string(),{""},{});
  const auto entry=std::find_if(archive.entries.begin(),archive.entries.end(),[](const auto &e){return e.fname=="print_info.json";});
  REQUIRE(entry!=archive.entries.end());const auto print_info=nlohmann::json::parse(entry->buf);
  REQUIRE(print_info.at("print_time").is_number());
  const auto actual=print_info.at("print_time").get<double>();
  CHECK(std::isfinite(actual));CHECK(actual>0.);
  CHECK(float(actual)==float(expected_seconds[variant]));
  if(fixtures) {
    sla::save_layer_project((folder/"calibration.sla-project").string(),sla::capture_generated_project(*job,*job->layer_document()));
    nlohmann::json expected{{"format",format},{"portrait",portrait},{"variant",variant},
      {"expected_seconds",expected_seconds[variant]},
      {"reference_status",variant==0?"baseline338.62 already FullOpen-verified":"source-derived edge; exact fixture FullOpen gate required"}};
    std::ofstream out((folder/"expected.json").string());out<<expected.dump(2);
  }
}

TEST_CASE("Anycubic ZIP refuses unrepresentable native time before publishing over an existing file",
          "[sla_anycubic_zip_time]") {
  const std::string format=GENERATE("pm4u","pm7","pm7m","pwsz");
  const int defect=GENERATE(0,1,2);CAPTURE(format,defect);
  auto config=printer_config();config.set("sla_archive_format",format);
  config.set("exposure_time",defect==2?1e-40:3.12);config.set("initial_exposure_time",defect==2?1e-40:9.12);
  if(defect==2){config.set("min_exposure_time",0.);config.set("min_initial_exposure_time",0.);}
  config.set("sla_bottom_layers",1);config.set("sla_transition_layers",0);
  for(const char *prefix:{"sla_","sla_bottom_"}) {
    config.set(std::string(prefix)+"lift_height",5.);config.set(std::string(prefix)+"lift_height2",0.);
    config.set(std::string(prefix)+"lift_speed",defect==1?1e-40:90.);
    config.set(std::string(prefix)+"retract_speed",150.);config.set(std::string(prefix)+"retract_height2",0.);
    config.set(std::string(prefix)+"wait_before_cure",defect==0?1e38:.7);
    config.set(std::string(prefix)+"wait_after_cure",0.);config.set(std::string(prefix)+"wait_after_lift",0.);
    config.set(std::string(prefix)+"light_pwm",255);
  }
  const auto original=config;const auto path=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("protected-zip-time-%%%%."+format);
  struct Cleanup {boost::filesystem::path path;~Cleanup(){boost::system::error_code error;boost::filesystem::remove(path,error);}} cleanup{path};
  {std::ofstream output(path.string(),std::ios::binary);output<<"protected previous print";}
  auto attempt=[&] {
    sla::CalibrationParameters parameters;parameters.size_x=parameters.size_y=2.;parameters.margin=1.;parameters.height=.15;
    auto job=sla::create_calibration(config,parameters);job->export_print(path.string());
  };
  // Defect0: finite native waits whose reference float job sum overflows.
  // Defect1: positive native speed rounds to zero in the actual decoder.
  // Defect2: positive exposure collapses to zero on the decoder cure grid.
  CHECK_THROWS(attempt());
  std::ifstream input(path.string(),std::ios::binary);const std::string published{std::istreambuf_iterator<char>(input),{}};
  CHECK(published=="protected previous print");CHECK(config.diff(original).empty());
}

TEST_CASE("Anycubic ZIP finite native time retains float capacity above the old uint32 ceiling",
          "[sla_anycubic_zip_time]") {
  auto config=printer_config();config.set("sla_archive_format","pm7");
  config.set("exposure_time",3.12);config.set("initial_exposure_time",9.12);
  config.set("sla_bottom_layers",1);config.set("sla_transition_layers",0);
  for(const char *prefix:{"sla_","sla_bottom_"}) {
    config.set(std::string(prefix)+"lift_height",5.);config.set(std::string(prefix)+"lift_height2",0.);
    config.set(std::string(prefix)+"lift_speed",90.);config.set(std::string(prefix)+"retract_speed",150.);
    config.set(std::string(prefix)+"retract_height2",0.);config.set(std::string(prefix)+"wait_before_cure",1e10);
    config.set(std::string(prefix)+"wait_after_cure",0.);config.set(std::string(prefix)+"wait_after_lift",0.);
    config.set(std::string(prefix)+"light_pwm",255);
  }
  sla::CalibrationParameters parameters;parameters.size_x=parameters.size_y=2.;parameters.margin=1.;parameters.height=.15;
  auto job=sla::create_calibration(config,parameters);
  const char *fixtures=std::getenv("SLA_ANYCUBIC_ZIP_TIME_FIXTURES");
  const auto folder=fixtures?boost::filesystem::path(fixtures)/"large-float":
    boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("zip-large-time-%%%%");
  struct Cleanup {boost::filesystem::path path;bool keep;~Cleanup(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}} cleanup{folder,fixtures!=nullptr};
  boost::filesystem::create_directories(folder);const auto path=folder/"calibration.pm7";job->export_print(path.string());
  const auto archive=read_zipper_archive(path.string(),{"print_info.json"},{});REQUIRE(archive.entries.size()==1);
  const auto info=nlohmann::json::parse(archive.entries.front().buf);const auto seconds=info.at("print_time").get<double>();
  CHECK(std::isfinite(seconds));CHECK(seconds>double(UINT32_MAX));
  // Three float rows each contribute2e10s. This tests supported native float
  // capacity and the .NET large-float Round guard, not an arbitrary saturation.
  CHECK(float(seconds)==float(6e10));
  if(fixtures)sla::save_layer_project((folder/"calibration.sla-project").string(),sla::capture_generated_project(*job,*job->layer_document()));
}

TEST_CASE("Legacy CBDDLP and Photon v1 calibration plans their actual threshold255 palette",
          "[sla_legacy_binary_calibration_palette]") {
  const std::string format=GENERATE("cbddlp_v1","photon_v1");CAPTURE(format);
  auto config=printer_config();config.set("sla_archive_format",format);
  sla::CalibrationParameters parameters;parameters.pattern=sla::CalibrationPattern::GrayscalePatches;
  parameters.size_x=parameters.size_y=2.;parameters.margin=1.;parameters.height=.15;
  parameters.grayscale_steps=2;parameters.grayscale_first=1;parameters.grayscale_last=15;
  const auto duplicate=sla::calibration_grayscale_plan(config,parameters);
  CHECK(duplicate.precision==sla::LayerMaskPrecision::NativeBitplanes1);
  CHECK(bool(duplicate.requested==std::vector<uint8_t>{1,15}));
  CHECK(bool(duplicate.effective==std::vector<uint8_t>{0,0}));
  CHECK(duplicate.distinct_levels==1);CHECK(duplicate.has_duplicates());
  CHECK_THROWS(sla::create_calibration(config,parameters));
  parameters.grayscale_first=254;parameters.grayscale_last=255;
  const auto positive=sla::calibration_grayscale_plan(config,parameters);
  CHECK(positive.precision==sla::LayerMaskPrecision::NativeBitplanes1);
  CHECK(bool(positive.requested==std::vector<uint8_t>{254,255}));
  CHECK(bool(positive.effective==std::vector<uint8_t>{0,255}));
  CHECK(positive.distinct_levels==2);CHECK_FALSE(positive.has_duplicates());
  // A literal one-plane contract: PWS's threshold128 is a different source
  // quantizer even though both decoded alphabets contain only black/white.
  for(unsigned value=0;value<256;++value) {
    CAPTURE(value);
    CHECK(sla::project_mask_value(uint8_t(value),sla::generated_mask_precision(format))==uint8_t(value==255?255:0));
  }
  CHECK_FALSE(sla::generated_layer_ownership(format,sla::FreshLayerProducer::Calibration));
}

TEST_CASE("Actual legacy binary calibration preserves source masks and recipe across restart",
          "[sla_legacy_binary_calibration_palette]") {
  const std::string format=GENERATE("cbddlp_v1","photon_v1");
  const bool portrait=GENERATE(false,true);CAPTURE(format,portrait);
  auto config=printer_config();config.set("sla_archive_format",format);
  config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
  config.set("exposure_time",3.12);config.set("initial_exposure_time",9.12);
  config.set("sla_bottom_layers",1);config.set("sla_transition_layers",0);
  for(const bool bottom:{false,true}) {
    const std::string key=bottom?"sla_bottom_":"sla_";
    // v1 omits motion records. Use its already-validated decoder defaults,
    // so this pixel test cannot accidentally widen the parameter contract.
    config.set(key+"lift_height",bottom?6.:.05);config.set(key+"lift_speed",100.);
    config.set(key+"retract_speed",100.);config.set(key+"lift_height2",0.);
    config.set(key+"retract_height2",0.);config.set(key+"wait_before_cure",0.);
    config.set(key+"wait_after_cure",0.);config.set(key+"wait_after_lift",0.);
    config.set(key+"light_pwm",255);
  }
  sla::CalibrationParameters parameters;parameters.pattern=sla::CalibrationPattern::GrayscalePatches;
  parameters.size_x=parameters.size_y=2.;parameters.margin=1.;parameters.height=.15;
  parameters.grayscale_steps=2;parameters.grayscale_first=254;parameters.grayscale_last=255;
  auto job=sla::create_calibration(config,parameters);
  REQUIRE(job->layer_document());REQUIRE(job->calibration_recipe());
  CHECK_FALSE(job->layer_document()->fresh_job());CHECK(job->layer_document()->parameter_capabilities().empty());
  // CHECK deliberately allows the actual RED exporter/fixtures to run even
  // while the planner still falsely records a lossless Gray8 source.
  CHECK(job->calibration_recipe()->precision==sla::LayerMaskPrecision::NativeBitplanes1);
  CHECK(bool(job->calibration_recipe()->requested==std::vector<uint8_t>{254,255}));
  CHECK(bool(job->calibration_recipe()->effective==std::vector<uint8_t>{0,255}));
  const char *fixtures=std::getenv("SLA_LEGACY_BINARY_CALIBRATION_FIXTURES");
  const auto folder=fixtures?boost::filesystem::path(fixtures)/(format+(portrait?"-portrait":"-landscape")):
    boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("binary-calibration-%%%%");
  struct Cleanup {boost::filesystem::path folder;bool keep;~Cleanup(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(folder,error);}}} cleanup{folder,fixtures!=nullptr};
  boost::filesystem::create_directories(folder);
  const std::string suffix=format=="cbddlp_v1"?"v1.cbddlp":"v1.photon";
  const size_t columns=portrait?150:400,rows=portrait?400:150;
  auto decode=[&](const boost::filesystem::path &path,bool compare_source) {
    std::ifstream stream(path.string(),std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(stream),{}};
    REQUIRE(bytes.size()>=112);
    auto u32=[&](size_t at) {REQUIRE(at<=bytes.size()-4);return uint32_t(bytes[at])|(uint32_t(bytes[at+1])<<8)|(uint32_t(bytes[at+2])<<16)|(uint32_t(bytes[at+3])<<24);};
    auto real=[&](size_t at) {const auto word=u32(at);float value;std::memcpy(&value,&word,4);return value;};
    CHECK(u32(0)==0x12fd0019u);CHECK(u32(4)==1);CHECK(u32(84)==0);CHECK(u32(92)==1);
    CHECK(u32(52)==columns);CHECK(u32(56)==rows);CHECK(u32(68)==3);CHECK(u32(48)==1);
    CHECK(real(32)==float(.05));CHECK(real(36)==float(3.12));CHECK(real(40)==float(9.12));
    const auto table=size_t(u32(64));REQUIRE(table<=bytes.size());REQUIRE(3*36<=bytes.size()-table);
    std::vector<std::vector<uint8_t>> decoded;
    for(size_t layer=0;layer<3;++layer) {
      const auto entry=table+36*layer;CHECK(real(entry)==float((layer+1)*.05));
      CHECK(real(entry+4)==float(layer?3.12:9.12));
      const auto begin=size_t(u32(entry+12)),length=size_t(u32(entry+16));
      REQUIRE(begin<=bytes.size());REQUIRE(length<=bytes.size()-begin);
      std::vector<uint8_t> mask;mask.reserve(columns*rows);
      // Independent bounded native RLE parser: high bit is white, lower7
      // bits are run count; one plane decodes to literal0/255, not127.
      for(size_t at=begin;at<begin+length;++at) {
        const unsigned count=bytes[at]&127u;REQUIRE(count>0);REQUIRE(count<=columns*rows-mask.size());
        mask.insert(mask.end(),count,(bytes[at]&128u)?uint8_t(255):uint8_t(0));
      }
      REQUIRE(mask.size()==columns*rows);
      if(compare_source) {
        const auto image=job->layer_document()->image(layer);
        CHECK(image.cols==columns);CHECK(image.rows==rows);CHECK(bool(mask==image.buf));
        CHECK(std::all_of(image.buf.begin(),image.buf.end(),[](uint8_t value){return value==0 || value==255;}));
      }
      decoded.push_back(std::move(mask));
    }
    return decoded;
  };
  auto export_and_check=[&](const std::string &name) {
    const auto path=folder/(name+"."+suffix);job->export_print(path.string());return decode(path,true);
  };
  sla::save_layer_project((folder/"calibration.sla-project").string(),sla::capture_generated_project(*job,*job->layer_document()));
  const auto original=export_and_check("calibration");
  const auto state=sla::read_project_layers((folder/"calibration.sla-project").string());REQUIRE(state);
  job=sla::restore_generated_project(*state);REQUIRE(job->calibration_recipe());
  CHECK_FALSE(job->layer_document()->fresh_job());CHECK(job->layer_document()->parameter_capabilities().empty());
  CHECK(job->calibration_recipe()->precision==sla::LayerMaskPrecision::NativeBitplanes1);
  CHECK(bool(job->calibration_recipe()->effective==std::vector<uint8_t>{0,255}));
  const auto restarted=export_and_check("restart");CHECK(bool(original==restarted));
  // Historical quantization records generation, not a mutable export target.
  // Changing a target must preserve edited source bytes without inventing
  // ownership or silently reinterpreting threshold255 as PWS threshold128.
  const auto historical=sla::capture_generated_project(*job,*job->layer_document());
  for(const char *target:{"uvj","SL1"}) {
    auto changed=historical;auto settings=sla::restore_project_settings(*changed.settings);
    settings.set("sla_archive_format",target);changed.settings=sla::capture_project_settings(settings);
    const auto path=folder/(std::string("historical-")+target+".sla-project");
    sla::save_layer_project(path.string(),changed);
    const auto loaded=sla::read_project_layers(path.string());REQUIRE(loaded);REQUIRE(loaded->calibration_recipe);
    CHECK(loaded->calibration_recipe->precision==sla::LayerMaskPrecision::NativeBitplanes1);
    CHECK(bool(loaded->calibration_recipe->effective==std::vector<uint8_t>{0,255}));
    CHECK_FALSE(loaded->document.fresh_job());CHECK(bool(loaded->document.image(0).buf==job->layer_document()->image(0).buf));
    boost::filesystem::remove(path);
  }
  // All256 actual input values pass through the ordinary native writer,
  // independently proving its source quantization law rather than comparing
  // production projectors with themselves. Keep only3 final native fixtures.
  auto edited=*job->layer_document();auto image=edited.image(0);REQUIRE(image.buf.size()>=256);
  for(unsigned value=0;value<256;++value)image.buf[value]=uint8_t(value);
  // Explicit unknown/Gray8 QA input bypasses the new source-editor palette;
  // otherwise replace_image would project before exercising raw writer law.
  std::vector<sla::EncodedRaster> raw_masks;
  for(size_t layer=0;layer<job->layer_document()->layer_count();++layer) {
    const auto raw=layer?job->layer_document()->image(layer):image;
    raw_masks.push_back(sla::PNGRasterEncoder{}(raw.buf.data(),raw.cols,raw.rows,1));
  }
  const auto raw_document=sla::LayerDocument::from_rasters(std::move(raw_masks));
  const auto spectrum_path=folder/("input-spectrum."+suffix);
  job->export_layer_document(raw_document,format,spectrum_path.string());
  const auto spectrum=decode(spectrum_path,false);
  for(unsigned value=0;value<256;++value){CAPTURE(value);CHECK(spectrum[0][value]==uint8_t(value==255?255:0));}
  boost::filesystem::remove(spectrum_path);
  for(unsigned value=0;value<256;++value)image.buf[value]=uint8_t(value==255?255:0);
  edited.replace_image(0,image);job->replace_layer_document(edited);
  export_and_check("palette");
  sla::save_layer_project((folder/"palette.sla-project").string(),sla::capture_generated_project(*job,*job->layer_document()));
}

TEST_CASE("Actual legacy model rasters agree with native masks without inventing parameter ownership",
          "[sla_legacy_model_palette]") {
  const std::string format=GENERATE("pm4u","pm7","pm7m","pwsz","cbddlp_v1","photon_v1");
  const bool portrait=GENERATE(false,true);CAPTURE(format,portrait);
  const bool binary=format=="cbddlp_v1" || format=="photon_v1";
  auto config=printer_config();config.set("sla_archive_format",format);config.set("printer_model","GENERIC");
  config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
  config.set("supports_enable",false);config.set("pad_enable",false);config.set("support_object_elevation",0.);
  config.set("initial_layer_height",.05);config.set("exposure_time",3.12);config.set("initial_exposure_time",9.12);
  config.set("sla_bottom_layers",1);config.set("sla_transition_layers",0);config.set("gamma_correction",1.);
  for(const bool bottom:{false,true}) {
    const std::string key=bottom?"sla_bottom_":"sla_";
    config.set(key+"lift_height",binary?(bottom?6.:.05):5.);
    config.set(key+"lift_speed",binary?100.:90.);config.set(key+"retract_speed",binary?100.:150.);
    config.set(key+"lift_height2",0.);config.set(key+"retract_height2",0.);
    config.set(key+"wait_before_cure",0.);config.set(key+"wait_after_cure",0.);config.set(key+"wait_after_lift",0.);
    config.set(key+"light_pwm",255);
  }
  Model model;auto *object=model.add_object("Owned test mesh legacy palette","",make_cube(4.137,2.313,.15));
  auto *instance=object->add_instance();instance->set_rotation(Vec3d(0.,0.,.371));instance->set_offset(Vec3d(10.,10.,0.));
  SLAPrint print;print.apply(model,config);REQUIRE(print.validate().empty());print.process();
  REQUIRE(print.layer_document());REQUIRE(print.layer_document()->layer_count()==3);
  CHECK_FALSE(print.layer_document()->fresh_job());CHECK(print.layer_document()->parameter_capabilities().empty());
  CHECK_FALSE(sla::generated_layer_ownership(format,sla::FreshLayerProducer::ModelSlice));
  CHECK_FALSE(print.calibration_recipe());
  const char *fixtures=std::getenv("SLA_LEGACY_MODEL_PALETTE_FIXTURES");
  const auto folder=fixtures?boost::filesystem::path(fixtures)/(format+(portrait?"-portrait":"-landscape")):
    boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("model-legacy-palette-%%%%");
  struct Cleanup {boost::filesystem::path folder;bool keep;~Cleanup(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(folder,error);}}} cleanup{folder,fixtures!=nullptr};
  boost::filesystem::create_directories(folder);
  const std::string suffix=binary?(format=="cbddlp_v1"?"v1.cbddlp":"v1.photon"):format;
  const size_t columns=portrait?150:400,rows=portrait?400:150,pixels=columns*rows;
  auto check_native=[&](const boost::filesystem::path &path,const sla::LayerDocument &source) {
    std::vector<std::vector<uint8_t>> decoded;
    if(binary) {
      std::ifstream input(path.string(),std::ios::binary);const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
      REQUIRE(bytes.size()>=112);
      auto word=[&](size_t at){REQUIRE(at<=bytes.size()-4);return uint32_t(bytes[at])|(uint32_t(bytes[at+1])<<8)|(uint32_t(bytes[at+2])<<16)|(uint32_t(bytes[at+3])<<24);};
      CHECK(word(0)==0x12fd0019u);CHECK(word(4)==1);CHECK(word(92)==1);CHECK(word(84)==0);
      CHECK(word(52)==columns);CHECK(word(56)==rows);CHECK(word(68)==3);
      const size_t table=word(64);REQUIRE(table<=bytes.size());REQUIRE(108<=bytes.size()-table);
      for(size_t layer=0;layer<3;++layer) {
        const size_t begin=word(table+36*layer+12),length=word(table+36*layer+16);
        REQUIRE(begin<=bytes.size());REQUIRE(length<=bytes.size()-begin);
        std::vector<uint8_t> mask;mask.reserve(pixels);
        for(size_t at=begin;at<begin+length;++at) {
          const unsigned count=bytes[at]&127;REQUIRE(count>0);REQUIRE(count<=pixels-mask.size());
          mask.insert(mask.end(),count,(bytes[at]&128)?uint8_t(255):uint8_t(0));
        }
        REQUIRE(mask.size()==pixels);decoded.push_back(std::move(mask));
      }
    } else {
      const auto archive=read_zipper_archive(path.string(),{""},{});
      for(size_t layer=0;layer<3;++layer) {
        const auto name="layer_images/layer_"+std::to_string(layer)+".pw0img";
        const auto entry=std::find_if(archive.entries.begin(),archive.entries.end(),[&](const auto &item){return item.fname==name;});
        REQUIRE(entry!=archive.entries.end());std::vector<uint8_t> mask;mask.reserve(pixels);
        for(size_t offset=0;offset<entry->buf.size();) {
          const unsigned token=entry->buf[offset++],level=token>>4;size_t count=token&15;
          if(level==0 || level==15){REQUIRE(offset<entry->buf.size());count=count*256+entry->buf[offset++];}
          REQUIRE(count>0);REQUIRE(count<=pixels-mask.size());mask.insert(mask.end(),count,uint8_t(level*17));
        }
        REQUIRE(mask.size()==pixels);decoded.push_back(std::move(mask));
      }
    }
    for(size_t layer=0;layer<3;++layer) {
      CAPTURE(path.string(),layer);const auto image=source.image(layer);
      CHECK(image.cols==columns);CHECK(image.rows==rows);
      CHECK(bool(decoded[layer]==image.buf));
      CHECK(std::all_of(image.buf.begin(),image.buf.end(),[&](uint8_t value){return binary?(value==0 || value==255):value%17==0;}));
    }
    return decoded;
  };
  const auto source=*print.layer_document();
  const auto direct=folder/("direct."+suffix);print.export_print(direct.string());const auto direct_pixels=check_native(direct,source);
  const auto snapshot=std::make_shared<const sla::ExportSnapshot>(print.capture_export_snapshot());REQUIRE(snapshot->document());
  CHECK_FALSE(snapshot->document()->fresh_job());
  const auto frozen=folder/("frozen."+suffix);
  const sla::ExportRequest request(snapshot,format,frozen.string());request.write();
  const auto frozen_pixels=check_native(frozen,*snapshot->document());CHECK(bool(frozen_pixels==direct_pixels));
  const auto explicit_file=folder/("explicit."+suffix);
  print.export_layer_document(source,format,explicit_file.string());CHECK(bool(check_native(explicit_file,source)==direct_pixels));
  sla::save_layer_project((folder/"source.sla-project").string(),sla::capture_project_layers(model,config,print));
  // This bounded first slice checks representable edits. Arbitrary-intensity
  // source editing needs an immutable mask contract, not mutable target inference.
  auto edited=source;edited.paint(0,1,1,0,binary?255:17);print.replace_layer_document(edited);
  auto history=*print.layer_document();REQUIRE(history.undo());REQUIRE(history.redo());
  CHECK(bool(history.image(0).buf==edited.image(0).buf));
  const auto edited_file=folder/("edited."+suffix);print.export_print(edited_file.string());check_native(edited_file,edited);
  const auto project=folder/"edited.sla-project";
  sla::save_layer_project(project.string(),sla::capture_project_layers(model,config,print));
  const auto loaded=sla::read_project_layers(project.string());REQUIRE(loaded);CHECK_FALSE(loaded->document.fresh_job());
  SLAPrint restored;restored.apply(model,config);restored.queue_project_layers(std::make_shared<const sla::ProjectRasterState>(*loaded));restored.process();
  REQUIRE(restored.layer_document());CHECK_FALSE(restored.project_layer_restore_failed());
  CHECK_FALSE(restored.layer_document()->fresh_job());CHECK(restored.layer_document()->parameter_capabilities().empty());
  for(size_t layer=0;layer<3;++layer)CHECK(bool(restored.layer_document()->image(layer).buf==edited.image(layer).buf));
  const auto restart=folder/("restart."+suffix);restored.export_print(restart.string());check_native(restart,*restored.layer_document());
}

TEST_CASE("Legacy model arbitrary edits preserve an immutable source palette without parameter ownership",
          "[sla_legacy_model_mask_contract]") {
  const std::string format=GENERATE("pm4u","pm7","pm7m","pwsz","cbddlp_v1","photon_v1");
  const bool portrait=GENERATE(false,true);CAPTURE(format,portrait);
  const bool binary=format=="cbddlp_v1" || format=="photon_v1";
  auto config=printer_config();config.set("sla_archive_format",format);config.set("printer_model","GENERIC");
  config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
  config.set("supports_enable",false);config.set("pad_enable",false);config.set("support_object_elevation",0.);
  config.set("initial_layer_height",.05);config.set("exposure_time",3.12);config.set("initial_exposure_time",9.12);
  config.set("sla_bottom_layers",1);config.set("sla_transition_layers",0);config.set("gamma_correction",1.);
  for(const bool bottom:{false,true}) {
    const std::string key=bottom?"sla_bottom_":"sla_";
    config.set(key+"lift_height",binary?(bottom?6.:.05):5.);
    config.set(key+"lift_speed",binary?100.:90.);config.set(key+"retract_speed",binary?100.:150.);
    config.set(key+"lift_height2",0.);config.set(key+"retract_height2",0.);
    config.set(key+"wait_before_cure",0.);config.set(key+"wait_after_cure",0.);config.set(key+"wait_after_lift",0.);
    config.set(key+"light_pwm",255);
  }
  Model model;auto *object=model.add_object("Owned test mesh legacy palette","",make_cube(4.137,2.313,.15));
  auto *instance=object->add_instance();instance->set_rotation(Vec3d(0.,0.,.371));instance->set_offset(Vec3d(10.,10.,0.));
  SLAPrint print;print.apply(model,config);REQUIRE(print.validate().empty());print.process();
  REQUIRE(print.layer_document());REQUIRE(print.layer_document()->layer_count()==3);
  CHECK_FALSE(print.layer_document()->fresh_job());CHECK(print.layer_document()->parameter_capabilities().empty());
  CHECK_FALSE(sla::generated_layer_ownership(format,sla::FreshLayerProducer::ModelSlice));
  CHECK_FALSE(print.calibration_recipe());
  const char *fixtures=std::getenv("SLA_LEGACY_MODEL_MASK_CONTRACT_FIXTURES");
  const auto folder=fixtures?boost::filesystem::path(fixtures)/(format+(portrait?"-portrait":"-landscape")):
    boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("model-legacy-palette-%%%%");
  struct Cleanup {boost::filesystem::path folder;bool keep;~Cleanup(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(folder,error);}}} cleanup{folder,fixtures!=nullptr};
  boost::filesystem::create_directories(folder);
  const std::string suffix=binary?(format=="cbddlp_v1"?"v1.cbddlp":"v1.photon"):format;
  const size_t columns=portrait?150:400,rows=portrait?400:150,pixels=columns*rows;
  auto check_native=[&](const boost::filesystem::path &path,const sla::LayerDocument &source) {
    std::vector<std::vector<uint8_t>> decoded;
    if(binary) {
      std::ifstream input(path.string(),std::ios::binary);const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
      REQUIRE(bytes.size()>=112);
      auto word=[&](size_t at){REQUIRE(at<=bytes.size()-4);return uint32_t(bytes[at])|(uint32_t(bytes[at+1])<<8)|(uint32_t(bytes[at+2])<<16)|(uint32_t(bytes[at+3])<<24);};
      CHECK(word(0)==0x12fd0019u);CHECK(word(4)==1);CHECK(word(92)==1);CHECK(word(84)==0);
      CHECK(word(52)==columns);CHECK(word(56)==rows);CHECK(word(68)==3);
      const size_t table=word(64);REQUIRE(table<=bytes.size());REQUIRE(108<=bytes.size()-table);
      for(size_t layer=0;layer<3;++layer) {
        const size_t begin=word(table+36*layer+12),length=word(table+36*layer+16);
        REQUIRE(begin<=bytes.size());REQUIRE(length<=bytes.size()-begin);
        std::vector<uint8_t> mask;mask.reserve(pixels);
        for(size_t at=begin;at<begin+length;++at) {
          const unsigned count=bytes[at]&127;REQUIRE(count>0);REQUIRE(count<=pixels-mask.size());
          mask.insert(mask.end(),count,(bytes[at]&128)?uint8_t(255):uint8_t(0));
        }
        REQUIRE(mask.size()==pixels);decoded.push_back(std::move(mask));
      }
    } else {
      const auto archive=read_zipper_archive(path.string(),{""},{});
      for(size_t layer=0;layer<3;++layer) {
        const auto name="layer_images/layer_"+std::to_string(layer)+".pw0img";
        const auto entry=std::find_if(archive.entries.begin(),archive.entries.end(),[&](const auto &item){return item.fname==name;});
        REQUIRE(entry!=archive.entries.end());std::vector<uint8_t> mask;mask.reserve(pixels);
        for(size_t offset=0;offset<entry->buf.size();) {
          const unsigned token=entry->buf[offset++],level=token>>4;size_t count=token&15;
          if(level==0 || level==15){REQUIRE(offset<entry->buf.size());count=count*256+entry->buf[offset++];}
          REQUIRE(count>0);REQUIRE(count<=pixels-mask.size());mask.insert(mask.end(),count,uint8_t(level*17));
        }
        REQUIRE(mask.size()==pixels);decoded.push_back(std::move(mask));
      }
    }
    for(size_t layer=0;layer<3;++layer) {
      CAPTURE(path.string(),layer);const auto image=source.image(layer);
      CHECK(image.cols==columns);CHECK(image.rows==rows);
      CHECK(bool(decoded[layer]==image.buf));
      CHECK(std::all_of(image.buf.begin(),image.buf.end(),[&](uint8_t value){return binary?(value==0 || value==255):value%17==0;}));
    }
    return decoded;
  };
  const auto source=*print.layer_document();
  const auto direct=folder/("direct."+suffix);print.export_print(direct.string());const auto direct_pixels=check_native(direct,source);
  const auto snapshot=std::make_shared<const sla::ExportSnapshot>(print.capture_export_snapshot());REQUIRE(snapshot->document());
  CHECK_FALSE(snapshot->document()->fresh_job());
  const auto frozen=folder/("frozen."+suffix);
  const sla::ExportRequest request(snapshot,format,frozen.string());request.write();
  const auto frozen_pixels=check_native(frozen,*snapshot->document());CHECK(bool(frozen_pixels==direct_pixels));
  const auto explicit_file=folder/("explicit."+suffix);
  print.export_layer_document(source,format,explicit_file.string());CHECK(bool(check_native(explicit_file,source)==direct_pixels));
  sla::save_layer_project((folder/"source.sla-project").string(),sla::capture_project_layers(model,config,print));
  auto check_manifest=[&](const boost::filesystem::path &path) {
    const auto archive=read_zipper_archive(path.string(),{"metadata/sla/layers.json"},{});
    REQUIRE(archive.entries.size()==1);const auto metadata=nlohmann::json::parse(archive.entries.front().buf);
    CHECK(metadata.at("version")==8);const auto &document=metadata.at("document");
    CHECK_FALSE(document.contains("fresh_job"));CHECK(document.contains("mask_contract"));
    if(document.contains("mask_contract")) {
      const auto &contract=document.at("mask_contract");CHECK(contract.is_object());CHECK(contract.size()==2);
      CHECK(contract.value("version",0)==1);CHECK(contract.value("precision",-1)==(binary?2:18));
    }
  };
  check_manifest(folder/"source.sla-project");
  // Requested bytes must be projected before becoming source/editor/Undo
  // data. These laws are literal tested contracts, independent of production.
  auto edited=source;auto spectrum=edited.image(0);REQUIRE(spectrum.buf.size()>=256);
  for(unsigned value=0;value<256;++value)spectrum.buf[value]=uint8_t(value);
  edited.replace_image(0,spectrum);
  for(unsigned value=0;value<256;++value) {
    CAPTURE(value);CHECK(edited.image(0).buf[value]==uint8_t(binary?(value==255?255:0):17*(value/16)));
  }
  edited.paint(0,1,1,0,128);
  CHECK(edited.image(0).get(1,1)==uint8_t(binary?0:136));
  const auto before_operation=edited.image(0).buf;
  REQUIRE(edited.transform_layers(0,0,sla::LayerDocument::MaskOperation::Fill,127));
  const auto filled=edited.image(0);
  CHECK(std::all_of(filled.buf.begin(),filled.buf.end(),[&](uint8_t value){return value==uint8_t(binary?0:119);}));
  REQUIRE(edited.undo());CHECK(bool(edited.image(0).buf==before_operation));
  REQUIRE(edited.redo());REQUIRE(edited.undo());CHECK(bool(edited.image(0).buf==before_operation));
  print.replace_layer_document(edited);
  auto history=*print.layer_document();REQUIRE(history.undo());REQUIRE(history.redo());
  CHECK(bool(history.image(0).buf==edited.image(0).buf));
  const auto edited_file=folder/("edited."+suffix);print.export_print(edited_file.string());check_native(edited_file,edited);
  const auto project=folder/"edited.sla-project";
  sla::save_layer_project(project.string(),sla::capture_project_layers(model,config,print));
  check_manifest(project);
  const auto loaded=sla::read_project_layers(project.string());REQUIRE(loaded);CHECK_FALSE(loaded->document.fresh_job());
  SLAPrint restored;restored.apply(model,config);restored.queue_project_layers(std::make_shared<const sla::ProjectRasterState>(*loaded));restored.process();
  REQUIRE(restored.layer_document());CHECK_FALSE(restored.project_layer_restore_failed());
  CHECK_FALSE(restored.layer_document()->fresh_job());CHECK(restored.layer_document()->parameter_capabilities().empty());
  for(size_t layer=0;layer<3;++layer)CHECK(bool(restored.layer_document()->image(layer).buf==edited.image(layer).buf));
  const auto restart=folder/("restart."+suffix);restored.export_print(restart.string());check_native(restart,*restored.layer_document());
  // Historical contract survives a different target without recomputing
  // provenance from that target. These temporary projects are not printfiles.
  for(const char *target:{"uvj","SL1"}) {
    auto changed=*loaded;auto settings=sla::restore_project_settings(*changed.settings);
    settings.set("sla_archive_format",target);changed.settings=sla::capture_project_settings(settings);
    const auto path=folder/(std::string("history-")+target+".sla-project");
    sla::save_layer_project(path.string(),changed);check_manifest(path);
    const auto historical=sla::read_project_layers(path.string());REQUIRE(historical);
    CHECK(bool(historical->document.image(0).buf==loaded->document.image(0).buf));
    auto brush=historical->document;brush.paint(0,2,1,0,128);
    CHECK(brush.image(0).get(1,2)==uint8_t(binary?0:136));
    boost::filesystem::remove(path);
  }
  // Actual descriptor-absent legacy PNG state is still supported. A current
  // printer target cannot retrospectively invent source palette provenance.
  auto legacy=*loaded;std::vector<sla::EncodedRaster> legacy_masks;
  for(size_t layer=0;layer<legacy.document.layer_count();++layer) {
    const auto image=legacy.document.image(layer);
    legacy_masks.push_back(sla::PNGRasterEncoder{}(image.buf.data(),image.cols,image.rows,1));
  }
  legacy.document=sla::LayerDocument::from_rasters(std::move(legacy_masks));
  const auto legacy_path=folder/"legacy-absence.sla-project";
  sla::save_layer_project(legacy_path.string(),legacy);
  const auto legacy_archive=read_zipper_archive(legacy_path.string(),{"metadata/sla/layers.json"},{});
  REQUIRE(legacy_archive.entries.size()==1);const auto legacy_json=nlohmann::json::parse(legacy_archive.entries.front().buf);
  CHECK(legacy_json.at("version")==2);CHECK_FALSE(legacy_json.at("document").contains("mask_contract"));
  const auto legacy_loaded=sla::read_project_layers(legacy_path.string());REQUIRE(legacy_loaded);
  auto unknown=legacy_loaded->document;unknown.paint(0,2,1,0,128);
  CHECK(unknown.image(0).get(1,2)==128);CHECK_FALSE(unknown.fresh_job());
  boost::filesystem::remove(legacy_path);
}

TEST_CASE("Independent mask contracts validate source bytes and preserve atomic editor history",
          "[sla_legacy_mask_contract_validation]") {
  const bool binary=GENERATE(false,true);CAPTURE(binary);
  const auto precision=binary?sla::LayerMaskPrecision::NativeBitplanes1:sla::LayerMaskPrecision::NativeGray4;
  png::ImageGreyscale image;image.cols=256;image.rows=1;image.buf.resize(256);
  for(unsigned i=0;i<256;++i)image.buf[i]=i%2?255:0;
  auto masks=[&](const png::ImageGreyscale &mask) {
    std::vector<sla::EncodedRaster> result;
    for(unsigned i=0;i<3;++i)result.push_back(sla::PNGRasterEncoder{}(mask.buf.data(),mask.cols,mask.rows,1));
    return result;
  };
  auto document=sla::LayerDocument::from_precision_rasters(masks(image),precision);
  CHECK(document.mask_contract_precision()==precision);CHECK_FALSE(document.fresh_job());
  CHECK(document.parameter_capabilities().empty());CHECK(document.archive_format().empty());
  const auto snapshot=document.snapshot();CHECK(snapshot.mask_contract_precision()==precision);
  REQUIRE_THROWS(sla::LayerDocument::from_precision_rasters(masks(image),precision,
      [](size_t,size_t){return false;}));
  CHECK(bool(document.image(0).buf==image.buf));CHECK_FALSE(document.can_undo());
  for(const auto invalid:{sla::LayerMaskPrecision::Gray8,sla::LayerMaskPrecision::NativeGray7,
                         sla::bitplane_mask_precision(2),sla::bitplane_mask_precision(16)})
    CHECK_THROWS(sla::LayerDocument::from_precision_rasters(masks(image),invalid));
  auto invalid_image=image;invalid_image.buf[0]=1;
  CHECK_THROWS(sla::LayerDocument::from_precision_rasters(masks(invalid_image),precision));
  CHECK_FALSE(document.transform_layers(0,2,sla::LayerDocument::MaskOperation::Fill,127,
      [](size_t done,size_t){return done==0;}));
  CHECK_FALSE(document.can_undo());CHECK(document.revision()==0);CHECK(document.mask_contract_precision()==precision);
  for(size_t layer=0;layer<3;++layer)CHECK(bool(document.image(layer).buf==image.buf));
  auto requested=image;for(unsigned i=0;i<256;++i)requested.buf[i]=uint8_t(i);
  document.replace_image(0,requested);const auto projected=document.image(0);
  for(unsigned i=0;i<256;++i) {
    CAPTURE(i);CHECK(projected.buf[i]==uint8_t(binary?(i==255?255:0):17*(i/16)));
  }
  const auto revision=document.revision();auto same=projected;same.buf[0]=1;
  document.replace_image(0,same);CHECK(document.revision()==revision);
  REQUIRE(document.undo());CHECK(bool(document.image(0).buf==image.buf));
  REQUIRE(document.redo());CHECK(bool(document.image(0).buf==projected.buf));
  CHECK(document.mask_contract_precision()==precision);CHECK(bool(snapshot.image(0).buf==image.buf));
  using Op=sla::LayerDocument::MaskOperation;
  for(const auto operation:{Op::Threshold,Op::Invert,Op::MirrorHorizontal,Op::MirrorVertical,Op::Rotate180,
      Op::Fill,Op::Erode,Op::Dilate,Op::Open,Op::Close,Op::Gradient,Op::WhiteTopHat,Op::BlackTopHat,
      Op::BoxBlur,Op::MedianBlur}) {
    CAPTURE(int(operation));auto edited=document;
    REQUIRE(edited.transform_layers(0,2,operation,127,{},1));
    for(size_t layer=0;layer<3;++layer) {
      const auto result=edited.image(layer);
      CHECK(std::all_of(result.buf.begin(),result.buf.end(),[&](uint8_t value){return binary?(value==0||value==255):value%17==0;}));
    }
    CHECK(edited.mask_contract_precision()==precision);
  }
}

TEST_CASE("Project8 mask contract rejects schema forgery downgrade and noncanonical PNG without publication",
          "[sla_legacy_mask_contract_validation]") {
  const bool binary=GENERATE(false,true);CAPTURE(binary);
  const auto precision=binary?sla::LayerMaskPrecision::NativeBitplanes1:sla::LayerMaskPrecision::NativeGray4;
  auto config=printer_config();config.set("sla_archive_format",std::string("pm7"));
  config.set("display_width",4.);config.set("display_height",3.);config.set("display_pixels_x",4);config.set("display_pixels_y",3);
  png::ImageGreyscale image;image.cols=4;image.rows=3;image.buf={0,255,0,255,255,0,255,0,0,255,0,255};
  std::vector<sla::EncodedRaster> masks;
  for(unsigned i=0;i<3;++i)masks.push_back(sla::PNGRasterEncoder{}(image.buf.data(),image.cols,image.rows,1));
  auto document=sla::LayerDocument::from_precision_rasters(std::move(masks),precision);
  auto job=SLAPrint::create_raster_job(config,document,{.05,.1,.15});
  const auto state=sla::capture_generated_project(*job,*job->layer_document());
  const auto folder=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("mask-contract-schema-%%%%");
  boost::filesystem::create_directories(folder);
  struct Cleanup {boost::filesystem::path path;~Cleanup(){boost::system::error_code error;boost::filesystem::remove_all(path,error);}} cleanup{folder};
  const auto source=folder/"source.sla-project";sla::save_layer_project(source.string(),state);
  const auto archive=read_zipper_archive(source.string(),{""},{});
  const auto manifest=std::find_if(archive.entries.begin(),archive.entries.end(),[](const auto &entry){return entry.fname=="metadata/sla/layers.json";});
  REQUIRE(manifest!=archive.entries.end());const auto original=nlohmann::json::parse(manifest->buf);
  CHECK(original.at("version")==8);CHECK(original.at("document").at("mask_contract").size()==2);
  const auto loaded=sla::read_project_layers(source.string());REQUIRE(loaded);
  CHECK(loaded->document.mask_contract_precision()==precision);CHECK_FALSE(loaded->document.fresh_job());
  CHECK(bool(loaded->document.image(0).buf==image.buf));
  // Live setters and project publication enforce the same authoritative
  // source/recipe agreement; a self-valid XYZ recipe is not an escape hatch.
  auto mismatch=std::make_shared<sla::CalibrationRecipe>();
  mismatch->precision=binary?sla::LayerMaskPrecision::NativeGray4:sla::LayerMaskPrecision::NativeBitplanes1;
  REQUIRE_NOTHROW(mismatch->validate());CHECK_THROWS(job->set_calibration_recipe(mismatch));
  const auto protected_file=folder/"protected.sla-project";
  auto protected_bytes=[&] {std::ifstream in(protected_file.string(),std::ios::binary);return std::string{std::istreambuf_iterator<char>(in),{}};};
  auto protect=[&] {{std::ofstream out(protected_file.string());out<<"previous valid project";}};
  {auto invalid=state;invalid.calibration_recipe=mismatch;protect();
    CHECK_THROWS(sla::save_layer_project(protected_file.string(),invalid));CHECK(protected_bytes()=="previous valid project");}
  for(unsigned defect=0;defect<27;++defect) {
    CAPTURE(defect);auto metadata=original;auto &contract=metadata["document"]["mask_contract"];
    switch(defect) {
      case 0:contract.erase("version");break;
      case 1:contract["version"]=0;break;
      case 2:contract["version"]=2;break;
      case 3:contract["version"]=true;break;
      case 4:contract["version"]=1.0;break;
      case 5:contract["precision"]=0;break;
      case 6:contract["precision"]=1;break;
      case 7:contract["precision"]=3;break;
      case 8:contract["precision"]=17;break;
      case 9:contract["precision"]=19;break;
      case 10:contract["precision"]=-1;break;
      case 11:contract["precision"]=true;break;
      case 12:contract["precision"]=double(int(precision));break;
      case 13:contract["precision"]="18";break;
      case 14:contract.erase("precision");break;
      case 15:contract["unused_projection"]=0;break;
      case 16:contract=nullptr;break;
      case 17:contract=nlohmann::json::array();break;
      case 18:metadata["version"]=2;break;
      case 19:metadata["version"]=6;break;
      case 20:metadata["document"].erase("mask_contract");break;
      case 21:metadata["document"]["fresh_job"]=nlohmann::json::object();break;
      case 22:metadata["document"]["archive_format"]="SL1";break;
      case 23:metadata["document"]["extra"]=false;break;
      case 24:metadata["version"]=1;break;
      case 25:contract["version"]="1";break;
      default:break; // duplicate precision key in raw JSON below
    }
    auto serialized=metadata.dump();
    if(defect==26) {
      const std::string key="\"precision\":"+std::to_string(int(precision));
      const auto at=serialized.find(key);REQUIRE(at!=std::string::npos);
      serialized.insert(at,key+",");
    }
    const auto invalid_file=folder/"invalid.sla-project";
    {Zipper zip(invalid_file.string());for(const auto &entry:archive.entries) {
      if(entry.fname==manifest->fname){zip.add_entry(entry.fname);zip<<serialized;}
      else zip.add_entry(entry.fname,entry.buf.data(),entry.buf.size());
    }zip.finalize();}
    protect();auto attempt=[&] {const auto invalid=sla::read_project_layers(invalid_file.string());
      REQUIRE(invalid);sla::save_layer_project(protected_file.string(),*invalid);};
    CHECK_THROWS(attempt());CHECK(protected_bytes()=="previous valid project");
    CHECK(bool(state.document.image(0).buf==image.buf));CHECK(state.document.mask_contract_precision()==precision);
  }
  // A declared palette cannot normalize corrupted persisted evidence. The
  // descriptor remains intact while an actual encoded PNG contains illegal1.
  const auto corrupt_file=folder/"illegal-pixel.sla-project";
  auto corrupt=image;corrupt.buf[0]=1;const auto bad_png=sla::PNGRasterEncoder{}(corrupt.buf.data(),corrupt.cols,corrupt.rows,1);
  {Zipper zip(corrupt_file.string());for(const auto &entry:archive.entries) {
    if(entry.fname=="metadata/sla/document/00000000.bin")zip.add_entry(entry.fname,bad_png.data(),bad_png.size());
    else zip.add_entry(entry.fname,entry.buf.data(),entry.buf.size());
  }zip.finalize();}
  CHECK_THROWS(sla::read_project_layers(corrupt_file.string()));
}

TEST_CASE("Guarded editor Apply preserves immutable mask contract continuity without side effects",
          "[sla_mask_contract_apply]") {
  const std::string action=GENERATE("loss","change","introduction","same contract");CAPTURE(action);
  auto config=printer_config();config.set("sla_archive_format",std::string("pm7"));
  config.set("display_width",4.);config.set("display_height",3.);config.set("display_pixels_x",4);config.set("display_pixels_y",3);
  auto make_document=[](std::optional<sla::LayerMaskPrecision> precision) {
    const std::vector<uint8_t> pixels(12,0);std::vector<sla::EncodedRaster> masks;
    masks.push_back(sla::PNGRasterEncoder{}(pixels.data(),4,3,1));
    return precision?sla::LayerDocument::from_precision_rasters(std::move(masks),*precision):
        sla::LayerDocument::from_rasters(std::move(masks));
  };
  const auto active=action=="introduction"?std::optional<sla::LayerMaskPrecision>{}:
      std::optional<sla::LayerMaskPrecision>{sla::LayerMaskPrecision::NativeGray4};
  const auto incoming=action=="loss"?std::optional<sla::LayerMaskPrecision>{}:
      std::optional<sla::LayerMaskPrecision>{action=="change"?sla::LayerMaskPrecision::NativeBitplanes1:
                                                              sla::LayerMaskPrecision::NativeGray4};
  unsigned cancellations=0;
  auto source=SLAPrint::create_raster_job(config,make_document(active),{.05});
  auto session=sla::SlicedResultSession::capture(*source);
  const auto expected=source->export_generation_guard();const auto revision=source->layer_document()->revision();
  const auto pixels=source->layer_document()->image(0).buf;const auto statistics=source->print_statistics();
  auto replacement=make_document(incoming);
  // Both alphabets accept the same all-zero PNG. A rejection must therefore
  // occur at guarded Apply, not a malformed factory or geometry validator.
  CHECK(bool(replacement.image(0).buf==pixels));CHECK(replacement.parameter_capabilities().empty());
  if(action=="same contract")replacement.paint(0,1,1,0,255);
  session->document()=replacement;
  source->set_cancel_callback([&]{++cancellations;});
  if(action=="same contract") {
    REQUIRE_NOTHROW(session->apply(*source));
    CHECK(cancellations==1);CHECK_FALSE(expected.current());CHECK(session->source_current());
    CHECK(source->layer_document()->mask_contract_precision()==active);
    CHECK(bool(source->layer_document()->image(0).buf==replacement.image(0).buf));
    CHECK(source->layer_document()->revision()==replacement.revision());
    CHECK(session->document().can_undo());CHECK_FALSE(session->document().dirty());
  } else {
    CHECK_THROWS(session->apply(*source));
    CHECK(cancellations==0);CHECK(expected.current());CHECK(session->source_current());
    CHECK(source->layer_document()->mask_contract_precision()==active);
    CHECK(source->layer_document()->revision()==revision);CHECK(bool(source->layer_document()->image(0).buf==pixels));
    const auto &after=source->print_statistics();
    CHECK(after.time_basis==statistics.time_basis);CHECK(after.raster_area_mm2==statistics.raster_area_mm2);
    CHECK(after.estimated_print_time==statistics.estimated_print_time);
    CHECK(after.estimated_print_time_tolerance==statistics.estimated_print_time_tolerance);
    CHECK(after.objects_used_material==statistics.objects_used_material);CHECK(after.support_used_material==statistics.support_used_material);
    CHECK(after.total_cost==statistics.total_cost);CHECK(after.total_weight==statistics.total_weight);
    CHECK(after.slow_layers_count==statistics.slow_layers_count);CHECK(after.fast_layers_count==statistics.fast_layers_count);
    CHECK(bool(after.layers_times_running_total==statistics.layers_times_running_total));
    CHECK(bool(after.layers_areas==statistics.layers_areas));
    // Whole-document replacement is deliberately a different API. Neither
    // this explicit adoption nor historical cache restore may inherit a
    // palette by guessing from the current printer's target format.
    REQUIRE_NOTHROW(source->replace_layer_document(replacement));
    CHECK(source->layer_document()->mask_contract_precision()==incoming);
    CHECK(bool(source->layer_document()->image(0).buf==replacement.image(0).buf));
  }
  source->set_cancel_callback([]{});
}

TEST_CASE("Actual PCB raster geometry projects proven legacy palettes before source PNG creation",
          "[sla_pcb_legacy_palette]") {
  const std::string format=GENERATE("pm4u","pm7","pm7m","pwsz","cbddlp_v1","photon_v1");
  const bool portrait=GENERATE(false,true);CAPTURE(format,portrait);
  const bool binary=format=="cbddlp_v1" || format=="photon_v1";
  auto config=printer_config();config.set("sla_archive_format",format);config.set("gamma_correction",1.);
  config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
  const auto first=sla::parse_gerber(prefix+"%ADD10C,3.137*%D10*X100000Y100000D03*M02*");
  const auto second=sla::parse_gerber(prefix+"%ADD10C,2.313*%D10*X170000Y130000D03*M02*");
  SLAPrinterConfig printer;printer.apply(config,true);
  const auto actual=sla::rasterize_pcb_layers({first,second},printer,false);
  auto reference_config=config;reference_config.set("sla_archive_format",std::string("uvj"));
  SLAPrinterConfig reference_printer;reference_printer.apply(reference_config,true);
  const auto gray8=sla::rasterize_pcb_layers({first,second},reference_printer,false);
  REQUIRE(actual.layer_count()==2);CHECK_FALSE(actual.fresh_job());CHECK(actual.parameter_capabilities().empty());
  CHECK(actual.mask_contract_precision()==(binary?sla::LayerMaskPrecision::NativeBitplanes1:sla::LayerMaskPrecision::NativeGray4));
  bool actual_aa=false;
  for(size_t layer=0;layer<2;++layer) {
    const auto source=gray8.image(layer),result=actual.image(layer);auto expected=source.buf;
    for(auto &pixel:expected) {
      actual_aa|=pixel!=0 && pixel!=255;
      pixel=binary?uint8_t(pixel==255?255:0):uint8_t(17*(pixel/16));
    }
    CHECK(result.cols==size_t(portrait?150:400));CHECK(result.rows==size_t(portrait?400:150));
    CHECK(bool(result.buf==expected));
  }
  // Curved physical artwork must exercise actual intermediate raster levels;
  // an aligned white rectangle would miss the original source/native defect.
  CHECK(actual_aa);
}

TEST_CASE("Stationary legacy PCB jobs preserve ZIP masks and explicitly refuse unsupported v1 motion",
          "[sla_pcb_legacy_palette]") {
  const std::string format=GENERATE("pm4u","pm7","pm7m","pwsz","cbddlp_v1","photon_v1");
  const bool portrait=GENERATE(false,true);const bool invert=GENERATE(false,true);CAPTURE(format,portrait,invert);
  const bool binary=format=="cbddlp_v1" || format=="photon_v1";
  auto config=printer_config();config.set("sla_archive_format",format);config.set("gamma_correction",1.);
  config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
  const auto accepted=config;
  const auto first=sla::parse_gerber(prefix+"%ADD10C,3.137*%D10*X100000Y100000D03*M02*");
  const auto second=sla::parse_gerber(prefix+"%ADD10C,2.313*%D10*X170000Y130000D03*M02*");
  const char *fixtures=std::getenv("SLA_PCB_LEGACY_PALETTE_FIXTURES");
  const auto folder=fixtures?boost::filesystem::path(fixtures)/(format+(portrait?"-portrait":"-landscape")+(invert?"-inverted":"-normal")):
      boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("pcb-legacy-palette-%%%%");
  boost::filesystem::create_directories(folder);
  struct Cleanup {boost::filesystem::path folder;bool keep;~Cleanup(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(folder,error);}}} cleanup{folder,fixtures!=nullptr};
  if(binary) {
    const auto keep=folder/"protected.v1.print";{std::ofstream out(keep.string());out<<"previous stationary output";}
    std::string reason;
    try {auto invalid=sla::create_pcb_exposure_job(config,{first,second},false,{},30.12,invert);invalid->export_print(keep.string());}
    catch(const std::exception &error){reason=error.what();}
    CHECK(reason.find("Chitubox v1 cannot store custom lift/retract")!=std::string::npos);
    std::ifstream input(keep.string());const std::string bytes{std::istreambuf_iterator<char>(input),{}};
    CHECK(bytes=="previous stationary output");CHECK(config.diff(accepted).empty());
    // No executable v1 PCB file is claimed: the schema omits stationary
    // motion, and the existing factory already refuses those unrepresentable0s.
    return;
  }
  auto job=sla::create_pcb_exposure_job(config,{first,second},false,{},30.12,invert);
  REQUIRE(job->layer_document());REQUIRE(job->layer_document()->layer_count()==2);
  CHECK_FALSE(job->layer_document()->fresh_job());CHECK(job->layer_document()->parameter_capabilities().empty());
  CHECK(job->layer_document()->mask_contract_precision()==sla::LayerMaskPrecision::NativeGray4);
  CHECK(config.diff(accepted).empty());
  auto readback=[&](const boost::filesystem::path &path,const sla::LayerDocument &source) {
    const auto archive=read_zipper_archive(path.string(),{""},{});
    for(size_t layer=0;layer<2;++layer) {
      const auto name="layer_images/layer_"+std::to_string(layer)+".pw0img";
      const auto entry=std::find_if(archive.entries.begin(),archive.entries.end(),[&](const auto &e){return e.fname==name;});
      REQUIRE(entry!=archive.entries.end());const auto image=source.image(layer);std::vector<uint8_t> decoded;
      for(size_t offset=0;offset<entry->buf.size();) {
        const unsigned token=entry->buf[offset++],level=token>>4;size_t count=token&15;
        if(level==0 || level==15){REQUIRE(offset<entry->buf.size());count=count*256+entry->buf[offset++];}
        REQUIRE(count>0);REQUIRE(count<=image.buf.size()-decoded.size());decoded.insert(decoded.end(),count,uint8_t(level*17));
      }
      REQUIRE(decoded.size()==image.buf.size());CHECK(bool(decoded==image.buf));
      CHECK(std::all_of(image.buf.begin(),image.buf.end(),[](uint8_t pixel){return pixel%17==0;}));
    }
  };
  const auto path=folder/("direct."+format);job->export_print(path.string());readback(path,*job->layer_document());
  const auto snapshot=std::make_shared<const sla::ExportSnapshot>(job->capture_export_snapshot());
  const auto frozen=folder/("frozen."+format);const sla::ExportRequest request(snapshot,format,frozen.string());request.write();
  REQUIRE(snapshot->document());readback(frozen,*snapshot->document());
  const auto project=folder/"pcb.sla-project";
  sla::save_layer_project(project.string(),sla::capture_generated_project(*job,*job->layer_document()));
  const auto archive=read_zipper_archive(project.string(),{"metadata/sla/layers.json"},{});
  REQUIRE(archive.entries.size()==1);const auto metadata=nlohmann::json::parse(archive.entries.front().buf);
  CHECK(metadata.at("version")==8);CHECK(metadata.at("document").contains("mask_contract"));
  const auto loaded=sla::read_project_layers(project.string());REQUIRE(loaded);
  CHECK(loaded->document.mask_contract_precision()==sla::LayerMaskPrecision::NativeGray4);
  auto restored=sla::restore_generated_project(*loaded);CHECK_FALSE(restored->layer_document()->fresh_job());
  const auto restart=folder/("restart."+format);restored->export_print(restart.string());readback(restart,*restored->layer_document());
}

TEST_CASE("Editor value projection describes actual immutable and unknown source behavior",
          "[sla_layer_edit_projection]") {
  const std::string domain=GENERATE("unknown","contract4","contract1","fresh4","fresh7","fresh1","fresh3");
  CAPTURE(domain);const bool binary=domain=="contract1" || domain=="fresh1";
  auto config=printer_config();config.set("sla_archive_format",std::string("SL1"));
  sla::CalibrationParameters parameters;parameters.size_x=parameters.size_y=2.;parameters.margin=1.;parameters.height=.05;
  auto seed=sla::create_calibration(config,parameters);REQUIRE(seed->layer_document()->fresh_job());
  const auto source_image=seed->layer_document()->image(0);
  const auto width=source_image.cols,height=source_image.rows;
  std::vector<uint8_t> zeros(width*height,0);
  auto masks=[&]{std::vector<sla::EncodedRaster> result;result.push_back(sla::PNGRasterEncoder{}(zeros.data(),width,height,1));return result;};
  sla::LayerDocument document;
  if(domain=="unknown")document=sla::LayerDocument::from_rasters(masks());
  else if(domain=="contract4" || domain=="contract1")
    document=sla::LayerDocument::from_precision_rasters(masks(),binary?sla::LayerMaskPrecision::NativeBitplanes1:sla::LayerMaskPrecision::NativeGray4);
  else {
    auto fresh=*seed->layer_document()->fresh_job();
    fresh.mask_precision=domain=="fresh4"?sla::LayerMaskPrecision::NativeGray4:
      domain=="fresh7"?sla::LayerMaskPrecision::NativeGray7:
      sla::bitplane_mask_precision(domain=="fresh3"?3:1);
    document=sla::LayerDocument::from_fresh_rasters(masks(),fresh);
  }
  const auto revision=document.revision();const bool undo=document.can_undo();
  const auto lut=document.edit_value_projection();
  CHECK(document.revision()==revision);CHECK(document.can_undo()==undo);CHECK(bool(document.image(0).buf==zeros));
  auto requested=document.image(0);for(unsigned value=0;value<256;++value)requested.buf[value]=uint8_t(value);
  auto edited=document;edited.replace_image(0,requested);const auto actual=edited.image(0);
  std::array<bool,256> seen{};
  for(unsigned value=0;value<256;++value) {
    CAPTURE(value);unsigned expected=value;
    if(domain=="contract4" || domain=="fresh4")expected=17*(value/16);
    else if(binary)expected=value==255?255:0;
    else if(domain=="fresh7")expected=value<2?0:(value|1);
    else if(domain=="fresh3") {const unsigned count=std::min(3u,(value+1)/85);expected=count?count*85-1:0;}
    CHECK(lut[value]==expected);CHECK(actual.buf[value]==lut[value]);seen[lut[value]]=true;
  }
  const auto distinct=std::count(seen.begin(),seen.end(),true);
  CHECK(distinct==(domain=="unknown"?256:domain=="fresh7"?128:domain=="fresh3"?4:binary?2:16));
  CHECK(bool(document.image(0).buf==zeros));CHECK(document.mask_contract_precision()==edited.mask_contract_precision());
}

TEST_CASE("Editor value projection uses native source PBM Anycubic and Chitubox laws",
          "[sla_layer_edit_projection]") {
  const std::string format=GENERATE("pbm","pws","pwc","cbddlp_v1","cbddlp","ctb","ctb_encrypted","cxdlpv4");
  CAPTURE(format);
  const auto folder=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("brush-source-projection-%%%%");
  boost::filesystem::create_directories(folder);
  struct Cleanup {boost::filesystem::path folder;~Cleanup(){boost::system::error_code error;boost::filesystem::remove_all(folder,error);}} cleanup{folder};
  const auto suffix=format=="cbddlp_v1"?std::string("v1.cbddlp"):format=="ctb_encrypted"?std::string("encrypted.ctb"):format;
  const auto path=folder/("source."+suffix);
  if(format=="pbm") {
    std::ofstream out(path.string(),std::ios::binary);out<<"P4\n256 1\n";
    const std::array<char,32> zeros{};out.write(zeros.data(),zeros.size());
  } else {
    SLAFullPrintConfig full;Test::configure_archive_fixture(full,format);full.set("sla_bottom_layers",1);full.set("sla_transition_layers",0);
    if(format=="cbddlp")full.set("sla_cbddlp_aa",3);
    DynamicPrintConfig config;config.apply(full);
    const std::vector<uint8_t> zeros(size_t(full.display_pixels_x.value)*full.display_pixels_y.value,0);
    auto document=sla::LayerDocument::from_rasters({sla::PNGRasterEncoder{}(zeros.data(),full.display_pixels_x.value,full.display_pixels_y.value,1)});
    auto job=SLAPrint::create_raster_job(config,document,{.05});job->export_print(path.string());
  }
  const auto document=sla::LayerDocument::load_archive(path.string());
  const auto before=document.image(0);const auto revision=document.revision();const auto lut=document.edit_value_projection();
  auto requested=before;REQUIRE(requested.buf.size()>=256);
  for(unsigned value=0;value<256;++value)requested.buf[value]=uint8_t(value);
  auto edited=document;edited.replace_image(0,requested);const auto actual=edited.image(0);
  for(unsigned value=0;value<256;++value) {
    CAPTURE(value);unsigned expected=value;
    if(format=="pbm")expected=value?255:0; // PBM is NOT threshold255 or128.
    else if(format=="pws")expected=value>=128?255:0;
    else if(format=="pwc")expected=17*(value/16);
    else if(format=="cbddlp_v1")expected=value==255?255:0;
    else if(format=="cbddlp") {const unsigned count=std::min(3u,(value+1)/85);expected=count?count*85-1:0;}
    else expected=value<2?0:(value|1);
    CHECK(lut[value]==expected);CHECK(actual.buf[value]==lut[value]);
  }
  CHECK(document.revision()==revision);CHECK(bool(document.image(0).buf==before.buf));
}
TEST_CASE("Merged overlapping legacy PCB ZIP jobs preserve their canonical source through native restart",
          "[sla_pcb_legacy_merged_palette]") {
  const std::string format=GENERATE("pm4u","pm7","pm7m","pwsz");
  const bool portrait=GENERATE(false,true);const bool invert=GENERATE(false,true);CAPTURE(format,portrait,invert);
  auto config=printer_config();config.set("sla_archive_format",format);config.set("gamma_correction",1.);
  config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
  const auto accepted=config;
  const auto first=sla::parse_gerber(prefix+"%ADD10C,3.137*%D10*X100000Y100000D03*M02*");
  const auto second=sla::parse_gerber(prefix+"%ADD10C,2.313*%D10*X111000Y102000D03*M02*");
  const char *fixtures=std::getenv("SLA_PCB_LEGACY_MERGED_FIXTURES");
  const auto folder=fixtures?boost::filesystem::path(fixtures)/(format+(portrait?"-portrait":"-landscape")+(invert?"-inverted":"-normal")):
      boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("pcb-legacy-palette-%%%%");
  boost::filesystem::create_directories(folder);
  struct Cleanup {boost::filesystem::path folder;bool keep;~Cleanup(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(folder,error);}}} cleanup{folder,fixtures!=nullptr};
  sla::PCBPlacement placement;placement.center=false;
  auto job=sla::create_pcb_exposure_job(config,{first,second},true,placement,30.12,invert);
  REQUIRE(job->layer_document());REQUIRE(job->layer_document()->layer_count()==1);
  CHECK_FALSE(job->layer_document()->fresh_job());CHECK(job->layer_document()->parameter_capabilities().empty());
  CHECK(job->layer_document()->mask_contract_precision()==sla::LayerMaskPrecision::NativeGray4);
  CHECK(config.diff(accepted).empty());
  auto gray_config=config;gray_config.set("sla_archive_format",std::string("uvj"));
  SLAPrinterConfig gray_printer;gray_printer.apply(gray_config,true);
  auto raw=sla::rasterize_pcb_layers({first,second},gray_printer,true,placement).image(0);
  for(auto &value:raw.buf) {if(invert)value=uint8_t(255-value);value=uint8_t(17*(value/16));}
  CHECK(bool(job->layer_document()->image(0).buf==raw.buf));
  // Literal circle-interior/overlap/background samples, independent of
  // Gerber boolean union and the Gray8 reference renderer. Coordinates are
  // physical mm with centering explicitly disabled, no display mirrors.
  const auto merged=job->layer_document()->image(0);
  auto sample=[&](double x,double y,bool copper) {
    const size_t physical_x=size_t(std::floor(x*10.));
    const size_t physical_y=size_t(std::floor(y*5.));
    const size_t column=portrait?149-physical_y:physical_x;
    const size_t row=portrait?399-physical_x:149-physical_y;
    REQUIRE(column<merged.cols);REQUIRE(row<merged.rows);
    const auto expected=uint8_t(copper!=invert?255:0);
    CAPTURE(x,y,column,row,copper,expected);
    CHECK(merged.buf[row*merged.cols+column]==expected);
  };
  sample(9.,10.,true);       // only circle1, radius1.5685 about10,10
  sample(10.5,10.1,true);    // inside BOTH circles: overlap must not erase
  sample(11.8,10.2,true);    // only circle2, radius1.1565 about11.1,10.2
  sample(14.,10.,false);    // background beyond both circles
  sample(10.5,12.,false);   // background above both circles
  auto readback=[&](const boost::filesystem::path &path,const sla::LayerDocument &source) {
    const auto archive=read_zipper_archive(path.string(),{""},{});
    for(size_t layer=0;layer<1;++layer) {
      const auto name="layer_images/layer_"+std::to_string(layer)+".pw0img";
      const auto entry=std::find_if(archive.entries.begin(),archive.entries.end(),[&](const auto &e){return e.fname==name;});
      REQUIRE(entry!=archive.entries.end());const auto image=source.image(layer);std::vector<uint8_t> decoded;
      for(size_t offset=0;offset<entry->buf.size();) {
        const unsigned token=entry->buf[offset++],level=token>>4;size_t count=token&15;
        if(level==0 || level==15){REQUIRE(offset<entry->buf.size());count=count*256+entry->buf[offset++];}
        REQUIRE(count>0);REQUIRE(count<=image.buf.size()-decoded.size());decoded.insert(decoded.end(),count,uint8_t(level*17));
      }
      REQUIRE(decoded.size()==image.buf.size());CHECK(bool(decoded==image.buf));
      CHECK(std::all_of(image.buf.begin(),image.buf.end(),[](uint8_t pixel){return pixel%17==0;}));
    }
  };
  const auto path=folder/("direct."+format);job->export_print(path.string());readback(path,*job->layer_document());
  const auto snapshot=std::make_shared<const sla::ExportSnapshot>(job->capture_export_snapshot());
  const auto frozen=folder/("frozen."+format);const sla::ExportRequest request(snapshot,format,frozen.string());request.write();
  REQUIRE(snapshot->document());readback(frozen,*snapshot->document());
  const auto project=folder/"pcb.sla-project";
  sla::save_layer_project(project.string(),sla::capture_generated_project(*job,*job->layer_document()));
  const auto archive=read_zipper_archive(project.string(),{"metadata/sla/layers.json"},{});
  REQUIRE(archive.entries.size()==1);const auto metadata=nlohmann::json::parse(archive.entries.front().buf);
  CHECK(metadata.at("version")==8);CHECK(metadata.at("layers")==1);CHECK(metadata.at("document").contains("mask_contract"));
  const auto loaded=sla::read_project_layers(project.string());REQUIRE(loaded);
  CHECK(loaded->document.mask_contract_precision()==sla::LayerMaskPrecision::NativeGray4);
  auto restored=sla::restore_generated_project(*loaded);CHECK_FALSE(restored->layer_document()->fresh_job());
  const auto restart=folder/("restart."+format);restored->export_print(restart.string());readback(restart,*restored->layer_document());
}

TEST_CASE("Inverted PCB generation cancels within its own request without publishing a partial job",
          "[sla_pcb_invert_cancellation]") {
  auto config = printer_config();
  config.set("sla_archive_format", std::string("pm7"));
  config.set("gamma_correction", 1.);
  const auto original = config;
  const auto first = sla::parse_gerber(prefix + "%ADD10C,3.137*%D10*X100000Y100000D03*M02*");
  const auto second = sla::parse_gerber(prefix + "%ADD10C,2.313*%D10*X170000Y130000D03*M02*");
  auto plain = sla::create_pcb_exposure_job(config, {first, second}, false, {}, 30.12, false);
  size_t successful_checks = 0;
  auto inverted = sla::create_pcb_exposure_job(config, {first, second}, false, {}, 30.12, true,
                                             [&] { ++successful_checks; });
  REQUIRE(successful_checks > 4);
  const auto existing_pixels = plain->layer_document()->image(0).buf;
  const auto existing_precision = plain->layer_document()->mask_contract_precision();
  const auto folder = boost::filesystem::temp_directory_path() /
                      boost::filesystem::unique_path("pcb-invert-cancel-%%%%");
  boost::filesystem::create_directories(folder);
  struct Cleanup {
    boost::filesystem::path path;
    ~Cleanup() { boost::system::error_code error; boost::filesystem::remove_all(path, error); }
  } cleanup{folder};
  const auto output = folder / "protected.pm7";
  // Cancellation samples the same successful inverted request's checkpoints.
  // It requires no extra pass, no comparison to normal polarity, and no exact
  // absolute callback count or wall-clock timing. Midpoint is not a phase label.
  for (const size_t allowance : {size_t(1), successful_checks / 2, successful_checks - 1}) {
    CAPTURE(allowance, successful_checks);
    { std::ofstream stream(output.string()); stream << "previous accepted PCB"; }
    size_t checks = 0;
    bool requested = false;
    std::unique_ptr<SLAPrint> incomplete;
    auto generate_and_publish = [&] {
      incomplete = sla::create_pcb_exposure_job(config, {first, second}, false, {}, 30.12, true, [&] {
        if (++checks >= allowance) { requested = true; throw sla::LayerOperationCancelled(); }
      });
      incomplete->export_print(output.string());
    };
    CHECK_THROWS_AS(generate_and_publish(), sla::LayerOperationCancelled);
    CHECK(requested);
    CHECK_FALSE(incomplete);
    std::ifstream stream(output.string(), std::ios::binary);
    const std::string published{std::istreambuf_iterator<char>(stream), {}};
    CHECK(bool(published == "previous accepted PCB"));
    CHECK(config.diff(original).empty());
    CHECK(bool(plain->layer_document()->image(0).buf == existing_pixels));
    CHECK(plain->layer_document()->mask_contract_precision() == existing_precision);
    auto retry = sla::create_pcb_exposure_job(config, {first, second}, false, {}, 30.12, true);
    REQUIRE(retry->layer_document()->layer_count() == 2);
    for (size_t layer = 0; layer < 2; ++layer)
      CHECK(bool(retry->layer_document()->image(layer).buf == inverted->layer_document()->image(layer).buf));
  }
}

TEST_CASE("PCB factory polarity is a clean generation baseline across ownership and project restart",
          "[sla_pcb_generation_baseline]") {
  const std::string format=GENERATE("pm7","uvj");
  const bool portrait=GENERATE(false,true),merge=GENERATE(false,true),inverted=GENERATE(false,true);
  CAPTURE(format,portrait,merge,inverted);
  auto config=printer_config();config.set("sla_archive_format",format);config.set("gamma_correction",1.);
  config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
  const auto configuration_before=config;
  using F = sla::LayerParameter;
  const auto first=sla::parse_gerber(prefix+"%ADD10C,3.137*%D10*X100000Y100000D03*M02*");
  const auto second=sla::parse_gerber(prefix+"%ADD10C,2.313*%D10*X170000Y130000D03*M02*");
  auto plain=sla::create_pcb_exposure_job(config,{first,second},merge,{},30.12,false);
  auto job=sla::create_pcb_exposure_job(config,{first,second},merge,{},30.12,inverted);
  REQUIRE(job->layer_document());const auto &generated=*job->layer_document();
  const size_t count=merge?1:2;REQUIRE(generated.layer_count()==count);
  CHECK_FALSE(generated.dirty());CHECK_FALSE(generated.can_undo());CHECK_FALSE(generated.can_redo());
  CHECK(config.diff(configuration_before).empty());
  CHECK(bool(generated.fresh_job())==(format=="uvj"));
  CHECK(generated.mask_contract_precision()==(format=="pm7"?
      std::optional<sla::LayerMaskPrecision>(sla::LayerMaskPrecision::NativeGray4):std::nullopt));
  for(size_t i=0;i<count;++i) {
    auto expected=plain->layer_document()->image(i).buf;
    if(inverted)for(auto &value:expected)value=uint8_t(255-value);
    CHECK(bool(generated.image(i).buf==expected));
  }
  // Polarity must not manufacture parameter ownership or replace the stationary recipe.
  const auto source_config = job->full_print_config();
  CHECK(source_config.diff(plain->full_print_config()).empty());
  CHECK(source_config.opt_float("exposure_time") == 30.12);
  CHECK(source_config.opt_float("initial_exposure_time") == 30.12);
  CHECK(source_config.opt_int("sla_bottom_layers") == 0);
  CHECK(source_config.opt_int("sla_transition_layers") == 0);
  for (const auto prefix : {"sla_", "sla_bottom_"})
    for (const auto name : {"lift_height", "lift_height2", "retract_height2", "lift_speed",
                           "lift_speed2", "retract_speed", "retract_speed2",
                           "wait_before_cure", "wait_after_cure", "wait_after_lift"})
      CHECK(source_config.opt_float(std::string(prefix) + name) == 0.);
  if (generated.fresh_job()) {
    const auto &fresh = *generated.fresh_job();
    CHECK(fresh.producer == sla::FreshLayerProducer::PCB);
    CHECK(fresh.globals.bottom_layers == 0);
    CHECK(fresh.globals.transition_layers == 0);
    REQUIRE(fresh.rows.size() == count);
    for (size_t layer = 0; layer < count; ++layer) {
      CHECK(fresh.rows[layer].at(F::PositionZ) == (layer + 1) * .05);
      CHECK(fresh.rows[layer].at(F::Exposure) == 30.12);
      CHECK(fresh.rows[layer].at(F::LightPWM) == 255.);
      for (const auto field : {F::LiftDistance, F::LiftDistance2, F::LiftSpeed,
                              F::LiftSpeed2, F::RetractDistance2, F::RetractSpeed,
                              F::RetractSpeed2, F::LightOff, F::WaitBefore,
                              F::WaitAfter, F::WaitAfterLift})
        CHECK(fresh.rows[layer].at(field) == 0.);
    }
  } else {
    CHECK(generated.parameter_capabilities().empty());
  }
  // Generation settings are not the first user edit. Undo of the first actual
  // edit returns exactly the generated polarity, with no older edit below it.
  auto editing=generated;const auto baseline=editing.image(0);auto painted=baseline;
  painted.buf[0]=painted.buf[0]?0:255;editing.replace_image(0,painted);
  REQUIRE(editing.can_undo());REQUIRE(editing.undo());
  CHECK(bool(editing.image(0).buf==baseline.buf));CHECK_FALSE(editing.dirty());
  CHECK_FALSE(editing.can_undo());CHECK(editing.can_redo());
  REQUIRE(editing.redo());CHECK(bool(editing.image(0).buf==painted.buf));
  CHECK(bool(generated.image(0).buf==baseline.buf));
  const char *fixture=std::getenv("SLA_PCB_GENERATION_BASELINE_FIXTURES");
  const auto folder=fixture?boost::filesystem::path(fixture)/(format+(portrait?"-portrait":"-landscape")+
      (merge?"-merged":"-separate")+(inverted?"-inverted":"-normal")):
      boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("pcb-generation-baseline-%%%%");
  boost::filesystem::create_directories(folder);
  struct Cleanup {boost::filesystem::path path;bool keep;~Cleanup(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}} cleanup{folder,fixture!=nullptr};
  const auto path=folder/"generation.sla-project";
  const auto captured = sla::capture_generated_project(*job, generated);
  CHECK(captured.origin == sla::ProjectLayerOrigin::GeneratedRaster);
  REQUIRE(captured.positions_z.size() == count);
  REQUIRE(job->print_layers().size() == count);
  for (size_t layer = 0; layer < count; ++layer) {
    const coord_t expected_level = coord_t(layer + 1) * 50000; // 0.05 mm on the geometry grid.
    CHECK(job->print_layers()[layer].level() == expected_level);
    // Owned rows retain DOUBLE Z; unowned source heights retain the exact
    // geometry grid. Neither path may silently replace the other's values.
    const double expected_z = generated.fresh_job()
        ? generated.fresh_job()->rows[layer].at(F::PositionZ)
        : double(expected_level) * SCALING_FACTOR;
    CHECK(captured.positions_z[layer] == expected_z);
  }
  sla::save_layer_project(path.string(), captured);
  const auto state=sla::read_project_layers(path.string());REQUIRE(state);
  auto restored=sla::restore_generated_project(*state);REQUIRE(restored->layer_document());
  const auto &restarted=*restored->layer_document();
  CHECK_FALSE(restarted.can_undo());CHECK_FALSE(restarted.can_redo());CHECK_FALSE(restarted.dirty());
  CHECK(restarted.mask_contract_precision()==generated.mask_contract_precision());
  CHECK(bool(restarted.fresh_job())==bool(generated.fresh_job()));
  for(size_t i=0;i<count;++i)CHECK(bool(restarted.image(i).buf==generated.image(i).buf));
  const auto native = folder / ("original." + format);
  const auto restart_native = folder / ("restart." + format);
  job->export_print(native.string());
  restored->export_print(restart_native.string());
  auto read_native_masks = [&](const boost::filesystem::path &output) {
    if (format == "uvj") {
      const auto imported = sla::LayerDocument::load_archive(output.string());
      REQUIRE(imported.layer_count() == count);
      for (size_t layer = 0; layer < count; ++layer)
        CHECK(bool(imported.image(layer).buf == generated.image(layer).buf));
      return;
    }
    // Independent bounded PW0 run decoding; production encoder/importer is not reused.
    const auto archive = read_zipper_archive(output.string(), {""}, {});
    for (size_t layer = 0; layer < count; ++layer) {
      const auto name = "layer_images/layer_" + std::to_string(layer) + ".pw0img";
      const auto entry = std::find_if(archive.entries.begin(), archive.entries.end(),
                                    [&](const auto &item) { return item.fname == name; });
      REQUIRE(entry != archive.entries.end());
      const auto image = generated.image(layer);
      std::vector<uint8_t> decoded;
      for (size_t offset = 0; offset < entry->buf.size();) {
        const unsigned token = entry->buf[offset++], level = token >> 4;
        size_t run = token & 15;
        if (level == 0 || level == 15) {
          REQUIRE(offset < entry->buf.size());
          run = run * 256 + entry->buf[offset++];
        }
        REQUIRE(run > 0);
        REQUIRE(decoded.size() <= image.buf.size());
        REQUIRE(run <= image.buf.size() - decoded.size());
        decoded.insert(decoded.end(), run, uint8_t(level * 17));
      }
      REQUIRE(decoded.size() == image.buf.size());
      CHECK(bool(decoded == image.buf));
    }
  };
  read_native_masks(native);
  read_native_masks(restart_native);
  CHECK_FALSE(generated.dirty());
  CHECK(config.diff(configuration_before).empty());
  if(fixture) {
    const nlohmann::json evidence={{"format",format},{"portrait",portrait},{"merged",merge},{"inverted",inverted},
      {"layers",count},{"factory_can_undo",generated.can_undo()},{"factory_can_redo",generated.can_redo()},
      {"factory_dirty",generated.dirty()},{"restart_can_undo",restarted.can_undo()},
      {"restart_can_redo",restarted.can_redo()},{"restart_dirty",restarted.dirty()}};
    std::ofstream out((folder/"baseline.json").string());out<<evidence.dump(2)<<'\n';
  }
}

TEST_CASE("First PNG canonical polarity matches real editor Invert for every source precision",
          "[sla_pcb_first_png_projection]") {
  const int id = GENERATE(Catch::Generators::range(0, 19));
  const bool inverted = GENERATE(false, true);
  CAPTURE(id, inverted);
  const auto precision = static_cast<sla::LayerMaskPrecision>(id);
  std::vector<uint8_t> pixels(256);
  for (unsigned value = 0; value < 256; ++value) pixels[value] = uint8_t(value);
  const auto source = pixels;
  const sla::ProjectedPNGMaskEncoder encoder{precision, {}, inverted};
  auto png = encoder(pixels.data(), 16, 16, 1);
  REQUIRE(png.size() > 0);
  std::vector<sla::EncodedRaster> masks;
  masks.push_back(std::move(png));
  const auto document = sla::LayerDocument::from_rasters(std::move(masks));
  const auto image = document.image(0);
  REQUIRE(image.buf.size() == pixels.size());
  const auto literal_projection = [id](unsigned value) {
    if (id == 18) return 17u * (value / 16);
    if (id == 1) return value < 2 ? 0u : (value | 1u);
    if (id >= 2 && id <= 17) {
      const unsigned planes = unsigned(id - 1), step = 256 / planes;
      const unsigned count = value ? std::min(planes, (value + 1) / step) : 0;
      return count ? count * step - 1 : 0u;
    }
    return value;
  };
  for (unsigned value = 0; value < 256; ++value) {
    CAPTURE(value);
    unsigned expected = literal_projection(value);
    if (inverted) expected = literal_projection(255 - expected);
    CHECK(unsigned(image.buf[value]) == expected);
  }
  // Compare the new encoding path with the actual existing editor semantics,
  // using a complete owned source descriptor instead of guessing a target law.
  auto config = printer_config();
  config.set("sla_archive_format", "uvj");
  sla::CalibrationParameters parameters;
  parameters.size_x = parameters.size_y = 2.;
  parameters.margin = 1.; parameters.height = .05;
  auto seed = sla::create_calibration(config, parameters);
  REQUIRE(seed->layer_document()->fresh_job());
  auto fresh = *seed->layer_document()->fresh_job();
  fresh.mask_precision = precision;
  fresh.geometry.columns = fresh.geometry.rows = 16;
  fresh.geometry.width_mm = fresh.geometry.height_mm = 16.;
  const sla::ProjectedPNGMaskEncoder normal{precision, {}};
  std::vector<sla::EncodedRaster> reference_masks;
  reference_masks.push_back(normal(pixels.data(), 16, 16, 1));
  auto reference = sla::LayerDocument::from_fresh_rasters(std::move(reference_masks), fresh);
  if (inverted)
    reference.transform_layers(0, 0, sla::LayerDocument::MaskOperation::Invert);
  CHECK(bool(reference.image(0).buf == image.buf));
  if (inverted && id == 1) {
    CHECK(unsigned(image.buf[3]) == 253);  // Q(255 - Q(3)), not raw252.
    CHECK(unsigned(image.buf[0]) == 255);
    CHECK(unsigned(image.buf[255]) == 0);
  }
  if (inverted && id == 4) {  // Three exposure planes: canonical0,84,169,254.
    CHECK(unsigned(image.buf[0]) == 254);
    CHECK(unsigned(image.buf[84]) == 169);
    CHECK(unsigned(image.buf[169]) == 84);
    CHECK(unsigned(image.buf[255]) == 0);
  }
  CHECK(bool(pixels == source));
}

TEST_CASE("Inverted first PNG validates grayscale bounds and cancellation without publishing",
          "[sla_pcb_first_png_projection]") {
  std::vector<uint8_t> pixels(256, 127);
  const auto source = pixels;
  const sla::ProjectedPNGMaskEncoder inverted{sla::LayerMaskPrecision::Gray8, {}, true};
  CHECK_THROWS_AS(inverted(nullptr, 16, 16, 1), RuntimeError);
  CHECK_THROWS_AS(inverted(pixels.data(), 0, 16, 1), RuntimeError);
  CHECK_THROWS_AS(inverted(pixels.data(), 16, 16, 4), RuntimeError);
  CHECK_THROWS_AS(inverted(pixels.data(), 32769, 1, 1), RuntimeError);
  sla::EncodedRaster published(std::vector<uint8_t>{9, 8, 7}, "sentinel");
  bool visited = false;
  const sla::ProjectedPNGMaskEncoder cancelled{sla::LayerMaskPrecision::Gray8,
      [&] { visited = true; throw sla::LayerOperationCancelled(); }, true};
  CHECK_THROWS_AS(published = cancelled(pixels.data(), 16, 16, 1), sla::LayerOperationCancelled);
  CHECK(visited);
  CHECK(published.size() == 3);
  CHECK(std::memcmp(published.data(), "\x09\x08\x07", 3) == 0);
  CHECK(bool(pixels == source));
  const auto retry = inverted(pixels.data(), 16, 16, 1);
  CHECK(retry.size() > 0);
  CHECK(bool(pixels == source));
  // The new trailing flag must preserve old two-member aggregate construction.
  const sla::ProjectedPNGMaskEncoder old_form{sla::LayerMaskPrecision::Gray8, {}};
  const sla::ProjectedPNGMaskEncoder explicit_false{sla::LayerMaskPrecision::Gray8, {}, false};
  const auto a = old_form(pixels.data(), 16, 16, 1);
  const auto b = explicit_false(pixels.data(), 16, 16, 1);
  REQUIRE(a.size() == b.size());
  CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);
}

TEST_CASE("First PNG inversion forwards cancellation during native row encoding without publication",
          "[sla_pcb_first_png_projection]") {
  const auto precision = GENERATE(sla::LayerMaskPrecision::Gray8, sla::LayerMaskPrecision::NativeGray4);
  CAPTURE(int(precision));
  constexpr size_t columns = 64, rows = 128;
  std::vector<uint8_t> pixels(columns * rows);
  for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = uint8_t(i % 256);
  const auto source = pixels;
  // The existing PNG encoder checks at each row. With a tiny, single-chunk
  // source this allowance reaches row encoding after projection, without
  // requiring an extra editor pass or an exact total checkpoint count.
  size_t visits = 0;
  const size_t row_allowance = rows / 2;
  sla::EncodedRaster published(std::vector<uint8_t>{9, 8, 7}, "sentinel");
  const sla::ProjectedPNGMaskEncoder encoder{precision, [&] {
    if (++visits >= row_allowance) throw sla::LayerOperationCancelled();
  }, true};
  CHECK_THROWS_AS(published = encoder(pixels.data(), columns, rows, 1), sla::LayerOperationCancelled);
  CHECK(visits >= row_allowance);
  CHECK(published.size() == 3);
  CHECK(std::memcmp(published.data(), "\x09\x08\x07", 3) == 0);
  CHECK(bool(pixels == source));
  const sla::ProjectedPNGMaskEncoder retry{precision, {}, true};
  const auto complete = retry(pixels.data(), columns, rows, 1);
  REQUIRE(complete.size() > 0);
  std::vector<sla::EncodedRaster> masks;
  masks.push_back(complete);
  const auto document = sla::LayerDocument::from_rasters(std::move(masks));
  const auto image = document.image(0);
  REQUIRE(image.buf.size() == pixels.size());
  for (size_t i = 0; i < pixels.size(); ++i) {
    const unsigned expected = precision == sla::LayerMaskPrecision::Gray8
        ? 255u - pixels[i] : 255u - 17u * (pixels[i] / 16);
    CHECK(unsigned(image.buf[i]) == expected);
  }
  CHECK(bool(pixels == source));
}

namespace {
DynamicPrintConfig gray4_anycubic_calibration_config(const std::string &format,unsigned version,bool portrait) {
  auto config=printer_config();config.set("sla_archive_format",format);
  config.set("sla_anycubic_version",std::to_string(version));config.set("printer_model",format=="pmx2"?"Photon Mono X2":"Photon Mono X 6K / M3 Plus");
  const double width=format=="pmx2"?196.61:198.15,height=format=="pmx2"?122.88:123.84;
  config.set("display_width",portrait?height:width);config.set("display_height",portrait?width:height);
  const auto bed_width=std::to_string(portrait?height:width),bed_height=std::to_string(portrait?width:height);
  config.set_deserialize_strict("bed_shape","0x0,"+bed_width+"x0,"+bed_width+"x"+bed_height+",0x"+bed_height);
  config.set("max_print_height",format=="pmx2"?200.:245.);
  config.set("display_pixels_x",400);config.set("display_pixels_y",250);
  config.option("display_orientation")->setInt(portrait?sladoPortrait:sladoLandscape);
  config.set("display_mirror_x",false);config.set("display_mirror_y",false);config.set("gamma_correction",1.);
  config.set("layer_height",.05);config.set("initial_layer_height",.05);
  config.set("exposure_time",2.);config.set("initial_exposure_time",10.);
  config.set("sla_bottom_layers",1);config.set("sla_transition_layers",0);
  for(const char *prefix:{"sla_","sla_bottom_"}) {
    const std::string key(prefix);config.set(key+"lift_height",3.);config.set(key+"lift_height2",0.);
    config.set(key+"lift_speed",60.);config.set(key+"lift_speed2",0.);
    config.set(key+"retract_speed",120.);config.set(key+"retract_height2",0.);config.set(key+"retract_speed2",0.);
    config.set(key+"wait_before_cure",0.);config.set(key+"wait_after_cure",0.);config.set(key+"wait_after_lift",0.);
    config.set(key+"light_pwm",255);
  }
  config.set("material_notes",std::string(""),true);config.set("printer_notes",std::string(""),true);
  return config;
}
std::vector<std::vector<uint8_t>> read_gray4_anycubic_fixture(const boost::filesystem::path &path,unsigned version,size_t columns,size_t rows) {
  std::ifstream input(path.string(),std::ios::binary);const std::vector<uint8_t> raw{std::istreambuf_iterator<char>(input),{}};
  REQUIRE(raw.size()>=48);
  auto u32=[&](size_t at){REQUIRE(at<=raw.size()-4);return uint32_t(raw[at])|(uint32_t(raw[at+1])<<8)|(uint32_t(raw[at+2])<<16)|(uint32_t(raw[at+3])<<24);};
  auto number=[&](size_t at){const auto word=u32(at);float f;std::memcpy(&f,&word,4);return f;};
  CHECK(std::string(raw.begin(),raw.begin()+8)=="ANYCUBIC");CHECK(u32(12)==version);
  const size_t header=u32(20),table=u32(36);REQUIRE(header<=raw.size());REQUIRE(96<=raw.size()-header);
  CHECK(u32(header+60)==columns);CHECK(u32(header+64)==rows);
  CHECK(number(header+20)==float(.05));CHECK(number(header+24)==2.f);CHECK(number(header+32)==10.f);
  REQUIRE(table<=raw.size());REQUIRE(20<=raw.size()-table);REQUIRE(u32(table+16)==3);
  REQUIRE(3*32<=raw.size()-table-20);std::vector<std::vector<uint8_t>> decoded;
  for(size_t layer=0;layer<3;++layer) {
    const auto at=table+20+layer*32;size_t cursor=u32(at),size=u32(at+4);
    REQUIRE(cursor<=raw.size());REQUIRE(size<=raw.size()-cursor);const size_t end=cursor+size;
    std::vector<uint8_t> image;
    while(cursor<end) {
      const unsigned token=raw[cursor++],bucket=token>>4;size_t count=token&15;
      if(bucket==0 || bucket==15){REQUIRE(cursor<end);count=count*256+raw[cursor++];}
      REQUIRE(count>0);REQUIRE(count<=columns*rows-image.size());
      image.insert(image.end(),count,uint8_t(bucket*17));
    }
    REQUIRE(image.size()==columns*rows);CHECK(number(at+16)==float(layer?2.:10.));
    decoded.push_back(std::move(image));
  }
  return decoded;
}
}
TEST_CASE("PMX2 and PWMB version matrix plans native4 without widening parameter ownership",
          "[sla_anycubic_515_calibration_palette]") {
  const std::string format=GENERATE("pmx2","pwmb");const unsigned version=GENERATE(515u,516u,517u);
  const bool portrait=GENERATE(false,true);CAPTURE(format,version,portrait);
  auto config=gray4_anycubic_calibration_config(format,version,portrait);
  CHECK(sla::generated_anycubic_version(format,"auto")==515);
  CHECK(sla::generated_anycubic_version(format,std::to_string(version))==version);
  const bool owned=version>=516;
  CHECK(bool(sla::generated_layer_ownership(format,sla::FreshLayerProducer::Calibration,std::to_string(version)))==owned);
  sla::CalibrationParameters p;p.pattern=sla::CalibrationPattern::GrayscalePatches;
  p.size_x=p.size_y=8.;p.margin=2.;p.height=.15;p.grayscale_steps=2;p.grayscale_first=1;p.grayscale_last=15;
  const auto collapsed=sla::calibration_grayscale_plan(config,p);
  CHECK(collapsed.precision==sla::LayerMaskPrecision::NativeGray4);
  CHECK(bool(collapsed.requested==std::vector<uint8_t>{1,15}));CHECK(bool(collapsed.effective==std::vector<uint8_t>{0,0}));
  CHECK(collapsed.has_duplicates());CHECK_THROWS(sla::create_calibration(config,p));
  p.grayscale_first=16;p.grayscale_last=47;const auto positive=sla::calibration_grayscale_plan(config,p);
  CHECK(positive.precision==sla::LayerMaskPrecision::NativeGray4);
  CHECK(bool(positive.requested==std::vector<uint8_t>{16,47}));CHECK(bool(positive.effective==std::vector<uint8_t>{17,34}));
  CHECK_FALSE(positive.has_duplicates());
  constexpr std::array<unsigned,16> literal{0,17,34,51,68,85,102,119,136,153,170,187,204,221,238,255};
  const auto precision=sla::generated_mask_precision(format,1,std::to_string(version));
  for(unsigned value=0;value<256;++value)CHECK(unsigned(sla::project_mask_value(uint8_t(value),precision))==literal[value/16]);
}
TEST_CASE("PMX2 and PWMB legacy calibration native PNG identity survives export and project restart",
          "[sla_anycubic_515_calibration_palette]") {
  const std::string format=GENERATE("pmx2","pwmb");const unsigned version=GENERATE(515u,516u,517u);
  const bool portrait=GENERATE(false,true);CAPTURE(format,version,portrait);
  auto config=gray4_anycubic_calibration_config(format,version,portrait);const auto initial=config;
  sla::CalibrationParameters p;p.pattern=sla::CalibrationPattern::GrayscalePatches;
  p.size_x=p.size_y=8.;p.margin=2.;p.height=.15;p.grayscale_steps=2;p.grayscale_first=16;p.grayscale_last=47;
  auto job=sla::create_calibration(config,p);REQUIRE(job->layer_document());REQUIRE(job->calibration_recipe());
  CHECK(job->calibration_recipe()->precision==sla::LayerMaskPrecision::NativeGray4);
  CHECK(bool(job->calibration_recipe()->requested==std::vector<uint8_t>{16,47}));
  CHECK(bool(job->calibration_recipe()->effective==std::vector<uint8_t>{17,34}));
  const bool owned=version>=516;CHECK(bool(job->layer_document()->fresh_job())==owned);
  if(owned) {CHECK_FALSE(job->layer_document()->parameter_capabilities().empty());CHECK_FALSE(job->layer_document()->mask_contract_precision());}
  else {CHECK(job->layer_document()->parameter_capabilities().empty());CHECK(job->layer_document()->mask_contract_precision()==sla::LayerMaskPrecision::NativeGray4);}
  const char *fixtures=std::getenv("SLA_ANYCUBIC_515_PALETTE_FIXTURES");
  const auto folder=fixtures?boost::filesystem::path(fixtures)/(format+"-"+std::to_string(version)+(portrait?"-portrait":"-landscape")):
      boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("any515-palette-%%%%");
  boost::filesystem::create_directories(folder);
  struct Cleanup {boost::filesystem::path folder;bool keep;~Cleanup(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(folder,error);}}} cleanup{folder,fixtures!=nullptr};
  config.save((folder/"source.ini").string());const size_t columns=portrait?250:400,rows=portrait?400:250;
  auto compare=[&](const boost::filesystem::path &file) {
    const auto decoded=read_gray4_anycubic_fixture(file,version,columns,rows);
    for(size_t i=0;i<3;++i) {
      const auto source=job->layer_document()->image(i);CHECK(source.cols==columns);CHECK(source.rows==rows);
      CHECK(bool(decoded[i]==source.buf));CHECK(std::all_of(source.buf.begin(),source.buf.end(),[](uint8_t v){return v%17==0;}));
    }
  };
  const auto direct=folder/("direct."+format);job->export_print(direct.string());compare(direct);
  auto snapshot=std::make_shared<const sla::ExportSnapshot>(job->capture_export_snapshot());
  const auto frozen=folder/("frozen."+format);const sla::ExportRequest request(snapshot,format,frozen.string());request.write();compare(frozen);
  sla::save_layer_project((folder/"calibration.sla-project").string(),sla::capture_generated_project(*job,*job->layer_document()));
  const auto persisted = read_zipper_archive((folder/"calibration.sla-project").string(),
                                              {"metadata/sla/layers.json"}, {});
  REQUIRE(persisted.entries.size() == 1);
  const auto metadata = nlohmann::json::parse(persisted.entries.front().buf);
  CHECK(metadata.at("version") == (owned ? 6 : 8));
  CHECK(metadata.at("document").contains("fresh_job") == owned);
  CHECK(metadata.at("document").contains("mask_contract") == !owned);
  if (!owned && metadata.at("document").contains("mask_contract")) {
    const auto &contract = metadata.at("document").at("mask_contract");
    CHECK(contract.at("version") == 1);
    CHECK(contract.at("precision") == 18);
    CHECK(contract.size() == 2);
  }
  CHECK(metadata.at("calibration_recipe").at("precision") == 18);
  CHECK(metadata.at("calibration_recipe").at("requested") == nlohmann::json::array({16, 47}));
  CHECK(metadata.at("calibration_recipe").at("effective") == nlohmann::json::array({17, 34}));
  const auto state=sla::read_project_layers((folder/"calibration.sla-project").string());REQUIRE(state);
  auto restored=sla::restore_generated_project(*state);REQUIRE(restored->calibration_recipe());
  CHECK(bool(restored->calibration_recipe()->effective==std::vector<uint8_t>{17,34}));
  CHECK(bool(restored->layer_document()->fresh_job())==owned);
  for(size_t i=0;i<3;++i)CHECK(bool(restored->layer_document()->image(i).buf==job->layer_document()->image(i).buf));
  job=std::move(restored);const auto restart=folder/("restart."+format);job->export_print(restart.string());compare(restart);
  CHECK(config.diff(initial).empty());
  if(version==515) {
    // Deliberately unknown Gray8 RAW writer truth. No canonical descriptor or
    // calibration recipe is invented for256 requested intensities.
    std::vector<uint8_t> pixels(columns*rows,0);for(unsigned i=0;i<256;++i)pixels[i]=uint8_t(i);
    const auto png=sla::PNGRasterEncoder{}(pixels.data(),columns,rows,1);
    auto raw=sla::LayerDocument::from_rasters({png,png,png});
    auto truth=SLAPrint::create_raster_job(config,raw,{.05,.1,.15});
    CHECK_FALSE(truth->layer_document()->fresh_job());CHECK_FALSE(truth->layer_document()->mask_contract_precision());
    const auto file=folder/("raw256."+format);truth->export_print(file.string());
    const auto decoded=read_gray4_anycubic_fixture(file,version,columns,rows);
    auto expected=pixels;for(auto &value:expected)value=uint8_t((value/16)*17);
    for(const auto &image:decoded)CHECK(bool(image==expected));
    sla::save_layer_project((folder/"raw256.sla-project").string(),sla::capture_generated_project(*truth,*truth->layer_document()));
  }
}

TEST_CASE("Anycubic515 native time follows represented rows and reference setup policy without ownership",
          "[sla_anycubic_515_native_time]") {
  const std::string format=GENERATE("pmx2","pwmb");
  const unsigned version=GENERATE(515u,516u,517u);
  const bool portrait=GENERATE(false,true);
  const unsigned scenario=GENERATE(0u,1u,2u,3u);
  CAPTURE(format,version,portrait,scenario);
  auto config=gray4_anycubic_calibration_config(format,version,portrait);
  // Literal reference scenarios: each has three records, exposures10/2/2,
  // and a300s setup term. The writer must truncate the final reference float,
  // rather than add300 to its old already-ceiled generic integer estimate.
  constexpr std::array<float,4> public_seconds{327.5f,329.f,325.25f,314.f};
  constexpr std::array<uint32_t,4> native_seconds{327u,329u,325u,314u};
  for(const char *prefix:{"sla_","sla_bottom_"}) {
    const std::string key(prefix);
    if(scenario==1)config.set(key+"wait_before_cure",.5);
    if(scenario==2) {
      config.set(key+"lift_height",2.5);config.set(key+"lift_speed",120.);
      config.set(key+"retract_speed",60.);
    }
    if(scenario==3) {
      config.set(key+"lift_height",0.);config.set(key+"lift_speed",0.);
      config.set(key+"retract_speed",0.);
    }
  }
  const auto initial=config;
  sla::CalibrationParameters p;p.pattern=sla::CalibrationPattern::GrayscalePatches;
  p.size_x=p.size_y=8.;p.margin=2.;p.height=.15;p.grayscale_steps=2;
  p.grayscale_first=16;p.grayscale_last=47;
  auto job=sla::create_calibration(config,p);
  REQUIRE(job->layer_document());const bool owned=version>=516;
  CHECK(bool(job->layer_document()->fresh_job())==owned);
  CHECK(bool(job->layer_document()->parameter_capabilities().empty())==!owned);
  const char *fixtures=std::getenv("SLA_ANYCUBIC_515_TIME_FIXTURES");
  const auto folder=fixtures?boost::filesystem::path(fixtures)/(format+"-"+std::to_string(version)+
    (portrait?"-portrait-":"-landscape-")+std::to_string(scenario)):
    boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("any515-time-%%%%");
  boost::filesystem::create_directories(folder);
  struct Cleanup {boost::filesystem::path path;bool keep;~Cleanup(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}} cleanup{folder,fixtures!=nullptr};
  config.save((folder/"source.ini").string());
  auto readback=[&](const boost::filesystem::path &path) {
    std::ifstream input(path.string(),std::ios::binary);const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    auto u32=[&](size_t at){REQUIRE(at<=bytes.size());REQUIRE(bytes.size()-at>=4);
      return uint32_t(bytes[at])|(uint32_t(bytes[at+1])<<8)|(uint32_t(bytes[at+2])<<16)|(uint32_t(bytes[at+3])<<24);};
    const size_t header=u32(20);CHECK(u32(12)==version);
    CHECK(u32(header+84)==native_seconds[scenario]);
    const auto masks=read_gray4_anycubic_fixture(path,version,portrait?250:400,portrait?400:250);
    for(size_t i=0;i<3;++i)CHECK(bool(masks[i]==job->layer_document()->image(i).buf));
  };
  const auto direct=folder/("direct."+format);job->export_print(direct.string());readback(direct);
  const auto frozen=folder/("frozen."+format);
  const auto snapshot=std::make_shared<const sla::ExportSnapshot>(job->capture_export_snapshot());
  sla::ExportRequest(snapshot,format,frozen.string()).write();readback(frozen);
  sla::save_layer_project((folder/"calibration.sla-project").string(),sla::capture_generated_project(*job,*job->layer_document()));
  const auto state=sla::read_project_layers((folder/"calibration.sla-project").string());REQUIRE(state);
  job=sla::restore_generated_project(*state);
  CHECK(bool(job->layer_document()->fresh_job())==owned);
  const auto restart=folder/("restart."+format);job->export_print(restart.string());readback(restart);
  if(version==515 && scenario==0) {
    // Deliberately misleading neutral/generic suffix: the explicitly created
    // PMX2/PWMB writer is authoritative. This is a raw writer dispatch probe,
    // not a claim that the filename is a correct printer import identity.
    auto writer_config=job->printer_config();
    writer_config.sla_archive_format.value="pwc";
    auto writer=SLAArchiveWriter::create(format,writer_config);REQUIRE(writer);
    auto *selected=dynamic_cast<AnycubicSLAArchive *>(writer.get());REQUIRE(selected);
    CHECK(writer_config.sla_archive_format.value=="pwc"); // factory input is unchanged
    CHECK(selected->default_version()==515);
    writer->encode_layers(*job->layer_document());
    const auto mismatch=folder/"explicit-writer-neutral.pwc";
    writer->export_print(mismatch.string(),*job,ThumbnailsList{},"");
    readback(mismatch);
  }
  CHECK(config.diff(initial).empty());
  if(fixtures) {
    std::ofstream expected((folder/"expected.json").string());
    expected<<nlohmann::json{{"scenario",scenario},{"version",version},{"format",format},
      {"public_time",public_seconds[scenario]},{"native_time",native_seconds[scenario]},
      {"owned",owned},{"setup_seconds",300},{"layer_count",3}}.dump(2);
  }
}

TEST_CASE("Anycubic515 decoded bottom speed fallback governs stored print time",
          "[sla_anycubic_515_bottom_speed_clock]") {
  const std::string format=GENERATE("pmx2","pwmb");
  const bool portrait=GENERATE(false,true);CAPTURE(format,portrait);
  auto config=gray4_anycubic_calibration_config(format,515,portrait);
  config.set("sla_bottom_lift_speed",.001);
  const auto initial=config;
  sla::CalibrationParameters p;p.pattern=sla::CalibrationPattern::GrayscalePatches;
  p.size_x=p.size_y=8.;p.margin=2.;p.height=.15;p.grayscale_steps=2;p.grayscale_first=16;p.grayscale_last=47;
  // Construction must actually succeed: the unowned generic source schedule
  // accepts this positive finite speed. Earlier refusal is not a clock proof.
  std::unique_ptr<SLAPrint> job;
  REQUIRE_NOTHROW(job=sla::create_calibration(config,p));REQUIRE(job);
  REQUIRE(job->layer_document());CHECK_FALSE(job->layer_document()->fresh_job());
  CHECK(job->layer_document()->parameter_capabilities().empty());
  CHECK(job->layer_document()->mask_contract_precision()==sla::LayerMaskPrecision::NativeGray4);
  const char *fixtures=std::getenv("SLA_ANYCUBIC_515_FALLBACK_FIXTURES");
  const auto folder=fixtures?boost::filesystem::path(fixtures)/(format+(portrait?"-portrait":"-landscape")):
    boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("any515-bottom-clock-%%%%");
  boost::filesystem::create_directories(folder);
  struct Cleanup {boost::filesystem::path path;bool keep;~Cleanup(){if(!keep){boost::system::error_code error;boost::filesystem::remove_all(path,error);}}} cleanup{folder,fixtures!=nullptr};
  config.save((folder/"source.ini").string());
  auto check_native=[&](const boost::filesystem::path &path) {
    std::ifstream input(path.string(),std::ios::binary);const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    auto u32=[&](size_t at){REQUIRE(at<=bytes.size());REQUIRE(bytes.size()-at>=4);
      return uint32_t(bytes[at])|(uint32_t(bytes[at+1])<<8)|(uint32_t(bytes[at+2])<<16)|(uint32_t(bytes[at+3])<<24);};
    auto number=[&](size_t at){auto word=u32(at);float value;std::memcpy(&value,&word,4);return value;};
    const auto header=u32(20),table=u32(36);CHECK(u32(12)==515);
    CHECK(number(header+44)==1.f); // Normal global60mm/min remains distinct.
    CHECK(number(table+20+12)==float(.001)/60.f);
    CHECK(number(table+20+12)>0.f); // Raw positive motion is not erased.
    CHECK(u32(header+84)==326); // Reference300+13.3+6.5+6.5=326.3, uint trunc.
    const auto decoded=read_gray4_anycubic_fixture(path,515,portrait?250:400,portrait?400:250);
    for(size_t i=0;i<3;++i)CHECK(bool(decoded[i]==job->layer_document()->image(i).buf));
  };
  const auto direct=folder/("direct."+format);job->export_print(direct.string());check_native(direct);
  const auto frozen=folder/("frozen."+format);
  const auto snapshot=std::make_shared<const sla::ExportSnapshot>(job->capture_export_snapshot());
  sla::ExportRequest(snapshot,format,frozen.string()).write();check_native(frozen);
  sla::save_layer_project((folder/"calibration.sla-project").string(),sla::capture_generated_project(*job,*job->layer_document()));
  const auto state=sla::read_project_layers((folder/"calibration.sla-project").string());REQUIRE(state);
  job=sla::restore_generated_project(*state);CHECK_FALSE(job->layer_document()->fresh_job());
  const auto restart=folder/("restart."+format);job->export_print(restart.string());check_native(restart);
  CHECK(config.diff(initial).empty());
  if(fixtures){std::ofstream expected((folder/"expected.json").string());expected<<nlohmann::json{
    {"version",515},{"format",format},{"portrait",portrait},{"public_time",326.3},
    {"native_time",326},{"public_bottom_lift_speed",100.},{"public_normal_lift_speed",60.},
    {"requested_bottom_speed_mmpm",.001},{"native_bottom_speed_mmps",float(.001)/60.f},
    {"setup_seconds",300},{"layer_count",3},{"owned",false}}.dump(2);}
}

TEST_CASE("Anycubic515 rejects invalid represented clocks before publishing an existing or new destination",
          "[sla_anycubic_515_time_publication_guards]") {
  const std::string format=GENERATE("pmx2","pwmb");
  const unsigned defect=GENERATE(0u,1u,2u);CAPTURE(format,defect);
  auto valid=gray4_anycubic_calibration_config(format,515,false);
  sla::CalibrationParameters p;p.pattern=sla::CalibrationPattern::GrayscalePatches;
  p.size_x=p.size_y=8.;p.margin=2.;p.height=.15;p.grayscale_steps=2;p.grayscale_first=16;p.grayscale_last=47;
  auto seed=sla::create_calibration(valid,p);REQUIRE(seed->layer_document());
  const auto &source=*seed->layer_document();
  std::vector<std::vector<uint8_t>> before;
  for(size_t i=0;i<source.layer_count();++i)before.push_back(source.image(i).buf);
  auto bad=valid;
  if(defect==0) {
    // Native float exposure is finite, but its complete schedule cannot fit
    // the uint32 header. Explicit printer bounds make this a legal source
    // construction rather than an unrelated profile-bounds rejection.
    bad.set("exposure_time",1e12);bad.set("initial_exposure_time",1e12);
    bad.set("max_exposure_time",1e13);bad.set("max_initial_exposure_time",1e13);
  } else if(defect==1) {
    // Infinity gives a finite duration in the old generic distance/speed sum;
    // it does not exercise a NaN-to-integer conversion in that old writer.
    bad.set("sla_lift_speed",std::numeric_limits<double>::infinity());
    bad.set("sla_bottom_lift_speed",std::numeric_limits<double>::infinity());
  } else {
    bad.set("sla_lift_speed",0.);bad.set("sla_bottom_lift_speed",0.);
  }
  const auto initial=bad;
  const auto folder=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("any515-clock-guard-%%%%");
  boost::filesystem::create_directories(folder);
  struct Cleanup {boost::filesystem::path path;~Cleanup(){boost::system::error_code error;boost::filesystem::remove_all(path,error);}} cleanup{folder};
  const auto kept=folder/("keep."+format),absent=folder/("new."+format);
  {std::ofstream output(kept.string(),std::ios::binary);output<<"previous accepted Anycubic";}
  std::unique_ptr<SLAPrint> job;
  if(defect==1 || defect==2) {
    // Both are existing source-factory gates, not proof that the new native
    // clock was reached. The first guard run proved the nonfinite input is
    // rejected by the general MachineSettings validation before any writer.
    bool refused=false;
    try {job=SLAPrint::create_raster_job(bad,source,{.05,.1,.15});}
    catch(const RuntimeError &error) {
      refused=true;
      const std::string expected=defect==1 ? "Invalid LCD setting: Lift speed" :
        "LCD motion speeds must be greater than zero for enabled moves";
      CHECK(std::string(error.what())==expected);
    }
    CHECK(refused);CHECK_FALSE(job);
  } else {
    // If construction starts rejecting either input, this REQUIRE fails;
    // the test must then be reclassified rather than count an earlier
    // refusal as a successful native writer guard.
    REQUIRE_NOTHROW(job=SLAPrint::create_raster_job(bad,source,{.05,.1,.15}));
    REQUIRE(job);CHECK_FALSE(job->layer_document()->fresh_job());
    auto writer=SLAArchiveWriter::create(format,job->printer_config());REQUIRE(writer);
    writer->encode_layers(*job->layer_document());
    for(const auto &target:{kept,absent}) {
      bool refused=false;
      try {writer->export_print(target.string(),*job,ThumbnailsList{},"");}
      catch(const ExportError &error) {
        refused=true;CHECK(std::string(error.what()).find("Anycubic515")!=std::string::npos);
      }
      CHECK(refused);
    }
  }
  std::ifstream input(kept.string(),std::ios::binary);const std::string kept_bytes{std::istreambuf_iterator<char>(input),{}};
  CHECK(bool(kept_bytes=="previous accepted Anycubic"));CHECK_FALSE(boost::filesystem::exists(absent));
  size_t entries=0;for(boost::filesystem::directory_iterator i(folder),end;i!=end;++i)++entries;
  CHECK(entries==1); // No additional native .tmp or publication files remain.
  CHECK(bad.diff(initial).empty());
  CHECK(source.mask_contract_precision()==sla::LayerMaskPrecision::NativeGray4);
  CHECK_FALSE(source.fresh_job());
  for(size_t i=0;i<source.layer_count();++i)CHECK(bool(source.image(i).buf==before[i]));
}
// Staged only: append to the existing registered calibration test TU.
TEST_CASE("Anycubic515 bottom-count boundary rejects before publication and preserves a valid ushort control",
          "[sla_anycubic_515_count_boundary]") {
  const std::string format=GENERATE("pmx2","pwmb");
  const size_t count=GENERATE(size_t(65535),size_t(65536));
  CAPTURE(format,count);
  auto config=gray4_anycubic_calibration_config(format,515,false);
  // Keep the real legacy printer LCD/mm/Z map; only reduce raster resolution.
  config.set("display_pixels_x",1);config.set("display_pixels_y",1);
  config.set("layer_height",.001);config.set("initial_layer_height",.001);
  config.set("exposure_time",1.);config.set("initial_exposure_time",1.);
  config.set("sla_bottom_layers",int(count));config.set("sla_transition_layers",0);
  for(const char *prefix:{"sla_","sla_bottom_"}) {
    const std::string key(prefix);
    for(const char *suffix:{"lift_height","lift_height2","lift_speed","lift_speed2",
                            "retract_speed","retract_height2","retract_speed2"})
      config.set(key+suffix,0.);
  }
  const auto initial=config;
  const uint8_t black=0;
  const auto png=sla::PNGRasterEncoder{}(&black,1,1,1);
  std::vector<sla::EncodedRaster> encoded(count,png);
  auto source=sla::LayerDocument::from_rasters(std::move(encoded));
  const auto source_revision=source.revision();
  const bool source_dirty=source.dirty(),source_undo=source.can_undo(),source_redo=source.can_redo();
  std::vector<double> positions;positions.reserve(count);
  for(size_t i=0;i<count;++i)positions.push_back(double(i+1)*.001);
  std::unique_ptr<SLAPrint> job;
  REQUIRE_NOTHROW(job=SLAPrint::create_raster_job(config,source,positions));
  REQUIRE(job);REQUIRE(job->layer_document());
  REQUIRE(job->layer_document()->layer_count()==count);
  CHECK_FALSE(job->layer_document()->fresh_job());
  const auto job_config=job->full_print_config();
  const auto job_revision=job->layer_document()->revision();
  auto writer=SLAArchiveWriter::create(format,job->printer_config());REQUIRE(writer);
  REQUIRE_NOTHROW(writer->encode_layers(*job->layer_document()));
  const auto folder=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("any515-count-%%%%");
  boost::filesystem::create_directories(folder);
  struct Cleanup {boost::filesystem::path path;~Cleanup(){boost::system::error_code error;boost::filesystem::remove_all(path,error);}} cleanup{folder};
  const auto kept=folder/("keep."+format),absent=folder/("new."+format);
  const std::string sentinel="previous accepted count-boundary output";
  {std::ofstream output(kept.string(),std::ios::binary);output<<sentinel;}
  if(count==65536) {
    for(const auto &path:{kept,absent}) {
      bool refused=false;
      try {writer->export_print(path.string(),*job,ThumbnailsList{},"");}
      catch(const ExportError &error) {
        refused=true;
        CHECK(std::string(error.what())=="Anycubic515 bottom count cannot be represented by the reference ushort field");
      }
      CHECK(refused);
    }
    std::ifstream input(kept.string(),std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input),{}};
    CHECK(bool(bytes==sentinel));CHECK_FALSE(boost::filesystem::exists(absent));
  } else {
    REQUIRE_NOTHROW(writer->export_print(absent.string(),*job,ThumbnailsList{},""));
    std::ifstream input(absent.string(),std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    auto u32=[&](size_t at){REQUIRE(at<=bytes.size());REQUIRE(bytes.size()-at>=4);
      return uint32_t(bytes[at])|(uint32_t(bytes[at+1])<<8)|(uint32_t(bytes[at+2])<<16)|(uint32_t(bytes[at+3])<<24);};
    const auto header=u32(20);CHECK(u32(12)==515);
    const auto count_bits=u32(header+36);float actual_count;std::memcpy(&actual_count,&count_bits,4);
    CHECK(actual_count==65535.f);
    CHECK(u32(header+84)==65835u); // 300 + 65535 stationary exposures of1second.
  }
  size_t entries=0;for(boost::filesystem::directory_iterator i(folder),end;i!=end;++i)++entries;
  CHECK(entries==(count==65536?1:2)); // No extra staging artifact.
  CHECK(config.diff(initial).empty());CHECK(job->full_print_config().diff(job_config).empty());
  CHECK(source.revision()==source_revision);CHECK(source.dirty()==source_dirty);
  CHECK(source.can_undo()==source_undo);CHECK(source.can_redo()==source_redo);
  CHECK(job->layer_document()->revision()==job_revision);
  CHECK_FALSE(job->layer_document()->dirty());CHECK_FALSE(job->layer_document()->can_undo());
  CHECK_FALSE(job->layer_document()->can_redo());
  bool unchanged=true;
  for(size_t i=0;i<count;++i) {
    for(const auto *document:std::array<const sla::LayerDocument *,2>{&source,job->layer_document()}) {
      const auto view=document->encoded_layer(i);
      unchanged=unchanged && view.sz==png.size() && std::memcmp(view.buf,png.data(),png.size())==0;
    }
  }
  CHECK(unchanged);
}

TEST_CASE("Anycubic515 export restores caller rounding mode on success and time overflow",
          "[sla_anycubic_515_clock_fenv]") {
  const std::string format=GENERATE("pmx2","pwmb");
  const bool overflow=GENERATE(false,true);CAPTURE(format,overflow);
  auto config=gray4_anycubic_calibration_config(format,515,false);
  if(overflow) {
    config.set("exposure_time",1e12);config.set("initial_exposure_time",1e12);
    config.set("max_exposure_time",1e13);config.set("max_initial_exposure_time",1e13);
  }
  // Construct/encode before changing the caller environment, so this checks
  // the real writer boundary and not unrelated factory floating-point policy.
  auto seed_config=gray4_anycubic_calibration_config(format,515,false);
  sla::CalibrationParameters p;p.pattern=sla::CalibrationPattern::GrayscalePatches;
  p.size_x=p.size_y=8.;p.margin=2.;p.height=.15;p.grayscale_steps=2;p.grayscale_first=16;p.grayscale_last=47;
  auto seed=sla::create_calibration(seed_config,p);REQUIRE(seed);REQUIRE(seed->layer_document());
  auto job=SLAPrint::create_raster_job(config,*seed->layer_document(),{.05,.1,.15});
  REQUIRE(job);REQUIRE(job->layer_document());
  auto writer=SLAArchiveWriter::create(format,job->printer_config());REQUIRE(writer);
  writer->encode_layers(*job->layer_document());
  const auto folder=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("any515-fenv-%%%%");
  boost::filesystem::create_directories(folder);
  struct Cleanup {boost::filesystem::path path;~Cleanup(){boost::system::error_code error;boost::filesystem::remove_all(path,error);}} cleanup{folder};
  const auto path=folder/("output."+format);
  std::fenv_t original{};REQUIRE(std::fegetenv(&original)==0);
  struct Restore {std::fenv_t original;~Restore(){std::fesetenv(&original);}} restore{original};
  REQUIRE(std::fesetround(FE_DOWNWARD)==0);
  REQUIRE(std::feraiseexcept(FE_DIVBYZERO)==0);
  if(overflow) {
    bool refused=false;
    try {writer->export_print(path.string(),*job,ThumbnailsList{},"");}
    catch(const ExportError &error) {
      refused=true;CHECK(std::string(error.what())=="Anycubic515 native time exceeds uint32");
    }
    CHECK(refused);
  }
  else CHECK_NOTHROW(writer->export_print(path.string(),*job,ThumbnailsList{},""));
  CHECK(std::fegetround()==FE_DOWNWARD);
  CHECK((std::fetestexcept(FE_DIVBYZERO)&FE_DIVBYZERO)!=0);
  CHECK(boost::filesystem::exists(path)==!overflow);
  if(!overflow) {
    std::ifstream input(path.string(),std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
    auto u32=[&](size_t at){REQUIRE(at<=bytes.size());REQUIRE(bytes.size()-at>=4);
      return uint32_t(bytes[at])|(uint32_t(bytes[at+1])<<8)|(uint32_t(bytes[at+2])<<16)|(uint32_t(bytes[at+3])<<24);};
    const auto header=u32(20);CHECK(u32(header+84)==327);
  }
}
