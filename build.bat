@echo off
rem Build PlainView.exe with the Visual Studio toolchain.
rem The C runtime is linked statically (/MT), so the executable runs on a clean
rem Windows install with no redistributable and no Visual Studio.
rem Usage: build.bat            (Release, /O2, output build\PlainView.exe)
rem        build.bat debug      (Debug, /Od /Zi, output build\debug)
setlocal EnableDelayedExpansion

set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
  echo Could not find vcvars64.bat at "%VCVARS%"
  exit /b 1
)
call "%VCVARS%" >nul || exit /b 1

set CONFIG=%1
if /i "%CONFIG%"=="debug" (
  set CFLAGS=/Od /Zi /MTd /D_DEBUG
  set OUTDIR=build\debug
) else (
  set CFLAGS=/O2 /MT /DNDEBUG
  set OUTDIR=build
)

if not exist %OUTDIR% mkdir %OUTDIR%

echo [1/2] resource compiler
rc /nologo /fo %OUTDIR%\app.res src\app.rc || exit /b 1

echo [2/2] compiling and linking
cl /nologo /std:c++20 /EHsc /W4 /WX /permissive- /utf-8 ^
   /DUNICODE /D_UNICODE !CFLAGS! ^
   /Fo%OUTDIR%\ /Fd%OUTDIR%\ ^
   src\cache.cpp src\colordialog.cpp src\common.cpp src\decode.cpp src\dialogutil.cpp ^
   src\folder.cpp src\register.cpp src\settings.cpp src\shellicon.cpp ^
   src\viewer.cpp src\options.cpp src\main.cpp ^
   /Fe%OUTDIR%\PlainView.exe ^
   /link %OUTDIR%\app.res /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib shlwapi.lib ^
   d2d1.lib windowscodecs.lib comdlg32.lib comctl32.lib psapi.lib uuid.lib advapi32.lib || exit /b 1

echo.
echo Built %OUTDIR%\PlainView.exe
endlocal
