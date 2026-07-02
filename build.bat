@echo off
setlocal enabledelayedexpansion

REM ============================================
REM Build Script for MapForGoblins DLL Mod
REM Toolchain: clang-cl + lld-link + ninja + xwin (NO Visual Studio / MSVC).
REM Same toolchain file as the Linux cross-build (clang-cl-xwin.cmake) —
REM one compiler everywhere, reproducible output (/Brepro: identical sources
REM produce byte-identical DLLs). See docs/plans/clang_only_toolchain_plan.md
REM and docs/memory/tooling/build-toolchain-clang-xwin.md (install recipe).
REM ============================================

set "SCRIPT_DIR=%~dp0"

REM Toolchain locations — env-var overridable per machine, defaults match the
REM current Windows box (memory: build-toolchain-clang-xwin).
if not defined MFG_LLVM_BIN set "MFG_LLVM_BIN=%USERPROFILE%\scoop\apps\llvm\current\bin"
if not defined MFG_NINJA    set "MFG_NINJA=C:\ninja-win\ninja.exe"
if not defined MFG_CMAKE    set "MFG_CMAKE=C:\Program Files\CMake\bin\cmake.exe"
if not defined MFG_XWIN     set "MFG_XWIN=D:/mfg_toolchain/xwin-sdk"

if not exist "%MFG_LLVM_BIN%\clang-cl.exe" (
    echo ERROR: clang-cl not found at %MFG_LLVM_BIN% - install LLVM via scoop or set MFG_LLVM_BIN.
    echo See docs\memory\tooling\build-toolchain-clang-xwin.md for the full toolchain recipe.
    exit /b 1
)
if not exist "%MFG_NINJA%" (
    echo ERROR: ninja not found at %MFG_NINJA% - set MFG_NINJA.
    exit /b 1
)
if not exist "%MFG_CMAKE%" (
    echo ERROR: cmake not found at %MFG_CMAKE% - set MFG_CMAKE.
    exit /b 1
)
if not exist "%MFG_XWIN%" (
    echo ERROR: xwin SDK splat not found at %MFG_XWIN% - set MFG_XWIN.
    echo Re-splat: xwin --accept-license --arch x86_64 splat --output D:\mfg_toolchain\xwin-sdk
    exit /b 1
)
set "PATH=%MFG_LLVM_BIN%;%PATH%"

