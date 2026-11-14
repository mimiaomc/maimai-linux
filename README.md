# maimai DX Linux 原生运行方案 (maimai-linux)

无需 Wine / Proton，直接通过 Unity 原生 Linux Player + DXBC→SPIR-V→GLSL 离线着色器转译 + 原生 `.so` 插件替代层 + MelonLoader 引导补丁，在 Linux (x86_64) 上原生运行 *maimai DX*（`SDGB` / `Sinmai`）。

> **免责声明**
> 本仓库**仅包含**自主编写的 Linux 兼容层 C 源码、C# Mod、构建脚本、资产转译/补丁工具与配置模板，**不包含**任何 SEGA / Unity / MelonLoader / 第三方 Mod 的专有游戏文件、可执行程序、密钥或私服配置。

---

## 目录结构

```text
maimai-linux/
├── LICENSE                               # GNU Lesser General Public License v2.1 (LGPL-2.1)
├── Makefile                              # 原生 .so 插件、dxbc2spv 工具与 LinuxQrFix Mod 构建脚本
├── README.md                             # 本文档
├── config/
│   ├── net_config.ini.example            # ALL.Net 联网与机台号配置模板
│   ├── coin_config.ini.example           # 投币按键与免费游玩配置模板
│   ├── LinuxQrFix.ini.example            # 1P/2P 触摸串口、灯板串口与手台 HID 端口配置模板
│   ├── mono_config                       # Mono DLL 映射表 (-> Sinmai_Data/MonoBleedingEdge/etc/mono/config)
│   └── udev/
│       └── 99-maimai-controller.rules    # 触摸屏 / 手台 / 灯板串口与 hidraw udev 规则
├── scripts/
│   ├── install.sh                        # 一键部署编译产物、Unity Linux 运行库、配置模板并可选转译 Shader
│   └── run_game.sh                       # 游戏启动脚本（设置环境变量与 LD_PRELOAD 并启动 Sinmai.x86_64）
├── src/
│   ├── mods/LinuxQrFix/
│   │   ├── LinuxQrFix.cs                 # MelonLoader Mod：Linux 串口/HID/摄像头兼容与保活
│   │   └── LinuxQrFix.csproj             # C# 工程文件
│   ├── stubs/                            # 原生 Linux 插件与 Mono Stub 源码
│   │   ├── amdaemon_api.c                # -> libamdaemon_api.so (ALL.Net/投币/Aime/IO/文件系统等模拟)
│   │   ├── chimelib_dll.c                # -> libchimelib_dll.so (二维码登录协议与 curl 子进程通信)
│   │   ├── clHCA.c / clhca.h / clhca_data.h
│   │   ├── cri_audio.c / cri_audio.h     # CriAtom PulseAudio 音频输出与 HCA 软件解码
│   │   ├── cri_video.c / cri_video.h     # CriMana libvpx VP9 视频解码与 OpenGL 纹理更新
│   │   ├── cri_ware_unity.c              # -> libcri_ware_unity.so (CriWare Unity 插件主入口)
│   │   ├── cri_mana_vpx.c                # -> libcri_mana_vpx.so
│   │   ├── libgdiplus_stub.c             # -> libgdiplus.so (及其 .so.0 符号链接)
│   │   ├── libkernel32_stub.c            # -> libkernel32.so & libntdll.so
│   │   ├── preload_stub.c                # -> libPreLoad.so (MelonLoader/0Harmony 引导 + Mono 运行时修复)
│   │   ├── QR_Image.c                    # -> libQR_Image.so
│   │   └── v4l2_filter.c                 # -> libv4l2_filter.so (LD_PRELOAD：摄像头节点过滤 + 游戏内字体扫描)
│   └── tools/
│       └── dxbc2spv.c                    # 基于 libvkd3d-shader 将 Windows DXBC 转译为 SPIR-V
└── tools/
    ├── patch_linux_player.py             # 对 Unity 2018.4.7f1 LinuxPlayer 应用 -force-glcore 6 字节补丁
    └── patch_game.py                     # 使用 dxbc2spv + spirv-cross 将游戏 Shader 转译为 Linux GLCore 资产
```

---

## 核心工作原理

