#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
AGENT_ID=com.local.gemini-telegram-trading-bot
LAUNCH_AGENTS="$HOME/Library/LaunchAgents"
LOG_DIR="$HOME/Library/Logs/GeminiTelegramTradingBot"
PLIST_PATH="$LAUNCH_AGENTS/$AGENT_ID.plist"

xml_escape() {
    printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'
}

PROJECT_XML=$(xml_escape "$PROJECT_ROOT")
LOG_XML=$(xml_escape "$LOG_DIR")
mkdir -p "$LAUNCH_AGENTS" "$LOG_DIR"
cat > "$PLIST_PATH" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$AGENT_ID</string>
  <key>ProgramArguments</key>
  <array><string>/bin/sh</string><string>$PROJECT_XML/scripts/run.sh</string></array>
  <key>WorkingDirectory</key><string>$PROJECT_XML</string>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>$LOG_XML/stdout.log</string>
  <key>StandardErrorPath</key><string>$LOG_XML/stderr.log</string>
</dict>
</plist>
EOF

chmod 600 "$PLIST_PATH"
printf 'LaunchAgent file created: %s\n' "$PLIST_PATH"
printf 'Load with: launchctl bootstrap gui/$(id -u) "%s"\n' "$PLIST_PATH"
printf 'Stop with: launchctl bootout gui/$(id -u)/%s\n' "$AGENT_ID"
