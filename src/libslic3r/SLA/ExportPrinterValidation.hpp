// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <string>
#include "libslic3r/Exception.hpp"

namespace Slic3r::sla {
// Validate inexpensive printer metadata before decoding or projecting masks.
// Legacy unowned writers retain their existing allowance for an empty ID.
inline std::string cxdlpv4_printer_identifier_error(const std::string &identifier, bool owned)
{
    if ((owned && identifier.empty()) || identifier.size()>255 || identifier.find('\0')!=std::string::npos)
        return "Invalid CXDLPv4 printer model identifier: use 1 to 255 bytes without null characters";
    return {};
}
inline void require_cxdlpv4_printer_identifier(const std::string &identifier, bool owned)
{
    const auto error=cxdlpv4_printer_identifier_error(identifier,owned);
    if (!error.empty()) throw ExportError(error);
}
} // namespace Slic3r::sla
