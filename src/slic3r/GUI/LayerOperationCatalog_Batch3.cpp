#include "LayerEditorInternal.hpp"
#include <wx/utils.h>
#include <wx/msgdlg.h>

// UVtools operations of Fala 42 (wave B): Blur, Mask by image, Layer import, Stir resin, Redraw model. The engine is in
// SLA/LayerBlur, LayerOperations (mask_image_*, layer_import_*, stir_resin_*), LayerListStream and LayerRedrawModel; the
// recipe (LayerRecipe.cpp) drives the same functions, so the GUI and the CLI behave the same.
namespace Slic3r::GUI {
namespace layer_editor_detail {
namespace {
using namespace ops_detail;
using Change = sla::LayerDocument::LayerListChange;

// A printer file / layer project as a stack of masks (decoded one layer at a time through the document).
layerops::LayerImportSource load_document_source(const std::string &path)
{
    if (!boost::filesystem::is_regular_file(boost::filesystem::path(path)))
        invalid(_u8L("The file does not exist: ") + path);
    layerops::LayerImportSource source;
    source.name = path;
    try {
        sla::LayerDocument other = sla::LayerDocument::load_archive(path);
        for (size_t i = 0; i < other.layer_count(); ++i) {
            auto image = other.image(i);
            layerops::Mask m(size_t(image.cols), size_t(image.rows));
            m.px = std::move(image.buf);
            source.layers.push_back(std::move(m));
        }
    } catch (const std::exception &e) {
        invalid(_u8L("The file could not be read: ") + path + " (" + e.what() + ")");
    }
    return source;
}

bool is_picture(const std::string &path)
{
    const auto ext = boost::algorithm::to_lower_copy(boost::filesystem::path(path).extension().string());
    return ext == ".png" || ext == ".bmp" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif";
}
} // namespace

void append_batch3_defs(std::vector<OpDef> &defs, const OpContext &initial)
{
    using namespace ops_detail;
    using namespace batch2_detail;
    auto add = [&](OpGroup g, const wxString &label, const wxString &description, std::vector<OpField> fields,
                   std::function<BuiltOp(const OpValues &, const OpContext &)> build) -> OpDef & {
        defs.push_back({g, label, description, std::move(fields), true, true, true, false, std::move(build)});
        return defs.back();
    };

    // ---- Blur ----
    add(OpGroup::Pixels, _L("Blur"),
        _L("Blurs the layer images (UVtools Blur): stack, box, pyramid, median, Gaussian or a 2D kernel. Grey levels are created, so the "
        "printer needs anti-aliasing. Reflect-edge filters read the pixels around a region of interest."),
        {pick("algo", _L("Algorithm"), {_L("Stack blur"), _L("Box blur"), _L("Pyramid"), _L("Median blur"), _L("Gaussian blur"), _L("Filter 2D (kernel)")}, 0,
              _L("Pyramid reduces and enlarges the image again. UVtools 7.0 computes the pyramid but never stores it, so there it changes nothing; "
              "this slicer applies the described behaviour. Filter 2D uses the raw kernel values (they are not normalised).")),
         num("size", _L("Kernel size (px)"), 1, 4095, 3, _L("Width and height of the kernel. Must be odd for Stack, Median and Gaussian blur. Not used by Pyramid and Filter 2D.")),
         txt("kernel", _L("Kernel (Filter 2D: rows of values 0..255)"), "1 1 1\n1 1 1\n1 1 1",
             _L("Filter 2D only. Rows separated by new lines, values by spaces, each 0..255. The values are used as they are (not divided by their sum): "
             "the default kernel adds up nine pixels and saturates to 255.")),
         num("ax", _L("Anchor X (-1 = centre)"), -1, 4095, -1, _L("Box blur and Filter 2D: horizontal anchor of the kernel.")),
         num("ay", _L("Anchor Y (-1 = centre)"), -1, 4095, -1, _L("Box blur and Filter 2D: vertical anchor of the kernel."))},
        [](const OpValues &v, const OpContext &c) {
            require_range(c);
            static constexpr layerops::BlurAlgorithm kAlgos[] = {layerops::BlurAlgorithm::StackBlur, layerops::BlurAlgorithm::BoxBlur,
                layerops::BlurAlgorithm::Pyramid, layerops::BlurAlgorithm::MedianBlur, layerops::BlurAlgorithm::GaussianBlur, layerops::BlurAlgorithm::Filter2D};
            layerops::BlurParams p;
            p.selection = c.selection(); p.algorithm = kAlgos[std::clamp(v.i("algo"), 0, 5)];
            p.size = v.u("size"); p.kernel_text = v.s("kernel"); p.anchor_x = v.i("ax"); p.anchor_y = v.i("ay");
            require_valid(layerops::validate_blur(p));
            BuiltOp op;
            op.mask = [p](auto &layers, const auto &, const layerops::LayerSelection &sel, const sla::LayerProgress &pr) {
                auto q = p; q.selection.first_layer = sel.first_layer; q.selection.last_layer = sel.last_layer; q.selection.roi = sel.roi;
                layerops::blur_layers(layers, q, pr);
            };
            op.kernel = [p](const layerops::StreamLayout &l) { return layerops::blur_kernel(p, l); };
            return op;
        });

    // ---- Mask by image ----
    add(OpGroup::Pixels, _L("Mask by image"),
        _L("Multiplies every pixel by a grey picture (UVtools Mask): white keeps the pixel, black clears it, grey dims it. The picture must "
        "have the size of the layer (or of the region of interest) unless it is resized."),
        {file_field("image", _L("Mask picture (PNG)"), _L("A grey picture. Colours are converted to grey."), wxString("Pictures|*.png;*.bmp;*.jpg;*.jpeg|All files|*")),
         flag("invert", _L("Invert the mask"), false, _L("White clears the pixel and black keeps it.")),
         flag("fit", _L("Resize to the layer size"), false, _L("Bilinear resize. Off: a picture of another size is refused."))},
        [](const OpValues &v, const OpContext &c) {
            require_range(c);
            layerops::MaskImageParams p;
            p.selection = c.selection(); p.invert = v.b("invert");
            p.image = load_grey_image(v.s("image"));
            const size_t tw = c.roi.empty() ? c.width : size_t(c.roi.w), th = c.roi.empty() ? c.height : size_t(c.roi.h);
            if (v.b("fit") && (p.image.width != tw || p.image.height != th)) p.image = layerops::resize_mask_bilinear(p.image, tw, th);
            require_valid(layerops::validate_mask_image(p, c.width, c.height));
            BuiltOp op;
            op.mask = [p](auto &layers, const auto &, const layerops::LayerSelection &sel, const sla::LayerProgress &pr) {
                auto q = p; q.selection.first_layer = sel.first_layer; q.selection.last_layer = sel.last_layer; q.selection.roi = sel.roi;
                layerops::mask_image_layers(layers, q, pr);
            };
            op.kernel = [p](const layerops::StreamLayout &l) { return layerops::mask_image_kernel(p, l); };
            return op;
        });

    // ---- Layer import ----
    {
        const bool can_list = initial.list_edits_possible;
        auto &def = add(OpGroup::Layers, _L("Layer import"),
            can_list ?
                _L("Imports pictures (PNG) or printer files as layers (UVtools Layer import): replace, merge, subtract, stack next to the model and "
                "more. Layers can be inserted, appended and the rest of the document discarded.") :
                _L("Imports pictures (PNG) or printer files as layers (UVtools Layer import). This document cannot take a different layer list, so "
                "only the pixels of existing layers change: Insert, appending layers and discarding layers are not available."),
            {file_field("files", _L("Files to import"), _L("PNG pictures (one per layer, in the listed order) and printer or layer-project files. "
                                                          "Several files are joined with |."),
                        wxString("Pictures and layer files|*.png;*.sl1;*.sl1s;*.zip;*.ctb;*.cbddlp;*.photon;*.pws;*.pw0;*.pwx;*.cws;*.sla-project;*.jxs;*.lgs;*.fdg;*.goo;*.nanodlp|All files|*"), true),
             pick("type", _L("Import type"), {_L("Insert"), _L("Replace"), _L("Stack"), _L("Merge (add)"), _L("Merge (maximum)"),
                                            _L("Subtract"), _L("Absolute difference"), _L("Bitwise AND"), _L("Bitwise OR"), _L("Bitwise XOR")}, 1,
                  _L("Insert adds new layers; the other types combine the imported layers with the existing ones.")),
             num("start", _L("Start layer (1-based)"), 1, 10000000, 1, _L("The first layer of the document that receives the first imported layer.")),
             flag("extend", _L("Append layers"), can_list,
                  can_list ? _L("Replace, Stack and Merge add blank layers at the end when the import has more layers than the document has left (append layers when the import is longer).") :
                             _L("Not available: this document cannot take a different layer list.")),
             flag("discard", _L("Discard layers after"), false,
                  can_list ? _L("Discard layers after the last imported one: removes every layer after the last imported layer.") : _L("Not available: this document cannot take a different layer list.")),
             num("margin", _L("Stack margin (px)"), 0, 65535, 50, _L("Stack type: gap between the model and the imported picture."))},
            [can_list](const OpValues &v, const OpContext &c) {
                static constexpr layerops::LayerImportType kTypes[] = {layerops::LayerImportType::Insert, layerops::LayerImportType::Replace,
                    layerops::LayerImportType::Stack, layerops::LayerImportType::MergeSum, layerops::LayerImportType::MergeMax, layerops::LayerImportType::Subtract,
                    layerops::LayerImportType::AbsDiff, layerops::LayerImportType::BitwiseAnd, layerops::LayerImportType::BitwiseOr, layerops::LayerImportType::BitwiseXor};
                layerops::LayerImportParams p;
                p.type = kTypes[std::clamp(v.i("type"), 0, 9)];
                p.start_layer = size_t(std::max(1, v.i("start")) - 1);
                p.extend_beyond_layer_count = v.b("extend"); p.discard_unmodified_layers = v.b("discard");
                p.stack_margin = v.u("margin");
                const auto files = split_paths(v.s("files"));
                if (files.empty()) invalid(_u8L("Choose at least one file to import."));
                {
                    wxBusyCursor busy;
                    layerops::LayerImportSource pictures;
                    pictures.name = "pictures";
                    for (const auto &file : files) {
                        if (is_picture(file)) {
                            const auto ext = boost::algorithm::to_lower_copy(boost::filesystem::path(file).extension().string());
                            if (ext != ".png") invalid(_u8L("Only PNG pictures can be imported: ") + file);
                            auto m = load_grey_image(file);
                            if (!pictures.layers.empty() && (m.width != pictures.layers.front().width || m.height != pictures.layers.front().height))
                                invalid(_u8L("The pictures must all have the same size: ") + file);
                            pictures.layers.push_back(std::move(m));
                        }
                    }
                    if (!pictures.layers.empty()) p.sources.push_back(std::move(pictures));
                    for (const auto &file : files)
                        if (!is_picture(file)) p.sources.push_back(load_document_source(file));
                }
                require_valid(layerops::validate_layer_import(p, c.layer_count, c.width, c.height));
                const bool changes_list = layerops::layer_import_changes_list(p, c.layer_count);
                if (changes_list && !can_list)
                    invalid(_u8L("This import adds or removes layers, but this document cannot take a different layer list: ") + c.list_refusal +
                            " " + _u8L("Use Replace or a merge type without appending and without discarding."));
                BuiltOp op;
                op.report = std::make_shared<std::string>();
                const auto report = op.report;
                if (changes_list) {
                    op.list = [p, report](std::vector<layerops::Mask> &layers, std::vector<double> &z, const sla::LayerProgress &pr) {
                        Change change;
                        const auto result = layerops::layer_import_layers(layers, z, p, pr);
                        change.list = result.list;
                        *report = result.report;
                        return change;
                    };
                    // The whole-vector form is the only form: no streamed kernel (the list changes).
                    op.mask = {};
                } else {
                    op.mask = [p, report](auto &layers, const auto &, const layerops::LayerSelection &, const sla::LayerProgress &pr) {
                        std::vector<double> z(layers.size(), 0.0);
                        const size_t count = layers.size();
                        const auto result = layerops::layer_import_layers(layers, z, p, pr);
                        if (layers.size() != count) throw std::runtime_error(_u8L("The import would change the layer count."));
                        *report = result.report;
                    };
                }
                return op;
            });
        def.uses_range = false; def.uses_roi = false; def.previewable = false;
    }

    // ---- Stir resin ----
    {
        layerops::StirResinParams d;
        add(OpGroup::Parameters, _L("Stir resin"),
            _L("Adds layers at the start of the print that only lift the build plate (UVtools Stir resin), to stir the resin before it is "
            "exposed. The new layers sit at Z = 0.001 mm: sliced or generated documents only. Exporting such a document is checked "
            "against the target format before anything is written."),
            {pick("method", _L("Method"), {_L("Stir only"), _L("Stir and print")}, 1,
                  _L("Stir only creates a document made of the stir layers alone and drops the print. Stir and print puts the stir layers in front of the existing layers.")),
             num("stirs", _L("Number of stirs"), 1, 65535, int(d.stirs), _L("Every stir is one extra layer.")),
             flag("dummy", _L("Output a dummy pixel"), true, _L("Lights a single pixel so that the layer is not dropped by the printer firmware.")),
             dbl("exp", _L("Exposure (s, 0 = 0.01 s)"), 0, 100000, 0, 2, _L("0 uses 0.01 s (0 for G-code printers).")),
             dbl("lift", _L("Lift height (mm)"), 0, 10000, d.lift_height_mm, 2),
             dbl("ls", _L("Lift speed (mm/min, 0 = printer maximum)"), 0, 100000, 0, 2),
             dbl("rs", _L("Retract speed (mm/min, 0 = lift speed)"), 0, 100000, 0, 2),
             dbl("wb", _L("Wait before stir (s)"), 0, 100000, 0, 2), dbl("wa", _L("Wait after lift (s)"), 0, 100000, 0, 2)},
            [](const OpValues &v, const OpContext &c) {
                require_parameters(c);
                layerops::StirResinParams p;
                p.method = v.i("method") == 0 ? layerops::StirMethod::StirOnly : layerops::StirMethod::StirAndPrint;
                p.stirs = v.u("stirs"); p.output_dummy_pixel = v.b("dummy"); p.exposure_s = v.d("exp");
                p.lift_height_mm = v.d("lift"); p.lift_speed = v.d("ls"); p.retract_speed = v.d("rs");
                p.wait_time_before_stir_s = v.d("wb"); p.wait_time_after_lift_s = v.d("wa");
                if (p.stirs == 0) invalid(_u8L("The number of stirs must be greater than 0."));
                require_valid(layerops::validate_stir_resin(p, c.printer));
                const auto ctx = c.printer;
                layerops::LayerParams base = c.table.empty() ? layerops::LayerParams{} : c.table.front();
                for (const auto &row : c.table) if (!row.is_bottom) { base = row; break; } // the first normal layer
                BuiltOp op;
                op.param_changes_list = true;
                op.stream = sla::liststream::stir_resin(p, ctx, base); op.stream_params = true;
                return op;
            }).uses_range = false;
        defs.back().uses_roi = false; defs.back().previewable = false;
    }

    // ---- Redraw model ----
    {
        auto &def = add(OpGroup::Pixels, _L("Redraw model"),
            _L("Redraws the supports or the model with a chosen brightness (UVtools Redraw model). It needs the same object sliced a second time "
            "as a model-only file (without supports and raft); its layers are matched to the top layers of this document."),
            {file_field("body", _L("Model-only file"), _L("The same object sliced without supports and raft (any printer or layer-project file)."),
                        wxString("Layer files|*.sl1;*.sl1s;*.zip;*.ctb;*.cbddlp;*.photon;*.pws;*.pw0;*.pwx;*.cws;*.sla-project;*.jxs;*.lgs;*.fdg;*.goo;*.nanodlp|All files|*")),
             pick("type", _L("Redraw"), {_L("Supports"), _L("Model")}, 0),
             pick("op", _L("Operator"), {_L("Set"), _L("Add"), _L("Subtract"), _L("Multiply"), _L("Divide"), _L("Minimum"), _L("Maximum"), _L("Absolute difference")}, 5,
                  _L("How the brightness is combined with the layer. Multiply, Divide and Minimum clear everything outside the redrawn area (as UVtools does).")),
             num("bright", _L("Brightness"), 0, 255, 220),
             flag("contact", _L("Contact points only"), true, _L("Supports: redraw only the support tips that touch the model.")),
             flag("ignore", _L("Ignore contactless pixels"), true)},
            [](const OpValues &v, const OpContext &c) {
                layerops::RedrawModelParams p;
                p.roi = c.roi;
                p.type = v.i("type") == 1 ? layerops::RedrawType::Model : layerops::RedrawType::Supports;
                static constexpr layerops::RedrawOperator kOps[] = {layerops::RedrawOperator::Set, layerops::RedrawOperator::Add, layerops::RedrawOperator::Subtract,
                    layerops::RedrawOperator::Multiply, layerops::RedrawOperator::Divide, layerops::RedrawOperator::Minimum, layerops::RedrawOperator::Maximum,
                    layerops::RedrawOperator::AbsDiff};
                p.op = kOps[std::clamp(v.i("op"), 0, 7)];
                p.brightness = uint8_t(std::clamp(v.i("bright"), 0, 255));
                p.contact_points_only = v.b("contact"); p.ignore_contactless_pixels = v.b("ignore");
                require_valid(layerops::validate_redraw_model(p));
                if (v.s("body").empty()) invalid(_u8L("Choose the model-only file first."));
                std::shared_ptr<sla::LayerDocument> body;
                {
                    wxBusyCursor busy;
                    if (!boost::filesystem::is_regular_file(boost::filesystem::path(v.s("body")))) invalid(_u8L("The file does not exist: ") + v.s("body"));
                    try { body = std::make_shared<sla::LayerDocument>(sla::LayerDocument::load_archive(v.s("body"))); }
                    catch (const std::exception &e) { invalid(_u8L("The file could not be read: ") + v.s("body") + " (" + e.what() + ")"); }
                }
                const auto first = body->image(0);
                require_valid(layerops::validate_redraw_model_files(p, c.layer_count, c.width, c.height, body->layer_count(), size_t(first.cols), size_t(first.rows)));
                const size_t count = body->layer_count();
                const auto fetch = [body](size_t j) {
                    auto image = body->image(j);
                    auto m = std::make_shared<layerops::Mask>(size_t(image.cols), size_t(image.rows));
                    m->px = std::move(image.buf);
                    return std::shared_ptr<const layerops::Mask>(m);
                };
                BuiltOp op;
                op.mask = [p, body, count, fetch](auto &layers, const auto &, const layerops::LayerSelection &, const sla::LayerProgress &pr) {
                    std::vector<layerops::Mask> bodies;
                    for (size_t j = 0; j < count; ++j) bodies.push_back(*fetch(j));
                    layerops::redraw_model_layers(layers, bodies, p, pr);
                };
                op.kernel = [p, count, fetch](const layerops::StreamLayout &l) { return layerops::redraw_model_kernel(p, l, count, fetch); };
                return op;
            });
        def.uses_range = false; def.previewable = false;
    }
}

} // namespace layer_editor_detail
} // namespace Slic3r::GUI
