#include "ProjectArchive.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/miniz_extension.hpp"
#include <array>
#include <boost/algorithm/string.hpp>
#include <boost/uuid/detail/sha1.hpp>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>

namespace Slic3r::sla {
namespace {
constexpr uint64_t document_budget = uint64_t(1) << 30;
constexpr const char *manifest_name = "Metadata/SLA/layers.json";
constexpr const char *layer_prefix = "Metadata/SLA/layers/";
std::string layer_name(size_t index) {
  std::ostringstream stream;
  stream << layer_prefix << std::setw(8) << std::setfill('0') << index
         << ".png";
  return stream.str();
}
class Fingerprint {
  boost::uuids::detail::sha1 hash;

public:
  void integer(uint64_t value) {
    std::array<unsigned char, 8> bytes{};
    for (unsigned i = 0; i < 8; ++i)
      bytes[i] = static_cast<unsigned char>(value >> (8 * i));
    hash.process_bytes(bytes.data(), bytes.size());
  }
  void text(const std::string &value) {
    integer(value.size());
    hash.process_bytes(value.data(), value.size());
  }
  static int64_t coordinate(double value) {
    if (!std::isfinite(value) || std::abs(value) > 1e7)
      throw RuntimeError("Invalid project geometry coordinate");
    // 3MF saves decimal mesh coordinates. Quantization avoids differences
    // caused solely by decimal parsing and vertex indexing on reload.
    return int64_t(std::llround(value * 100000.));
  }
  void point(const Vec3d &point) {
    for (int axis = 0; axis < 3; ++axis)
      integer(uint64_t(coordinate(point[axis])));
  }
  std::string finish() {
    boost::uuids::detail::sha1::digest_type digest;
    hash.get_digest(digest);
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto word : digest)
      stream << std::setw(8) << word;
    return stream.str();
  }
};
void geometry_config(Fingerprint &hash, const ConfigBase &config) {
  SLAPrintObjectConfig normalized;
  normalized.apply(config, true);
  auto keys = normalized.keys();
  std::sort(keys.begin(), keys.end());
  for (const auto &key : keys) {
    hash.text(key);
    hash.text(normalized.opt_serialize(key));
  }
}
void validate_metadata(const ProjectRasterState &state, size_t count) {
  if (!count || count > 100000 || state.positions_z.size() != count)
    throw RuntimeError("Invalid saved project layer count");
  if (state.source_fingerprint.size() != 40 ||
      !std::all_of(state.source_fingerprint.begin(),
                   state.source_fingerprint.end(), [](unsigned char c) {
                     return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                   }))
    throw RuntimeError("Invalid project raster fingerprint");
  double previous = 0.;
  for (const auto z : state.positions_z) {
    if (!std::isfinite(z) || z <= previous)
      throw RuntimeError("Invalid saved project layer heights");
    previous = z;
  }
}
void validate_state(const ProjectRasterState &state) {
  validate_metadata(state, state.document.layer_count());
}
void validate_png(const png::ReadBuf &bytes, size_t columns, size_t rows) {
  if (bytes.sz < 33 || !png::is_png(bytes))
    throw RuntimeError("Invalid saved project PNG");
  const auto *data = static_cast<const uint8_t *>(bytes.buf);
  auto u32 = [&](size_t offset) {
    return (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) |
           (uint32_t(data[offset + 2]) << 8) | data[offset + 3];
  };
  if (u32(16) != columns || u32(20) != rows || data[24] != 8 || data[25] != 0)
    throw RuntimeError(
        "Project layer PNG does not match its grayscale screen geometry");
  png::ImageGreyscale image;
  if (!png::decode_png(bytes, image) || image.cols != columns ||
      image.rows != rows)
    throw RuntimeError("Cannot decode saved project layer PNG");
}
} // namespace