REM Packaging profile: default = ERR; pass "--vanilla" / "--convergence" / "--erte"
REM (any position). SINGLE-DLL: every profile now builds the SAME DLL from the same
REM build-err/ tree (the per-profile GENERATED_SUBDIR bake split is gone; ERR-only
REM config is runtime-detected). The profile only selects the PACKAGE assets
REM (README, worldmap gfx, snapshot dir) and the data source for the offline
REM pipeline (MFG_PROFILE is exported so build_pipeline.py + config.py pick it).
REM NOTE: the ERR build dir moved build/ -> build-err/ with the ninja port (the old
REM build/ holds an incompatible msbuild tree; delete it whenever).
set "PKG_PREFIX=ERR"
set "SNAP_DIR=%SCRIPT_DIR%pre-release"
set "DISP_PROFILE=err"
set "BUILD_DIR=%SCRIPT_DIR%build-err"
set "README_SRC=%SCRIPT_DIR%assets\README.txt"
set "GFX_SRC=%SCRIPT_DIR%assets\menu\02_120_worldmap_err.gfx"
echo %*| findstr /i /c:"--vanilla" >nul && set "MFG_PROFILE=vanilla"
echo %*| findstr /i /c:"--convergence" >nul && set "MFG_PROFILE=convergence"
echo %*| findstr /i /c:"--erte" >nul && set "MFG_PROFILE=erte"
if "%MFG_PROFILE%"=="vanilla" set "PKG_PREFIX=Vanilla"
if "%MFG_PROFILE%"=="vanilla" set "SNAP_DIR=%SCRIPT_DIR%pre-release-vanilla"
if "%MFG_PROFILE%"=="vanilla" set "DISP_PROFILE=vanilla"
if "%MFG_PROFILE%"=="vanilla" set "README_SRC=%SCRIPT_DIR%assets\README_vanilla.txt"
if "%MFG_PROFILE%"=="vanilla" set "GFX_SRC=%SCRIPT_DIR%assets\menu\02_120_worldmap_vanilla.gfx"
if "%MFG_PROFILE%"=="convergence" set "PKG_PREFIX=Convergence"
if "%MFG_PROFILE%"=="convergence" set "SNAP_DIR=%SCRIPT_DIR%pre-release-convergence"
if "%MFG_PROFILE%"=="convergence" set "DISP_PROFILE=convergence"
if "%MFG_PROFILE%"=="convergence" set "README_SRC=%SCRIPT_DIR%assets\README_convergence.txt"
if "%MFG_PROFILE%"=="convergence" set "GFX_SRC=%SCRIPT_DIR%assets\menu\02_120_worldmap_convergence.gfx"
if "%MFG_PROFILE%"=="erte" set "PKG_PREFIX=ERTE"
if "%MFG_PROFILE%"=="erte" set "SNAP_DIR=%SCRIPT_DIR%pre-release-erte"
if "%MFG_PROFILE%"=="erte" set "DISP_PROFILE=erte"
if "%MFG_PROFILE%"=="erte" set "README_SRC=%SCRIPT_DIR%assets\README_erte.txt"
if "%MFG_PROFILE%"=="erte" set "GFX_SRC=%SCRIPT_DIR%assets\menu\02_120_worldmap_erte.gfx"
echo [PROFILE] %DISP_PROFILE%  build=%BUILD_DIR% (single DLL, all profiles)

if /i "%~1"=="clean" goto :clean
if /i "%~1"=="configure" goto :configure
if /i "%~1"=="generate" goto :generate
if /i "%~1"=="snapshot" goto :snapshot
if /i "%~1"=="release" goto :release

REM Default: configure if needed, then build
call :ensure_configured
if errorlevel 1 exit /b 1
call :build
if errorlevel 1 exit /b 1

echo.
echo Output: %BUILD_DIR%\MapForGoblins.dll (+ .pdb)
dir /b "%BUILD_DIR%\MapForGoblins.*" 2>nul
echo.
exit /b 0

:build
echo.
echo Building MapForGoblins (ninja + clang-cl)...
echo ----------------------------------------
"%MFG_NINJA%" -C "%BUILD_DIR%" MapForGoblins mfg_inigen
if errorlevel 1 (
    echo [FAILED] MapForGoblins
    exit /b 1
)
echo [SUCCESS] MapForGoblins
exit /b 0

:generate
echo.
echo Running data pipeline (incremental, hash-cached)...
echo ============================================
py "%SCRIPT_DIR%tools\build_pipeline.py" %*
echo.
exit /b 0

:ensure_configured
if not exist "%BUILD_DIR%\build.ninja" (
    echo Build not configured. Running configure...
    call :configure
    if errorlevel 1 exit /b 1
)
exit /b 0

:configure
REM %1 (optional) = extra -D flags, e.g. -DVERSION_PRE=""
echo.
echo Configuring CMake (Ninja + clang-cl-xwin)...
echo ============================================
"%MFG_CMAKE%" -B "%BUILD_DIR%" -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="%MFG_NINJA%" ^
  -DCMAKE_TOOLCHAIN_FILE="%SCRIPT_DIR%clang-cl-xwin.cmake" ^
  -DXWIN=%MFG_XWIN% ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 %~1
if errorlevel 1 (
    echo ERROR: CMake configuration failed!
    exit /b 1
)
echo CMake configuration complete.
exit /b 0

:snapshot
echo.
echo === Building snapshot (pre-release) ===

call :parse_version
if errorlevel 1 exit /b 1
echo Snapshot version: pre-%VER%

