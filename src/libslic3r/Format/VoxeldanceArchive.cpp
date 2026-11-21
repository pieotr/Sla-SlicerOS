// Native SLA raster/PNG path for VDT. AGPLv3 or later.
#include "VoxeldanceArchive.hpp"
#include "libslic3r/SLA/PrinterRaster.hpp"
namespace Slic3r {
std::unique_ptr<sla::RasterBase> VoxeldanceTangoArchive::create_raster() const {
    return sla::create_printer_raster(m_cfg);
}
sla::RasterEncoder VoxeldanceTangoArchive::get_encoder() const { return sla::PNGRasterEncoder{}; }
}
