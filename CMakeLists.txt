cmake_minimum_required(VERSION 3.21)
project(VimuScopeX VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(WIN32)
    # M_PI and friends from <cmath> on every Windows toolchain.
    add_compile_definitions(_USE_MATH_DEFINES)
endif()
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
set(CMAKE_AUTORCC ON)

# Font Awesome is vendored: resources/fonts/fontawesome-solid.otf plus the
# committed src/fa_codepoints.h. Nothing here depends on an unpacked Font
# Awesome release, and the icons work on any machine that can build the project.
# See resources/fonts/FONTAWESOME-LICENSE.txt.

# Spectrum's FFT: FFTW3, a real, well-tested library rather than a
# hand-rolled implementation, found via the fftw3.pc file the package
# itself installs (pacman -S fftw on Arch).
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW3 REQUIRED IMPORTED_TARGET fftw3)
# Protocol decoding (CAN/CAN FD, LIN, K-Line) is optional: without
# libsigrokdecode the app still builds and the Decoding page says what to
# install.
pkg_check_modules(SRD IMPORTED_TARGET libsigrokdecode)

# ---------------------------------------------------------------------------
# Vimu SDK location
#
#   -DVIMU_SDK_DIR=/path/to/VmMsoSDK-master   -> searched recursively
#   -DVIMU_SDK_LIB=/full/path/to/libvmmultmso.so  -> used verbatim
# ---------------------------------------------------------------------------
set(VIMU_SDK_DIR "" CACHE PATH     "Root of the extracted VmMsoSDK checkout")
set(VIMU_SDK_LIB "" CACHE FILEPATH "Explicit path to the vmmultmso library")

