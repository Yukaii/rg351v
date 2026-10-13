# Linux/aarch64 container for building pikaball_revamped for the RG351V.
# Rocky Linux 9 (glibc 2.34, older than the device's 2.36) with GCC 13 from
# gcc-toolset-13 (the game's battery::embed NTTP idiom needs GCC 13+).
# No ninja package on Rocky: it comes from pip.
FROM rockylinux:9

RUN dnf install -y git pkg-config python3-pip \
  gcc-toolset-13 cmake alsa-lib-devel libdrm-devel mesa-libgbm-devel \
  mesa-libEGL-devel mesa-libGLES-devel libglvnd-devel libudev-devel \
  systemd-devel dbus-devel \
  && dnf clean all \
  && pip3 install --no-cache-dir ninja

ENV PATH=/opt/rh/gcc-toolset-13/root/usr/bin:${PATH}
ENV LD_LIBRARY_PATH=/opt/rh/gcc-toolset-13/root/usr/lib64
ENV CC=gcc CXX=g++

WORKDIR /build
