# OpenCV native calibration dependency

The app uses OpenCV core/imgproc directly for native ExposureFinder drawing:
contours, anti-aliasing and Hershey text. No UVtools executable, Emgu binding or
.NET runtime is part of this production dependency.

OpenCV 4.5+ is Apache-2.0. The portable (docker) build bundles OpenCV 4.12.0 (core+imgproc, built from the official tarball, SHA256 44c106d5bb47efec04e531fd93008b3fcd1d27138985c5baf4eafac0e1ec9e9d); the local development installation may be newer (e.g. 5.x).
Upstream: OpenCV contributors, https://opencv.org/ and https://github.com/opencv/opencv.
See OpenCV-Apache-2.0.txt. Distribution must retain the corresponding upstream
third-party notices for the actual supplied build, not just this summary.

Native dependency closure and notices are copied by tools/bundle_native_libraries.py.
The host compiler/ABI and CPU requirements of those binaries still apply;
copying them is not evidence of cross-platform or older-CPU compatibility.
