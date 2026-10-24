#!/bin/sh
set -eu
test "$(id -u)" = 0 || { echo '请使用 sudo ./install.sh "$USER"'; exit 1; }
user=${1:?请指定桌面用户名}
uid=$(id -u "$user")
test "$uid" -ne 0 || { echo '请指定普通桌面用户'; exit 1; }
test "$(uname -m)" = aarch64 || { echo '仅支持 ARM64'; exit 1; }
home=$(getent passwd "$user" | cut -d: -f6)
case "$home" in /home/*) ;; *) echo '不支持此主目录'; exit 1;; esac
test ! -e /opt/sunshine-runtime || { echo '已有运行目录，请勿重复安装'; exit 1; }
test ! -e /usr/local/assets && test ! -L /usr/local/assets || { echo '/usr/local/assets 已存在，停止安装'; exit 1; }
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test -s "$base/runtime/usr/local/bin/sunshine"
python3 -c 'import dbus'
test -d /usr/lib/aarch64-linux-gnu/spa-0.2
test -d /usr/lib/aarch64-linux-gnu/pipewire-0.3
cp -a "$base/runtime" /opt/sunshine-runtime
ln -s /opt/sunshine-runtime/usr/local/assets /usr/local/assets
install -m 755 "$base/launch.sh" /usr/local/bin/sunshine-kylin
install -d /usr/local/libexec
install -m 755 "$base/capture.py" /usr/local/libexec/sunshine-kylin-capture.py
install -m 755 "$base/session_guard.py" /usr/local/libexec/sunshine-kylin-session.py
install -d -o "$user" -g "$(id -gn "$user")" "$home/.config/systemd/user" "$home/.config/sunshine-kylin"
chmod 700 "$home/.config/sunshine-kylin"
ip=$(tailscale ip -4 2>/dev/null | head -n 1 || true)
test -n "$ip" || ip=127.0.0.1
cat > "$home/.config/sunshine-kylin/sunshine.conf" <<EOF
sunshine_name = Kylin-9000C
address_family = ipv4
bind_address = $ip
upnp = disabled
origin_web_ui_allowed = wan
capture = kwin
encoder = software
sw_preset = ultrafast
hevc_mode = 1
av1_mode = 1
min_log_level = 2
file_apps = $home/.config/sunshine-kylin/apps.json
file_state = $home/.config/sunshine-kylin/sunshine_state.json
credentials_file = $home/.config/sunshine-kylin/credentials.json
log_path = $home/.config/sunshine-kylin/sunshine.log
pkey = $home/.config/sunshine-kylin/credentials/cakey.pem
cert = $home/.config/sunshine-kylin/credentials/cacert.pem
EOF
printf '%s\n' 'SUNSHINE_CAPTURE_WIDTH=1920' 'SUNSHINE_CAPTURE_HEIGHT=1080' > "$home/.config/sunshine-kylin/capture.env"
printf '%s\n' '{"env":{},"apps":[{"name":"Desktop","image-path":"desktop.png"}]}' > "$home/.config/sunshine-kylin/apps.json"
cp "$base/"*.service "$home/.config/systemd/user/"
chown -R "$user:$(id -gn "$user")" "$home/.config/sunshine-kylin"
chown "$user:$(id -gn "$user")" "$home/.config/systemd/user/sunshine-kylin"*.service
printf 'KERNEL=="uinput", SUBSYSTEM=="misc", OWNER="%s", MODE="0600"\n' "$user" > /etc/udev/rules.d/85-sunshine-kylin-uinput.rules
modprobe uinput
udevadm control --reload-rules
chown "$user" /dev/uinput
chmod 600 /dev/uinput
sudo -u "$user" env XDG_RUNTIME_DIR="/run/user/$uid" DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$uid/bus" systemctl --user daemon-reload
sudo -u "$user" env XDG_RUNTIME_DIR="/run/user/$uid" DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$uid/bus" systemctl --user enable sunshine-kylin.service
echo '安装完成。以桌面用户执行以下命令（替换账号和密码）：'
echo 'sunshine-kylin "$HOME/.config/sunshine-kylin/sunshine.conf" --creds 管理用户名 独立管理密码'
echo 'systemctl --user start sunshine-kylin.service'
echo "管理地址：https://$ip:47990"
echo '如地址为 127.0.0.1，先连接 Tailscale，再修改 sunshine.conf 的 bind_address。'
