cmake_minimum_required(VERSION 3.20)
project(AudioClipEditor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8 /W3 /EHsc /permissive-)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

file(GLOB SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
)

# The resource compiler reads these, but a dependency scan only sees #includes: without
# this, a generator other than Visual Studio's (Ninja, say) wouldn't rebuild the
# resources when art/make_icon.py rewrites the icons.
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/resources.rc" PROPERTIES OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/app.ico;${CMAKE_SOURCE_DIR}/src/project.ico;${CMAKE_SOURCE_DIR}/src/app.manifest")

add_executable(AudioClipEditor WIN32 ${SOURCES}
    "${CMAKE_SOURCE_DIR}/src/app.manifest"
    "${CMAKE_SOURCE_DIR}/src/resources.rc"
)

target_link_libraries(AudioClipEditor PRIVATE
    comctl32        # common controls
    comdlg32        # open/save file dialogs
    gdi32           # GDI drawing
    gdiplus         # GDI+ (antialiased waveforms)
    ole32           # COM
    uuid
    mfplat          # Media Foundation core
    mfreadwrite     # Source Reader / Sink Writer
    mfuuid          # MF GUIDs
    ksuser          # WASAPI KS GUIDs
    winmm
    shell32         # known-folder lookup (default export directory)
    shlwapi
    avrt            # MMCSS (audio thread priority)
    propsys
    shcore          # GetDpiForMonitor (a dialog shown before the app has a window)
)

if(MSVC)
    # We embed our own manifest via resources.rc; stop the linker adding a second.
    target_link_options(AudioClipEditor PRIVATE "/MANIFEST:NO")
endif()

set_target_properties(AudioClipEditor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
)
