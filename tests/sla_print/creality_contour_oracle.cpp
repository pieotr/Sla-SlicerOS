// Optional development check: compile with the locally installed OpenCV.
// OpenCV is not a runtime or build dependency of SLA-Slicer.
#include "libslic3r/Format/CrealityContourArea.hpp"
#include <opencv2/imgproc.hpp>
#include <opencv2/geometry/2d.hpp>
#include <iostream>
#include <random>

int main()
{
    std::mt19937 random(20261004);
    size_t checked=0;
    for(unsigned scenario=0;scenario<75536;++scenario) {
        const int width=scenario<65536?4:37,height=scenario<65536?4:29;
        cv::Mat mask(height,width,CV_8UC1);
        for(int i=0;i<width*height;++i)
            mask.data[i]=scenario<65536?((scenario>>i)&1)*255:(random()%100<scenario%100?1+random()%255:0);
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(mask.clone(),contours,cv::RETR_EXTERNAL,cv::CHAIN_APPROX_SIMPLE);
        double expected=0;
        for(const auto &contour:contours) expected=std::max(expected,cv::contourArea(contour));
        const double actual=Slic3r::sla::creality_largest_contour_area(mask.data,width,height);
        if(actual!=expected) {std::cerr<<"Mismatch "<<scenario<<": "<<actual<<" / "<<expected<<'\n';return 1;}
        ++checked;
    }
    std::cout<<"Compared "<<checked<<" masks with OpenCV RETR_EXTERNAL: identical areas\n";
}
