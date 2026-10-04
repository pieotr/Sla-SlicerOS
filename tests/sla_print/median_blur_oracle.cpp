// Optional development oracle. OpenCV is not an application dependency.
#include "libslic3r/SLA/MedianFilter.hpp"
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <random>
#include <chrono>
int main(int argc,char **) {
    std::mt19937 random(20261004);
    size_t checked=0;
    for(int width:{1,2,3,7,37}) for(int height:{1,2,3,11,29})
    // OpenCV's 16-bit histogram overflows beyond a 255-square kernel.
    // Larger supported radii are checked analytically in native tests.
    for(unsigned radius:{0u,1u,2u,4u,19u,127u}) for(unsigned sample=0;sample<8;++sample) {
        cv::Mat original(height,width,CV_8UC1),reference;
        for(int i=0;i<width*height;++i)original.data[i]=sample==0?255:sample==1?0:uint8_t(random());
        cv::medianBlur(original,reference,2*radius+1);
        const auto actual=Slic3r::sla::median_blur({original.data,original.data+width*height},width,height,radius);
        for(size_t i=0;i<actual.size();++i) if(actual[i]!=reference.data[i]) {
            std::cerr<<"Mismatch "<<width<<'x'<<height<<" radius "<<radius<<" sample "<<sample<<" pixel "<<i<<" expected "<<unsigned(reference.data[i])<<" actual "<<unsigned(actual[i])<<'\n';return 1;
        }
        ++checked;
    }
    std::cout<<"All "<<checked<<" OpenCV median-filter images match exactly\n";
    if(argc>1) {
        constexpr size_t width=1920,height=1080;
        std::vector<uint8_t> image(width*height);
        for(auto &pixel:image)pixel=uint8_t(random());
        for(unsigned radius:{1u,3u,127u,4096u}) {
            const auto start=std::chrono::steady_clock::now();
            const auto result=Slic3r::sla::median_blur(image,width,height,radius);
            const double milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            uint64_t sum=0;for(auto pixel:result)sum+=pixel;
            std::cout<<width<<'x'<<height<<" radius "<<radius<<": "<<milliseconds<<" ms, pixel sum "<<sum<<'\n';
        }
    }
}