# None given: Vimu's own SDK (MIT, github.com/vimu-tech/VmMsoSDK), fetched
# once into the build tree, so a clean checkout configures with no setup.
if(NOT VIMU_SDK_DIR AND NOT VIMU_SDK_LIB AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib")
    include(FetchContent)
    FetchContent_Declare(vimusdk
        GIT_REPOSITORY https://github.com/vimu-tech/VmMsoSDK.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  no-cmake-here)        # populate only, nothing to add_subdirectory
    message(STATUS "Fetching the Vimu SDK from github.com/vimu-tech/VmMsoSDK ...")
    FetchContent_MakeAvailable(vimusdk)
    set(VIMU_SDK_DIR "${vimusdk_SOURCE_DIR}")
endif()

set(_vimu_lib "${VIMU_SDK_LIB}")

if(NOT _vimu_lib)
    set(_roots "${CMAKE_CURRENT_SOURCE_DIR}/lib")
    if(VIMU_SDK_DIR)
        list(APPEND _roots "${VIMU_SDK_DIR}")
    endif()

    set(_candidates "")
    foreach(_root IN LISTS _roots)
        if(EXISTS "${_root}")
            if(WIN32 AND MINGW)
                # MinGW links the SDK's DLL directly; no MSVC import library
                # needed (its C exports are plain extern "C").
                file(GLOB_RECURSE _hit "${_root}/vmmultmso.dll")
            elseif(WIN32)
                file(GLOB_RECURSE _hit
                     "${_root}/libvmmultmso.dll.a"
                     "${_root}/vmmultmso.lib")
            else()
                file(GLOB_RECURSE _hit
                     "${_root}/libvmmultmso.so"
                     "${_root}/libvmmultmso.so.*")
            endif()
            list(APPEND _candidates ${_hit})
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _candidates)

    if(NOT WIN32 AND _candidates)
        # The SDK ships the same library for x86-64, armhf, armel, aarch64 and
        # Raspberry Pi. Drop everything that cannot run on this machine.
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
            list(FILTER _candidates EXCLUDE REGEX "aarch64|arm-linux|arm64|RaspberryPi|/X86/")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
            list(FILTER _candidates INCLUDE REGEX "aarch64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
            list(FILTER _candidates INCLUDE REGEX "arm-linux|RaspberryPi")
        endif()

        # Release over Debug.
        set(_filtered ${_candidates})
        list(FILTER _filtered EXCLUDE REGEX "/Debug/")
        if(_filtered)
            set(_candidates ${_filtered})
        endif()

        # Newest distro build available.
        foreach(_prefer IN ITEMS "Ubuntu22" "Ubuntu20")
            set(_filtered ${_candidates})
            list(FILTER _filtered INCLUDE REGEX "${_prefer}")
            if(_filtered)
                set(_candidates ${_filtered})
                break()
            endif()
        endforeach()

        # Unversioned name if it is present, versioned file otherwise.
        set(_filtered ${_candidates})
        list(FILTER _filtered INCLUDE REGEX "libvmmultmso\\.so$")
        if(_filtered)
            set(_candidates ${_filtered})
        endif()
    endif()

    if(WIN32 AND _candidates)
        # SharedLibrary/Windows/{X86,X64}/{Debug,Release}: match this
        # build's architecture, and Release over Debug.
        set(_filtered ${_candidates})
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            list(FILTER _filtered INCLUDE REGEX "/[Xx]64/")
        else()
            list(FILTER _filtered INCLUDE REGEX "/[Xx]86/")
        endif()
        if(_filtered)
            set(_candidates ${_filtered})
        endif()
        set(_filtered ${_candidates})
        list(FILTER _filtered EXCLUDE REGEX "/[Dd]ebug/")
        if(_filtered)
            set(_candidates ${_filtered})
        endif()
    endif()

    list(LENGTH _candidates _count)
    if(_count GREATER 0)
        list(GET _candidates 0 _vimu_lib)
        if(_count GREATER 1)
            message(STATUS "Several vmmultmso libraries survived filtering; using the first:")
            foreach(_c IN LISTS _candidates)
                message(STATUS "    ${_c}")
            endforeach()
            message(STATUS "Override with -DVIMU_SDK_LIB=<path> if that is the wrong one.")
        endif()
    endif()
endif()

if(NOT _vimu_lib)
    # Is only the single-device library shipped for this platform?
    set(_single "")
    if(VIMU_SDK_DIR AND NOT WIN32)
        file(GLOB_RECURSE _single "${VIMU_SDK_DIR}/libvmmso.so" "${VIMU_SDK_DIR}/libvmmso.so.*")
    endif()
    if(_single)
        message(FATAL_ERROR
            "Found the SINGLE-device library but not the multi-device one:\n"
            "    ${_single}\n"
            "This project is written against VmMultMsoLib.h (multi-device API).\n"
            "The single-device API is the same call set without the dev_id argument,\n"
            "so porting is mechanical, but it needs VmMsoLib.h instead.")
    endif()
    message(FATAL_ERROR
        "Could not find the Vimu shared library.\n"
        "Locate it with:\n"
        "    find /path/to/VmMsoSDK -iname '*vmmultmso*'\n"
        "then configure with:\n"
        "    cmake -S . -B build -DVIMU_SDK_DIR=/path/to/VmMsoSDK\n"
        "or point at the file directly with -DVIMU_SDK_LIB=/full/path/libvmmultmso.so")
endif()

get_filename_component(_vimu_libdir "${_vimu_lib}" DIRECTORY)
message(STATUS "Vimu library : ${_vimu_lib}")

add_executable(VimuScopeX
    src/main.cpp
    src/mainwindow.cpp
    src/cardbutton.cpp
    src/scopeview.cpp
    src/navigatorview.cpp
    src/probes.cpp
    src/probedialog.cpp
    src/icons.cpp
    src/fonts.cpp
    src/acquisitionmanager.cpp
    src/streamengine.cpp
    src/recordwriter.cpp
    src/annotationbox.cpp
    src/spectrumview.cpp
    src/xyview.cpp
    src/decoderengine.cpp
    src/remoteserver.cpp
    src/liveworker.cpp
    src/panelcontrols.cpp
    src/vsxdialog.cpp)

target_include_directories(VimuScopeX PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Application artwork, always present.
target_sources(VimuScopeX PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources/app.qrc)
if(WIN32)
    # The .exe's own icon (Explorer, shortcuts, taskbar before Qt is up).
    enable_language(RC)
    target_sources(VimuScopeX PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/VimuScopeX.rc)
endif()

# If the SDK is available, prefer its copy of the header so it can never drift
# from the shared library version.
if(VIMU_SDK_DIR AND EXISTS "${VIMU_SDK_DIR}/SharedLibrary/VmMultMsoLib.h")
    target_include_directories(VimuScopeX BEFORE PRIVATE "${VIMU_SDK_DIR}/SharedLibrary")
    message(STATUS "Vimu header  : ${VIMU_SDK_DIR}/SharedLibrary/VmMultMsoLib.h")
endif()
target_link_libraries(VimuScopeX PRIVATE Qt6::Widgets Qt6::Network "${_vimu_lib}" PkgConfig::FFTW3)

# Readable function names in the "[stall-trace]" stack traces (perftrace.h).
if(UNIX AND NOT APPLE)
    target_link_options(VimuScopeX PRIVATE -rdynamic)
endif()

# Optional: the startup animation and the plug in / out sounds. Without Qt
# Multimedia the app builds as before, with a still splash and no sounds.
#
# A failed search is cached (Qt6Multimedia_DIR-NOTFOUND): a build configured
# before the package was installed kept building without it, silently. Forget
# a cached miss so every configure looks again, and search without QUIET so
# a miss prints CMake's reason.
if(DEFINED Qt6Multimedia_DIR AND NOT Qt6Multimedia_DIR)
    unset(Qt6Multimedia_DIR CACHE)
endif()
# Qt SVG (optional): the Hugeicons in resources/icons. Without it the app
# keeps the Font Awesome glyphs.
if(DEFINED Qt6Svg_DIR AND NOT Qt6Svg_DIR)
    unset(Qt6Svg_DIR CACHE)
endif()
find_package(Qt6 COMPONENTS Svg)
if(Qt6Svg_FOUND)
    target_link_libraries(VimuScopeX PRIVATE Qt6::Svg)
    target_compile_definitions(VimuScopeX PRIVATE VSX_HAVE_SVG)
    message(STATUS "Qt SVG ${Qt6Svg_VERSION}: Hugeicons enabled")
else()
    message(WARNING "Qt SVG not found: building with the Font Awesome icons. "
                    "Install qt6-svg (Arch) / mingw-w64-ucrt-x86_64-qt6-svg (MSYS2) for the new ones.")
endif()

find_package(Qt6 COMPONENTS Multimedia)
if(Qt6Multimedia_FOUND)
    target_link_libraries(VimuScopeX PRIVATE Qt6::Multimedia)
    target_compile_definitions(VimuScopeX PRIVATE VSX_HAVE_MULTIMEDIA=1)
    message(STATUS "Qt Multimedia ${Qt6Multimedia_VERSION}: startup animation and sounds enabled")
else()
    message(WARNING "Qt Multimedia not found: building with a still splash and no sounds. "
                    "Arch: pacman -S qt6-multimedia qt6-multimedia-ffmpeg, then rebuild.")
endif()
if(SRD_FOUND)
    target_link_libraries(VimuScopeX PRIVATE PkgConfig::SRD)
    target_compile_definitions(VimuScopeX PRIVATE VSX_HAVE_SRD=1)
    message(STATUS "libsigrokdecode ${SRD_VERSION}: protocol decoding enabled")
else()
    message(STATUS "libsigrokdecode not found: protocol decoding disabled")
endif()
# Our own sigrok decoders (K-Line) are loaded from a 'decoders' folder next
# to the executable; refreshed on every build.
add_custom_command(TARGET VimuScopeX POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/decoders"
            "$<TARGET_FILE_DIR:VimuScopeX>/decoders")

if(UNIX AND NOT APPLE)
    # libvmmultmso.so pulls in libvmsignal.so from the same directory. A
    # DT_RUNPATH (the modern default) is NOT used when the loader resolves a
    # dependency's own dependencies, so force the old DT_RPATH, which is.
    set_target_properties(VimuScopeX PROPERTIES
        BUILD_RPATH   "${_vimu_libdir}"
        INSTALL_RPATH "${_vimu_libdir}")
    target_link_options(VimuScopeX PRIVATE "-Wl,--disable-new-dtags")
endif()

if(WIN32)
    # A normal windowed app: no console window behind it.
    set_target_properties(VimuScopeX PROPERTIES WIN32_EXECUTABLE ON)
    find_file(VIMU_RUNTIME_DLL
        NAMES libvmmultmso.dll vmmultmso.dll
        HINTS "${_vimu_libdir}" "${_vimu_libdir}/../bin" "${CMAKE_CURRENT_SOURCE_DIR}/bin"
        NO_DEFAULT_PATH)
    if(VIMU_RUNTIME_DLL)
        add_custom_command(TARGET VimuScopeX POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${VIMU_RUNTIME_DLL}" "$<TARGET_FILE_DIR:VimuScopeX>")
    endif()
endif()

# Diagnostic tool: sweeps Capture() masks with a clean SDK session per attempt.
add_executable(maskprobe EXCLUDE_FROM_ALL tools/maskprobe.cpp)
target_include_directories(maskprobe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(VIMU_SDK_DIR AND EXISTS "${VIMU_SDK_DIR}/SharedLibrary/VmMultMsoLib.h")
    target_include_directories(maskprobe BEFORE PRIVATE "${VIMU_SDK_DIR}/SharedLibrary")
endif()
target_link_libraries(maskprobe PRIVATE "${_vimu_lib}")
if(UNIX AND NOT APPLE)
    set_target_properties(maskprobe PROPERTIES
        BUILD_RPATH   "${_vimu_libdir}"
        INSTALL_RPATH "${_vimu_libdir}")
    target_link_options(maskprobe PRIVATE "-Wl,--disable-new-dtags")
    find_package(Threads REQUIRED)
    target_link_libraries(maskprobe PRIVATE Threads::Threads)
endif()

install(TARGETS VimuScopeX RUNTIME DESTINATION bin)
