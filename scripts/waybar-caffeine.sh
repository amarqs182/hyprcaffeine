#!/bin/bash
# Caffeine indicator for Waybar — permanent monochrome icon
# Parses JSON from hyprctl caffeine (v2.0+)

state=$(hyprctl caffeine 2>/dev/null)

enabled="false"
if [[ $state == {* ]]; then
    # JSON output — parse enabled field
    enabled=$(echo "$state" | grep -o '"enabled": [a-z]*' | head -1 | grep -o 'true')
fi

if [[ $enabled == "true" ]]; then
    echo '{"text": "󰛦", "tooltip": "Caffeine ON — idle/sleep/screensaver inhibited", "class": "active"}'
else
    echo '{"text": "󰛦", "tooltip": "Caffeine OFF — clique para ativar", "class": "inactive"}'
fi
