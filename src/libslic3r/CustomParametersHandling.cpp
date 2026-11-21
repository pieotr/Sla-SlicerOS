#include "CustomParametersHandling.hpp"

#include "nlohmann/json.hpp"

#include <exception>

namespace Slic3r {

std::optional<CustomParameterValues> parse_custom_parameters(const std::string& input)
{
    if (input.empty())
        return CustomParameterValues();

    using json = nlohmann::json;

    try {
        json j = json::parse(input);

        if (!j.is_object()) {
            return std::nullopt;
        }

        CustomParameterValues values;

        for (auto& [key, val] : j.items()) {
            if (val.is_null()) {
                values[key] = std::monostate{};
            } else if (val.is_string()) {
                values[key] = val.get<std::string>();
            } else if (val.is_number_integer()) {
                values[key] = val.get<int>();
            } else if (val.is_number_float()) {
                values[key] = val.get<double>();
            } else if (val.is_boolean()) {
                values[key] = val.get<bool>();
            } else if (val.is_array()) {
                    return std::nullopt;
            } else {
                // Unsupported type
                return std::nullopt;
            }
        }
        return values;
    } catch (const json::parse_error&) {
        return std::nullopt;
    }

    return std::nullopt;
}



std::string merge_json(const std::string& bottom_json, const std::string& top_json)
{
    if (bottom_json.empty())
        return top_json;
    if (top_json.empty())
        return bottom_json;

    using json = nlohmann::json;

    try {
        json j_bottom = json::parse(bottom_json);
        json j_top = json::parse(top_json);

        if (!j_bottom.is_object() || !j_top.is_object()) {
            return top_json;
        }

        j_bottom.merge_patch(j_top);
        return j_bottom.dump();
    }
    catch (const json::parse_error&) {
        return top_json;
    }
}

} // namespace Slic3r
