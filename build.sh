#!/usr/bin/env bash
# Builds ddraw.dll and dsound.dll (32-bit) with llvm-mingw.
set -e
cd "$(dirname "$0")"
LLVM="${LLVM_MINGW:-$(ls -d /c/Users/*/AppData/Local/Microsoft/WinGet/Packages/MartinStorsjo.LLVM-MinGW*/llvm-mingw-* 2>/dev/null | head -1)}"
CXX="$LLVM/bin/i686-w64-mingw32-clang++"
OUT=build/out
mkdir -p $OUT build/obj
CXXFLAGS="-O2 -std=c++17 -Wall -Wno-unused-function -Wno-missing-braces -fno-exceptions -DUNICODE_CFG ${EXTRA_CXXFLAGS}"
LDFLAGS="-shared -static -s -Wl,--enable-stdcall-fixup -Wl,--kill-at"
VKSDK="${VULKAN_SDK:-$(ls -d /c/VulkanSDK/*/ 2>/dev/null | head -1)}"
DDRAW_SRC="src/common/common.cpp $(ls src/ddraw/*.cpp src/ddraw/backends/*.cpp | grep -v novulkan)"
if [ -n "$VKSDK" ] && [ -x "$VKSDK/Bin/glslangValidator.exe" ]; then
  python tools/gen_vk_shaders.py "$VKSDK/Bin/glslangValidator.exe"
  CXXFLAGS="$CXXFLAGS -Ibuild -I$VKSDK/Include"
else
  echo "Vulkan SDK not found: building without the Vulkan backend"
  DDRAW_SRC="$(echo $DDRAW_SRC | sed 's#src/ddraw/backends/vulkan.cpp##') src/ddraw/backends/novulkan.cpp"
fi
echo "== ddraw.dll"
$CXX $CXXFLAGS $DDRAW_SRC src/ddraw/ddraw.def -o $OUT/ddraw.dll $LDFLAGS -lgdi32 -luser32 -lole32 -luuid -ldxguid -lwinmm -ldwmapi -ldxguid -lopengl32 -lpsapi
if [ -d src/dsound ] && ls src/dsound/*.cpp >/dev/null 2>&1; then
  echo "== dsound.dll"
  $CXX $CXXFLAGS src/common/common.cpp src/dsound/*.cpp src/dsound/dsound.def -o $OUT/dsound.dll $LDFLAGS -lole32 -luuid -lwinmm -lavrt -lksuser -ldxguid -luuid
fi
ls -la $OUT
