// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>

namespace Slic3r::sla {
// UVtools/OpenCV medianBlur uses replicated edges, unlike box blur's
// Reflect101. Column histograms keep work independent of kernel area.
inline std::vector<uint8_t> median_blur(const std::vector<uint8_t> &input,
                                      std::size_t width, std::size_t height, unsigned radius,
                                      const std::function<void()> &check_cancel = {}) {
    if (!width || !height || width>32768 || height>32768 ||
        width*height>128ULL*1024*1024 || width*height!=input.size() || radius>4096)
        throw std::invalid_argument("Invalid median blur dimensions or radius");
    if (!radius) return input;
    struct Histogram {
        std::array<uint32_t,256> bins{};
        std::array<uint32_t,16> groups{};
        void add(uint8_t value, uint32_t count) { bins[value]+=count;groups[value/16]+=count; }
        void replace(uint8_t leaving,uint8_t entering) {
            --bins[leaving];--groups[leaving/16];++bins[entering];++groups[entering/16];
        }
        void accumulate(const Histogram &column,uint32_t count) {
            for(size_t i=0;i<bins.size();++i)bins[i]+=column.bins[i]*count;
            for(size_t i=0;i<groups.size();++i)groups[i]+=column.groups[i]*count;
        }
        void shift(const Histogram &leaving,const Histogram &entering) {
            for(size_t i=0;i<bins.size();++i)bins[i]=bins[i]-leaving.bins[i]+entering.bins[i];
            for(size_t i=0;i<groups.size();++i)groups[i]=groups[i]-leaving.groups[i]+entering.groups[i];
        }
        uint8_t median(uint32_t target) const {
            size_t group=0;
            while(groups[group]<target)target-=groups[group++];
            size_t value=group*16;
            while(bins[value]<target)target-=bins[value++];
            return uint8_t(value);
        }
    };
    const auto clamp=[](int value,size_t length) {return size_t(std::clamp(value,0,int(length)-1));};
    const uint32_t kernel=2*radius+1, target=(kernel*kernel)/2+1;
    std::vector<Histogram> columns(width);
    std::vector<uint8_t> output(input.size());
    for(size_t x=0;x<width;++x) {
        if(check_cancel && x%256==0)check_cancel();
        auto &column=columns[x];
        column.add(input[x],radius+1);
        for(size_t y=1;y<=std::min(size_t(radius),height-1);++y)column.add(input[y*width+x],1);
        if(radius>=height)column.add(input[(height-1)*width+x],uint32_t(radius-height+1));
    }
    for(size_t y=0;y<height;++y) {
        if(check_cancel)check_cancel();
        if(y) {
            const size_t leaving=clamp(int(y)-int(radius)-1,height)*width;
            const size_t entering=clamp(int(y)+int(radius),height)*width;
            for(size_t x=0;x<width;++x)columns[x].replace(input[leaving+x],input[entering+x]);
        }
        Histogram window;
        window.accumulate(columns[0],radius+1);
        for(size_t x=1;x<=std::min(size_t(radius),width-1);++x)window.accumulate(columns[x],1);
        if(radius>=width)window.accumulate(columns.back(),uint32_t(radius-width+1));
        for(size_t x=0;x<width;++x) {
            if(check_cancel && x%256==0)check_cancel();
            if(x)window.shift(columns[clamp(int(x)-int(radius)-1,width)],columns[clamp(int(x)+int(radius),width)]);
            output[y*width+x]=window.median(target);
        }
    }
    return output;
}
} // namespace Slic3r::sla
