#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <LocalesUtils.hpp>
#include <cereal/types/polymorphic.hpp>
#include <cereal/types/string.hpp> 
#include <cereal/types/vector.hpp> 
#include <cereal/archives/binary.hpp>

using namespace Slic3r;


TEST_CASE("Dynamic config serialization - tests ConfigBase", "[Config]"){
    DynamicPrintConfig config;
    INFO("Serialize float");
    config.set_key_value("layer_height", new ConfigOptionFloat(0.3));
    CHECK(config.opt_serialize("layer_height") == "0.3");

    INFO("Serialize int");
    config.set_key_value("faded_layers", new ConfigOptionInt(12));
    CHECK(config.opt_serialize("faded_layers") == "12");

    INFO("Serialize percent");
    config.set_key_value("support_small_pillar_diameter_percent", new ConfigOptionPercent(30));
    CHECK(config.opt_serialize("support_small_pillar_diameter_percent") == "30%");

    INFO("Serialize bool");
    config.set_key_value("supports_enable", new ConfigOptionBool(true));
    CHECK(config.opt_serialize("supports_enable") == "1");

    INFO("Serialize enum");
    config.set_key_value("display_orientation", new ConfigOptionEnum<SLADisplayOrientation>(sladoLandscape));
    CHECK(config.opt_serialize("display_orientation") == "landscape");
    config.set_deserialize_strict("display_orientation", "portrait");
    CHECK(config.option<ConfigOptionEnum<SLADisplayOrientation>>("display_orientation")->value == sladoPortrait);

    INFO("Serialize string");
    config.set_key_value("sla_archive_format", new ConfigOptionString("A"));
    CHECK(config.opt_serialize("sla_archive_format") == "A");

    INFO("Serialize string with newline");
    config.set_key_value("sla_archive_format", new ConfigOptionString("foo\nbar"));
    CHECK(config.opt_serialize("sla_archive_format") == "foo\\nbar");
    config.set_deserialize_strict("sla_archive_format", "bar\\nbaz");
    INFO("Deserialize string with newline");
    CHECK(config.opt_string("sla_archive_format") == "bar\nbaz");

    INFO("Serialize points");
    config.set_key_value("bed_shape", new ConfigOptionPoints({{10, 20}, {30, 45}}));
    CHECK(config.opt_serialize("bed_shape") == "10x20,30x45");
    INFO("Deserialize points");
    config.set_deserialize_strict("bed_shape", "20x10");
    CHECK(config.option<ConfigOptionPoints>("bed_shape")->values == std::vector{Vec2d{20, 10}});

    INFO("Serialize floats");
    config.set_key_value("delay_before_exposure", new ConfigOptionFloats({0.2, 3}));
    CHECK(config.opt_serialize("delay_before_exposure") == "0.2,3");
    INFO("Deserialize floats");
    config.set_deserialize_strict("delay_before_exposure", "0.1,0.4");
    CHECK_THAT(config.option<ConfigOptionFloats>("delay_before_exposure")->values, Catch::Matchers::Approx(std::vector{0.1, 0.4}));
    INFO("Deserialize floats from one value");
    config.set_deserialize_strict("delay_before_exposure", "3");
    CHECK_THAT(config.option<ConfigOptionFloats>("delay_before_exposure")->values, Catch::Matchers::Approx(std::vector{3.0}));

    INFO("Serialize ints");
    config.set_key_value("tilt_down_cycles", new ConfigOptionInts({180, 210}));
    CHECK(config.opt_serialize("tilt_down_cycles") == "180,210");
    INFO("Deserialize ints");
    config.set_deserialize_strict("tilt_down_cycles", "195,220");
    CHECK(config.option<ConfigOptionInts>("tilt_down_cycles")->values == std::vector{195,220});

    INFO("Serialize bools");
    config.set_key_value("use_tilt", new ConfigOptionBools({true, false}));
    CHECK(config.opt_serialize("use_tilt") == "1,0");
    INFO("Deserialize bools");
    config.set_deserialize_strict("use_tilt", "0,1,1");
    CHECK(config.option<ConfigOptionBools>("use_tilt")->values == std::vector<unsigned char>{false, true, true});

    INFO("Deserialize bools from empty stirng");
    config.set_deserialize_strict("use_tilt", "");
    CHECK(config.option<ConfigOptionBools>("use_tilt")->values == std::vector<unsigned char>{});

    INFO("Deserialize bools from value");
    config.set_deserialize_strict({{"use_tilt", 1}});
    CHECK(config.option<ConfigOptionBools>("use_tilt")->values == std::vector<unsigned char>{true});

    INFO("Serialize strings");
    config.set_key_value("compatible_printers", new ConfigOptionStrings({"foo", "bar"}));
    CHECK(config.opt_serialize("compatible_printers") == "foo;bar");
    INFO("Deserialize strings");
    config.set_deserialize_strict("compatible_printers", "bar;baz");
    CHECK(config.option<ConfigOptionStrings>("compatible_printers")->values == std::vector<std::string>{"bar", "baz"});
}