std::string raster_source_fingerprint(const Model &model,
                                      const ConfigBase &configuration) {
  Fingerprint hash;
  hash.text("SLA-project-raster-source-v1");
  geometry_config(hash, configuration);
  SLAFullPrintConfig full;
  full.apply(configuration, true);
  for (const char *key : {"initial_layer_height",   "material_correction",
                          "material_correction_x",  "material_correction_y",
                          "material_correction_z",  "material_print_speed",
                          "relative_correction",    "relative_correction_x",
                          "relative_correction_y",  "relative_correction_z",
                          "absolute_correction",    "elefant_foot_compensation",
                          "elefant_foot_min_width", "zcorrection_layers",
                          "display_width",          "display_height",
                          "display_pixels_x",       "display_pixels_y",
                          "display_mirror_x",       "display_mirror_y",
                          "display_orientation",    "gamma_correction",
                          "sla_output_precision"}) {
    if (full.has(key)) {
      hash.text(key);
      hash.text(full.opt_serialize(key));
    }
  }
  // Material overrides change support geometry and XY compensation even
  // when the process preset itself stays unchanged.
  auto full_keys = full.keys();
  std::sort(full_keys.begin(), full_keys.end());
  for (const auto &key : full_keys)
    if (key.rfind("material_ow_", 0) == 0) {
      hash.text(key);
      hash.text(full.opt_serialize(key));
    }
  hash.integer(model.objects.size());
  for (const auto *object : model.objects) {
    hash.integer(object->printable);
    geometry_config(hash, object->config.get());
    hash.integer(object->instances.size());
    for (const auto *instance : object->instances) {
      hash.integer(instance->printable);
      const auto transform = instance->get_matrix();
      hash.integer(object->sla_support_points.size());
      for (const auto &support : object->sla_support_points) {
        hash.point(transform * support.pos.cast<double>());
        hash.integer(
            uint64_t(Fingerprint::coordinate(support.head_front_radius)));
        hash.integer(support.is_island());
      }
      hash.integer(object->sla_drain_holes.size());
      for (const auto &hole : object->sla_drain_holes) {
        hash.point(transform * hole.pos.cast<double>());
        hash.point(transform.linear() * hole.normal.cast<double>());
        hash.integer(uint64_t(Fingerprint::coordinate(hole.radius)));
        hash.integer(uint64_t(Fingerprint::coordinate(hole.height)));
      }
      hash.integer(object->volumes.size());
      for (const auto *volume : object->volumes) {
        hash.integer(unsigned(volume->type()));
        geometry_config(hash, volume->config.get());
        const auto &mesh = volume->mesh().its;
        // Bound temporary sorting storage independently of PNG storage.
        if (mesh.indices.size() > 3500000)
          throw RuntimeError(
              "Project fingerprint exceeds the 256 MiB geometry budget");
        std::vector<std::array<int64_t, 9>> faces;
        faces.reserve(mesh.indices.size());
        const auto combined = transform * volume->get_matrix();
        for (const auto &face : mesh.indices) {
          std::array<std::array<int64_t, 3>, 3> points;
          for (int vertex = 0; vertex < 3; ++vertex) {
            const Vec3d point =
                combined *
                mesh.vertices.at(size_t(face[vertex])).cast<double>();
            for (int axis = 0; axis < 3; ++axis)
              points[vertex][axis] = Fingerprint::coordinate(point[axis]);
          }
          // Cyclic canonicalization preserves winding while ignoring
          // vertex/triangle indexing changes from 3MF serialization.
          const size_t first = size_t(
              std::min_element(points.begin(), points.end()) - points.begin());
          std::array<int64_t, 9> values;
          for (size_t vertex = 0; vertex < 3; ++vertex)
            for (size_t axis = 0; axis < 3; ++axis)
              values[vertex * 3 + axis] = points[(first + vertex) % 3][axis];
          faces.push_back(values);
        }
        std::sort(faces.begin(), faces.end());
        hash.integer(faces.size());
        for (const auto &face : faces)
          for (const auto value : face)
            hash.integer(uint64_t(value));
      }
    }
  }
  return hash.finish();
}

ProjectRasterState capture_project_layers(const Model &model,
                                          const ConfigBase &config,
                                          const SLAPrint &print) {
  if (!print.finished() || !print.layer_document())
    throw RuntimeError("A project raster cache requires a completed SLA slice");
  ProjectRasterState state{
      *print.layer_document(), {}, raster_source_fingerprint(model, config)};
  for (const auto &layer : print.print_layers())
    state.positions_z.push_back(double(layer.level()) * SCALING_FACTOR);
  validate_state(state);
  return state;
}

void write_project_layers(mz_zip_archive &archive,
                          const ProjectRasterState &state) {
  validate_state(state);
  const auto image = state.document.image(0);
  if (!image.cols || !image.rows || image.cols > 32768 || image.rows > 32768 ||
      image.cols * image.rows > 128 * 1024 * 1024)
    throw RuntimeError("Invalid saved project screen geometry");
  nlohmann::json manifest{{"version", 1},
                          {"source_fingerprint", state.source_fingerprint},
                          {"columns", image.cols},
                          {"rows", image.rows},
                          {"positions_z", state.positions_z},
                          {"layers", state.document.layer_count()}};
  const auto metadata = manifest.dump();
  if (!mz_zip_writer_add_mem(&archive, manifest_name, metadata.data(),
                             metadata.size(), MZ_BEST_SPEED))
    throw FileIOError("Cannot write project layer manifest");
  uint64_t total = 0;
  for (size_t i = 0; i < state.document.layer_count(); ++i) {
    const auto bytes = state.document.encoded_layer(i);
    if (bytes.sz > document_budget - total)
      throw RuntimeError("Saved project layers exceed the 1 GiB budget");
    total += bytes.sz;
    validate_png(bytes, image.cols, image.rows);
    if (!mz_zip_writer_add_mem(&archive, layer_name(i).c_str(), bytes.buf,
                               bytes.sz, MZ_BEST_SPEED))
      throw FileIOError("Cannot write project layer PNG");
  }
}

