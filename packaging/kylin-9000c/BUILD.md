# 编译记录

基础源码：`426d12e17c646d5e4eb04a3378c214ddebc77da9`，加本分支补丁。

1. 在 ARM64 Ubuntu 24.04 独立目录或容器安装 GCC 14、CMake、Ninja、Node.js 24 及 Sunshine Linux 构建依赖。
2. 准备 Boost 1.92.0 和 [Sunshine build-deps v2026.1004.34232](https://github.com/LizardByte/build-deps/releases/tag/v2026.1004.34232) 的 ARM64 FFmpeg 库。
3. 从目标麒麟系统提取 `libpipewire-0.3-dev`、`libspa-0.2-dev` 0.3.19 和对应 `libpipewire-0.3.so.0` 到 `/opt/pipewire-legacy`。修改其中 pkg-config 文件的 prefix 为 `/opt/pipewire-legacy/usr`。
4. 编译：

```bash
export CC=gcc-14 CXX=g++-14
export PKG_CONFIG_PATH=/opt/pipewire-legacy/usr/lib/aarch64-linux-gnu/pkgconfig
export BUILD_VERSION=2026.1023.0 BRANCH=release/kylin-9000c
cmake -S . -B cmake-build-kylin -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DBUILD_DOCS=OFF -DBUILD_TESTS=OFF \
  -DSUNSHINE_ENABLE_TRAY=OFF -DSUNSHINE_ENABLE_CUDA=OFF \
  -DSUNSHINE_ENABLE_VULKAN=OFF -DSUNSHINE_ENABLE_KWIN=ON \
  -DFFMPEG_PREPARED_BINARIES=/build/ffmpeg \
  -DFETCHCONTENT_SOURCE_DIR_BOOST=/build/boost-1.92.0
cmake --build cmake-build-kylin --parallel 4
cmake --install cmake-build-kylin
```

发布包保留独立动态链接器、运行库、软件 EGL 驱动及资源文件。程序通过 `launch.sh` 使用独立库，采集使用目标系统的 UKUI D-Bus 接口。

验证：ARM64 原生编译通过；目标电脑实测约 29.5 FPS；Moonlight 图像、输入和界面元素经用户确认。未完成完整上游测试套件；尚未验证其他发行版、GPU、多显示器或 HDR。

Python 格式工具要求 Python 3.14，当前构建环境不可用，未运行该上游格式工具。

采集集成测试：先暂停正在运行的采集辅助服务，在已登录 UKUI Wayland 的桌面用户下运行 `python3 test_capture.py`。验证完整帧、非黑像素、连续更新及退出清理；测试后恢复采集服务。
