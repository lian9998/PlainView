cmake_minimum_required(VERSION 3.20)
project(PlainView LANGUAGES CXX RC)

# One executable, no third-party dependencies: Win32 + WIC + Direct2D.
add_executable(PlainView WIN32
    src/common.cpp
    src/decode.cpp
    src/folder.cpp
    src/options.cpp
    src/register.cpp
    src/settings.cpp
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
    psapi
    uuid
    advapi32
)

set_target_properties(PlainView PROPERTIES
    OUTPUT_NAME "PlainView"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
