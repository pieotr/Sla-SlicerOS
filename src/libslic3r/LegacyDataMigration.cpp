// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LegacyDataMigration.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"

namespace Slic3r {

namespace fs = boost::filesystem;

namespace {

// ---------------------------------------------------------------- ini parsing (read-only, line preserving)

struct IniSection
{
    std::string                        name;   // "" = top level (before the first [section])
    std::vector<std::string>           lines;  // raw lines, without the "[name]" line
    std::map<std::string, std::string> kv;     // key -> value (trimmed)
};

std::string trim(const std::string &s) { return boost::trim_copy(s); }

std::string unquote(std::string s)
{
    s = trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        s = trim(s.substr(1, s.size() - 2));
    return s;
}

bool read_file(const fs::path &path, std::string &out)
{
    std::ifstream in(path.string(), std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::vector<IniSection> parse_ini(const std::string &text)
{
    std::vector<IniSection> sections(1);
    std::istringstream      in(text);
    std::string             line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::string t = trim(line);
        if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
            sections.emplace_back();
            sections.back().name = trim(t.substr(1, t.size() - 2));
            continue;
        }
        IniSection &s = sections.back();
        s.lines.push_back(line);
        if (t.empty() || t.front() == '#' || t.front() == ';')
            continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos)
            continue;
        s.kv[trim(t.substr(0, eq))] = trim(t.substr(eq + 1));
    }
    return sections;
}

bool is_sla_value(const std::string &v) { return unquote(v) == "SLA"; }

// ---------------------------------------------------------------- vendor bundle analysis

struct VendorInfo
{
    std::string           id;
    std::set<std::string> sla_models;    // printer_model names that are SLA
    std::set<std::string> sla_printers;  // concrete (non abstract) printer preset names that are SLA
    bool has_sla() const { return !sla_models.empty() || !sla_printers.empty(); }
};

VendorInfo analyze_vendor(const std::string &id, const std::string &text)
{
    VendorInfo info;
    info.id = id;
    const std::vector<IniSection> sections = parse_ini(text);
    std::map<std::string, const IniSection*> printers, models;
    for (const IniSection &s : sections) {
        if (boost::starts_with(s.name, "printer:"))
            printers[trim(s.name.substr(8))] = &s;
        else if (boost::starts_with(s.name, "printer_model:"))
            models[trim(s.name.substr(14))] = &s;
    }
    for (const auto &[name, s] : models) {
        auto it = s->kv.find("technology");
        if (it != s->kv.end() && is_sla_value(it->second))
            info.sla_models.insert(name);
    }
    // Resolve printer_technology through the "inherits" chain (depth limited, cycle safe).
    std::function<std::string(const std::string&, int)> technology = [&](const std::string &name, int depth) -> std::string {
        auto it = printers.find(name);
        if (it == printers.end() || depth > 16)
            return {};
        const IniSection &s = *it->second;
        if (auto t = s.kv.find("printer_technology"); t != s.kv.end())
            return unquote(t->second);
        if (auto m = s.kv.find("printer_model"); m != s.kv.end() && info.sla_models.count(unquote(m->second)))
            return "SLA";
        if (auto inh = s.kv.find("inherits"); inh != s.kv.end()) {
            std::vector<std::string> parents;
            boost::split(parents, unquote(inh->second), boost::is_any_of(";"));
            for (std::string &p : parents)
                if (std::string tech = technology(unquote(p), depth + 1); !tech.empty())
                    return tech;
        }
        return {};
    };
    for (const auto &[name, s] : printers) {
        if (!name.empty() && name.front() == '*')
            continue;
        if (technology(name, 0) == "SLA") {
            info.sla_printers.insert(name);
            if (auto m = s->kv.find("printer_model"); m != s->kv.end())
                info.sla_models.insert(unquote(m->second));
        }
    }
    return info;
}

// ---------------------------------------------------------------- copy helpers

struct Copier
{
    fs::path                 dst_root;
    LegacyMigrationResult   &result;

    void note(const fs::path &dst) { result.copied.push_back(fs::relative(dst, dst_root).generic_string()); }

    bool copy_file(const fs::path &src, const fs::path &dst)
    {
        boost::system::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(warning) << "Legacy data import: cannot copy " << src << " -> " << dst << ": " << ec.message();
            return false;
        }
        note(dst);
        return true;
    }

    void copy_tree(const fs::path &src, const fs::path &dst, size_t &counter)
    {
        boost::system::error_code ec;
        if (!fs::is_directory(src, ec))
            return;
        std::vector<fs::path> entries;
        for (fs::directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec))
            entries.push_back(it->path());
        std::sort(entries.begin(), entries.end());
        for (const fs::path &p : entries) {
            if (fs::is_symlink(p, ec))
                continue;
            if (fs::is_directory(p, ec))
                copy_tree(p, dst / p.filename(), counter);
            else if (fs::is_regular_file(p, ec) && copy_file(p, dst / p.filename()))
                ++counter;
        }
    }
};