TEST_CASE("Get keys", "[Config]"){
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    CHECK(!config.keys().empty());
    CHECK(config.has("layer_height"));
    CHECK(config.has("bed_shape"));
}

TEST_CASE("Set not already set option", "[Config]") {
    DynamicPrintConfig config;
    config.set_deserialize_strict("faded_layers", "3");
    CHECK(config.opt_int("faded_layers") == 3);
}

TEST_CASE("Config apply dynamic to static", "[Config]") {
    DynamicPrintConfig config;
    config.set_deserialize_strict("faded_layers", "7");

    SLAFullPrintConfig config2;
    config2.apply(config, true);

    CHECK(config2.opt_int("faded_layers") == 7);
}

TEST_CASE("Config apply static to dynamic", "[Config]") {
    SLAFullPrintConfig config;

    DynamicPrintConfig config2;
    config2.apply(config, true);

    CHECK(
        config2.opt_int("faded_layers") ==
        DynamicPrintConfig::full_print_config().opt_int("faded_layers")
    );
}

TEST_CASE("Config apply dynamic to dynamic", "[Config]") {

    DynamicPrintConfig config;
    config.set_key_value("bed_shape", new ConfigOptionPoints({{0, 0}, {20, 0}, {0, 20}}));
    DynamicPrintConfig config2;
    config2.apply(config, true);

    CHECK(
        config2.option<ConfigOptionPoints>("bed_shape")->values ==
        std::vector<Vec2d>{{0, 0}, {20, 0}, {0, 20}}
    );
}

TEST_CASE("No interference between DynamicConfig objects", "[Config]") {
    DynamicPrintConfig config;
    config.set_key_value("sla_archive_format", new ConfigOptionString("line"));
    DynamicPrintConfig config2;
    config2.set_key_value("sla_archive_format", new ConfigOptionString("hilbertcurve"));
    CHECK(config.opt_string("sla_archive_format") == "line");
}

struct SerializationTestData {
    std::string name;
    std::vector<std::string> values;
    std::string serialized;
};

TEST_CASE("Config serialization of multiple values", "[Config]"){
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    std::vector<SerializationTestData> test_data{
        {
            "empty",
            {},
            ""
        },
        {
            "single empty",
            {""},
            "\"\""
        },
        {
            "single noempty, simple",
            {"RGB"},
            "RGB"
        },
        {
            "multiple noempty, simple",
            {"ABC", "DEF", "09182745@!#$*(&"},
            "ABC;DEF;09182745@!#$*(&"
        },
        {
            "multiple, simple, some empty",
            {"ABC", "DEF", "", "09182745@!#$*(&", ""},
            "ABC;DEF;;09182745@!#$*(&;"
        },
        {
            "complex",
            {"some \"quoted\" notes", "yet\n some notes", "whatever \n notes", ""},
            "\"some \\\"quoted\\\" notes\";\"yet\\n some notes\";\"whatever \\n notes\";"
        }
    };

    for (const SerializationTestData& data : test_data) {
        config.set_key_value("compatible_printers", new ConfigOptionStrings(data.values));
        CHECK(config.opt_serialize("compatible_printers") == data.serialized);

        config.set_deserialize_strict("compatible_printers", "");
        CHECK(config.option<ConfigOptionStrings>("compatible_printers")->values == std::vector<std::string>{});

        config.set_deserialize_strict("compatible_printers", data.serialized);
        CHECK(config.option<ConfigOptionStrings>("compatible_printers")->values == data.values);
    }
}

