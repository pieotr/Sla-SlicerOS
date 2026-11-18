// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Layer recipe: a small declarative JSON format that chains the native layer operations (goals G6/R17, G12).
// It replaces UVtools' C# scripting with something that cannot run arbitrary code: a recipe is DATA, a list of
// steps, each one naming an operation from a fixed catalogue (see operation_catalogue()). Arbitrary C# scripts
// are NOT supported. The format is documented in doc/AUTOMATION.md.
//
// Guarantees
//  * parse_recipe() validates the whole file before anything runs: JSON syntax, unknown operations, unknown or
//    mistyped parameters, value ranges. Errors carry a location ("steps[2].angle_degrees").
//  * run_recipe() works on an in-memory document. Every step is atomic (the document APIs it uses either apply a
//    step completely, as one undo step, or not at all). Output files (save / convert / exports / reports) are
//    written to hidden staging names next to their targets and renamed into place only after the LAST step
//    succeeded. A failed or cancelled run therefore leaves every output and the input file untouched.
//  * dry_run executes nothing that changes data: it opens the input (read only), checks every step against the
//    document and checks that the outputs could be written.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "LayerDocument.hpp"
#include "LayerProgress.hpp"

namespace Slic3r::sla::recipe {

constexpr int kRecipeVersion = 1;

// Thrown by parse_recipe / load_recipe. what() is "<location>: <message>".
struct RecipeError : std::runtime_error
{
    RecipeError(std::string location, std::string message);
    std::string location; // "line 3, column 5" for JSON syntax errors, otherwise a path like "steps[2].threshold"
    std::string message;
};

struct StepCode; // compiled form of one step, private to LayerRecipe.cpp

struct Step
{
    size_t      index = 0;      // position in "steps"
    std::string op;             // operation name, e.g. "flip"
    std::string label;          // optional "name" of the step, used in reports
    std::string location;       // "steps[<index>]"
    bool        final_output = false; // the last save / convert step: --output replaces its path
    std::shared_ptr<const StepCode> code;
};

struct Recipe
{
    int               version = kRecipeVersion;
    std::string       description;
    std::vector<Step> steps;
    std::string       base_dir; // relative paths of the recipe resolve against this folder (set by load_recipe)
};

// One entry of the operation catalogue (for --help and the documentation).
struct OperationInfo
{
    std::string name;
    std::string summary;
};
std::vector<OperationInfo> operation_catalogue();

Recipe parse_recipe(const std::string &json_text);
// Reads and parses a file; relative paths inside the recipe resolve against the recipe's folder.
Recipe load_recipe(const std::string &path);

struct RunOptions
{
    std::string input;                      // replaces the path of the "open" step (CLI --input)
    std::string output;                     // replaces the path of the last save/convert step (CLI --output);
                                            // without such a step an implicit native save is appended
    bool        dry_run = false;
    std::string base_dir;                   // overrides Recipe::base_dir; empty = recipe folder, else current folder
    std::optional<LayerDocument> document;  // start from this document instead of an "open" step
    // (done, total) over the whole recipe in 1/100 of a step. Returning false cancels the run.
    LayerProgress progress;
};

struct StepResult
{
    enum class Status { Done, Planned /* dry run: checked, not executed */, Failed, Cancelled, NotRun };
    size_t      index = 0;
    std::string op, label;
    Status      status = Status::NotRun;
    std::string message; // what happened, or why the step failed
};

struct RunResult
{
    bool ok        = false;
    bool cancelled = false;
    bool dry_run   = false;
    std::string error;                        // first failure, "steps[2] (threshold): ..." ; empty when ok
    size_t failed_step = 0;                   // valid when !ok && !cancelled
    std::vector<StepResult>  steps;
    std::vector<std::string> outputs;         // published files / folders (empty for dry runs and failures)
    std::optional<LayerDocument> document;    // final state, only for a successful non-dry run
};

// Never throws for step failures (they are reported in RunResult); throws only for programming errors.
RunResult   run_recipe(const Recipe &, RunOptions options = {});
std::string run_result_to_json(const RunResult &);

} // namespace Slic3r::sla::recipe
