#!/bin/bash

rm -rf build || true

meson build/ \
    -Dbackend-wayland=true \
    -Dbackend-x11=false \
    -Drenderer-gl=true \
    -Dshell-desktop=false \
    -Dxwayland=true \
    -Dbackend-drm=false \
    -Dbackend-headless=false \
    -Dbackend-pipewire=false \
    -Dbackend-rdp=false \
    -Dbackend-vnc=false \
    -Drenderer-vulkan=false \
    -Dremoting=false \
    -Dpipewire=false \
    -Dshell-ivi=false \
    -Dshell-kiosk=true \
    -Dshell-lua=false \
    -Dcolor-management-lcms=false \
    -Dimage-jpeg=false \
    -Dimage-webp=false \
    -Ddemo-clients=false \
    -Dtools=[] \
    -Dtests=false \
    -Ddoc=false \
    -Dperfetto=false \
    -Dbackend-default=wayland \
    -Ddefault_library=static \
    -Ddefault_both_libraries=static \
    -Dsystemd=false \
    -Dtest-junit-xml=false \
    --prefix=$(pwd)/install

ninja -C build/ install
