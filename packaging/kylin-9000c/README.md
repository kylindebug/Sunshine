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
4. 保持桌面已登录。当前显示分辨率应为 1920×1080。

## 串流期间保活与断开锁屏

本分支增加 `sunshine-kylin-session.service`。每条真实串流在视频开始前获取本机 socket 租约，
UKUI 会话管理器在租约存在期间抑制 idle/suspend，并定期接收活动通知。最后一个客户端断开时，
保护程序立即请求锁屏并查询锁定状态；不必在 Moonlight 中选择“退出应用”。网络失联则在 Sunshine
判定超时后锁屏。Sunshine 崩溃会关闭租约并触发同样处理。保护程序退出会使串流停止。

默认安装启用 **保活与断连锁屏**，这不等于物理屏幕和输入隔离。

## 物理隐私模式（需单独验证显示器）

UKUI Wayland 的普通 DPMS 熄屏可能被虚拟键鼠唤醒，因此不作为隐私保证。可选模式通过显示器
DDC/CI 电源控制关闭实体屏幕，同时通过 KWin 逐项禁用物理输入，保留 sysfs 确认为虚拟设备的
`libvirtualhid` 输入。恢复时先确认锁屏，再恢复此前启用的输入和显示器原电源状态。

需要显示器支持 VCP D6=4、关闭后仍可读取电源状态，并为桌面用户授予对应 I2C 设备访问权限。
此能力不是所有 VGA/HDMI 显示器都支持。未经实际验证不要启用，也不要把默认模式当作隐私模式。
显示器断开、身份变化、电源状态无法确认、输入禁用失败都会终止保护租约。记录保存在 journal，
下次启动先恢复；锁屏无法确认时保留隔离状态。输入热插拔通过周期检查处理，不能保证新设备在
枚举前绝无输入；本功能也不能防御有管理员权限或能操作显示器硬件的人。

配置 JSON 是所有已连接显示器的数组，例如（值必须从本机读取，不能照抄）：

```json
[{"bus": 2, "connector": "card0-VGA-1", "edid_sha256": "显示器EDID文件的SHA256"}]
```

读取信息：`ddcutil detect --brief`、`ddcutil --bus 总线编号 --terse getvcp D6`、
`sha256sum /sys/class/drm/card*-*/edid`。在 session service 的 ExecStart 追加
`--privacy-config 配置文件绝对路径` 后重启服务。身份和关闭状态均通过验证后才允许串流启动。

回归测试：`python3 -m unittest discover -s packaging/kylin-9000c -p test_session_guard.py`；
C++ socket 测试使用 `KylinSessionGuardTest.*`。测试不改变真实屏幕或输入设备。

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
