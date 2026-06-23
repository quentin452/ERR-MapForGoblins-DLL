# Cross-compile toolchain: build the MSVC-ABI Windows x64 DLL on Linux using
# clang-cl + lld-link against an xwin-splatted MSVC CRT + Windows SDK.
#   xwin --accept-license splat --output ~/.local/share/xwin
#   cmake -B build-linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=clang-cl-xwin.cmake
#   ninja -C build-linux MapForGoblins
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(XWIN "$ENV{HOME}/.local/share/xwin" CACHE PATH "xwin splat dir")

set(CMAKE_C_COMPILER   clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER       lld-link)
set(CMAKE_RC_COMPILER  llvm-rc)
set(CMAKE_MT           llvm-mt)

# Triple + clang-cl needs explicit target on a non-Windows host.
set(_triple --target=x86_64-pc-windows-msvc)

# System include dirs (/imsvc = treat as system, silence SDK warnings).
set(_incs
  "/imsvc${XWIN}/crt/include"
  "/imsvc${XWIN}/sdk/include/ucrt"
  "/imsvc${XWIN}/sdk/include/um"
  "/imsvc${XWIN}/sdk/include/shared"
  "/imsvc${XWIN}/sdk/include/winrt"
  "/imsvc${XWIN}/sdk/include/cppwinrt")
string(JOIN " " _incs_str ${_incs})

# /arch:AVX2 -> clang exposes AVX/AVX2/BMI2 intrinsics (Pattern16 needs them;
#   MSVC exposes intrinsics unconditionally, clang gates them by target feature).
# /DFMT_CONSTEVAL= -> neutralize bundled-fmt consteval format-string checks that
#   clang 22 rejects (spdlog uses bundled fmt here, not std::format).
set(_extra "/arch:AVX2 /DFMT_CONSTEVAL=")
set(CMAKE_C_FLAGS_INIT   "${_triple} ${_extra} ${_incs_str}")
set(CMAKE_CXX_FLAGS_INIT "${_triple} ${_extra} ${_incs_str}")

# llvm-rc needs the SDK include dirs to resolve <winver.h> for the VERSIONINFO
# resource (src/version.rc.in → antivirus-FP metadata).
set(CMAKE_RC_FLAGS_INIT
    "-I ${XWIN}/sdk/include/um -I ${XWIN}/sdk/include/shared -I ${XWIN}/sdk/include/ucrt")

# Library search paths for the linker.
set(_libs
  "/libpath:${XWIN}/crt/lib/x86_64"
  "/libpath:${XWIN}/sdk/lib/ucrt/x86_64"
  "/libpath:${XWIN}/sdk/lib/um/x86_64")
string(JOIN " " _libs_str ${_libs})
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_libs_str}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_libs_str}")

# Cross: don't probe/run target binaries; static lib for try_compile.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
