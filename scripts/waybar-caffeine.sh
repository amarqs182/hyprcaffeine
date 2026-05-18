#!/bin/bash
# Caffeine indicator for Waybar — permanent monochrome icon
# Parses JSON from hyprctl caffeine (v2.0+)
# Requires: hyprctl, jq (optional, falls back to grep)

state=$(hyprctl caffeine 2>/dev/null)

if command -v jq &>/dev/null; then
    enabled=$(echo "$state" | jq -r '.enabled // false' 2>/dev/null)
else
    enabled="false"
    if [[ $state == {* ]]; then
        enabled=$(echo "$state" | grep -oP '"enabled":\s*\K[a-z]+' | head -1)
    fi
fi

if [[ $enabled == "true" ]]; then
    echo '{"text": "󰛦", "tooltip": "Caffeine ON — idle/sleep/screensaver inhibited", "class": "active"}'
else
    echo '{"text": "󰛦", "tooltip": "Caffeine OFF — clique para ativar", "class": "inactive"}'
fi
