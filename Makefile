CC ?= gcc
CFLAGS ?= -O2 -Wall
SHARED_CFLAGS := $(CFLAGS) -fPIC
PKG_CONFIG ?= pkg-config
VKD3D_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libvkd3d-shader 2>/dev/null || echo -I/usr/include/vkd3d)
VKD3D_LIBS ?= $(shell $(PKG_CONFIG) --libs libvkd3d-shader 2>/dev/null || echo -lvkd3d-shader)

BUILD_DIR ?= build
GAME_DIR ?=

DOTNET ?= $(shell command -v dotnet 2>/dev/null)
MCS ?= $(shell command -v mcs 2>/dev/null)
CSC_DLL ?= $(shell \
	{ \
		if [ -n "$(DOTNET)" ]; then \
			SDK_LINE=$$("$(DOTNET)" --list-sdks 2>/dev/null | head -n 1); \
			VER=$${SDK_LINE%% *}; \
			DIR=$${SDK_LINE#*\[}; \
			DIR=$${DIR%\]}; \
			if [ -n "$$VER" ] && [ -f "$$DIR/$$VER/Roslyn/bincore/csc.dll" ]; then \
				echo "$$DIR/$$VER/Roslyn/bincore/csc.dll"; \
			fi; \
		fi; \
		for p in /usr/share/dotnet/sdk/*/Roslyn/bincore/csc.dll /usr/lib64/dotnet/sdk/*/Roslyn/bincore/csc.dll /usr/lib/dotnet/sdk/*/Roslyn/bincore/csc.dll; do \
			if [ -f "$$p" ]; then echo "$$p"; break; fi; \
		done; \
	} | head -n 1)

PLUGINS := \
	$(BUILD_DIR)/plugins/libamdaemon_api.so \
	$(BUILD_DIR)/plugins/libchimelib_dll.so \
	$(BUILD_DIR)/plugins/libcri_ware_unity.so \
	$(BUILD_DIR)/plugins/libcri_mana_vpx.so \
	$(BUILD_DIR)/plugins/libPreLoad.so \
	$(BUILD_DIR)/plugins/libQR_Image.so \
	$(BUILD_DIR)/plugins/libv4l2_filter.so

MONO_LIBS := \
	$(BUILD_DIR)/mono/libgdiplus.so \
	$(BUILD_DIR)/mono_bleeding_edge/libkernel32.so \
	$(BUILD_DIR)/mono_bleeding_edge/libntdll.so

TOOLS := $(BUILD_DIR)/dxbc2spv

.PHONY: all native tools mod install clean help

all: native tools
ifneq ($(strip $(GAME_DIR)),)
all: mod
endif

native: $(PLUGINS) $(MONO_LIBS)

tools: $(TOOLS)

mod: $(BUILD_DIR)/mods/LinuxQrFix.dll

$(BUILD_DIR)/plugins $(BUILD_DIR)/mono $(BUILD_DIR)/mono_bleeding_edge $(BUILD_DIR)/mods:
	mkdir -p $@

$(BUILD_DIR)/plugins/libamdaemon_api.so: src/stubs/amdaemon_api.c | $(BUILD_DIR)/plugins
	$(CC) -shared $(SHARED_CFLAGS) $< -o $@ -lm -ldl -lpthread

$(BUILD_DIR)/plugins/libchimelib_dll.so: src/stubs/chimelib_dll.c | $(BUILD_DIR)/plugins
	$(CC) -shared $(SHARED_CFLAGS) $< -o $@ -lpthread

$(BUILD_DIR)/plugins/libcri_ware_unity.so: src/stubs/cri_ware_unity.c src/stubs/cri_audio.c src/stubs/cri_video.c src/stubs/clHCA.c src/stubs/cri_audio.h src/stubs/cri_video.h src/stubs/clhca.h src/stubs/clhca_data.h | $(BUILD_DIR)/plugins
	$(CC) -shared $(SHARED_CFLAGS) -Isrc/stubs src/stubs/cri_ware_unity.c src/stubs/cri_audio.c src/stubs/cri_video.c src/stubs/clHCA.c -o $@ -lm -ldl -lpthread

$(BUILD_DIR)/plugins/libcri_mana_vpx.so: src/stubs/cri_mana_vpx.c | $(BUILD_DIR)/plugins
	$(CC) -shared $(SHARED_CFLAGS) $< -o $@

$(BUILD_DIR)/plugins/libPreLoad.so: src/stubs/preload_stub.c | $(BUILD_DIR)/plugins
	$(CC) -shared $(SHARED_CFLAGS) $< -o $@ -ldl

$(BUILD_DIR)/plugins/libQR_Image.so: src/stubs/QR_Image.c | $(BUILD_DIR)/plugins
	$(CC) -shared $(SHARED_CFLAGS) $< -o $@

$(BUILD_DIR)/plugins/libv4l2_filter.so: src/stubs/v4l2_filter.c | $(BUILD_DIR)/plugins
	$(CC) -shared $(SHARED_CFLAGS) $< -o $@ -ldl

$(BUILD_DIR)/mono/libgdiplus.so: src/stubs/libgdiplus_stub.c | $(BUILD_DIR)/mono
	$(CC) -shared $(SHARED_CFLAGS) $< -o $@
	ln -sf libgdiplus.so $(BUILD_DIR)/mono/libgdiplus
	ln -sf libgdiplus.so $(BUILD_DIR)/mono/libgdiplus.so.0

$(BUILD_DIR)/mono_bleeding_edge/libkernel32.so: src/stubs/libkernel32_stub.c | $(BUILD_DIR)/mono_bleeding_edge
	$(CC) -shared $(SHARED_CFLAGS) $< -o $@

$(BUILD_DIR)/mono_bleeding_edge/libntdll.so: src/stubs/libkernel32_stub.c | $(BUILD_DIR)/mono_bleeding_edge
	$(CC) -shared $(SHARED_CFLAGS) $< -o $@

$(BUILD_DIR)/dxbc2spv: src/tools/dxbc2spv.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(VKD3D_CFLAGS) $< -o $@ $(VKD3D_LIBS)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/mods/LinuxQrFix.dll: src/mods/LinuxQrFix/LinuxQrFix.cs | $(BUILD_DIR)/mods
	@if [ -z "$(strip $(GAME_DIR))" ]; then \
		echo "Error: GAME_DIR is required to compile LinuxQrFix.dll (e.g. make mod GAME_DIR=/path/to/SDGB)" >&2; \
		exit 1; \
	fi
	@if [ ! -f "$(GAME_DIR)/Sinmai_Data/Managed/Assembly-CSharp.dll" ] || [ ! -f "$(GAME_DIR)/MelonLoader/net35/MelonLoader.dll" ]; then \
		echo "Error: Missing Managed or MelonLoader/net35 assemblies in GAME_DIR=$(GAME_DIR)" >&2; \
		exit 1; \
	fi
	@REFS="-r:$(GAME_DIR)/Sinmai_Data/Managed/mscorlib.dll \
		-r:$(GAME_DIR)/Sinmai_Data/Managed/System.dll \
		-r:$(GAME_DIR)/Sinmai_Data/Managed/System.Core.dll \
		-r:$(GAME_DIR)/Sinmai_Data/Managed/Assembly-CSharp.dll \
		-r:$(GAME_DIR)/Sinmai_Data/Managed/UnityEngine.dll \
		-r:$(GAME_DIR)/Sinmai_Data/Managed/UnityEngine.CoreModule.dll \
		-r:$(GAME_DIR)/Sinmai_Data/Managed/UnityEngine.AudioModule.dll \
		-r:$(GAME_DIR)/MelonLoader/net35/MelonLoader.dll \
		-r:$(GAME_DIR)/MelonLoader/net35/0Harmony.dll"; \
	if [ -n "$(CSC_DLL)" ] && [ -n "$(DOTNET)" ]; then \
		"$(DOTNET)" exec "$(CSC_DLL)" -nologo -noconfig -nostdlib -target:library -out:$@ $$REFS $<; \
	elif [ -n "$(MCS)" ]; then \
		"$(MCS)" -nostdlib -noconfig -target:library -out:$@ $$REFS $<; \
	else \
		echo "Error: Neither .NET SDK csc.dll (DOTNET=$(DOTNET)) nor Mono mcs was found." >&2; \
		exit 1; \
	fi

install:
	@if [ -z "$(strip $(GAME_DIR))" ]; then \
		echo "Error: GAME_DIR is required (e.g. make install GAME_DIR=/path/to/SDGB)" >&2; \
		exit 1; \
	fi
	bash ./scripts/install.sh --build-dir "$(BUILD_DIR)" "$(GAME_DIR)"

clean:
	rm -rf "$(BUILD_DIR)"

help:
	@echo "Targets:"
	@echo "  make                               Build native .so stubs and dxbc2spv"
	@echo "  make GAME_DIR=/path/to/SDGB        Build native .so stubs, dxbc2spv, and LinuxQrFix.dll"
	@echo "  make native                        Build only native .so stubs"
	@echo "  make tools                         Build only dxbc2spv (requires libvkd3d-shader)"
	@echo "  make mod GAME_DIR=/path/to/SDGB    Build only LinuxQrFix.dll against game assemblies"
	@echo "  make install GAME_DIR=/path/to/SDGB Deploy compiled files and config templates"
	@echo "  make clean                         Remove build outputs"
