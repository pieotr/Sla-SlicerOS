///|/ Copyright (c) Prusa Research 2019 - 2020 Vojtěch Bubník @bubnikv
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "ObjectID.hpp"

namespace Slic3r {

size_t ObjectBase::s_last_id = 0;

ObjectWithTimestamp::Timestamp ObjectWithTimestamp::s_last_timestamp = 1;

} // namespace Slic3r

// CEREAL_REGISTER_TYPE(Slic3r::ObjectBase)
