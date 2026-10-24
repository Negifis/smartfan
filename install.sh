#!/bin/sh
# SmartFan installer.
#   sudo ./install.sh              install or update
#   sudo ./install.sh --uninstall  remove (your sensors.conf is kept)
# Works both from the source tree (after `make`) and from an unpacked release zip.
set -e

if [ "$(id -u)" != 0 ]; then
  echo "Запусти через sudo: sudo ./install.sh"
  exit 1
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
USER_NAME="${SUDO_USER:-$(stat -f %Su /dev/console)}"
USER_HOME="$(dscl . -read "/Users/$USER_NAME" NFSHomeDirectory | awk '{print $2}')"
USER_ID="$(id -u "$USER_NAME")"

BIN=/usr/local/libexec/smartfan
DUMP=/usr/local/bin/smartfan-dump
PLIST=/Library/LaunchDaemons/local.smartfan.plist
LOGS=/Library/Logs/SmartFan
SUPPORT="$USER_HOME/Library/Application Support/SmartFan"
APP="$USER_HOME/Applications/SmartFan.app"
AGENT="$USER_HOME/Library/LaunchAgents/local.smartfan.menu.plist"

if [ "$1" = "--uninstall" ]; then
  launchctl bootout "gui/$USER_ID/local.smartfan.menu" 2>/dev/null || true
  launchctl bootout system/local.smartfan 2>/dev/null || true  # the service hands fans back to macOS on exit
  rm -rf "$PLIST" "$BIN" "$DUMP" "$AGENT" "$APP"
  echo "SmartFan удалён, вентиляторами снова управляет macOS."
  echo "Настройки остались в: $SUPPORT"
  exit 0
fi

if [ "$(sysctl -n hw.optional.arm64 2>/dev/null)" = 1 ]; then
  echo "SmartFan работает только на Mac с процессором Intel. Apple Silicon (M1–M4) пока не поддерживается."
  exit 1
fi

if [ -x "$HERE/build/smartfan" ]; then SRC="$HERE/build"
elif [ -x "$HERE/smartfan" ]; then SRC="$HERE"
else
  echo "Не найдены собранные файлы. Сначала выполни: make"
  exit 1
fi
xattr -dr com.apple.quarantine "$SRC" 2>/dev/null || true

# Sensor thresholds: keep the user's file, otherwise take the one for this model.
MODEL="$(sysctl -n hw.model)"
if [ ! -f "$SUPPORT/sensors.conf" ]; then
  if [ ! -f "$HERE/config/$MODEL.conf" ]; then
    echo "Для модели $MODEL пока нет готового набора датчиков (config/$MODEL.conf)."
    echo "Посмотри датчики своего Mac: $SRC/smartfan-dump"
    echo "и создай config/$MODEL.conf по образцу config/MacBookPro15,1.conf."
    exit 1
  fi
  sudo -u "$USER_NAME" mkdir -p "$SUPPORT"
  sudo -u "$USER_NAME" cp "$HERE/config/$MODEL.conf" "$SUPPORT/sensors.conf"
  echo "Пороги датчиков для $MODEL: $SUPPORT/sensors.conf"
fi

# Service: the binary runs as root, so it lives in a root-owned folder.
launchctl bootout system/local.smartfan 2>/dev/null || true
mkdir -p /usr/local/libexec /usr/local/bin "$LOGS"
install -o root -g wheel -m 755 "$SRC/smartfan" "$BIN"
install -o root -g wheel -m 755 "$SRC/smartfan-dump" "$DUMP"
cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>local.smartfan</string>
  <key>ProgramArguments</key>
  <array>
    <string>$BIN</string>
    <string>--config</string>
    <string>$SUPPORT/sensors.conf</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>5</integer>
  <key>ProcessType</key><string>Interactive</string>
  <key>StandardOutPath</key><string>$LOGS/smartfan.log</string>
  <key>StandardErrorPath</key><string>$LOGS/smartfan.log</string>
</dict>
</plist>
EOF
chown root:wheel "$PLIST"
chmod 644 "$PLIST"
launchctl bootstrap system "$PLIST"

# Menu bar app, started at login for this user.
launchctl bootout "gui/$USER_ID/local.smartfan.menu" 2>/dev/null || true
sudo -u "$USER_NAME" mkdir -p "$USER_HOME/Applications" "$USER_HOME/Library/LaunchAgents"
rm -rf "$APP"
cp -R "$SRC/SmartFan.app" "$APP"
chown -R "$USER_NAME" "$APP"
cat > "$AGENT" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>local.smartfan.menu</string>
  <key>ProgramArguments</key><array><string>$APP/Contents/MacOS/SmartFan</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>
  <key>ProcessType</key><string>Interactive</string>
</dict>
</plist>
EOF
chown "$USER_NAME" "$AGENT"
launchctl bootstrap "gui/$USER_ID" "$AGENT"

sleep 3
echo "SmartFan установлен: служба запущена, значок добавлен в строку меню."
if pgrep -q -f "Macs Fan Control.app"; then
  echo "Внимание: запущен Macs Fan Control. Выбери в нём режим Auto или закрой его, иначе они будут спорить за вентиляторы."
fi
cat "$LOGS/status.txt" 2>/dev/null || true