SCENARIO("Config accessor functions perform as expected.", "[Config]") {
    auto test = [](ConfigBase &config) {
        WHEN("A boolean option is set to a boolean value") {
            REQUIRE_NOTHROW(config.set("supports_enable", false));
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionBool>("supports_enable")->getBool() == false);
            }
        }
        WHEN("A boolean option is set to a string value representing a 0 or 1") {
            CHECK_NOTHROW(config.set_deserialize_strict("supports_enable", "0"));
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionBool>("supports_enable")->getBool() == false);
            }
        }
        WHEN("A boolean option is set to a string value representing something other than 0 or 1") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("supports_enable", "Z"), BadOptionTypeException);
            }
            AND_THEN("Value is unchanged.") {
                REQUIRE(config.opt<ConfigOptionBool>("supports_enable")->getBool() == true);
            }
        }
        WHEN("A boolean option is set to an int value") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("supports_enable", 1), BadOptionTypeException);
            }
        }
        WHEN("A numeric option is set from serialized string") {
            config.set_deserialize_strict("tilt_down_cycles", "100");
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionInts>("tilt_down_cycles")->get_at(0) == 100);
            }
        }
        WHEN("An floating-point option is set through the integer interface") {
            config.set("layer_height", 1);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionFloat>("layer_height")->getFloat() == 1.0);
            }
        }
        WHEN("A floating-point option is set through the double interface") {
            config.set("layer_height", 0.5);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionFloat>("layer_height")->getFloat() == 0.5);
            }
        }
        WHEN("An integer-based option is set through the double interface") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("faded_layers", 5.5), BadOptionTypeException);
            }
        }
        WHEN("A numeric option is set to a non-numeric value.") {
            THEN("A BadOptionValueException exception is thown.") {
                REQUIRE_THROWS_AS(config.set_deserialize_strict("layer_height", "zzzz"), BadOptionValueException);
            }
            THEN("The value does not change.") {
                REQUIRE(config.opt<ConfigOptionFloat>("layer_height")->getFloat() == 0.05);
            }
        }
        WHEN("A string option is set through the string interface") {
            config.set("sla_archive_format", "100");
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionString>("sla_archive_format")->value == "100");
            }
        }
        WHEN("A string option is set through the integer interface") {
            config.set("sla_archive_format", 100);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionString>("sla_archive_format")->value == "100");
            }
        }
        WHEN("A string option is set through the double interface") {
            config.set("sla_archive_format", 100.5);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionString>("sla_archive_format")->value == float_to_string_decimal_point(100.5));
            }
        }
        WHEN("A percent option is set through the string interface.") {
            config.set_deserialize_strict("support_small_pillar_diameter_percent", "50%");
            THEN("The value is 50") {
                REQUIRE(config.opt<ConfigOptionPercent>("support_small_pillar_diameter_percent")->value == 50);
            }
        }
        WHEN("An invalid option is requested during set.") {
            THEN("A UnknownOptionException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", 1), UnknownOptionException);
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", 1.0), UnknownOptionException);
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", "1"), UnknownOptionException);
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", true), UnknownOptionException);
            }
        }

        WHEN("An invalid option is requested during get.") {
            THEN("A UnknownOptionException exception is thrown.") {
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionString>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionFloat>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionInt>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionBool>("deadbeef_invalid_option", false), UnknownOptionException);
            }
        }

        WHEN("getX called on an unset option.") {
            THEN("The default is returned.") {
                REQUIRE(config.opt_float("layer_height") == 0.05);
                REQUIRE(config.opt_int("faded_layers") == 10);
                REQUIRE(config.opt_bool("supports_enable") == true);
            }
        }

        WHEN("getFloat called on an option that has been set.") {
            config.set("layer_height", 0.5);
            THEN("The set value is returned.") {
                REQUIRE(config.opt_float("layer_height") == 0.5);
            }
        }
    };
    GIVEN("DynamicPrintConfig generated from default options") {
        auto config = Slic3r::DynamicPrintConfig::full_print_config();
        test(config);
    }
    GIVEN("SLAFullPrintConfig generated from default options") {
        Slic3r::SLAFullPrintConfig config;
        test(config);
    }
}

SCENARIO("Config ini load/save interface", "[Config]") {
    WHEN("new_from_ini is called") {
		Slic3r::DynamicPrintConfig config;
		std::string path = std::string(TEST_DATA_DIR) + "/test_config/new_from_ini.ini";
		config.load_from_ini(path, ForwardCompatibilitySubstitutionRule::Disable);
        THEN("Config object contains ini file options.") {
			REQUIRE(config.option_throw<ConfigOptionString>("sla_archive_format", false)->value == "pwmx");
        }
    }
}

SCENARIO("DynamicPrintConfig serialization", "[Config]") {
    WHEN("DynamicPrintConfig is serialized and deserialized") {
        SLAFullPrintConfig full_print_config;
        DynamicPrintConfig cfg;
        cfg.apply(full_print_config, false);
        std::string serialized;
        try {
            std::ostringstream ss;
            cereal::BinaryOutputArchive oarchive(ss);
            oarchive(cfg);
            serialized = ss.str();
        } catch (const std::runtime_error & /* e */) {
            // e.what();
        }

        THEN("Config object contains ini file options.") {
            DynamicPrintConfig cfg2;
            try {
                std::stringstream ss(serialized);
                cereal::BinaryInputArchive iarchive(ss);
                iarchive(cfg2);
            } catch (const std::runtime_error & /* e */) {
                // e.what();
            }
            REQUIRE(cfg == cfg2);
        }
    }
}
