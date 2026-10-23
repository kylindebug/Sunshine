# 第三方组件

- Sunshine：GPL-3.0，源代码和 COPYING 随 Release 提供。https://github.com/kylindebug/Sunshine
- Sunshine build-deps / FFmpeg：https://github.com/LizardByte/build-deps/tree/bd28fbdd5511da8fc2ef303c381627a390045a78 。FFmpeg 与静态编解码库使用该项目发布的 ARM64 构建，构建脚本包含对应源码版本和下载地址。
- Boost 1.92.0：Boost Software License。https://www.boost.org/releases/1.92.0/
- 独立运行库来自 Ubuntu 24.04 官方签名软件包；麒麟 PipeWire 0.3.19 库来自目标系统官方软件源。各包版权与许可保留在 `runtime/usr/share/doc`。
- 系统端 GPU 驱动不随包分发，不安装或替换 GPU 驱动。

完整 Sunshine 源码归档包含子模块源码；发行版运行库的来源和版本记录在 `runtime-packages.tsv`。对应 Ubuntu 源包可通过 Ubuntu 源仓库按包名与版本获取。
