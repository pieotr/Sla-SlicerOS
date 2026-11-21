// W9a: 3MF projects that still carry FFF-only entries must keep loading (geometry only),
// SLA data (support points, drain holes) must survive, and the writer must not emit
// the removed FFF entries any more.
#include <catch2/catch_test_macros.hpp>
#include <boost/filesystem.hpp>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/miniz_extension.hpp"

using namespace Slic3r;

namespace {

using ZipEntries = std::vector<std::pair<std::string, std::string>>;

ZipEntries read_zip(const std::string &path)
{
    ZipEntries out;
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(open_zip_reader(&zip, path));
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
        mz_zip_archive_file_stat stat;
        REQUIRE(mz_zip_reader_file_stat(&zip, i, &stat));
        if (mz_zip_reader_is_file_a_directory(&zip, i))
            continue;
        std::string data(size_t(stat.m_uncomp_size), '\0');
        REQUIRE(mz_zip_reader_extract_to_mem(&zip, i, data.data(), data.size(), 0));
        out.emplace_back(stat.m_filename, std::move(data));
    }
    close_zip_reader(&zip);
    return out;
}

void write_zip(const std::string &path, const ZipEntries &entries)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(open_zip_writer(&zip, path));
    for (const auto &[name, data] : entries)
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    close_zip_writer(&zip);
}

bool has_entry(const ZipEntries &entries, const std::string &name)
{
    for (const auto &e : entries)
        if (e.first == name)
            return true;
    return false;
}

std::string &entry(ZipEntries &entries, const std::string &name)
{
    for (auto &e : entries)
        if (e.first == name)
            return e.second;
    FAIL("missing entry " << name);
    return entries.front().second;
}

Model make_sla_model()
{
    Model model;
    ModelObject *object = model.add_object("cube", "cube.stl", make_cube(20., 20., 20.));
    object->add_instance();
    sla::SupportPoint a;
    a.pos = Vec3f(1.f, 2.f, 0.f);
    a.head_front_radius = 0.4f;
    sla::SupportPoint b;
    b.pos = Vec3f(10.f, 10.f, 20.f);
    b.head_front_radius = 0.6f;
    object->sla_support_points = {a, b};
    object->sla_points_status = sla::PointsStatus::UserModified;
    object->sla_drain_holes.emplace_back(Vec3f(5.f, 5.f, 0.f), Vec3f(0.f, 0.f, -1.f), 2.f, 8.f);
    return model;
}

DynamicPrintConfig technology_config(PrinterTechnology technology)
{
    DynamicPrintConfig config;
    config.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(technology));
    return config;
}

// The PrinterTechnology enum has no FFF value any more, so an FFF project is authored by rewriting the raw
// "printer_technology" line of the embedded configuration, exactly as an old PrusaSlicer wrote it.
void make_config_say_fff(ZipEntries &entries)
{
    std::string &config = entry(entries, "Metadata/Slic3r_PE.config");
    const size_t pos = config.find("printer_technology = SLA");
    REQUIRE(pos != std::string::npos);
    config.replace(pos, std::string("printer_technology = SLA").size(), "printer_technology = FFF");
}

// Projects as written by PrusaSlicer 2.9 for an FFF printer: store a project, then add the FFF-only entries.
void add_fff_entries(ZipEntries &entries)
{
    entries.emplace_back("Metadata/Slic3r_PE_layer_heights_profile.txt", "object_id=1|0.000000;0.200000;20.000000;0.200000");
    entries.emplace_back("Metadata/Prusa_Slicer_layer_config_ranges.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<objects><object id=\"1\"><range min_z=\"0\" max_z=\"5\"><option opt_key=\"extruder\">2</option></range></object></objects>\n");
    entries.emplace_back("Metadata/Prusa_Slicer_custom_gcode_per_print_z.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<custom_gcodes_per_print_z><code print_z=\"5\" type=\"1\" extruder=\"1\" color=\"#FF0000\" extra=\"\" gcode=\"M600\"/>"
        "<mode value=\"SingleExtruder\"/></custom_gcodes_per_print_z>\n");
    entries.emplace_back("Metadata/Prusa_Slicer_wipe_tower_information.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<wipe_tower_information position_x=\"180\" position_y=\"140\" rotation_deg=\"0\"/>\n");
    std::string &model = entry(entries, "3D/3dmodel.model");
    const size_t first = model.find("<triangle ");
    REQUIRE(first != std::string::npos);
    model.insert(first + 10, "slic3rpe:custom_supports=\"4\" slic3rpe:custom_seam=\"8\" slic3rpe:mmu_segmentation=\"C\" slic3rpe:fuzzy_skin=\"4\" ");
}

} // namespace

