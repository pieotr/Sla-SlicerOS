// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ResinStock.hpp"
#include "ExportPublication.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <cmath>
#include <iterator>

namespace Slic3r::sla {

bool ResinMaterial::consume(double ml, double seconds)
{
    if (!(ml > 0.) || bottle_volume_ml == 0.) return bottles_in_stock > 0;
    // UVtools: bottles = (int)(volume / bottleVolume); remainder = volume % bottleVolume.
    int bottles = int(ml / bottle_volume_ml);
    const double remainder = std::fmod(ml, bottle_volume_ml);
    if (remainder > 0.) {
        double remaining = bottle_remaining_ml - remainder;
        if (remaining < 0.) {
            ++bottles;
            remaining += bottle_volume_ml;
        }
        bottle_remaining_ml = remaining;
    }
    bottles_in_stock -= bottles;
    consumed_ml += ml;
    print_time_h += seconds / 3600.;
    ++print_count;
    return bottles_in_stock > 0;
}

std::string validate_resin_material(const ResinMaterial &m)
{
    if (m.name.empty()) return "The material name is empty.";
    if (m.name.find('\0') != std::string::npos) return "The material name contains a NUL character.";
    auto bad = [](double v) { return !std::isfinite(v) || v < 0.; };
    if (!std::isfinite(m.bottle_volume_ml) || m.bottle_volume_ml <= 0.) return "The bottle volume must be greater than zero.";
    if (bad(m.density)) return "The density must be a finite non-negative number.";
    if (bad(m.bottle_cost)) return "The bottle cost must be a finite non-negative number.";
    if (bad(m.bottle_remaining_ml) || m.bottle_remaining_ml > m.bottle_volume_ml) return "The remaining volume must be between zero and the bottle volume.";
    if (bad(m.consumed_ml) || bad(m.print_time_h)) return "The consumed volume and print time must be finite non-negative numbers.";
    if (m.print_count < 0) return "The print count must not be negative.";
    return {};
}

ResinMaterial       *ResinStock::find(const std::string &name) { for (auto &m : materials) if (m.name == name) return &m; return nullptr; }
const ResinMaterial *ResinStock::find(const std::string &name) const { for (auto &m : materials) if (m.name == name) return &m; return nullptr; }

void ResinStock::add(ResinMaterial material)
{
    if (const auto problem = validate_resin_material(material); !problem.empty()) throw ResinStockError(problem);
    if (find(material.name)) throw ResinStockError("A material named \"" + material.name + "\" already exists.");
    materials.push_back(std::move(material));
}

bool ResinStock::remove(const std::string &name)
{
    for (auto it = materials.begin(); it != materials.end(); ++it)
        if (it->name == name) { materials.erase(it); return true; }
    return false;
}

std::string ResinStock::default_path() { return (boost::filesystem::path(data_dir()) / "resin_stock.json").string(); }

namespace {
constexpr int stock_version = 1;
template<class T> T field(const nlohmann::json &object, const char *key)
{
    if (!object.contains(key)) throw ResinStockError(std::string("missing field \"") + key + "\"");
    try { return object.at(key).get<T>(); }
    catch (const nlohmann::json::exception &) { throw ResinStockError(std::string("field \"") + key + "\" has the wrong type"); }
}
}

ResinStock ResinStock::load(const std::string &path)
{
    boost::system::error_code ec;
    if (!boost::filesystem::exists(path, ec)) return {};
    boost::nowide::ifstream in(path, std::ios::binary);
    if (!in) throw ResinStockError("Cannot open the resin stock file " + path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ResinStock stock;
    try {
        const auto root = nlohmann::json::parse(text);
        if (!root.is_object()) throw ResinStockError("the top level is not an object");
        if (field<int>(root, "version") != stock_version) throw ResinStockError("unsupported version");
        const auto &list = root.at("materials");
        if (!list.is_array()) throw ResinStockError("\"materials\" is not an array");
        for (const auto &item : list) {
            if (!item.is_object()) throw ResinStockError("a material entry is not an object");
            ResinMaterial m;
            m.name = field<std::string>(item, "name");
            m.bottle_volume_ml = field<double>(item, "bottle_volume_ml");
            m.density = field<double>(item, "density");
            m.bottle_cost = field<double>(item, "bottle_cost");
            m.bottles_in_stock = field<int>(item, "bottles_in_stock");
            m.bottle_remaining_ml = field<double>(item, "bottle_remaining_ml");
            m.consumed_ml = field<double>(item, "consumed_ml");
            m.print_time_h = field<double>(item, "print_time_h");
            m.print_count = field<int>(item, "print_count");
            if (const auto problem = validate_resin_material(m); !problem.empty())
                throw ResinStockError("material \"" + m.name + "\": " + problem);
            stock.add(std::move(m));
        }
    } catch (const ResinStockError &error) {
        throw ResinStockError("The resin stock file " + path + " is not valid: " + error.what() +
            ". It was not changed; fix or move it away.");
    } catch (const std::exception &error) {
        throw ResinStockError("The resin stock file " + path + " is not valid: " + error.what() +
            ". It was not changed; fix or move it away.");
    }
    return stock;
}

void ResinStock::save(const std::string &path) const
{
    nlohmann::json root;
    root["version"] = stock_version;
    auto &list = root["materials"] = nlohmann::json::array();
    for (const auto &m : materials) {
        if (const auto problem = validate_resin_material(m); !problem.empty())
            throw ResinStockError("Cannot save material \"" + m.name + "\": " + problem);
        list.push_back({{"name", m.name}, {"bottle_volume_ml", m.bottle_volume_ml}, {"density", m.density},
                        {"bottle_cost", m.bottle_cost}, {"bottles_in_stock", m.bottles_in_stock},
                        {"bottle_remaining_ml", m.bottle_remaining_ml}, {"consumed_ml", m.consumed_ml},
                        {"print_time_h", m.print_time_h}, {"print_count", m.print_count}});
    }
    const std::string text = root.dump(2) + "\n";
    boost::system::error_code ec;
    boost::filesystem::create_directories(boost::filesystem::path(path).parent_path(), ec);
    try {
        publish_export(path, [&](const std::string &staged) {
            {
                boost::nowide::ofstream out(staged, std::ios::binary | std::ios::trunc);
                if (!out) throw ResinStockError("Cannot create the staging file for the resin stock");
                out << text;
                out.flush();
                if (!out) throw ResinStockError("Cannot write the resin stock");
            }
            // Readback: what is about to replace the old file must parse back to the same inventory.
            boost::nowide::ifstream in(staged, std::ios::binary);
            const std::string back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (back != text) throw ResinStockError("The staged resin stock does not match what was written");
        }, [] {});
    } catch (const ResinStockError &) {
        throw;
    } catch (const std::exception &error) {
        throw ResinStockError(std::string("Cannot save the resin stock: ") + error.what());
    }
}

} // namespace Slic3r::sla
