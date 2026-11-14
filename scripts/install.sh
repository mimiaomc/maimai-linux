#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
UNITY_SUPPORT_DIR=""
PATCH_SHADERS=0
GAME_DIR=""

usage() {
    cat <<EOF
Usage: $0 [OPTIONS] <GAME_DIR>

Deploy built Linux native stubs, Mono config, run script, LinuxQrFix mod,
and default configuration files into an existing maimai DX (SDGB) directory.

Options:
  --build-dir <DIR>             Build directory created by make (default: ./build)
  --unity-linux-support <DIR>   Path to Unity 2018.4.7f1 Linux Playback Support
                                (either linux64_withgfx_nondevelopment_mono or its parent dir)
  --patch-shaders               Run tools/patch_game.py on <GAME_DIR> (creates .orig backups)
  -h, --help                    Show this help message

Examples:
  $0 /path/to/SDGB
  $0 --unity-linux-support /path/to/linux64_withgfx_nondevelopment_mono --patch-shaders /path/to/SDGB
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir)
            [ $# -ge 2 ] || { echo "[!] --build-dir requires a value" >&2; exit 1; }
            BUILD_DIR="$2"
            shift 2
            ;;
        --unity-linux-support)
            [ $# -ge 2 ] || { echo "[!] --unity-linux-support requires a value" >&2; exit 1; }
            UNITY_SUPPORT_DIR="$2"
            shift 2
            ;;
        --patch-shaders)
            PATCH_SHADERS=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "[!] Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
        *)
            if [ -n "$GAME_DIR" ]; then
                echo "[!] Multiple GAME_DIR arguments provided: $GAME_DIR and $1" >&2
                exit 1
            fi
            GAME_DIR="$1"
            shift
            ;;
    esac
done

if [ -z "$GAME_DIR" ]; then
    usage >&2
    exit 1
fi

if [ ! -d "$GAME_DIR/Sinmai_Data/Managed" ]; then
    echo "[!] Invalid GAME_DIR '$GAME_DIR': missing Sinmai_Data/Managed" >&2
    exit 1
fi

GAME_DIR="$(cd "$GAME_DIR" && pwd)"

if [ ! -d "$BUILD_DIR/plugins" ]; then
    echo "[!] Build directory '$BUILD_DIR/plugins' not found. Run 'make' first." >&2
    exit 1
fi

backup_once() {
    local target="$1"
    if [ -f "$target" ] && [ ! -f "${target}.orig" ]; then
        cp -p "$target" "${target}.orig"
        echo "[+] Backup created: ${target}.orig"
    fi
}

install_init_config() {
    local src="$1"
    local dst="$2"
    if [ -f "$dst" ]; then
        echo "[=] Keeping existing config: $dst"
    else
        cp "$src" "$dst"
        echo "[+] Created default config: $dst"
    fi
}

if [ -n "$UNITY_SUPPORT_DIR" ]; then
    if [ -d "$UNITY_SUPPORT_DIR/Variations/linux64_withgfx_nondevelopment_mono" ]; then
        UNITY_SUPPORT_DIR="$UNITY_SUPPORT_DIR/Variations/linux64_withgfx_nondevelopment_mono"
    fi
    if [ ! -f "$UNITY_SUPPORT_DIR/LinuxPlayer" ]; then
        echo "[!] LinuxPlayer not found under '$UNITY_SUPPORT_DIR'" >&2
        exit 1
    fi
    echo "[*] Installing Unity 2018.4.7f1 Linux Playback Support from: $UNITY_SUPPORT_DIR"
    python3 "$REPO_ROOT/tools/patch_linux_player.py" "$UNITY_SUPPORT_DIR/LinuxPlayer" "$GAME_DIR/Sinmai.x86_64"

    mkdir -p "$GAME_DIR/Sinmai_Data/Plugins/x86_64" \
             "$GAME_DIR/Sinmai_Data/MonoBleedingEdge/x86_64" \
             "$GAME_DIR/Sinmai_Data/MonoBleedingEdge/etc/mono/4.5" \
             "$GAME_DIR/Sinmai_Data/Resources"

    if [ -f "$UNITY_SUPPORT_DIR/Data/Plugins/x86_64/ScreenSelector.so" ]; then
        cp -f "$UNITY_SUPPORT_DIR/Data/Plugins/x86_64/ScreenSelector.so" "$GAME_DIR/Sinmai_Data/Plugins/x86_64/"
    fi
    cp -f "$UNITY_SUPPORT_DIR/Data/MonoBleedingEdge/x86_64/libmonobdwgc-2.0.so" "$GAME_DIR/Sinmai_Data/MonoBleedingEdge/x86_64/"
    cp -f "$UNITY_SUPPORT_DIR/Data/MonoBleedingEdge/x86_64/libMonoPosixHelper.so" "$GAME_DIR/Sinmai_Data/MonoBleedingEdge/x86_64/"

    if [ -f "$UNITY_SUPPORT_DIR/Data/Resources/unity default resources" ]; then
        backup_once "$GAME_DIR/Sinmai_Data/Resources/unity default resources"
        cp -f "$UNITY_SUPPORT_DIR/Data/Resources/unity default resources" "$GAME_DIR/Sinmai_Data/Resources/unity default resources"
    fi
    if [ ! -f "$GAME_DIR/Sinmai_Data/MonoBleedingEdge/etc/mono/4.5/machine.config" ] && \
       [ -f "$UNITY_SUPPORT_DIR/Data/MonoBleedingEdge/etc/mono/4.5/machine.config" ]; then
        cp "$UNITY_SUPPORT_DIR/Data/MonoBleedingEdge/etc/mono/4.5/machine.config" "$GAME_DIR/Sinmai_Data/MonoBleedingEdge/etc/mono/4.5/machine.config"
    fi
