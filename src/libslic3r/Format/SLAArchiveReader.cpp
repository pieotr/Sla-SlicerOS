///|/ Copyright (c) Prusa Research 2020 - 2023 Tomáš Mészáros @tamasmeszaros, Pavel Mikuš @Godrak, Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena, Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "SLAArchiveReader.hpp"
#include "SL1.hpp"
#include "SL1_SVG.hpp"
#include "libslic3r/I18N.hpp"

#include "libslic3r/SlicesToTriangleMesh.hpp"

#include <boost/filesystem/path.hpp>
#include <boost/algorithm/string.hpp>

#include "SLAArchiveFormatRegistry.hpp"

#include <array>
#include <map>

namespace Slic3r {

std::unique_ptr<SLAArchiveReader> SLAArchiveReader::create(
    const std::string       &fname,
    const std::string       &format_id,
    SLAImportQuality         quality,
    const ProgrFn & progr)
{
    // Explicit IDs also allow extensionless files. Do not silently substitute
    // another reader when a caller requests an unknown or unsupported format.
    if (!format_id.empty()) {
        const auto factory = get_reader_factory(format_id.c_str());
        return factory ? factory(fname, quality, progr) : nullptr;
    }

    // Keep compound suffixes intact. Equal suffixes retain registry order until
    // content-based detection is available for ambiguous containers such as ZIP.
    const std::string filename = boost::algorithm::to_lower_copy(
        boost::filesystem::path(fname).filename().string());
    const ArchiveEntry *match = nullptr;
    size_t longest_suffix = 0;
    for (const ArchiveEntry &entry : registered_sla_archives()) {
        for (const std::string &extension : get_extensions(entry)) {
            const std::string suffix = "." + boost::algorithm::to_lower_copy(extension);
            if (suffix.size() > longest_suffix && boost::algorithm::ends_with(filename, suffix)) {
                match = &entry;
                longest_suffix = suffix.size();
            }
        }
    }
    return match && match->rdfactoryfn ? match->rdfactoryfn(fname, quality, progr) : nullptr;
}

struct SliceParams { double layerh = 0., initial_layerh = 0.; };

static SliceParams get_slice_params(const DynamicPrintConfig &cfg)
{
    auto *opt_layerh = cfg.option<ConfigOptionFloat>("layer_height");
    auto *opt_init_layerh = cfg.option<ConfigOptionFloat>("initial_layer_height");

    if (!opt_layerh || !opt_init_layerh)
        throw MissingProfileError("Invalid SL1 / SL1S file");

    return SliceParams{opt_layerh->getFloat(), opt_init_layerh->getFloat()};
}

ConfigSubstitutions import_sla_archive(const std::string       &zipfname,
                                       const std::string       &format_id,
                                       indexed_triangle_set    &out,
                                       DynamicPrintConfig      &profile,
                                       SLAImportQuality         quality,
                                       const ProgrFn & progr)
{
    ConfigSubstitutions ret;

    if (auto reader = SLAArchiveReader::create(zipfname, format_id, quality, progr)) {
        std::vector<ExPolygons> slices;
        ret = reader->read(slices, profile);

        SliceParams slicp = get_slice_params(profile);

        if (!slices.empty())
            out = slices_to_mesh(slices, 0, slicp.layerh, slicp.initial_layerh);

    } else {
        throw ReaderUnimplementedError("Reader unimplemented");
    }

    return ret;
}

ConfigSubstitutions import_sla_archive(const std::string  &zipfname,
                                       const std::string  &format_id,
                                       DynamicPrintConfig &out)
{
    ConfigSubstitutions ret;

    if (auto reader = SLAArchiveReader::create(zipfname, format_id)) {
        ret = reader->read(out);
    } else {
        throw ReaderUnimplementedError("Reader unimplemented");
    }

    return ret;
}

} // namespace Slic3r