REM Incremental data pipeline (hash-based cache). Pass --force-all to rebuild everything.
echo Running incremental pipeline...
py "%SCRIPT_DIR%tools\build_pipeline.py" %*
if errorlevel 1 (
    echo [FAILED] build_pipeline.py
    exit /b 1
)

REM Reconfigure with the pre- prefix (also resets a cached VERSION_PRE="" from a
REM prior release run — the cache would otherwise keep the release value forever).
call :configure "-DVERSION_PRE=pre"
if errorlevel 1 exit /b 1
call :build
if errorlevel 1 (
    echo [FAILED] Snapshot build
    exit /b 1
)

REM Package into pre-release folder (SNAP_DIR set per profile at top)
call :package "%SNAP_DIR%"
if errorlevel 1 exit /b 1
call :archive_pdb "pre-%VER%"
powershell -NoProfile -Command "(Get-Content '%README_SRC%') -replace '%%VERSION%%','pre-%VER%' | Set-Content '%SNAP_DIR%\README.txt'"

echo.
echo [SUCCESS] Snapshot packaged: %SNAP_DIR% (pre-%VER%)
echo.
exit /b 0

:release
echo.
echo === Building release ===

call :parse_version
if errorlevel 1 exit /b 1

REM Split X.Y.Z
for /f "tokens=1,2,3 delims=." %%a in ("%VER%") do (
    set "V_MAJOR=%%a"
    set "V_MINOR=%%b"
    set "V_PATCH=%%c"
)
echo Release version: %VER% (%V_MAJOR%.%V_MINOR%.%V_PATCH%)

REM Incremental data pipeline (hash-based cache). Pass --force-all to rebuild everything.
echo Running incremental pipeline...
py "%SCRIPT_DIR%tools\build_pipeline.py" %*
if errorlevel 1 (
    echo [FAILED] build_pipeline.py
    exit /b 1
)

REM Build with VERSION_PRE="" (release, no pre- prefix). No Rebuild hack needed:
REM the old msbuild LTCG cache could embed stale code; ninja + thin-LTO track the
REM regenerated version.h correctly, and /Brepro makes the output reproducible.
call :configure "-DVERSION_PRE="""
if errorlevel 1 exit /b 1
call :build
if errorlevel 1 (
    echo [FAILED] Release build
    exit /b 1
)

REM Package into release folder (PKG_PREFIX = ERR or Vanilla per profile)
set "REL_DIR=%SCRIPT_DIR%%PKG_PREFIX% - MapForGoblins - DLL - v%VER%"
call :package "%REL_DIR%"
if errorlevel 1 exit /b 1
call :archive_pdb "v%VER%"
powershell -NoProfile -Command "(Get-Content '%README_SRC%') -replace '%%VERSION%%','%VER%' | Set-Content '%REL_DIR%\README.txt'"

echo.
echo Release packaged: %REL_DIR%

REM Bump patch version: X.Y.Z -> X.Y.(Z+1). Pass --no-bump to skip (used when
REM releasing BOTH profiles for the same version: run the first release with
REM --no-bump, the second one bumps once).
echo %*| findstr /i /c:"--no-bump" >nul && goto :release_nobump

set /a "V_NEXT=%V_PATCH%+1"
set "NEXT_VER=%V_MAJOR%.%V_MINOR%.%V_NEXT%"
echo Bumping to pre-%NEXT_VER%...

REM Update CMakeLists.txt version
powershell -Command "(Get-Content '%SCRIPT_DIR%CMakeLists.txt') -replace '  VERSION   \"%VER%\"', '  VERSION   \"%NEXT_VER%\"' | Set-Content '%SCRIPT_DIR%CMakeLists.txt'"

REM Reconfigure with the pre- prefix at the bumped version.
call :configure "-DVERSION_PRE=pre" >nul 2>&1

echo Done. Next dev version: pre-%NEXT_VER%
echo.
exit /b 0

:release_nobump
REM Reconfigure back to the pre- prefix at the SAME version (no bump).
call :configure "-DVERSION_PRE=pre" >nul 2>&1
echo Done. Version kept at %VER% (--no-bump).
echo.
exit /b 0

