// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <boost/filesystem.hpp>
#include <string>
#include "libslic3r/Exception.hpp"
#include "ExportGeneration.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r::sla {
// Writers and their readback validators work on a sibling staging file. The
// destination is replaced only after both succeed and cancellation is checked.
// The extension override lets native readers recognize a converted staging
// file even when the caller's destination suffix is an alias.
template<class Export, class Cancel>
void publish_export(const std::string &filename, Export &&write, Cancel &&check_cancel,
                    const std::string &staged_extension = {},
                    const ExportGenerationGuard *generation = nullptr)
{
    check_cancel();
    const boost::filesystem::path destination(filename);
    const auto temporary = destination.parent_path() /
        (boost::filesystem::unique_path(".sla-export-%%%%-%%%%").string() +
            (staged_extension.empty() ? destination.extension().string() : "." + staged_extension));
    try {
        write(temporary.string());
        if (!boost::filesystem::exists(temporary) || boost::filesystem::file_size(temporary) == 0)
            throw FileIOError("The SLA encoder did not produce a complete file");
        // Encoding/readback may take a long time. Honor cancellation before
        // replacing the user's previous result, and remove the staged file.
        check_cancel();
        if (generation) {
            generation->publish(temporary.string(), destination.string());
        } else {
            const auto error = rename_file(temporary.string(), destination.string());
            if (error) throw FileIOError("Cannot replace SLA output: " + error.message());
        }
    } catch (...) {
        boost::system::error_code ignored;
        boost::filesystem::remove(temporary, ignored);
        throw;
    }
}
} // namespace Slic3r::sla
