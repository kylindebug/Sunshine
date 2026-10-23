#!/bin/sh
set -eu
runtime=/opt/sunshine-runtime
cd "$runtime/usr/local"
export SPA_PLUGIN_DIR=/usr/lib/aarch64-linux-gnu/spa-0.2
export PIPEWIRE_MODULE_DIR=/usr/lib/aarch64-linux-gnu/pipewire-0.3
export PIPEWIRE_CONFIG_DIR=/etc/pipewire
export __EGL_VENDOR_LIBRARY_DIRS="$runtime/usr/share/glvnd/egl_vendor.d"
export LIBGL_DRIVERS_PATH="$runtime/usr/lib/aarch64-linux-gnu/dri"
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
exec "$runtime/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1" --library-path "$runtime/opt/pipewire-legacy/usr/lib/aarch64-linux-gnu:$runtime/usr/lib/aarch64-linux-gnu:$runtime/usr/lib/aarch64-linux-gnu/pulseaudio:$runtime/usr/local/lib" "$runtime/usr/local/bin/sunshine" "$@"
