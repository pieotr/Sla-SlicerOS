// SPDX-License-Identifier: AGPL-3.0-or-later
#include "libslic3r/SLA/ResinStock.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <boost/filesystem.hpp>
#include <fstream>
#include <sstream>
using namespace Slic3r::sla;
using Catch::Approx;

namespace {
struct TempDir {
    boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("resin-stock-%%%%-%%%%");
    TempDir() { boost::filesystem::create_directories(path); }
    ~TempDir() { boost::system::error_code ec; boost::filesystem::remove_all(path, ec); }
};
std::string slurp(const boost::filesystem::path &p) { std::ifstream in(p.string(), std::ios::binary); std::stringstream s; s << in.rdbuf(); return s.str(); }
void spit(const boost::filesystem::path &p, const std::string &t) { std::ofstream out(p.string(), std::ios::binary); out << t; }
}

TEST_CASE("Resin consume follows UVtools Material.Consume", "[sla_resin_stock]") {
    ResinMaterial m; m.name = "A"; m.bottle_volume_ml = 1000; m.bottle_remaining_ml = 300; m.bottles_in_stock = 1;
    // remainder 450 > remaining 300: a new bottle is opened, 300 - 450 + 1000 = 850 left, one bottle fewer.
    CHECK_FALSE(m.consume(450, 7200));
    CHECK(m.bottle_remaining_ml == Approx(850));
    CHECK(m.bottles_in_stock == 0);
    CHECK(m.consumed_ml == Approx(450));
    CHECK(m.print_time_h == Approx(2.));
    CHECK(m.print_count == 1);

    ResinMaterial b; b.name = "B"; b.bottles_in_stock = 5; b.bottle_remaining_ml = 1000;
    CHECK(b.consume(2500, 0));            // two whole bottles plus 500 ml from the third
    CHECK(b.bottles_in_stock == 3);
    CHECK(b.bottle_remaining_ml == Approx(500));
    CHECK(b.consume(100, 0));             // fits in the open bottle
    CHECK(b.bottles_in_stock == 3);
    CHECK(b.bottle_remaining_ml == Approx(400));
    CHECK(b.consumed_ml == Approx(2600));

    ResinMaterial exact; exact.name = "E"; exact.bottles_in_stock = 2;
    exact.consume(1000, 0);               // exactly one bottle: no remainder, the open bottle is untouched
    CHECK(exact.bottles_in_stock == 1);
    CHECK(exact.bottle_remaining_ml == Approx(1000));

    ResinMaterial none; none.name = "N"; none.bottle_volume_ml = 1000;
    none.consume(0, 100); none.consume(-5, 100);
    CHECK(none.consumed_ml == 0); CHECK(none.print_count == 0);
    ResinMaterial zero_bottle; zero_bottle.name = "Z"; zero_bottle.bottle_volume_ml = 0;
    zero_bottle.consume(10, 10);
    CHECK(zero_bottle.consumed_ml == 0);
}

TEST_CASE("Resin stock round trips through an atomic save", "[sla_resin_stock]") {
    TempDir dir; const auto file = (dir.path / "nested" / "resin_stock.json").string();
    ResinStock stock;
    ResinMaterial m; m.name = "Siraya Tech Fast"; m.bottle_cost = 41.5; m.bottles_in_stock = 3; m.bottle_remaining_ml = 640.25; m.consumed_ml = 12.5; m.print_time_h = 1.25; m.print_count = 2;
    stock.add(m);
    ResinMaterial n; n.name = u8"Żywica";
    stock.add(n);
    stock.save(file);
    const auto loaded = ResinStock::load(file);
    REQUIRE(loaded.materials.size() == 2);
    CHECK(loaded.materials[0].name == "Siraya Tech Fast");
    CHECK(loaded.materials[0].bottle_remaining_ml == Approx(640.25));
    CHECK(loaded.materials[0].print_count == 2);
    CHECK(loaded.materials[1].name == u8"Żywica");
    // No staging leftovers beside the file.
    for (auto &entry : boost::filesystem::directory_iterator(dir.path / "nested")) CHECK(entry.path().filename() == "resin_stock.json");
    // A second save replaces the first completely.
    ResinStock again; again.add(n); again.save(file);
    CHECK(ResinStock::load(file).materials.size() == 1);
}

TEST_CASE("Resin stock refuses invalid input and keeps the previous file", "[sla_resin_stock]") {
    TempDir dir; const auto file = (dir.path / "resin_stock.json").string();
    ResinStock stock; ResinMaterial m; m.name = "Keep"; stock.add(m); stock.save(file);
    const auto before = slurp(file);
    CHECK_THROWS_AS(stock.add(m), ResinStockError);              // duplicate
    ResinMaterial empty; CHECK_THROWS_AS(stock.add(empty), ResinStockError);
    ResinMaterial bad; bad.name = "Bad"; bad.bottle_volume_ml = 0; CHECK_THROWS_AS(stock.add(bad), ResinStockError);
    bad.bottle_volume_ml = 1000; bad.bottle_cost = -1; CHECK_THROWS_AS(stock.add(bad), ResinStockError);
    bad.bottle_cost = 1; bad.bottle_remaining_ml = 2000; CHECK_THROWS_AS(stock.add(bad), ResinStockError);
    stock.materials.push_back(bad);                                 // bypass add(): save() must still refuse
    CHECK_THROWS_AS(stock.save(file), ResinStockError);
    CHECK(slurp(file) == before);
    for (auto &entry : boost::filesystem::directory_iterator(dir.path)) CHECK(entry.path().filename() == "resin_stock.json");
}

TEST_CASE("Corrupt resin stock is a clear error and is never reset", "[sla_resin_stock]") {
    TempDir dir; const auto file = dir.path / "resin_stock.json";
    CHECK(ResinStock::load(file.string()).materials.empty());       // missing file: empty inventory
    const std::string cases[] = {
        "", "not json", "{\"version\":1,\"materials\":[{\"name\":\"A\"",   // truncated
        "[]", "{\"version\":2,\"materials\":[]}", "{\"version\":1}", "{\"version\":1,\"materials\":{}}",
        "{\"version\":1,\"materials\":[1]}",
        "{\"version\":1,\"materials\":[{\"name\":\"A\",\"bottle_volume_ml\":\"x\",\"density\":1,\"bottle_cost\":1,\"bottles_in_stock\":1,\"bottle_remaining_ml\":1,\"consumed_ml\":0,\"print_time_h\":0,\"print_count\":0}]}",
        "{\"version\":1,\"materials\":[{\"name\":\"A\",\"bottle_volume_ml\":1000,\"density\":1,\"bottle_cost\":1,\"bottles_in_stock\":1,\"bottle_remaining_ml\":5000,\"consumed_ml\":0,\"print_time_h\":0,\"print_count\":0}]}",
    };
    for (const auto &text : cases) {
        INFO(text);
        spit(file, text);
        try { ResinStock::load(file.string()); FAIL("load must throw"); }
        catch (const ResinStockError &error) {
            CHECK(std::string(error.what()).find("resin_stock.json") != std::string::npos);
            CHECK(std::string(error.what()).find("not changed") != std::string::npos);
        }
        CHECK(slurp(file.string()) == text);                         // loading never rewrites the file
    }
}