TEST_CASE("3MF loader skips FFF-only entries and keeps SLA data", "[3mf][sla_fff_project]")
{
    const boost::filesystem::path dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("sla-fff-3mf-%%%%-%%%%");
    boost::filesystem::create_directories(dir);
    struct Cleanup { boost::filesystem::path p; ~Cleanup() { boost::system::error_code ec; boost::filesystem::remove_all(p, ec); } } cleanup{dir};
    const std::string plain = (dir / "plain.3mf").string();
    const std::string fff   = (dir / "fff.3mf").string();

    const DynamicPrintConfig plain_config = technology_config(ptSLA);
    {
        Model model = make_sla_model();
        REQUIRE(store_3mf(plain.c_str(), &model, &plain_config, false));
    }

    SECTION("the writer no longer emits FFF-only entries or painted facet attributes")
    {
        const ZipEntries entries = read_zip(plain);
        for (const char *removed : {"Metadata/Slic3r_PE_layer_heights_profile.txt", "Metadata/Prusa_Slicer_layer_config_ranges.xml",
                                    "Metadata/Prusa_Slicer_custom_gcode_per_print_z.xml", "Metadata/Prusa_Slicer_wipe_tower_information.xml"})
            CHECK_FALSE(has_entry(entries, removed));
        CHECK(has_entry(entries, "Metadata/Slic3r_PE_sla_support_points.txt"));
        CHECK(has_entry(entries, "Metadata/Slic3r_PE_sla_drain_holes.txt"));
        std::string xml;
        for (const auto &e : entries)
            if (e.first == "3D/3dmodel.model")
                xml = e.second;
        CHECK(xml.find("slic3rpe:custom_supports") == std::string::npos);
        CHECK(xml.find("PaintingVersion") == std::string::npos);
        CHECK(entries.size() > 0);
        std::string cfg;
        for (const auto &e : entries)
            if (e.first == "Metadata/Slic3r_PE.config")
                cfg = e.second;
        CHECK(cfg.find("wipe_tower_x") == std::string::npos);
    }

    SECTION("an FFF project loads geometry and SLA data, sets the FFF flag, and does not throw")
    {
        ZipEntries entries = read_zip(plain);
        add_fff_entries(entries);
        make_config_say_fff(entries);
        write_zip(fff, entries);

        Model model;
        DynamicPrintConfig config;
        ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::EnableSilent};
        boost::optional<Semver> version;
        bool ok = false;
        REQUIRE_NOTHROW(ok = load_3mf(fff.c_str(), config, substitutions, &model, false, version));
        REQUIRE(ok);
        REQUIRE(model.objects.size() == 1);
        const ModelObject &object = *model.objects.front();
        REQUIRE(object.volumes.size() == 1);
        CHECK(object.volumes.front()->mesh().facets_count() == 12);
        CHECK(model.loaded_from_fff_project);
        // "printer_technology = FFF" is recognised on the raw string only: no enum value, no substitution.
        CHECK(substitutions.legacy_fff_technology);
        CHECK(config.option("printer_technology") == nullptr);
        CHECK(substitutions.substitutions.empty());
        REQUIRE(object.sla_support_points.size() == 2);
        CHECK(object.sla_support_points[0].pos.isApprox(Vec3f(1.f, 2.f, 0.f)));
        CHECK(object.sla_support_points[1].pos.isApprox(Vec3f(10.f, 10.f, 20.f)));
        CHECK(object.sla_support_points[1].head_front_radius == 0.6f);
        CHECK(object.sla_points_status == sla::PointsStatus::UserModified);
        CHECK(object.sla_drain_holes.size() == 1);

        // Saving it again drops every FFF entry and keeps the SLA ones.
        const std::string resaved = (dir / "resaved.3mf").string();
        REQUIRE(store_3mf(resaved.c_str(), &model, &config, false));
        const ZipEntries again = read_zip(resaved);
        CHECK_FALSE(has_entry(again, "Metadata/Prusa_Slicer_custom_gcode_per_print_z.xml"));
        CHECK_FALSE(has_entry(again, "Metadata/Slic3r_PE_layer_heights_profile.txt"));
        CHECK(has_entry(again, "Metadata/Slic3r_PE_sla_support_points.txt"));
    }

    SECTION("an SLA project carrying only leftover FFF entries is not reported as an FFF project")
    {
        const std::string sla_path = (dir / "sla.3mf").string();
        {
            Model model = make_sla_model();
            const DynamicPrintConfig sla_config = technology_config(ptSLA);
            REQUIRE(store_3mf(sla_path.c_str(), &model, &sla_config, false));
        }
        ZipEntries entries = read_zip(sla_path);
        add_fff_entries(entries);
        write_zip((dir / "sla_leftovers.3mf").string(), entries);

        Model model;
        DynamicPrintConfig config;
        ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::EnableSilent};
        boost::optional<Semver> version;
        REQUIRE(load_3mf((dir / "sla_leftovers.3mf").string().c_str(), config, substitutions, &model, false, version));
        REQUIRE(model.objects.size() == 1);
        CHECK_FALSE(model.loaded_from_fff_project);
        CHECK(model.objects.front()->sla_support_points.size() == 2);
    }

    SECTION("a plain SLA round trip keeps the support points")
    {
        Model model;
        DynamicPrintConfig config;
        ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::EnableSilent};
        boost::optional<Semver> version;
        REQUIRE(load_3mf(plain.c_str(), config, substitutions, &model, false, version));
        REQUIRE(model.objects.size() == 1);
        REQUIRE(model.objects.front()->sla_support_points.size() == 2);
        CHECK(model.objects.front()->sla_support_points[0] == make_sla_model().objects.front()->sla_support_points[0]);
    }
}

TEST_CASE("seam_test_object.3mf (an FFF project fixture) loads geometry only", "[3mf][sla_fff_project]")
{
    const std::string path = std::string(TEST_DATA_DIR) + "/seam_test_object.3mf";
    Model model;
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::EnableSilent};
    boost::optional<Semver> version;
    bool ok = false;
    REQUIRE_NOTHROW(ok = load_3mf(path.c_str(), config, substitutions, &model, false, version));
    REQUIRE(ok);
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    CHECK(model.objects.front()->volumes.front()->mesh().facets_count() > 200000);
    CHECK(model.loaded_from_fff_project);
}
