cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
project(PlainView LANGUAGES CXX RC)

# Static CRT: the executable is a single portable file with no redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# One executable, no third-party dependencies: Win32 + WIC + Direct2D.
add_executable(PlainView WIN32
    src/cache.cpp
    src/colordialog.cpp
    src/common.cpp
    src/dialogutil.cpp
    src/decode.cpp
    src/folder.cpp
    src/options.cpp
    src/register.cpp
    src/settings.cpp
    src/shellicon.cpp
    src/viewer.cpp
    src/main.cpp
    src/app.rc
    src/app.manifest
)

target_compile_features(PlainView PRIVATE cxx_std_20)
target_compile_definitions(PlainView PRIVATE UNICODE _UNICODE)

if(MSVC)
    target_compile_options(PlainView PRIVATE /W4 /permissive- /utf-8)
endif()

target_link_libraries(PlainView PRIVATE
    user32
    gdi32
    ole32
    oleaut32
    shell32
    shlwapi
    d2d1
    windowscodecs
    comdlg32
    comctl32
    psapi
    uuid
    advapi32
)

set_target_properties(PlainView PROPERTIES
    OUTPUT_NAME "PlainView"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