1. **Unity LinuxPlayer + `-force-glcore` 补丁 (`tools/patch_linux_player.py`)**
   Windows 版 *Sinmai* 基于 **Unity 2018.4.7f1**。使用官方同版本 Linux Playback Support 中的 `LinuxPlayer`（`linux64_withgfx_nondevelopment_mono`）替换程序主体为 `Sinmai.x86_64`。由于游戏原资产构建自 Windows 编辑器，不包含 Linux OpenGL Core 目标标记，Unity 启动传入 `-force-glcore` 时会拒绝加载；`patch_linux_player.py` 在二进制检查点写入 6 字节跳转补丁，允许直接启用 GLCore 渲染。
2. **离线着色器转译 (`dxbc2spv` + `spirv-cross` + `tools/patch_game.py`)**
   遍历 `Sinmai_Data/Resources/unity_builtin_extra`、`globalgamemanagers.assets`、`resources.assets`，提取 Windows Direct3D DXBC 字节码，依次经由 `libvkd3d-shader` (`dxbc2spv`) 转成 SPIR-V、经由 `spirv-cross` 转成 GLSL 150，并针对 TextMeshPro SDF (`Distance Field`) 字体采样等特殊着色器做 `sdfBilinear` 修复后，作为平台 `15`（OpenGL Core）子程序重新压缩回 `.assets` 文件。
3. **原生插件与 Mono 兼容层 (`src/stubs/`)**
   - `libPreLoad.so`：注册崩溃信号处理器，打通 Linux 下 `MelonLoader/net35` 引导与 Mono `MAI2System.IniFile` 修复。
   - `libamdaemon_api.so` / `libchimelib_dll.so`：实现游戏所需的 AMDaemon、Aime 读卡器、ALL.Net (`net_config.ini`)、投币记账 (`coin_config.ini`) 与二维码通信（通过外部 `/usr/bin/curl` 执行 HTTPS 请求，避免与 Unity 内置库产生 OpenSSL 符号冲突）。
   - `libcri_ware_unity.so` / `libcri_mana_vpx.so`：基于 `libpulse-simple.so.0`、内嵌 HCA 解码与 `libvpx` + OpenGL 接管游戏音频与 PV 视频播放。
   - `libv4l2_filter.so`：作为 `LD_PRELOAD` 加载，在打开 `/dev/video*` 时过滤掉不支持彩色视频流的元数据节点，并在 Unity 扫描系统 `/usr/share/fonts` 时自动额外扫描游戏根目录的 `Fonts/`。
   - `config/mono_config` + `libgdiplus.so` / `libkernel32.so` / `libntdll.so` / `libQR_Image.so`：为 `.NET` / `P/Invoke` 提供 Linux 映射。
4. **`LinuxQrFix` Mod (`src/mods/LinuxQrFix/LinuxQrFix.cs`)**
   运行于 MelonLoader `net35` 环境下，负责摄像头探测、统一读取游戏根目录 `LinuxQrFix.ini` 中的串口与 HID 映射（`Touch1P`→`COM3`、`Touch2P`→`COM4`、`Led1P`→`COM21`、`Led2P`→`COM23`、`Hid1P`/`Hid2P`→`/dev/hidraw*`）、控制器（如 `2e3c:5750`）断连保活与按键兼容。

---

## 环境要求

### 1. 构建工具与系统依赖

以 **Fedora** 为例（其他发行版请安装对应包）：

- **编译工具**：`gcc`、`make`
- **Shader 转译依赖**：
  - `libvkd3d-shader` 开发头文件与动态库（例如 Fedora: `vkd3d-devel` / `libvkd3d-shader-devel`；Debian/Ubuntu: `libvkd3d-shader-dev`）
  - `spirv-cross` 可执行文件（发行版包或从 [KhronosGroup/SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) 源码编译）
- **Python 3 与依赖库**：`python3`、`UnityPy`、`lz4`（例如 `pip install UnityPy lz4`）
- **C# 编译环境（二选一，用于编译 `LinuxQrFix.dll`）**：
  - `.NET SDK`（`dotnet`，自动使用其 SDK 自带的 `Roslyn/bincore/csc.dll`，无需联网拉取 NuGet 包）
  - 或 Mono C# 编译器（`mcs`）
- **游戏运行时动态库**：
  - `curl` (`/usr/bin/curl`)
  - `zlib` (`libz.so.1`)
  - `pulseaudio-libs` (`libpulse-simple.so.0`)
  - `libvpx`（运行时依次尝试加载 `libvpx.so.9` / `.10` / `.8` / `.7` / `.so`）
  - OpenGL (`libGL.so.1`)

