// Optional development oracle. OpenCV is not an application dependency.
#include "libslic3r/SLA/MaskFilter.hpp"
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <random>
int main() {
    std::mt19937 random(20261004);
    size_t checked=0;
    for(int width:{1,2,3,7,37}) for(int height:{1,2,3,11,29})
    for(unsigned radius:{0u,1u,2u,4u,19u,4096u}) for(unsigned sample=0;sample<8;++sample) {
        cv::Mat original(height,width,CV_8UC1),reference;
        for(int i=0;i<width*height;++i)original.data[i]=sample==0?255:sample==1?0:uint8_t(random());
        cv::blur(original,reference,cv::Size(2*radius+1,2*radius+1),cv::Point(-1,-1),cv::BORDER_REFLECT_101);
        const auto actual=Slic3r::sla::box_blur({original.data,original.data+width*height},width,height,radius);
        for(size_t i=0;i<actual.size();++i) if(actual[i]!=reference.data[i]) {
            std::cerr<<"Mismatch "<<width<<'x'<<height<<" radius "<<radius<<" sample "<<sample<<" pixel "<<i<<" expected "<<unsigned(reference.data[i])<<" actual "<<unsigned(actual[i])<<'\n';return 1;
        }
        ++checked;
    }
    std::cout<<"All "<<checked<<" OpenCV box-filter images match exactly\n";
}
