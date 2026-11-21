# OpenCV 4.12.0, core + imgproc only (SLA-Slicer uses cv::stackBlur, which needs >= 4.7, plus drawing/filters).
# Same version and tarball hash as tools/docker/Dockerfile.build.
#
# Built by default on Windows and macOS. On Linux the system OpenCV (>= 4.7) is used unless this recipe
# is requested explicitly: -DDEP_OpenCV=ON (build_linux.sh --bundled-opencv).
#
# Static libraries are installed so no OpenCV DLL/dylib has to be shipped. No IPP/ITT/OpenCL/TBB/OpenMP, no
# image codecs, no apps/tests/python/java/highgui/videoio. zlib comes from the ZLIB package (Windows) or the system.
if (MSVC OR APPLE)
    set(_dep_opencv_default ON)
else ()
    set(_dep_opencv_default OFF)
endif ()
option(DEP_OpenCV "Build OpenCV (core, imgproc) as a dependency instead of using the system one" ${_dep_opencv_default})

if (DEP_OpenCV)
    set(_opencv_msvc_args "")
    if (MSVC)
        # Static OpenCV must use the same (dynamic) CRT as the application.
        set(_opencv_msvc_args -DBUILD_WITH_STATIC_CRT:BOOL=OFF)
    endif ()

    add_cmake_project(OpenCV
        URL "https://github.com/opencv/opencv/archive/refs/tags/4.12.0.tar.gz"
        URL_HASH SHA256=44c106d5bb47efec04e531fd93008b3fcd1d27138985c5baf4eafac0e1ec9e9d
        CMAKE_ARGS
            -DBUILD_SHARED_LIBS:BOOL=OFF
            -DBUILD_LIST:STRING=core,imgproc
            -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON
            -DBUILD_TESTS:BOOL=OFF
            -DBUILD_PERF_TESTS:BOOL=OFF
            -DBUILD_EXAMPLES:BOOL=OFF
            -DBUILD_DOCS:BOOL=OFF
            -DBUILD_opencv_apps:BOOL=OFF
            -DBUILD_opencv_python3:BOOL=OFF
            -DBUILD_opencv_python2:BOOL=OFF
            -DBUILD_JAVA:BOOL=OFF
            -DBUILD_opencv_java:BOOL=OFF
            -DBUILD_PACKAGE:BOOL=OFF
            -DBUILD_ZLIB:BOOL=OFF
            -DBUILD_PROTOBUF:BOOL=OFF
            -DBUILD_ITT:BOOL=OFF
            -DWITH_IPP:BOOL=OFF
            -DWITH_ITT:BOOL=OFF
            -DWITH_ADE:BOOL=OFF
            -DWITH_TBB:BOOL=OFF
            -DWITH_OPENMP:BOOL=OFF
            -DWITH_OPENCL:BOOL=OFF
            -DWITH_FFMPEG:BOOL=OFF
            -DWITH_GTK:BOOL=OFF
            -DWITH_QT:BOOL=OFF
            -DWITH_EIGEN:BOOL=OFF
            -DWITH_LAPACK:BOOL=OFF
            -DWITH_PROTOBUF:BOOL=OFF
            -DWITH_JPEG:BOOL=OFF
            -DWITH_PNG:BOOL=OFF
            -DWITH_TIFF:BOOL=OFF
            -DWITH_WEBP:BOOL=OFF
            -DWITH_OPENJPEG:BOOL=OFF
            -DWITH_JASPER:BOOL=OFF
            -DWITH_OPENEXR:BOOL=OFF
            -DWITH_V4L:BOOL=OFF
            -DWITH_1394:BOOL=OFF
            -DWITH_WIN32UI:BOOL=OFF
            -DWITH_DSHOW:BOOL=OFF
            -DWITH_MSMF:BOOL=OFF
            -DOPENCV_GENERATE_PKGCONFIG:BOOL=OFF
            ${_opencv_msvc_args}
    )

    set(DEP_OpenCV_DEPENDS ZLIB)
endif ()