std::optional<ProjectRasterState> read_project_layers(const std::string &path) {
  struct Reader : MZ_Archive {
    explicit Reader(const std::string &path) {
      if (!open_zip_reader(&arch, path))
        throw FileIOError(get_errorstr());
    }
    ~Reader() { close_zip_reader(&arch); }
  } zip(path);
  std::map<std::string, mz_uint> entries;
  for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip.arch); ++i) {
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip.arch, i, &stat))
      throw FileIOError(zip.get_errorstr());
    const auto name =
        boost::algorithm::to_lower_copy(std::string(stat.m_filename));
    if (name.rfind("metadata/sla/", 0) == 0 && !entries.emplace(name, i).second)
      throw RuntimeError("Duplicate SLA project entry");
  }
  const auto found =
      entries.find(boost::algorithm::to_lower_copy(std::string(manifest_name)));
  if (found == entries.end())
    return std::nullopt;
  auto extract = [&](mz_uint index, uint64_t limit) {
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip.arch, index, &stat) ||
        stat.m_uncomp_size > limit)
      throw RuntimeError("SLA project entry exceeds its memory budget");
    std::vector<uint8_t> bytes(size_t(stat.m_uncomp_size));
    if (!mz_zip_reader_extract_to_mem(&zip.arch, index, bytes.data(),
                                      bytes.size(), 0))
      throw FileIOError(zip.get_errorstr());
    return bytes;
  };
  const auto metadata =
      nlohmann::json::parse(extract(found->second, 32 * 1024 * 1024));
  if (!metadata.at("version").is_number_integer() ||
      metadata.at("version") != 1)
    throw RuntimeError("Unsupported SLA project layer version");
  auto integer = [&](const char *key, int64_t maximum) {
    const auto &value = metadata.at(key);
    if (!value.is_number_integer() || value.get<int64_t>() <= 0 ||
        value.get<int64_t>() > maximum)
      throw RuntimeError("Invalid SLA project layer geometry/count");
    return size_t(value.get<int64_t>());
  };
  const size_t columns = integer("columns", 32768),
               rows = integer("rows", 32768), count = integer("layers", 100000);
  if (columns * rows > 128 * 1024 * 1024)
    throw RuntimeError("Saved project screen exceeds the pixel budget");
  ProjectRasterState state;
  state.source_fingerprint =
      metadata.at("source_fingerprint").get<std::string>();
  state.positions_z = metadata.at("positions_z").get<std::vector<double>>();
  // Validate metadata before allocating and decoding all masks.
  validate_metadata(state, count);
  std::vector<EncodedRaster> layers;
  layers.reserve(count);
  uint64_t total = 0;
  for (size_t i = 0; i < count; ++i) {
    const auto entry =
        entries.find(boost::algorithm::to_lower_copy(layer_name(i)));
    if (entry == entries.end())
      throw RuntimeError("Missing saved project layer PNG");
    auto bytes = extract(entry->second, document_budget - total);
    total += bytes.size();
    validate_png(png::ReadBuf{bytes.data(), bytes.size()}, columns, rows);
    layers.emplace_back(std::move(bytes), "png");
  }
  // Reject orphan/extra masks as well as gaps in the manifest's sequence.
  for (const auto &entry : entries)
    if (entry.first.rfind("metadata/sla/layers/", 0) == 0) {
      if (entry.first.size() != std::string(layer_prefix).size() + 12)
        throw RuntimeError("Invalid saved project layer name");
      const auto number =
          entry.first.substr(std::string(layer_prefix).size(), 8);
      if (!std::all_of(number.begin(), number.end(),
                       [](unsigned char c) { return c >= '0' && c <= '9'; }) ||
          entry.first.substr(entry.first.size() - 4) != ".png" ||
          std::stoul(number) >= count)
        throw RuntimeError("Unexpected saved project layer entry");
    }
  state.document = LayerDocument::from_rasters(std::move(layers));
  state.document.mark_restored_edit();
  validate_state(state);
  return state;
}

bool restore_project_layers(SLAPrint &print, const Model &model,
                            const ConfigBase &config,
                            const ProjectRasterState &state) {
  validate_state(state);
  if (!print.finished() || !print.layer_document() ||
      state.document.layer_count() != print.print_layers().size() ||
      raster_source_fingerprint(model, config) != state.source_fingerprint)
    return false;
  for (size_t i = 0; i < state.positions_z.size(); ++i)
    if (std::abs(state.positions_z[i] -
                 double(print.print_layers()[i].level()) * SCALING_FACTOR) >
        SCALING_FACTOR)
      return false;
  print.replace_layer_document(state.document);
  return true;
}
} // namespace Slic3r::sla