fi

echo "[*] Installing compiled native plugins and Mono stubs..."
mkdir -p "$GAME_DIR/Sinmai_Data/Plugins/x86_64" \
         "$GAME_DIR/Sinmai_Data/Mono/x86_64" \
         "$GAME_DIR/Sinmai_Data/MonoBleedingEdge/x86_64" \
         "$GAME_DIR/Sinmai_Data/MonoBleedingEdge/etc/mono" \
         "$GAME_DIR/Mods"

cp -f "$BUILD_DIR/plugins/"*.so "$GAME_DIR/Sinmai_Data/Plugins/x86_64/"
cp -f "$BUILD_DIR/mono/libgdiplus.so" "$GAME_DIR/Sinmai_Data/Mono/x86_64/libgdiplus.so"
ln -sf libgdiplus.so "$GAME_DIR/Sinmai_Data/Mono/x86_64/libgdiplus"
ln -sf libgdiplus.so "$GAME_DIR/Sinmai_Data/Mono/x86_64/libgdiplus.so.0"
cp -f "$BUILD_DIR/mono_bleeding_edge/libkernel32.so" "$BUILD_DIR/mono_bleeding_edge/libntdll.so" "$GAME_DIR/Sinmai_Data/MonoBleedingEdge/x86_64/"

backup_once "$GAME_DIR/Sinmai_Data/MonoBleedingEdge/etc/mono/config"
cp -f "$REPO_ROOT/config/mono_config" "$GAME_DIR/Sinmai_Data/MonoBleedingEdge/etc/mono/config"

if [ -f "$BUILD_DIR/mods/LinuxQrFix.dll" ]; then
    cp -f "$BUILD_DIR/mods/LinuxQrFix.dll" "$GAME_DIR/Mods/LinuxQrFix.dll"
    echo "[+] Installed mod: $GAME_DIR/Mods/LinuxQrFix.dll"
else
    echo "[!] Note: $BUILD_DIR/mods/LinuxQrFix.dll not built yet (run 'make mod GAME_DIR=$GAME_DIR' if needed)."
fi

cp -f "$REPO_ROOT/scripts/run_game.sh" "$GAME_DIR/run_game.sh"
chmod +x "$GAME_DIR/run_game.sh"

install_init_config "$REPO_ROOT/config/net_config.ini.example" "$GAME_DIR/net_config.ini"
install_init_config "$REPO_ROOT/config/coin_config.ini.example" "$GAME_DIR/coin_config.ini"
install_init_config "$REPO_ROOT/config/LinuxQrFix.ini.example" "$GAME_DIR/LinuxQrFix.ini"
install_init_config "$REPO_ROOT/config/audio_config.ini.example" "$GAME_DIR/audio_config.ini"

if [ "$PATCH_SHADERS" -eq 1 ]; then
    echo "[*] Backing up shader asset files before patching..."
    backup_once "$GAME_DIR/Sinmai_Data/Resources/unity_builtin_extra"
    backup_once "$GAME_DIR/Sinmai_Data/globalgamemanagers.assets"
    backup_once "$GAME_DIR/Sinmai_Data/resources.assets"
    DXBC2SPV_PATH="$BUILD_DIR/dxbc2spv" python3 "$REPO_ROOT/tools/patch_game.py" "$GAME_DIR"
fi

echo ""
echo "[+] Installation finished for $GAME_DIR"
echo "    Next checks:"
echo "      1. Edit $GAME_DIR/net_config.ini (server / keychip_id / board_id)"
echo "      2. Edit $GAME_DIR/coin_config.ini, LinuxQrFix.ini and audio_config.ini as needed"
echo "      3. Ensure Fonts/NotoSansCJK-Regular.ttc exists in $GAME_DIR if CJK text needs fallback"
echo "      4. Install config/udev/99-maimai-controller.rules to /etc/udev/rules.d/ for cabinet serial/HID hardware"
echo "      5. Launch via: cd \"$GAME_DIR\" && ./run_game.sh"