### 2. 需要自备的外部游戏/运行时文件

请事先准备一个完整的原版 *SDGB* 游戏目录（以下记作 `<GAME_DIR>`，建议提前完整备份），并准备好：

1. **Unity `2018.4.7f1` Linux Playback Support**
   提取其中的 `Variations/linux64_withgfx_nondevelopment_mono` 目录（包含 `LinuxPlayer`、`Data/Plugins/x86_64/ScreenSelector.so`、`Data/MonoBleedingEdge/x86_64/libmonobdwgc-2.0.so`、`libMonoPosixHelper.so`、`Data/Resources/unity default resources` 等）。
2. **MelonLoader `v0.6.4`（包含 `MelonLoader/net35/` 等托管运行库）**
   将 `MelonLoader/` 目录放入 `<GAME_DIR>/MelonLoader/`，并将你需要的其他 Mod（如 `AquaMai.dll` 等）放入 `<GAME_DIR>/Mods/`。
3. **中文字体（可选但强烈建议）**
   如游戏内缺字，可将 `NotoSansCJK-Regular.ttc` 放到 `<GAME_DIR>/Fonts/NotoSansCJK-Regular.ttc`，由 `libv4l2_filter.so` 自动挂载扫描。

---

## 编译与安装

### 1. 编译本仓库源码

```bash
# 1) 仅编译原生 .so 插件与 dxbc2spv 工具
make

# 2) 同时编译 LinuxQrFix.dll（需指向已含 Sinmai_Data/Managed 和 MelonLoader/net35 的游戏目录）
make GAME_DIR=/path/to/SDGB
```

> 若系统有多个 `dotnet` 路径，可手动指定编译器，例如：`make mod GAME_DIR=/path/to/SDGB DOTNET=/path/to/dotnet`。

### 2. 部署到游戏目录并转译资产

确保 `spirv-cross` 已在 `PATH` 中（或通过环境变量 `SPIRV_CROSS_PATH=/path/to/spirv-cross` 指定），随后执行：

```bash
bash ./scripts/install.sh \
  --unity-linux-support /path/to/Unity/2018.4.7f1/LinuxSupport/Variations/linux64_withgfx_nondevelopment_mono \
  --patch-shaders \
  /path/to/SDGB
```

该脚本会自动完成以下操作：
- 为将被修改的 `unity default resources`、`unity_builtin_extra`、`globalgamemanagers.assets`、`resources.assets` 与 `mono/config` 创建 `.orig` 备份（仅当 `.orig` 尚不存在时）；
- 对官方 `LinuxPlayer` 打上 `-force-glcore` 补丁生成 `<GAME_DIR>/Sinmai.x86_64` 并复制对应 Unity Linux 运行库；
- 安装所有 `lib*.so`、`config/mono_config`、`Mods/LinuxQrFix.dll` 与 `<GAME_DIR>/run_game.sh`（自动赋予可执行权限）；
- 仅在配置文件不存在时初始化 `<GAME_DIR>/net_config.ini`、`coin_config.ini` 与 `LinuxQrFix.ini`（避免覆盖已有配置）；
- 调用 `tools/patch_game.py` 转译全部游戏着色器。

> 如果此前已经部署过 Unity Linux 运行库和 Shader，后续仅重新编译覆盖插件/Mod 时可直接运行 `bash ./scripts/install.sh /path/to/SDGB`（或 `make install GAME_DIR=/path/to/SDGB`）。

---

## 硬件权限与配置文件说明

所有用户配置文件均统一放置在**游戏根目录** `<GAME_DIR>/` 下，修改后重启游戏生效：

### 1. 机台与触摸/灯板 `udev` 规则（使用外设时）

将 `config/udev/99-maimai-controller.rules` 安装到系统并加入 `dialout` 用户组：

```bash
sudo cp config/udev/99-maimai-controller.rules /etc/udev/rules.d/
sudo usermod -aG dialout "$USER"
sudo udevadm control --reload-rules
sudo udevadm trigger --action=add --subsystem-match=tty --subsystem-match=hidraw
```

