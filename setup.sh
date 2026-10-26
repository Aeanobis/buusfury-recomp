#!/bin/sh
# Fetches and builds the third-party pieces under deps/. No root needed.
#   mGBA 0.10.5   hardware layer (video, audio, timers, BIOS calls, saves), MPL-2.0
#   xBRZ          picture upscaler, GPLv3
#   SDL2 headers  the SDL2 runtime library itself must already be installed
#   cmake         only used to build mGBA
set -e
cd "$(dirname "$0")"
mkdir -p deps && cd deps

if [ ! -d mgba ]; then
	git clone -q --depth 1 --branch 0.10.5 https://github.com/mgba-emu/mgba.git mgba
	git -C mgba apply ../runtime/mgba-recomp-hook.patch
fi

if [ ! -d xbrzscale ]; then
	git clone -q --depth 1 https://github.com/atheros/xbrzscale.git xbrzscale
fi

# HD picture mode: ncnn (GPU neural network runtime, BSD) and the Real-ESRGAN models
if [ ! -d vkroot ]; then
	mkdir -p debs-vk
	(cd debs-vk && apt-get download libvulkan-dev)
	for d in debs-vk/*.deb; do dpkg -x "$d" vkroot; done
fi
if [ ! -d ncnn ]; then
	git clone -q --depth 1 --branch 20240820 https://github.com/Tencent/ncnn.git ncnn
	git -C ncnn submodule update --init --depth 1 glslang
fi
if [ ! -d realesrgan ]; then
	mkdir realesrgan
	python3 -I - <<'PY'
import io, urllib.request, zipfile
u = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-ubuntu.zip"
zipfile.ZipFile(io.BytesIO(urllib.request.urlopen(u, timeout=300).read())).extractall("realesrgan")
PY
fi

if [ ! -d sdlroot ]; then
	mkdir -p debs
	(cd debs && apt-get download libsdl2-dev)
	for d in debs/*.deb; do dpkg -x "$d" sdlroot; done
fi

CMAKE=$(command -v cmake || true)
if [ -z "$CMAKE" ]; then
	if [ ! -d cmakewheel ]; then
		python3 -I - <<'PY'
import io, json, os, urllib.request, zipfile
j = json.load(urllib.request.urlopen("https://pypi.org/pypi/cmake/3.31.6/json", timeout=30))
u = [f for f in j["urls"] if "manylinux" in f["filename"] and "x86_64" in f["filename"]][0]
z = zipfile.ZipFile(io.BytesIO(urllib.request.urlopen(u["url"], timeout=300).read()))
z.extractall("cmakewheel")
for n in z.namelist():
    if "/bin/" in n:
        os.chmod(os.path.join("cmakewheel", n), 0o755)
PY
	fi
	CMAKE=$PWD/cmakewheel/cmake/data/bin/cmake
fi

mkdir -p ncnn-build
(cd ncnn-build && "$CMAKE" ../ncnn -DCMAKE_BUILD_TYPE=Release -DNCNN_VULKAN=ON -DNCNN_BUILD_TOOLS=OFF \
	-DNCNN_BUILD_EXAMPLES=OFF -DNCNN_BUILD_BENCHMARK=OFF -DNCNN_BUILD_TESTS=OFF -DNCNN_SHARED_LIB=OFF \
	-DNCNN_OPENMP=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DVulkan_INCLUDE_DIR="$PWD/../vkroot/usr/include" \
	-DVulkan_LIBRARY=/lib/x86_64-linux-gnu/libvulkan.so.1 > /dev/null && make -j"$(nproc)" | tail -1)

mkdir -p mgba-build && cd mgba-build
"$CMAKE" ../mgba -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-fPIC \
	-DBUILD_QT=OFF -DBUILD_SDL=OFF -DBUILD_SHARED=OFF -DBUILD_STATIC=ON -DM_CORE_GB=OFF \
	-DUSE_FFMPEG=OFF -DUSE_ZLIB=OFF -DUSE_MINIZIP=OFF -DUSE_PNG=OFF -DUSE_LIBZIP=OFF \
	-DUSE_SQLITE3=OFF -DUSE_ELF=OFF -DUSE_LUA=OFF -DUSE_EDITLINE=OFF -DUSE_GDB_STUB=OFF \
	-DUSE_DISCORD_RPC=OFF -DUSE_EPOXY=OFF -DUSE_LZMA=OFF -DENABLE_SCRIPTING=OFF \
	-DUSE_DEBUGGERS=OFF -DBUILD_GL=OFF -DBUILD_GLES2=OFF -DBUILD_GLES3=OFF -DBUILD_LTO=OFF > /dev/null
make -j"$(nproc)" | tail -1
