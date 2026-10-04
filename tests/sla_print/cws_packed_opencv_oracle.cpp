// Development oracle only: follow UVtools' imdecode(BGR).Reshape(1) exactly.
// PNG fixtures come from a native RGB.CWS export and its canonical UVJ masks.
#include <opencv2/imgcodecs.hpp>
#include <opencv2/core.hpp>
#include <filesystem>
#include <iostream>
int main(int argc,char **argv) {
    if(argc!=3){std::cerr<<"Expected packed and canonical PNG directories\n";return 2;}
    size_t checked=0,pixels=0;
    for(const auto &entry:std::filesystem::directory_iterator(argv[2])) {
        if(entry.path().extension()!=".png")continue;
        const auto expected=cv::imread(entry.path().string(),cv::IMREAD_GRAYSCALE);
        const auto packed=cv::imread((std::filesystem::path(argv[1])/entry.path().filename()).string(),cv::IMREAD_COLOR);
        if(expected.empty() || packed.empty() || packed.channels()!=3)return 1;
        const auto decoded=packed.reshape(1);
        if(decoded.size()!=expected.size() || cv::countNonZero(decoded!=expected)) {
            std::cerr<<"Packed pixels disagree with canonical mask: "<<entry.path()<<'\n';return 1;
        }
        ++checked;pixels+=expected.total();
    }
    if(!checked)return 1;
    std::cout<<"All "<<checked<<" RGB.CWS masks ("<<pixels<<" pixels) match canonical UVJ via OpenCV BGR/reshape\n";
}