std::vector<fs::path> sorted_ini_files(const fs::path &dir)
{
    std::vector<fs::path> out;
    boost::system::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path p = it->path();
        const std::string name = p.filename().string();
        if (!name.empty() && name.front() != '.' && boost::iends_with(name, ".ini") && fs::is_regular_file(p, ec))
            out.push_back(p);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------- the filtered application ini

bool is_fff_preset_key(const std::string &key)
{
    // [presets] keys: only the SLA selections (and anything sla_*) are used by this application.
    return !(key == "printer" || boost::starts_with(key, "sla_"));
}

// Top level keys written by FFF-capable versions that this application neither reads nor writes.
bool is_fff_top_level_key(const std::string &key)
{
    static const std::set<std::string> fff = {
        "no_templates", "associate_gcode", "associate_bgcode", "last_gcode_dir", "last_gcode_output_path"
    };
    return fff.count(key) != 0 || boost::starts_with(key, "gcode") || boost::starts_with(key, "filament");
}

std::string key_of(const std::string &line)
{
    const std::string t = trim(line);
    if (t.empty() || t.front() == '#' || t.front() == ';')
        return {};
    const size_t eq = t.find('=');
    return eq == std::string::npos ? std::string() : trim(t.substr(0, eq));
}

std::string value_of(const std::string &line)
{
    const std::string t = trim(line);
    const size_t eq = t.find('=');
    return eq == std::string::npos ? std::string() : trim(t.substr(eq + 1));
}

} // namespace

// ================================================================ public API

