# HyprCaffeine

Caffeine mode plugin for [Hyprland](https://hyprland.org) — inhibits idle notifications, screen saver, and system sleep with a single toggle.

## Features

- **Idle inhibition** — blocks Wayland `ext-idle-notify` protocol (prevents screen dimming/lock via hypridle)
- **Sleep inhibition** — blocks system suspend/hibernate via systemd-logind D-Bus `Inhibit("sleep",...,"block")`
- **Auto-off timer** — automatically disables caffeine after N seconds
- **hyprctl query** — `hyprctl caffeine` returns JSON state
- **Hyprland notifications** — visual feedback on toggle
- **Waybar integration** — persistent monochrome icon, click to toggle, right-click for config popup
- **Walker popup menu** — floating config dialog (no terminal needed)

## Requirements

- Hyprland 0.54+
- libsystemd (for sd-bus / logind inhibit)
- walker (for `caffeine-config` popup menu)

## Build

```bash
cd ~/.hyprplugins/hyprcaffeine
make clean && make
```

Produces `hyprcaffeine.so`.

## Install

```bash
# Copy to plugin directory
cp hyprcaffeine.so ~/.hyprplugins/hyprcaffeine/hyprcaffeine2.so

# Load at runtime
hyprctl plugin load ~/.hyprplugins/hyprcaffeine/hyprcaffeine2.so

# Or add to autostart
echo 'exec-once = hyprctl plugin load ~/.hyprplugins/hyprcaffeine/hyprcaffeine2.so' >> ~/.config/hypr/autostart.conf
```

## Configuration

Add to your `hyprland.conf` or sourced config:

```ini
plugin:hyprcaffeine:enabled = 0             # Enable on startup (default: 0)
plugin:hyprcaffeine:inhibit_sleep = 1       # Inhibit system suspend (default: 1)
plugin:hyprcaffeine:inhibit_screensaver = 1 # Inhibit screen saver (default: 1)
plugin:hyprcaffeine:auto_off_timeout = 0    # Auto-disable after N seconds (default: 0 = manual)
```

## Usage

### Dispatcher

```bash
hyprctl dispatch caffeine toggle   # Toggle on/off
hyprctl dispatch caffeine on       # Enable
hyprctl dispatch caffeine off      # Disable
```

### Query state

```bash
hyprctl caffeine
# { "enabled": true, "sleep_inhibited": true, "cfg_inhibit_sleep": true,
#   "cfg_inhibit_screensaver": true, "cfg_auto_off_timeout": 0 }
```

### Keybinds

```ini
bindd = SUPER, F8, Caffeine, exec, hyprctl dispatch caffeine toggle
bindd = SUPER SHIFT, F8, Caffeine Config, exec, caffeine-config
```

### Config popup menu

```bash
caffeine-config
```

Opens a floating Walker --dmenu popup with:
- Toggle Caffeine on/off
- Toggle sleep inhibition
- Toggle screen saver inhibition
- Set auto-off timeout (seconds)
- Reload plugin

## Waybar Integration

### Module config (`config.jsonc`)

```jsonc
"custom/caffeine": {
    "format": "{}",
    "exec": "$HOME/.local/bin/waybar-caffeine.sh",
    "on-click": "hyprctl dispatch caffeine toggle; pkill -RTMIN+11 waybar",
    "on-click-right": "caffeine-config",
    "interval": 2,
    "return-type": "json",
    "signal": 11
}
```

Add `"custom/caffeine"` to your modules list.

### Waybar script (`~/.local/bin/waybar-caffeine.sh`)

```bash
#!/bin/bash
state=$(hyprctl caffeine 2>/dev/null)
enabled="false"
if [[ $state == {* ]]; then
    enabled=$(echo "$state" | grep -o '"enabled": [a-z]*' | head -1 | grep -o 'true')
fi

if [[ $enabled == "true" ]]; then
    echo '{"text": "󰛦", "tooltip": "Caffeine ON — idle/sleep/screensaver inhibited", "class": "active"}'
else
    echo '{"text": "󰛦", "tooltip": "Caffeine OFF — click to enable", "class": "inactive"}'
fi
```

### CSS (monochrome — always visible)

```css
#custom-caffeine {
    margin-right: 17px;
    color: @foreground;
}
#custom-caffeine.active {
    color: @foreground;
}
#custom-caffeine.inactive {
    opacity: 0.35;
}
```

The 󰛦 icon is always visible in the bar — left click toggles, right click opens config popup.

### Window rules (for Walker popup)

```ini
windowrule = float on, match:class walker
windowrule = pin on, match:class walker
```

## Architecture

| Component | Mechanism | Purpose |
|---|---|---|
| Idle inhibit | `PROTO::idle->setInhibit()` | Blocks Wayland idle notifications |
| Sleep inhibit | `sd_bus_call_method()` → logind `Inhibit("sleep",...,"block")` | Holds inhibit FD open = sleep blocked |
| Auto-off timer | `alarm()` + `SIGALRM` | Disables after N seconds |
| hyprctl query | `registerHyprCtlCommand()` → `SP<SHyprCtlCommand>` | Returns JSON state |
| Dispatcher | `addDispatcherV2()` | Toggle on/off/toggle |
| Waybar signal | `pkill -RTMIN+11 waybar` | Forces module refresh |

### Lifecycle

1. **PLUGIN_INIT** — registers config values, dispatcher, hyprctl command; reads config; enables if `enabled=1`
2. **setCaffeineEnabled(true)** — inhibits idle, acquires logind inhibit FD, sets alarm
3. **setCaffeineEnabled(false)** — releases everything, notifies, signals waybar
4. **PLUGIN_EXIT** — cleans state, **unregisters hyprctl command** (critical to prevent SEGV on unload)

## Troubleshooting

| Problem | Cause | Fix |
|---|---|---|
| Hyprland crashes on plugin unload (SEGV) | hyprctl command not unregistered | v2.1+: `g_pCaffeineCmd` stored and unregistered in `PLUGIN_EXIT` |
| `sleep_inhibited: false` despite `inhibit_sleep=1` | logind unavailable or no permission | Check `systemctl status systemd-logind` |
| Waybar icon not updating | Signal not received | Run `pkill -RTMIN+11 waybar` manually; verify `signal: 11` in config |
| Config popup opens empty terminal | Old script used gum (needs TTY) | v2.1+: uses Walker --dmenu floating popup |
| Hyprland config errors on windowrules | Missing value after rule name | Correct format: `float on, match:class walker` (not `float, match:class walker`) |

## Version History

- **2.1** — Fixed SEGV on unload: store `SP<SHyprCtlCommand>` and call `unregisterHyprCtlCommand()` in `PLUGIN_EXIT`. Added Walker --dmenu config popup.
- **2.0** — Initial working version with idle/sleep inhibition, hyprctl query, auto-off timer.

## License

MIT