规则加载后会创建如下固定设备链接（修改完用户组需重新登录生效）：
- `/dev/ttyUSB_at32`：AT32 (`2e3c:5750`) 复合串口设备（自动绑定 `usbserial_generic`）
- `/dev/ttyUSB_ch340`：CH340 (`1a86:7523`) 串口设备（常见于灯板）
- `/dev/hidraw_maimai`：AT32 (`2e3c:5750`) HID 按键设备

### 2. 硬件端口映射：`<GAME_DIR>/LinuxQrFix.ini`

```ini
[Serial]
# 1P / 2P 触摸屏串口设备（对应游戏内 COM3 / COM4）
Touch1P=/dev/ttyUSB_at32
Touch2P=

# 1P / 2P 灯板串口设备（对应游戏内 COM21 / COM23）
Led1P=/dev/ttyUSB_ch340
Led2P=

[Hid]
# 1P / 2P 手台按键 HID 设备（支持具体路径、auto 或 none）
Hid1P=/dev/hidraw_maimai
Hid2P=
```

- 串口字段留空表示不映射对应 COM 口；如果你的触摸与灯板分别使用了不同芯片或反接，只需互换 `Touch1P` 与 `Led1P` 的设备路径即可。
- `Hid1P`/`Hid2P` 留空或填 `auto` 会自动探测，填 `none` 可禁用内置 HID 轮询。

### 3. 联网配置：`<GAME_DIR>/net_config.ini`

主要配置项：
- `server`：ALL.Net 服务器地址、IP:端口或完整 URL
- `protocol`：`auto` / `lite` / `poweron`
- `keychip_id` / `board_id`：替换为你的有效机台号 / 加密狗 ID（支持 `A63E-01EXXXXXXXX` 16 位格式或 11 位短格式）
- `game_id` / `title_ver` / `region_code` / `ssl_verify` / `timeout_sec`

### 4. 投币配置：`<GAME_DIR>/coin_config.ini`

- `coins`：当前保存币数
- `coins_per_credit`：每点数所需币数
- `coin_key`：游戏窗口聚焦时生效的投币按键（如 `5`、`Insert`、`Space` 等）
- `free_play`：`true` / `false`

---

## 启动与日志排查

在游戏目录执行：

```bash
cd /path/to/SDGB
./run_game.sh
```

`run_game.sh` 会自动探测 `DISPLAY` / `WAYLAND_DISPLAY`，设置 `MONO_PATH`、`LD_LIBRARY_PATH` 与 `LD_PRELOAD=libv4l2_filter.so`，并以：

```text
./Sinmai.x86_64 -force-glcore -screen-fullscreen 0 -screen-width 1080 -screen-height 1920 --melonloader.disablestartscreen
```

启动游戏。常见运行日志路径如下：
- **标准输出总日志**：`/tmp/sinmai_stdout.log`
- **MelonLoader / Mod 日志**：`<GAME_DIR>/MelonLoader/Latest.log`
- **AMDaemon / 网络 / 二维码 Stub 日志**：`/tmp/amd_stub.log`
- **CriWare 音视频日志**：`/tmp/cri_stub.log`
- **PreLoad 引导日志**：`/tmp/preload_stub.log`
- **原生崩溃堆栈回溯**：`/tmp/crash_backtrace.log`

> **提示**：若使用 `ldd Sinmai_Data/Plugins/x86_64/ScreenSelector.so` 检查到缺少 `libgtk-x11-2.0.so.0`，通常不影响直接通过 `-force-glcore` 参数启动游戏。

---

## 致谢

- **`clHCA` 解码器 (`src/stubs/clHCA.c`, `clhca.h`, `clhca_data.h`)**：感谢 **nyaga** (`Nyagamon/HCADecoder`)、**kode54**、**bnnm** (`VGAudio`) 与 **Youjose** (`CriCodecs`) 的开源贡献。
- **着色器转译工具链**：[libvkd3d-shader](https://gitlab.winehq.org/wine/vkd3d)、[KhronosGroup/SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)、[UnityPy](https://github.com/K0lb3/UnityPy)。
- **Mod 加载框架**：[LavaGang/MelonLoader](https://github.com/LavaGang/MelonLoader)。

---

## 开源许可证 (License)

本项目依据 **GNU Lesser General Public License v2.1 (LGPL-2.1)** 开源发布，完整许可证文本见 [LICENSE](LICENSE)。