LegacyMigrationResult migrate_legacy_data_dir(const std::string &legacy_dir_str, const std::string &new_dir_str)
{
    LegacyMigrationResult result;
    const fs::path legacy = legacy_dir_str, target = new_dir_str;
    boost::system::error_code ec;

    if (!fs::is_directory(legacy, ec)) { result.skipped_reason = "no-legacy-datadir"; return result; }
    if (fs::exists(target, ec))        { result.skipped_reason = "new-datadir-exists"; return result; }
    if (fs::weakly_canonical(legacy, ec) == fs::weakly_canonical(target, ec)) { result.skipped_reason = "same-directory"; return result; }

    // Stage next to the target and rename at the end: a crash never leaves a half populated data directory
    // (its existence would suppress the migration forever).
    fs::path stage = target;
    stage += ".migrating-" + std::to_string(get_current_pid());
    fs::remove_all(stage, ec);

    try {
        fs::create_directories(stage);
        Copier copier { stage, result };

        // --- user profiles
        for (const fs::path &p : sorted_ini_files(legacy / "sla_print"))
            if (copier.copy_file(p, stage / "sla_print" / p.filename())) ++result.sla_prints;
        for (const fs::path &p : sorted_ini_files(legacy / "sla_material"))
            if (copier.copy_file(p, stage / "sla_material" / p.filename())) ++result.sla_materials;

        std::set<std::string> user_sla_printers, user_fff_printers;
        for (const fs::path &p : sorted_ini_files(legacy / "printer")) {
            std::string text;
            if (!read_file(p, text))
                continue;
            const std::string name = p.stem().string();
            const std::vector<IniSection> parsed = parse_ini(text);
            const IniSection &top = parsed.front();
            auto tech = top.kv.find("printer_technology");
            if (tech == top.kv.end() || !is_sla_value(tech->second)) {
                // FFF, or a pre-2.0 profile without printer_technology (always FFF): not used by this application.
                user_fff_printers.insert(name);
                continue;
            }
            if (!copier.copy_file(p, stage / "printer" / p.filename()))
                continue;
            ++result.printers;
            user_sla_printers.insert(name);
            // Bed texture / model files placed next to the profile by the configuration wizard.
            for (const char *key : { "bed_custom_texture", "bed_custom_model" }) {
                auto it = top.kv.find(key);
                if (it == top.kv.end())
                    continue;
                const fs::path ref = unquote(it->second);
                if (ref.empty() || ref.has_parent_path())
                    continue;
                const fs::path src = legacy / "printer" / ref;
                if (fs::is_regular_file(src, ec) && copier.copy_file(src, stage / "printer" / ref))
                    ++result.other_files;
            }
        }

        // --- vendor bundles that define SLA printers (kept verbatim: they are read-only system profiles)
        std::map<std::string, VendorInfo> sla_vendors;
        std::set<std::string>             sla_system_printers;
        for (const fs::path &p : sorted_ini_files(legacy / "vendor")) {
            std::string text;
            if (!read_file(p, text))
                continue;
            VendorInfo info = analyze_vendor(p.stem().string(), text);
            if (!info.has_sla())
                continue;
            if (!copier.copy_file(p, stage / "vendor" / p.filename()))
                continue;
            ++result.vendors;
            sla_system_printers.insert(info.sla_printers.begin(), info.sla_printers.end());
            copier.copy_tree(legacy / "vendor" / info.id, stage / "vendor" / info.id, result.other_files);
            sla_vendors.emplace(info.id, std::move(info));
        }

        // --- other files that this application keeps in its data directory
        copier.copy_tree(legacy / "shapes", stage / "shapes", result.other_files);
        {
            const fs::path stock = legacy / "resin_stock.json";
            if (fs::is_regular_file(stock, ec) && copier.copy_file(stock, stage / "resin_stock.json"))
                ++result.other_files;
        }

        // --- the application config: only what an SLA-only application uses
        std::string legacy_ini;
        if (read_file(legacy / "PrusaSlicer.ini", legacy_ini)) {
            const std::vector<IniSection> sections = parse_ini(legacy_ini);
            std::ostringstream out;
            out << "# " << header_slic3r_generated() << "\n";
            for (const std::string &line : sections.front().lines) {
                const std::string key = key_of(line);
                if (key.empty() ? trim(line).empty() : !is_fff_top_level_key(key))
                    out << line << "\n";
            }
            for (size_t i = 1; i < sections.size(); ++i) {
                const IniSection &s = sections[i];
                std::vector<std::string> keep;
                if (s.name == "presets") {
                    for (const std::string &line : s.lines) {
                        const std::string key = key_of(line);
                        if (key.empty() || is_fff_preset_key(key))
                            continue;
                        if (key == "printer") {
                            const std::string v = unquote(value_of(line));
                            const bool sla = user_sla_printers.count(v) || (!user_fff_printers.count(v) && sla_system_printers.count(v));
                            if (!sla)
                                continue;
                        }
                        keep.push_back(line);
                    }
                } else if (boost::starts_with(s.name, "vendor:")) {
                    auto it = sla_vendors.find(s.name.substr(7));
                    if (it == sla_vendors.end())
                        continue;
                    for (const std::string &line : s.lines) {
                        const std::string key = key_of(line);
                        if (boost::starts_with(key, "model:") && it->second.sla_models.count(key.substr(6)))
                            keep.push_back(line);
                    }
                } else if (s.name == "filaments" || s.name == "extras" || s.name == "physical_printer"
                           || boost::starts_with(s.name, "filament:")) {
                    continue;
                } else {
                    for (const std::string &line : s.lines) {
                        const std::string key = key_of(line);
                        if (!key.empty())
                            keep.push_back(line);
                    }
                }
                if (keep.empty())
                    continue;
                out << "\n[" << s.name << "]\n";
                for (const std::string &line : keep)
                    out << line << "\n";
            }
            out << "\n";
            const fs::path dst = stage / (std::string(SLIC3R_APP_KEY) + ".ini");
            std::ofstream f(dst.string(), std::ios::binary | std::ios::trunc);
            f << out.str();
            f.close();
            if (!f)
                throw std::runtime_error("cannot write " + dst.string());
            result.config_written = true;
            copier.note(dst);
        }

        std::sort(result.copied.begin(), result.copied.end());
        fs::rename(stage, target);   // atomic publish; the legacy directory was never written
        result.performed = true;
        BOOST_LOG_TRIVIAL(info) << "Legacy data import from " << legacy << " into " << target << ": "
                                << result.sla_prints << " SLA print, " << result.sla_materials << " SLA material, "
                                << result.printers << " printer profile(s), " << result.vendors << " vendor bundle(s), "
                                << result.other_files << " other file(s), config "
                                << (result.config_written ? "written" : "absent");
        for (const std::string &rel : result.copied)
            BOOST_LOG_TRIVIAL(info) << "Legacy data import: copied " << rel;
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Legacy data import from " << legacy << " failed: " << ex.what();
        fs::remove_all(stage, ec);   // only our own staging directory
        result = LegacyMigrationResult();
        result.skipped_reason = std::string("error: ") + ex.what();
    }
    return result;
}

static LegacyMigrationResult g_last_migration;

LegacyMigrationResult migrate_legacy_data_dir_if_needed(const std::string &legacy_dir, const std::string &new_dir, bool datadir_explicit)
{
    LegacyMigrationResult result;
    const char *disabled = std::getenv("SLA_SLICER_NO_MIGRATION");
    if (disabled != nullptr && std::string(disabled) == "1")
        result.skipped_reason = "disabled";
    else if (datadir_explicit)
        result.skipped_reason = "explicit-datadir";
    else if (new_dir.empty() || legacy_dir.empty())
        result.skipped_reason = "no-legacy-datadir";
    else
        result = migrate_legacy_data_dir(legacy_dir, new_dir);
    if (!result.performed)
        BOOST_LOG_TRIVIAL(debug) << "Legacy data import skipped: " << result.skipped_reason;
    g_last_migration = result;
    return result;
}

std::string legacy_prusaslicer_data_dir(const std::string &default_new_dir)
{
    if (default_new_dir.empty())
        return {};
    return (fs::path(default_new_dir).parent_path() / "PrusaSlicer").make_preferred().string();
}

const LegacyMigrationResult& last_legacy_migration() { return g_last_migration; }

} // namespace Slic3r
