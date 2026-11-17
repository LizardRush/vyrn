cmake_minimum_required(VERSION 3.25)
project(vyrn VERSION 0.1.0 DESCRIPTION "Teach an AI to play games with three small Python scripts" LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# app/ and cli/ use C++20; vyrn_ipc and vyrn_core set C++17 themselves (libtorch's tested standard).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CTest)

# Linux, macOS and Windows each build natively (docs/dev/platforms.md). Linux-only parts: vyrn-focus, the panic
# script, the menu entry and the MIME types.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(VYRN_LINUX ON)
else()
  set(VYRN_LINUX OFF)
endif()

option(VYRN_GUI "Build the Qt app (vyrn)" ON)
option(VYRN_FOCUS "Build the Wayland focus helper (vyrn-focus; Linux only)" ${VYRN_LINUX})
# The app, the project API (vyrn_project), sharing files (vyrn_package) and the MCP server. OFF builds only the
# core, vyrn-cli's check/train/watch/bench and the core's tests.
option(VYRN_WITH_APP_PARTS "Build the app, vyrn_project, vyrn_package and the MCP server" ON)
# nlohmann_json, GoogleTest and zlib: when find_package() can't find one, download a pinned release (checked by its
# SHA-256) and build it with Vyrn (cmake/VyrnDependencies.cmake). On by default on macOS and Windows; Linux uses the
# system packages (install.sh --deps). Qt, QScintilla and libtorch are never downloaded by CMake.
if(VYRN_LINUX)
  set(_vyrn_fetch_default OFF)
else()
  set(_vyrn_fetch_default ON)
endif()
option(VYRN_FETCH_DEPS "Download nlohmann_json, GoogleTest and zlib when they are not installed" ${_vyrn_fetch_default})
include(VyrnDependencies)

if(MSVC)
  # Source files are UTF-8; standard C++ (two-phase lookup, __cplusplus), C++ exceptions. windows.h without min/max
  # macros and the rarely used parts. (The C++-only switches stay off C files, such as a downloaded zlib.)
  add_compile_options(/utf-8 "$<$<COMPILE_LANGUAGE:CXX>:/permissive-;/Zc:__cplusplus;/EHsc>")
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
elseif(WIN32)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

find_package(Threads REQUIRED)
# Linux: the worker runs with /usr/bin/python3 (apt numpy, Pillow, evdev, pytest); prefer it over other python3s on
# PATH. macOS and Windows: whatever find_package(Python3) finds (Homebrew's python3, python.exe / the py launcher).
if(VYRN_LINUX AND NOT DEFINED Python3_EXECUTABLE AND EXISTS /usr/bin/python3)
  set(Python3_EXECUTABLE /usr/bin/python3 CACHE FILEPATH "Python for the worker, tests and resource generation")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# libtorch: Torch_DIR or CMAKE_PREFIX_PATH, then where install-mac.sh / install-windows.ps1 put it.
find_package(Torch ${VYRN_TORCH_FIND_ARGS})
if(NOT Torch_FOUND)
  vyrn_torch_missing()   # stops, saying what to run
endif()
if(VYRN_FETCH_DEPS)
  vyrn_fetch_missing_deps()   # nlohmann_json, zlib, GoogleTest: installed ones first, else downloaded
else()
  find_package(nlohmann_json 3.11 REQUIRED)
endif()
if(VYRN_GUI)
  find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets)
  find_package(QScintilla REQUIRED)
endif()

# Warning flags for our own code only (third-party headers come in as SYSTEM includes).
add_library(vyrn_warnings INTERFACE)
if(MSVC)
  # /wd4100: unreferenced parameter (like -Wno-unused-parameter); /wd4996: POSIX names and "unsafe" CRT calls.
  target_compile_options(vyrn_warnings INTERFACE /W4 /wd4100 /wd4996)
else()
  target_compile_options(vyrn_warnings INTERFACE -Wall -Wextra -Wno-unused-parameter)
endif()

# Windows programs run with UTF-8 as their ANSI code page (cmake/utf8.manifest), so narrow paths, getenv() and
# std::filesystem's std::string conversions are UTF-8 there too, like on Linux and macOS.
function(vyrn_executable target)
  if(WIN32)
    target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/cmake/utf8.manifest")
  endif()
endfunction()