:archive_pdb
REM %1 = version tag (e.g. v1.0.18 / pre-1.0.18). Keep the matching PDB for
REM crash-RVA symbolication (tools/resolve_crash.py needs the DLL+PDB PAIR of
REM the build that crashed). The pdb is NOT shipped in the package (26 MB +
REM full symbol names); it lives under pdb-archive/ (git-ignored) instead.
set "PDB_DIR=%SCRIPT_DIR%pdb-archive\%~1-%DISP_PROFILE%"
if not exist "%PDB_DIR%" mkdir "%PDB_DIR%"
copy /Y "%BUILD_DIR%\MapForGoblins.dll" "%PDB_DIR%\" >nul
copy /Y "%BUILD_DIR%\MapForGoblins.pdb" "%PDB_DIR%\" >nul
echo PDB archived: %PDB_DIR%
exit /b 0

:package
REM %1 = target root dir. Lays out the package per profile.
REM   ERR              : <root>\dll\offline\{dll,ini} + <root>\addons\MapForGoblins\menu\gfx
REM   vanilla/convergence : <root>\MapForGoblins\{dll,ini} + <root>\MapForGoblins\menu\gfx
REM The ini is GENERATED from the in-code schema via mfg_inigen, not copied.
REM Single-DLL: one full ini everywhere; ERR-only entries are runtime-disabled
REM off-ERR by the DLL itself (goblin::err_features_enabled).
set "PKG_ROOT=%~1"
set "INIGEN=%BUILD_DIR%\mfg_inigen.exe"
if not exist "%INIGEN%" (
    echo [FAILED] mfg_inigen.exe not found under %BUILD_DIR%
    exit /b 1
)
if exist "%PKG_ROOT%" rmdir /s /q "%PKG_ROOT%"
if defined MFG_PROFILE (
    REM non-ERR gfx = that game's own worldmap base + our icon frames
    REM (tools/build_vanilla_gfx.py), NOT the ERR-based _new.gfx (that one
    REM carries re-iconed markers + references to textures absent elsewhere).
    REM GFX_SRC is set per profile at the top of this script.
    mkdir "%PKG_ROOT%\MapForGoblins\menu" 2>nul
    copy /Y "%BUILD_DIR%\MapForGoblins.dll" "%PKG_ROOT%\MapForGoblins\" >nul
    "%INIGEN%" "%PKG_ROOT%\MapForGoblins\MapForGoblins.ini"
    copy /Y "%GFX_SRC%" "%PKG_ROOT%\MapForGoblins\menu\02_120_worldmap.gfx" >nul
    copy /Y "%SCRIPT_DIR%LICENSE.txt" "%PKG_ROOT%\" >nul
) else (
    mkdir "%PKG_ROOT%\dll\offline" 2>nul
    mkdir "%PKG_ROOT%\addons\MapForGoblins\menu" 2>nul
    copy /Y "%BUILD_DIR%\MapForGoblins.dll" "%PKG_ROOT%\dll\offline\" >nul
    "%INIGEN%" "%PKG_ROOT%\dll\offline\MapForGoblins.ini"
    copy /Y "%GFX_SRC%" "%PKG_ROOT%\addons\MapForGoblins\menu\02_120_worldmap.gfx" >nul
    copy /Y "%SCRIPT_DIR%LICENSE.txt" "%PKG_ROOT%\" >nul
)
exit /b 0

:parse_version
set "VER="
for /f "tokens=2" %%v in ('findstr /C:"  VERSION   " "%SCRIPT_DIR%CMakeLists.txt"') do set "VER=%%~v"
if "%VER%"=="" (
    echo ERROR: Could not parse version from CMakeLists.txt
    exit /b 1
)
exit /b 0

:clean
echo Cleaning build directory...
if exist "%BUILD_DIR%" (
    rmdir /s /q "%BUILD_DIR%"
    echo Done.
) else (
    echo Nothing to clean.
)
exit /b 0
