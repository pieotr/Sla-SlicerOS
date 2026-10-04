// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace Slic3r { namespace sla {
// CXDLP v2/v3 stores the largest external contour area, measured between
// pixel centres (as OpenCV RETR_EXTERNAL does), rather than the pixel count.
// Trace the Moore neighbourhood of each 8-connected component. Row flooding
// avoids keeping a queue entry for every pixel of a large solid mask.
inline double creality_largest_contour_area(const uint8_t *pixels, size_t width, size_t height)
{
    if (!width || !height || width > 16384 || height > 8192)
        throw std::invalid_argument("Invalid Creality contour resolution");
    const int dx[]{-1,-1,0,1,1,1,0,-1},dy[]{0,-1,-1,-1,0,1,1,1};
    auto foreground=[&](int x,int y){return x>=0 && y>=0 && x<int(width) && y<int(height) && pixels[size_t(y)*width+x]!=0;};
    std::vector<uint8_t> visited(width*height,0);
    std::vector<uint32_t> pending;
    double largest=0;
    for(size_t seed=0;seed<width*height;++seed) {
        if(!pixels[seed] || visited[seed]) continue;
        const int start_x=int(seed%width),start_y=int(seed/width);
        int x=start_x,y=start_y,back_x=x-1,back_y=y,first_x=-1,first_y=-1;
        int64_t twice_area=0;
        for(size_t steps=0;;++steps) {
            if(steps>8*width*height) throw std::runtime_error("Creality contour did not close");
            int direction=0;
            for(;direction<8;++direction) if(x+dx[direction]==back_x && y+dy[direction]==back_y) break;
            if(direction==8) throw std::runtime_error("Invalid contour neighbourhood");
            int next=-1;
            for(int i=1;i<=8;++i) {
                const int candidate=(direction+i)%8;
                if(foreground(x+dx[candidate],y+dy[candidate])) {next=candidate;break;}
            }
            if(next<0) break; // A single pixel has zero contour area.
            const int nx=x+dx[next],ny=y+dy[next];
            if(first_x<0) {first_x=nx;first_y=ny;}
            else if(x==start_x && y==start_y && nx==first_x && ny==first_y) break;
            twice_area+=int64_t(x)*ny-int64_t(nx)*y;
            const int previous=(next+7)%8;
            back_x=x+dx[previous];back_y=y+dy[previous];x=nx;y=ny;
        }
        largest=std::max(largest,std::abs(double(twice_area))*.5);
        pending.push_back(uint32_t(seed));visited[seed]=2;
        while(!pending.empty()) {
            const auto point=pending.back();pending.pop_back();
            if(visited[point]==1) continue;
            const int row=int(point/width),column=int(point%width);
            int left=column,right=column;
            while(left>0 && foreground(left-1,row) && visited[size_t(row)*width+left-1]!=1) --left;
            while(right+1<int(width) && foreground(right+1,row) && visited[size_t(row)*width+right+1]!=1) ++right;
            for(int c=left;c<=right;++c) visited[size_t(row)*width+c]=1;
            for(const int adjacent:{row-1,row+1}) {
                if(adjacent<0 || adjacent>=int(height)) continue;
                bool in_run=false;
                for(int c=std::max(0,left-1);c<=std::min(int(width)-1,right+1);++c) {
                    const size_t offset=size_t(adjacent)*width+c;
                    if(pixels[offset] && visited[offset]!=1) {
                        if(!in_run && visited[offset]==0) {pending.push_back(uint32_t(offset));visited[offset]=2;}
                        in_run=true;
                    } else in_run=false;
                }
            }
        }
    }
    return largest;
}
}} // namespace Slic3r::sla
