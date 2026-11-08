// SPDX-License-Identifier: AGPL-3.0-or-later
// Shared pixel-centre contour tree for vector layer formats and resin statistics.
// Areas use pixel-centre contours (shoelace), not occupied-pixel counts.
#ifndef SLIC3R_SLA_PIXEL_CONTOUR_TREE_HPP
#define SLIC3R_SLA_PIXEL_CONTOUR_TREE_HPP
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <utility>
#include "libslic3r/Exception.hpp"
namespace Slic3r::sla {
// Indices refer to this result vector. An outer root has parent=-1, root=self.
struct PixelContour {
    std::vector<std::pair<int,int>> points;
    int32_t parent = -1, root = -1;
    bool hole = false;
    double area = 0, perimeter = 0;
};
struct PixelContourStats {
    double total_area = 0, smallest_area = 0, largest_area = 0;
    uint32_t external_count = 0;
    uint64_t occupied_pixels = 0;
    // Bounding rectangle uses right/bottom exclusive, as UVtools/OpenCV do.
    uint32_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
};
inline PixelContourStats trace_pixel_contours(const uint8_t *pixels, size_t width, size_t height,
                                               std::vector<PixelContour> *contours = nullptr)
{
    if (!pixels || !width || !height || width > 32768 || height > 32768 ||
        uint64_t(width) * height > 128ULL * 1024 * 1024)
        throw ExportError("Invalid pixel contour raster");
    PixelContourStats stats;
    if(contours)contours->clear();
    stats.min_x=uint32_t(width);stats.min_y=uint32_t(height);
    const size_t source_width=width;
    for(size_t y=0;y<height;++y) for(size_t x=0;x<width;++x) {
        if(!pixels[y*width+x]) continue;
        ++stats.occupied_pixels;
        stats.min_x=std::min(stats.min_x,uint32_t(x));stats.min_y=std::min(stats.min_y,uint32_t(y));
        stats.max_x=std::max(stats.max_x,uint32_t(x+1));stats.max_y=std::max(stats.max_y,uint32_t(y+1));
    }
    if(!stats.occupied_pixels) {stats.min_x=stats.min_y=0;return stats;}
    // Like UVtools LayerMatBoundingRectangle, only label the occupied ROI.
    // Tiny objects on a high-resolution LCD need neither a full-size label grid
    // nor a second source copy; black layers allocate no contour workspace.
    width=stats.max_x-stats.min_x;height=stats.max_y-stats.min_y;
    const int w = int(width), h = int(height);
    const size_t count = width * height;
    auto pixel = [&](size_t i) {
        return pixels[(stats.min_y+i/width)*source_width+stats.min_x+i%width];
    };
    const int dx[]{-1,-1,0,1,1,1,0,-1}, dy[]{0,-1,-1,-1,0,1,1,1};
    auto foreground = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < w && y < h && pixel(size_t(y)*width + x) != 0;
    };
    // Moore boundary walk. Starting beside the exterior yields an outer border;
    // starting beside a hole yields that hole's foreground border. Keeping the
    // first directed edge handles thin branches and diagonal self-touching.
    auto area = [&](int sx, int sy, int bx, int by, std::vector<std::pair<int,int>> *points) {
        int x=sx, y=sy, first_x=-1, first_y=-1;
        int64_t twice_area=0;
        for (size_t steps=0;;++steps) {
            if (steps > 8*count) throw ExportError("pixel contour did not close");
            int direction=0;
            while (direction < 8 && (x+dx[direction]!=bx || y+dy[direction]!=by)) ++direction;
            if (direction == 8) throw ExportError("Invalid pixel contour neighbourhood");
            int next=-1;
            for (int i=1;i<=8;++i) {
                const int candidate=(direction+i)%8;
                if (foreground(x+dx[candidate], y+dy[candidate])) {next=candidate;break;}
            }
            if (next < 0) {
                if(points)points->emplace_back(x+int(stats.min_x),y+int(stats.min_y));
                break; // An isolated pixel has zero contour area.
            }
            const int nx=x+dx[next], ny=y+dy[next];
            if (first_x < 0) {first_x=nx;first_y=ny;}
            else if (x==sx && y==sy && nx==first_x && ny==first_y) break;
            if(points)points->emplace_back(x+int(stats.min_x),y+int(stats.min_y));
            twice_area += int64_t(x)*ny - int64_t(nx)*y;
            const int previous=(next+7)%8;
            bx=x+dx[previous];by=y+dy[previous];x=nx;y=ny;
        }
        return std::abs(double(twice_area))*.5;
    };
    // One signed label grid: foreground is 8-connected, background 4-connected.
    // Opposite connectivities are required for diagonal contacts to match the
    // OpenCV contour topology. Row flooding avoids one queue item per pixel.
    std::vector<int32_t> labels(count,0);
    std::vector<uint32_t> pending;
    struct Foreground { uint32_t seed=0, root=0; double area=0; };
    struct Background { uint32_t parent=0; bool exterior=false; double area=0; };
    std::vector<Foreground> objects(1);
    std::vector<Background> spaces(1);
    // Optional path storage stays separate from compact statistics components.
    // Stats-only NanoDLP calls pay neither point-vector nor parent-path storage.
    std::vector<std::vector<std::pair<int,int>>> object_points,space_points;
    std::vector<uint32_t> object_parent_spaces;
    if(contours){object_points.resize(1);space_points.resize(1);object_parent_spaces.resize(1);}
    auto flood = [&](size_t seed, int32_t label, bool solid) {
        bool exterior=false;
        pending.push_back(uint32_t(seed));
        // Queued cells are marked immediately to bound duplicates, while
        // processed spans may traverse cells queued by another neighbour.
        labels[seed]=label;
        const int32_t queued = label;
        std::vector<std::pair<uint32_t,uint32_t>> spans;
        // Temporarily negate this flood's label for processed spans. Pixel kind
        // disambiguates it from permanent opposite-kind labels; restore once
        // this component is complete. No second visited raster is needed.
        const int32_t done = -label;
        while (!pending.empty()) {
            const uint32_t point=pending.back();pending.pop_back();
            if (labels[point]==done) continue;
            const int row=int(point/width), column=int(point%width);
            auto available = [&](int c) {
                const auto offset=size_t(row)*width+c;
                return (pixel(offset)!=0)==solid && (labels[offset]==0 || labels[offset]==queued);
            };
            int left=column,right=column;
            while (left>0 && available(left-1)) --left;
            while (right+1<w && available(right+1)) ++right;
            if (row==0 || row==h-1 || left==0 || right==w-1) exterior=true;
            for(int c=left;c<=right;++c) {
                const uint32_t offset=uint32_t(size_t(row)*width+c);
                labels[offset]=done;
            }
            spans.emplace_back(uint32_t(size_t(row)*width+left),uint32_t(right-left+1));
            for (const int adjacent : {row-1,row+1}) {
                if (adjacent<0 || adjacent>=h) continue;
                const int margin=solid?1:0;
                bool in_run=false;
                for (int c=std::max(0,left-margin);c<=std::min(w-1,right+margin);++c) {
                    const size_t offset=size_t(adjacent)*width+c;
                    if ((pixel(offset)!=0)==solid && (labels[offset]==0 || labels[offset]==queued)) {
                        if (!in_run && labels[offset]==0) {pending.push_back(uint32_t(offset));labels[offset]=queued;}
                        in_run=true;
                    } else in_run=false;
                }
            }
        }
        for(const auto &span:spans)
            std::fill(labels.begin()+span.first,labels.begin()+span.first+span.second,label);
        return exterior;
    };
    for (size_t i=0;i<count;++i) {
        if (!pixel(i)) continue;
        const uint32_t x=uint32_t(i%width),y=uint32_t(i/width);
        if (labels[i]) continue;
        const uint32_t id=uint32_t(objects.size());
        Foreground object;object.seed=uint32_t(i);
        std::vector<std::pair<int,int>> points;
        object.area=area(int(x),int(y),int(x)-1,int(y),contours?&points:nullptr);
        objects.push_back(object);
        if(contours){object_points.push_back(std::move(points));object_parent_spaces.push_back(0);}
        flood(i,int32_t(id),true);
    }
    for (size_t i=0;i<count;++i) {
        if (pixel(i) || labels[i]) continue;
        const uint32_t id=uint32_t(spaces.size());
        const bool exterior=flood(i,-int32_t(id),false);
        Background space;space.exterior=exterior;
        std::vector<std::pair<int,int>> points;
        if (!exterior) {
            const int x=int(i%width),y=int(i/width);
            space.parent=uint32_t(labels[i-width]);
            space.area=area(x,y-1,x,y,contours?&points:nullptr);
        }
        spaces.push_back(space);
        if(contours)space_points.push_back(std::move(points));
    }
    std::vector<double> family_areas(objects.size(),0);
    for (uint32_t id=1;id<objects.size();++id) {
        const auto seed=objects[id].seed;
        uint32_t parent=0;
        if (seed>=width) {
            const auto label=labels[seed-width];
            if(label<0 && !spaces[size_t(-label)].exterior) {
                if(contours)object_parent_spaces[id]=uint32_t(-label);
                parent=spaces[size_t(-label)].parent;
            }
        }
        // Scan order guarantees a containing foreground precedes its island.
        if(parent>=id) throw ExportError("Invalid pixel contour hierarchy");
        objects[id].root=parent?objects[parent].root:id;
        family_areas[objects[id].root]+=objects[id].area;
    }
    for (size_t id=1;id<spaces.size();++id)
        if (!spaces[id].exterior) family_areas[objects[spaces[id].parent].root]-=spaces[id].area;
    bool first=true;
    for (size_t id=1;id<objects.size();++id) {
        if (objects[id].root!=id) continue;
        const double value=family_areas[id];
        stats.total_area+=value;
        stats.smallest_area=first?value:std::min(stats.smallest_area,value);
        stats.largest_area=first?value:std::max(stats.largest_area,value);
        ++stats.external_count;first=false;
    }
    if(contours) {
        std::vector<int32_t> space_index(spaces.size(),-1);
        int32_t next=int32_t(objects.size()-1);
        for(size_t id=1;id<spaces.size();++id)
            if(!spaces[id].exterior)space_index[id]=next++;
        contours->reserve(size_t(next));
        auto add=[&](std::vector<std::pair<int,int>> points,int32_t parent,int32_t root,bool hole,double contour_area) {
            // CHAIN_APPROX_SIMPLE keeps only turns in the cyclic boundary. Do
            // not deduplicate repeated nonadjacent points: thin branches and
            // diagonal self-touching contours need those directed edges.
            std::vector<std::pair<int,int>> simple;
            for(size_t i=0;i<points.size();++i) {
                const auto &previous=points[(i+points.size()-1)%points.size()];
                const auto &current=points[i];const auto &following=points[(i+1)%points.size()];
                if(points.size()<=2 || current.first-previous.first!=following.first-current.first ||
                   current.second-previous.second!=following.second-current.second)
                    simple.push_back(current);
            }
            double perimeter=0;
            if(simple.size()>1)for(size_t i=0;i<simple.size();++i) {
                const auto &a=simple[i],&b=simple[(i+1)%simple.size()];
                const float x=float(a.first-b.first),y=float(a.second-b.second);
                perimeter+=std::sqrt(x*x+y*y); // OpenCV accumulates float distances in double.
            }
            contours->push_back({std::move(simple),parent,root,hole,contour_area,perimeter});
        };
        for(size_t id=1;id<objects.size();++id) {
            auto &object=objects[id];
            add(std::move(object_points[id]),object_parent_spaces[id]?space_index[object_parent_spaces[id]]:-1,
                int32_t(object.root-1),false,object.area);
        }
        for(size_t id=1;id<spaces.size();++id) {
            auto &space=spaces[id];if(space.exterior)continue;
            add(std::move(space_points[id]),int32_t(space.parent-1),int32_t(objects[space.parent].root-1),true,space.area);
        }
    }
    return stats;
}
}
#endif
