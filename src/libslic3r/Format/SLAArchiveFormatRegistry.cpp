///|/ Copyright (c) Prusa Research 2023 Tomáš Mészáros @tamasmeszaros, Pavel Mikuš @Godrak
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include <set>
#include <memory>
#include <algorithm>
#include <cctype>

#include "SL1.hpp"
#include "SL1_SVG.hpp"
#include "AnycubicSLA.hpp"
#include "CrealityCXDLPv4.hpp"
#include "SLAArchiveWriterFactory.hpp"
#include "libslic3r/I18N.hpp"
#include "SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Format/SLAArchiveReader.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {

class Registry {
    static std::unique_ptr<Registry> registry;

    std::set<ArchiveEntry> entries;

    Registry ()
    {
        entries = {
            {
                "SL1",                      // id
                L("SL1 archive"),    // description
                "sl1",                      // main extension
                {"sl1s", "zip", "rgb.zip"},            // extension aliases

                // Writer factory
                [] (const auto &cfg) { return std::make_unique<SL1Archive>(cfg); },

                // Reader factory
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "SL1SVG",
                L("SL1 SVG archive"),
                "sl1_svg",
                {"zip"},
                [] (const auto &cfg) { return std::make_unique<SL1_SVGArchive>(cfg); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1_SVGReader>(fname, quality, progr);
                }
            },
            // Anycubic / Photon - All supported variants
            anycubic_sla_format_versioned("pws", "Photon / Photon S", ANYCUBIC_SLA_FORMAT_VERSION_1),
            anycubic_sla_format_versioned("pw0", "Photon Zero", ANYCUBIC_SLA_FORMAT_VERSION_1),
            anycubic_sla_format_versioned("pwx", "Photon X", ANYCUBIC_SLA_FORMAT_VERSION_1),
            anycubic_sla_format_versioned("dlp", "Photon Ultra", ANYCUBIC_SLA_FORMAT_VERSION_1),
            anycubic_sla_format_versioned("dl2p", "Photon D2", ANYCUBIC_SLA_FORMAT_VERSION_515),
            anycubic_sla_format_versioned("pwmx", "Photon Mono X", ANYCUBIC_SLA_FORMAT_VERSION_1),
            anycubic_sla_format_versioned("pmx2", "Photon Mono X2", ANYCUBIC_SLA_FORMAT_VERSION_515),
            anycubic_sla_format_versioned("pwmb", "Photon Mono X 6K / M3 Plus", ANYCUBIC_SLA_FORMAT_VERSION_515),
            anycubic_sla_format_versioned("px6s", "Photon Mono X 6Ks", ANYCUBIC_SLA_FORMAT_VERSION_515),
            anycubic_sla_format_versioned("pwmo", "Photon Mono", ANYCUBIC_SLA_FORMAT_VERSION_1),
            anycubic_sla_format_versioned("pm3n", "Photon Mono 2", ANYCUBIC_SLA_FORMAT_VERSION_515),
            anycubic_sla_format_versioned("pm4n", "Photon Mono 4", ANYCUBIC_SLA_FORMAT_VERSION_515),
            anycubic_sla_format_versioned("pwms", "Photon Mono SE", ANYCUBIC_SLA_FORMAT_VERSION_1),
            anycubic_sla_format_versioned("pwma", "Photon Mono 4K", ANYCUBIC_SLA_FORMAT_VERSION_515),
            anycubic_sla_format_versioned("pmsq", "Photon Mono SQ", ANYCUBIC_SLA_FORMAT_VERSION_1),
            anycubic_sla_format_versioned("pm3", "Photon M3", ANYCUBIC_SLA_FORMAT_VERSION_515),
            anycubic_sla_format_versioned("pm3m", "Photon M3 Max", ANYCUBIC_SLA_FORMAT_VERSION_515),
            anycubic_sla_format_versioned("pm3r", "Photon M3 Premium", ANYCUBIC_SLA_FORMAT_VERSION_515),
            anycubic_sla_format_versioned("pm5", "Photon Mono M5", ANYCUBIC_SLA_FORMAT_VERSION_515),
            anycubic_sla_format_versioned("pm5s", "Photon Mono M5s", ANYCUBIC_SLA_FORMAT_VERSION_515),
            anycubic_sla_format_versioned("m5sp", "Photon Mono M5s Pro", ANYCUBIC_SLA_FORMAT_VERSION_515),
            anycubic_sla_format_versioned("pwc", "Anycubic Custom", ANYCUBIC_SLA_FORMAT_VERSION_515),
            anycubic_sla_format_versioned("pm4u", "Photon Mono 4U", ANYCUBIC_SLA_FORMAT_VERSION_515),
            anycubic_sla_format_versioned("pm7", "Photon Mono M7", ANYCUBIC_SLA_FORMAT_VERSION_515),
            anycubic_sla_format_versioned("pm7m", "Photon Mono M7 Max", ANYCUBIC_SLA_FORMAT_VERSION_515),
            anycubic_sla_format_versioned("pwsz", "Photon Mono M7 Pro", ANYCUBIC_SLA_FORMAT_VERSION_515),

            // Chitubox / Photon variants (CBDDLPv1, CBDDLPv2, CTBv3, CTBv4, encrypted CTB)
            {
                "cbddlp",
                L("Chitubox CBDDLP"),
                "cbddlp",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "cbt",
                L("Chitubox CTB"),
                "cbt",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "photon",
                L("Chitubox Photon"),
                "photon",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "photons",
                L("Chitubox PhotonS"),
                "photons",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "ctb",
                L("Chitubox CTB v3/v4"),
                "ctb",
                {"encrypted.ctb"},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "phz",
                L("Chitubox PHZ"),
                "phz",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },

            // Creality CXDLP family
            {
                "cxdlp",
                L("Creality CXDLP"),
                "cxdlp",
                {"v1.cxdlp"},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "cxdlpv4",
                L("Creality CXDLPv4"),
                "cxdlpv4",
                {},
                [] (const auto &cfg) { return std::make_unique<CrealityCXDLPv4Archive>(cfg); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },

            // NanoDLP
            {
                "nanodlp",
                L("NanoDLP"),
                "nanodlp",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },

            // Longer Orange (LGS family)
            {
                "lgs",
                L("Longer Orange 10"),
                "lgs",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "lgs30",
                L("Longer Orange 30"),
                "lgs30",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "lgs120",
                L("Longer Orange 120"),
                "lgs120",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "lgs4k",
                L("Longer Orange 4K"),
                "lgs4k",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },

            // Voxeladditives / Other families
            {
                "vdt",
                L("Voxeldance Tango"),
                "vdt",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "cws",
                L("NovaMaker CWS"),
                "cws",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "n4",
                L("Anet N4"),
                "n4",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "n7",
                L("Anet N7"),
                "n7",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "fdg",
                L("Voxelab FDG"),
                "fdg",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "goo",
                L("Elegoo GOO"),
                "goo",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "prz",
                L("Phrozen Sonic Mini 8K S"),
                "prz",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "zcode",
                L("UnizMaker IBEE"),
                "zcode",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "jxs",
                L("Uniformation GKone"),
                "jxs",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "zcodex",
                L("Z-Suite ZCodex"),
                "zcodex",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "mdlp",
                L("Makerbase MDLP"),
                "mdlp",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "gr1",
                L("GR1 Workshop"),
                "gr1",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "svgx",
                L("Flashforge SVGX"),
                "svgx",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "qdt",
                L("Emake3D Galaxy"),
                "qdt",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "osla",
                L("Open SLA"),
                "osla",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "osf",
                L("Vlare OSF"),
                "osf",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
            {
                "uvj",
                L("UVJ Format"),
                "uvj",
                {},
                [] (const auto &cfg) { return std::make_unique<AnycubicSLAArchive>(cfg, ANYCUBIC_SLA_FORMAT_VERSION_1); },
                [] (const std::string &fname, SLAImportQuality quality, const ProgrFn &progr) {
                    return std::make_unique<SL1Reader>(fname, quality, progr);
                }
            },
        };
    }

public:

    static const Registry& get_instance()
    {
        if (!registry)
            registry.reset(new Registry());

        return *registry;
    }

    static const std::set<ArchiveEntry>& get()
    {
        return get_instance().entries;
    }
};

std::unique_ptr<Registry> Registry::registry = nullptr;

const std::set<ArchiveEntry>& registered_sla_archives()
{
    return Registry::get();
}

std::vector<std::string> get_extensions(const ArchiveEntry &entry)
{
    auto ret = reserve_vector<std::string>(entry.ext_aliases.size() + 1);

    ret.emplace_back(entry.ext);
    for (const char *alias : entry.ext_aliases)
        ret.emplace_back(alias);

    return ret;
}

namespace {
const ArchiveEntry *find_archive_entry_by_id(const char *formatid);
}

ArchiveWriterFactory get_writer_factory(const char *formatid)
{
    ArchiveWriterFactory ret;
    if (const ArchiveEntry *entry = find_archive_entry_by_id(formatid))
        ret = entry->wrfactoryfn;

    return ret;
}

ArchiveReaderFactory get_reader_factory(const char *formatid)
{

    ArchiveReaderFactory ret;
    if (const ArchiveEntry *entry = find_archive_entry_by_id(formatid))
        ret = entry->rdfactoryfn;

    return ret;
}

const char *get_default_extension(const char *formatid)
{
    static constexpr const char *Empty = "";

    const char * ret = Empty;

    if (const ArchiveEntry *entry = find_archive_entry_by_id(formatid))
        ret = entry->ext;

    return ret;
}

const ArchiveEntry * get_archive_entry(const char *formatid)
{
    return find_archive_entry_by_id(formatid);
}

namespace {

std::string to_lower_copy(const char *str)
{
    if (str == nullptr)
        return {};

    std::string out(str);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

const ArchiveEntry *find_archive_entry_by_id(const char *formatid)
{
    if (formatid == nullptr || *formatid == '\0')
        return nullptr;

    const auto &entries = Registry::get();
    auto it = entries.find(ArchiveEntry{formatid});
    if (it != entries.end())
        return &(*it);

    const std::string needle = to_lower_copy(formatid);
    for (const ArchiveEntry &entry : entries)
        if (to_lower_copy(entry.id) == needle)
            return &entry;

    return nullptr;
}

std::string normalized_extension(const char *extension)
{
    if (extension == nullptr)
        return {};

    std::string out(extension);
    if (!out.empty() && out.front() == '.')
        out.erase(out.begin());

    // Keep extension tokens strict to avoid UI wildcard artifacts leaking in
    // (for example trailing ')' from malformed filename edits).
    out.erase(std::remove_if(out.begin(), out.end(), [](unsigned char c) {
        return !std::isalnum(c);
    }), out.end());

    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool extension_matches(const std::string &needle, const char *candidate)
{
    if (candidate == nullptr)
        return false;

    const std::string cand = normalized_extension(candidate);
    return needle == cand;
}

} // namespace

const ArchiveEntry * get_archive_entry_by_extension(const char *extension)
{
    const std::string needle = normalized_extension(extension);
    if (needle.empty())
        return nullptr;

    for (const ArchiveEntry &entry : Registry::get()) {
        if (extension_matches(needle, entry.ext))
            return &entry;
        for (const char *alias : entry.ext_aliases)
            if (extension_matches(needle, alias))
                return &entry;
    }

    return nullptr;
}

} // namespace Slic3r