# Where the source tree is, so dev builds find python/, templates/ and docs/ without installing.
set(VYRN_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# ---- the vyrn Python package as a wheel (docs/dev/standalone.md section 4) ----------------------------------------
# Compile puts it next to a compiled AI. python/tools/make_wheel.py builds it at build time (standard library only,
# offline) into <build>/wheels; vyrn_install_data() installs it into <data>/wheels. A build that runs from its source
# tree finds it in <build>/wheels (vyrn::find_wheel(), core/src/package/standalone.cpp: VYRN_BUILD_WHEELS_DIR), so
# nothing is ever written into the source tree. The script is globbed with CONFIGURE_DEPENDS: once it appears, the
# next build configures again and makes the wheel; until then this is skipped (Compile then needs --no-wheel).
set(VYRN_WHEELS_DIR "${PROJECT_BINARY_DIR}/wheels")
set(VYRN_WHEEL_FILE "${VYRN_WHEELS_DIR}/vyrn-${PROJECT_VERSION}-py3-none-any.whl")
file(GLOB VYRN_MAKE_WHEEL CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/python/tools/make_wheel.py")
if(VYRN_MAKE_WHEEL)
  file(GLOB_RECURSE _vyrn_wheel_inputs CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/python/vyrn/*.py" "${CMAKE_CURRENT_SOURCE_DIR}/python/vyrn/*.in"
       "${CMAKE_CURRENT_SOURCE_DIR}/python/pyproject.toml")
  add_custom_command(
    OUTPUT "${VYRN_WHEEL_FILE}"
    COMMAND "${Python3_EXECUTABLE}" -B "${VYRN_MAKE_WHEEL}" --out-dir "${VYRN_WHEELS_DIR}" --version "${PROJECT_VERSION}"
    DEPENDS "${VYRN_MAKE_WHEEL}" ${_vyrn_wheel_inputs}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building the vyrn Python package (vyrn-${PROJECT_VERSION}-py3-none-any.whl)"
    VERBATIM)
  add_custom_target(vyrn_wheel ALL DEPENDS "${VYRN_WHEEL_FILE}")
else()
  message(STATUS "vyrn wheel: skipped until python/tools/make_wheel.py exists")
endif()

# ---- install layout per system (docs/dev/platforms.md) -----------------------------------------
# Linux:   <prefix>/bin + <prefix>/share/vyrn (install.sh: ~/.local), plus the menu entry and file types.
# Windows: one folder: <prefix>\bin (the programs and every DLL they need; install-windows.ps1 runs windeployqt for
#          Qt's) + <prefix>\share\vyrn (install-windows.ps1: %LOCALAPPDATA%\Programs\Vyrn).
# macOS:   everything inside <prefix>/Vyrn.app (install-mac.sh: ~/Applications). app/CMakeLists.txt installs the
#          bundle and its data (Contents/Resources/vyrn); vyrn-cli and vyrn-mcp go next to the app, in
#          Contents/MacOS: CMAKE_INSTALL_BINDIR points there, so nothing lands outside the bundle. Without the app
#          (VYRN_GUI=OFF) it's <prefix>/bin + <prefix>/share/vyrn, as on Linux.
set(VYRN_MAC_BUNDLE OFF)
if(APPLE AND VYRN_GUI AND VYRN_WITH_APP_PARTS)
  set(VYRN_MAC_BUNDLE ON)
  set(VYRN_APP_BUNDLE_NAME "Vyrn")   # Vyrn.app: the app target's OUTPUT_NAME
  set(CMAKE_INSTALL_BINDIR "${VYRN_APP_BUNDLE_NAME}.app/Contents/MacOS")
  # Installed programs find Qt, QScintilla and libtorch in Contents/Frameworks once macdeployqt copied them there,
  # and where they were linked from (Homebrew, ~/Library/Vyrn/libtorch) until then.
  set(CMAKE_INSTALL_RPATH "@executable_path/../Frameworks")
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
endif()

# Installs the data the app and vyrn-cli read at run time into <destination> (relative to the prefix): the Python
# library and worker, the templates, the user docs, Vyrn Link's reference tools (docs/dev/link.md: the fake game
# the link-runner template plays, and the stand-in server for mod authors) and the vyrn package's wheel (wheels/).
# Used below for <prefix>/share/vyrn; app/CMakeLists.txt may call it for Vyrn.app/Contents/Resources/vyrn.
function(vyrn_install_data destination)
  set(src "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
  if(EXISTS "${src}/python")
    install(DIRECTORY "${src}/python/" DESTINATION "${destination}/python"
            PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE)
  endif()
  if(EXISTS "${src}/templates")
    install(DIRECTORY "${src}/templates/" DESTINATION "${destination}/templates"
            PATTERN "__pycache__" EXCLUDE PATTERN "brains" EXCLUDE PATTERN "runs" EXCLUDE)
  endif()
  if(EXISTS "${src}/docs/user")
    install(DIRECTORY "${src}/docs/user/" DESTINATION "${destination}/docs")
  endif()
  if(EXISTS "${src}/tools/link/link_fake_game.py")
    install(PROGRAMS "${src}/tools/link/link_fake_game.py" "${src}/tools/link/link_test_server.py"
            DESTINATION "${destination}/tools/link")
    install(FILES "${src}/tools/link/wslite.py" DESTINATION "${destination}/tools/link")
  endif()
  # Which commit this copy was built from (written by the get-* scripts; absent in a plain checkout): the app's update
  # check compares it with GitHub's.
  if(EXISTS "${src}/.vyrn-commit")
    install(FILES "${src}/.vyrn-commit" DESTINATION "${destination}" RENAME commit)
  endif()
  # The vyrn package's wheel, made at build time (above): Compile copies it next to a compiled AI. OPTIONAL: a
  # build without python/tools/make_wheel.py has none.
  if(VYRN_WHEEL_FILE)
    install(FILES "${VYRN_WHEEL_FILE}" DESTINATION "${destination}/wheels" OPTIONAL)
  endif()
endfunction()

add_subdirectory(tools/hello)
# cli/mcp before cli: vyrn-cli links vyrn_mcp (and vyrn_package) when those targets exist.
set(VYRN_SUBDIRS core cli/mcp cli app)
if(VYRN_LINUX)
  list(APPEND VYRN_SUBDIRS focus)   # vyrn-focus: Wayland (wlr-foreign-toplevel) only
endif()
list(APPEND VYRN_SUBDIRS tests)
foreach(dir IN LISTS VYRN_SUBDIRS)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt")
    add_subdirectory(${dir})
  endif()
endforeach()

# ---- macOS: the app bundle -------------------------------------------------------------------
if(VYRN_MAC_BUNDLE)
  if(TARGET vyrn)
    get_target_property(_vyrn_bundle vyrn OUTPUT_NAME)
    if(NOT _vyrn_bundle)
      set(_vyrn_bundle vyrn)
    endif()
    if(NOT _vyrn_bundle STREQUAL VYRN_APP_BUNDLE_NAME)
      message(WARNING "The app installs as ${_vyrn_bundle}.app, but vyrn-cli and vyrn-mcp go into "
                      "${VYRN_APP_BUNDLE_NAME}.app/Contents/MacOS: set the app's OUTPUT_NAME to ${VYRN_APP_BUNDLE_NAME}.")
    endif()
  else()
    message(WARNING "No app target (vyrn): vyrn-cli and vyrn-mcp still install into ${CMAKE_INSTALL_BINDIR}.")
  endif()
endif()

# ---- Windows: the DLLs the programs need, next to them ----------------------------------------------------------
if(WIN32)
  file(GLOB _vyrn_torch_dlls "${TORCH_INSTALL_PREFIX}/lib/*.dll")
  if(_vyrn_torch_dlls)
    install(FILES ${_vyrn_torch_dlls} DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
  if(VYRN_GUI AND QScintilla_DLL)
    install(FILES "${QScintilla_DLL}" DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
endif()

# ---- data files shared by the app and the CLI --------------------------------------------------
# Inside Vyrn.app they come from app/CMakeLists.txt (Contents/Resources/vyrn), so they are not installed here too.
set(VYRN_DATA_DEST "${CMAKE_INSTALL_DATADIR}/vyrn")
if(NOT VYRN_MAC_BUNDLE)
  vyrn_install_data("${VYRN_DATA_DEST}")
endif()
# Linux only: the panic script (labwc keybinds run it), the menu entry, the icon theme and the MIME types. macOS and
# Windows get their panic hotkey, app bundle / Start menu entry and file types from the app's own packaging.
if(VYRN_LINUX)
  install(PROGRAMS scripts/vyrn-panic DESTINATION ${CMAKE_INSTALL_BINDIR})
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/vyrn.desktop")
    install(FILES packaging/vyrn.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
  endif()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/vyrn.png")
    install(FILES packaging/vyrn.png DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps")
  endif()
  # .vrlp / .vrl files (docs/dev/package.md section 9); install.sh runs update-mime-database afterwards.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/vyrn-mime.xml")
    install(FILES packaging/vyrn-mime.xml DESTINATION "${CMAKE_INSTALL_DATADIR}/mime/packages" RENAME vyrn.xml)
    foreach(kind project brain)
      install(FILES packaging/vyrn.png DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/mimetypes"
              RENAME application-x-vyrn-${kind}.png)
    endforeach()
  endif()
endif()
