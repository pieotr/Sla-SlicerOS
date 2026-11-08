// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <string_view>

namespace Slic3r::sla {
// Native addressed ChituboxFile family; CTBEncryptedFile has a separate schema.
inline bool is_chitubox_layer_format(std::string_view id)
{
    return id == "ctb" || id == "ctb_v1" || id == "ctb_v3" || id == "ctb_v4" ||
           id == "ctb_v5" || id == "ctb_gktwo" || id == "cbddlp" ||
           id == "cbddlp_v1" || id == "photon" || id == "photon_v1";
}
inline bool is_ctb_encrypted_layer_format(std::string_view id)
{
    return id == "ctb_encrypted" || id == "ctb_encrypted_v4";
}
inline bool is_ctb_mask_format(std::string_view id)
{
    return is_chitubox_layer_format(id) || is_ctb_encrypted_layer_format(id);
}
inline bool is_chitubox_binary_format(std::string_view id)
{
    return id == "cbddlp" || id == "cbddlp_v1" || id == "photon" || id == "photon_v1";
}
}
