# Sunshine 银河麒麟麒麟 9000C 版

适用：银河麒麟 V10 SP1、麒麟 9000C、Maleoon 910、UKUI Wayland。

已验证：1920×1080，约 30 FPS，鼠标键盘正常。使用麒麟原生共享内存采集；未修复系统 PipeWire 黑屏。默认 H.264 软件编码。

## 安装

1. 下载 Release 中的 `sunshine-kylin-9000c-arm64.tar.gz` 和 `SHA256SUMS`。
2. 在目标电脑打开终端：

```bash
sha256sum -c SHA256SUMS --ignore-missing
tar -xzf sunshine-kylin-9000c-arm64.tar.gz
cd sunshine-kylin-9000c
sudo ./install.sh "$USER"
```

3. 按安装提示设置管理账号密码，启动服务。
4. 保持桌面已登录，关闭自动休眠。当前显示分辨率应为 1920×1080。

已经部署本项目旧版的电脑不要重复安装。安装器遇到已有运行目录会停止，不覆盖原配置。

## 连接

1. 电脑和客户端安装 Tailscale，加入同一个网络；本机运行 `tailscale ip -4` 查看地址。
2. 在电脑防火墙中，仅对 `tailscale0` 放行 TCP `47984,47989,47990,48010`、UDP `47998:48000`。保留原有 SSH 规则。
3. 客户端安装 Moonlight，手动添加电脑的 Tailscale 地址。
4. 打开 `https://电脑地址:47990`，登录管理账号，在配对页面填写 Moonlight 显示的四位 PIN。
5. Moonlight 选择 **Desktop**。先用 **H.264、1080p、30 FPS、10 Mbps**；网络较慢改用 720p、5 Mbps。

自签名证书提示属于正常现象。不要把管理端口暴露到公网。更换客户端需要重新配对。

## 常用命令

```bash
systemctl --user status sunshine-kylin.service
systemctl --user restart sunshine-kylin.service
journalctl --user -u sunshine-kylin.service -n 50
```

其他分辨率：同时修改 `~/.config/sunshine-kylin/capture.env` 的宽高，再重启服务。请填写目标电脑实际显示分辨率。

## 构建与源码

安装包附带 ARM64 已编译程序和独立运行库，不替换系统 glibc、Mesa 或桌面。

构建环境与源码入口见 [BUILD.md](BUILD.md)。完整源码和第三方声明与安装包在同一 Release。
