#!/bin/bash
set -euo pipefail

cd "$(dirname "$0")"

if [ -z "${DISPLAY:-}" ] && [ -e "/tmp/.X11-unix/X0" ]; then
    export DISPLAY=:0
fi

if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -e "$XDG_RUNTIME_DIR/wayland-0" ]; then
    export WAYLAND_DISPLAY=wayland-0
fi

echo "正在启动 maimai DX Linux 原生版..."
echo "Display: DISPLAY=${DISPLAY:-<unset>}, WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-<unset>}"

export MONO_PATH="$PWD/Sinmai_Data/Managed:$PWD/MelonLoader/net35:$PWD/MelonLoader/Dependencies/SupportModules:$PWD/MelonLoader/Managed:$PWD/Mods"
export LD_LIBRARY_PATH="$PWD/Sinmai_Data/Mono/x86_64:$PWD/Sinmai_Data/MonoBleedingEdge/x86_64:$PWD/Sinmai_Data/Plugins/x86_64:${LD_LIBRARY_PATH:-}"

if [ -f "$PWD/Sinmai_Data/Plugins/x86_64/libv4l2_filter.so" ]; then
    export LD_PRELOAD="$PWD/Sinmai_Data/Plugins/x86_64/libv4l2_filter.so${LD_PRELOAD:+:$LD_PRELOAD}"
fi

exec ./Sinmai.x86_64 -force-glcore -screen-fullscreen 0 -screen-width 1080 -screen-height 1920 --melonloader.disablestartscreen 2>&1 | tee /tmp/sinmai_stdout.log
