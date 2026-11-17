# The chip: Qt-free core. Public headers in include/vyrn/ use only the standard library.
# Source files are globbed so new files are picked up without editing this file
# (re-run `cmake -S . -B build` or just build: CONFIGURE_DEPENDS re-globs).

# ---- vyrn_platform: everything that differs between Linux, macOS and Windows (docs/dev/platforms.md) -------------
# src/platform/<name>.cpp builds everywhere, *_posix.cpp on Linux and macOS, *_linux.cpp / *_mac.cpp / *_win.cpp only
# on their own system. Standard library + system libraries only (no torch, no Qt).
file(GLOB VYRN_PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/*.cpp")
if(WIN32)
  list(FILTER VYRN_PLATFORM_SOURCES EXCLUDE REGEX "_(posix|linux|mac)\\.cpp$")
elseif(APPLE)
  list(FILTER VYRN_PLATFORM_SOURCES EXCLUDE REGEX "_(linux|win)\\.cpp$")
else()
  list(FILTER VYRN_PLATFORM_SOURCES EXCLUDE REGEX "_(mac|win)\\.cpp$")
endif()
add_library(vyrn_platform STATIC ${VYRN_PLATFORM_SOURCES})
set_target_properties(vyrn_platform PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(vyrn_platform
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vyrn_platform PUBLIC Threads::Threads PRIVATE vyrn_warnings)
if(WIN32)
  # user32: SendInput (release_all_keys); ws2_32: sockets; bcrypt: BCryptGenRandom (the worker token);
  # iphlpapi: GetAdaptersAddresses (Vyrn Link's addresses).
  target_link_libraries(vyrn_platform PUBLIC user32 ws2_32 bcrypt iphlpapi)
elseif(APPLE)
  # CoreGraphics (CGEventPost for release_all_keys) comes with ApplicationServices.
  target_link_libraries(vyrn_platform PUBLIC "-framework ApplicationServices")
endif()

# ---- vyrn_ipc: frame codec + Python worker process (no torch) -----------------------------------
file(GLOB VYRN_IPC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ipc/*.cpp")
if(VYRN_IPC_SOURCES)
  add_library(vyrn_ipc STATIC ${VYRN_IPC_SOURCES})
  set_target_properties(vyrn_ipc PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  target_include_directories(vyrn_ipc
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(vyrn_ipc PUBLIC nlohmann_json::nlohmann_json Threads::Threads vyrn_platform PRIVATE vyrn_warnings)
else()
  message(STATUS "vyrn_ipc: no sources yet")
endif()

# ---- vyrn_link: Vyrn Link, the WebSocket server games and mods connect to (no torch) ---------------
# docs/user/vyrn-link.md (protocol v1), docs/dev/link.md. Standard library + sockets (platform/net.h) + nlohmann_json.
file(GLOB VYRN_LINK_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/link/*.cpp")
if(VYRN_LINK_SOURCES)
  add_library(vyrn_link STATIC ${VYRN_LINK_SOURCES})
  set_target_properties(vyrn_link PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  target_include_directories(vyrn_link
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(vyrn_link PRIVATE VYRN_VERSION="${PROJECT_VERSION}")
  target_link_libraries(vyrn_link PUBLIC nlohmann_json::nlohmann_json Threads::Threads vyrn_platform PRIVATE vyrn_warnings)
endif()

# ---- vyrn_core: brain (libtorch), runners, session ------------------------------------------------
file(GLOB VYRN_CORE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/brain/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/run/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/util/*.cpp")
if(VYRN_CORE_SOURCES)
  add_library(vyrn_core STATIC ${VYRN_CORE_SOURCES})
  set_target_properties(vyrn_core PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  target_include_directories(vyrn_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(vyrn_core PRIVATE
    VYRN_SOURCE_DIR="${VYRN_SOURCE_DIR}"
    VYRN_VERSION="${PROJECT_VERSION}")
  # torch is PRIVATE: executables link its libraries but never see its headers or flags.
  target_link_libraries(vyrn_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads vyrn_platform
                                  PRIVATE ${TORCH_LIBRARIES} vyrn_warnings)
  if(TARGET vyrn_ipc)
    target_link_libraries(vyrn_core PUBLIC vyrn_ipc)
  endif()
  if(TARGET vyrn_link)
    target_link_libraries(vyrn_core PUBLIC vyrn_link)
  endif()
  # Parsing torch/torch.h costs ~20 s per file on a Pi 5: precompile it once.
  option(VYRN_PCH "Precompile torch headers for vyrn_core" ON)
  if(VYRN_PCH)
    target_precompile_headers(vyrn_core PRIVATE <torch/torch.h>)
  endif()
else()
  message(STATUS "vyrn_core: no sources yet")
endif()

# ---- vyrn_project: projects, templates, settings, brains, locks, PNG (the app and the MCP server use it) -------
if(VYRN_WITH_APP_PARTS AND TARGET vyrn_core)
  file(GLOB VYRN_PROJECT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/project/*.cpp")
  if(VYRN_PROJECT_SOURCES)
    find_package(ZLIB REQUIRED)
    add_library(vyrn_project STATIC ${VYRN_PROJECT_SOURCES})
    set_target_properties(vyrn_project PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_include_directories(vyrn_project
      PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
      PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(vyrn_project PUBLIC vyrn_core PRIVATE ZLIB::ZLIB nlohmann_json::nlohmann_json vyrn_warnings)
  endif()
endif()

# ---- vyrn_package: .vrl / .vrlp sharing files (docs/dev/package.md). Converts weights, so it uses torch. ---------
if(VYRN_WITH_APP_PARTS AND TARGET vyrn_project)
  file(GLOB VYRN_PACKAGE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/package/*.cpp")
  if(VYRN_PACKAGE_SOURCES)
    add_library(vyrn_package STATIC ${VYRN_PACKAGE_SOURCES})
    set_target_properties(vyrn_package PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_include_directories(vyrn_package
      PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
      PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    # VYRN_BUILD_WHEELS_DIR: where this build makes the vyrn wheel (the top-level CMakeLists.txt); find_wheel() looks
    # there when the data folder is the source tree.
    target_compile_definitions(vyrn_package PRIVATE
      VYRN_SOURCE_DIR="${VYRN_SOURCE_DIR}"
      VYRN_BUILD_WHEELS_DIR="${VYRN_WHEELS_DIR}"
      VYRN_VERSION="${PROJECT_VERSION}")
    find_package(ZLIB REQUIRED)
    target_link_libraries(vyrn_package PUBLIC vyrn_project vyrn_core
                                       PRIVATE ${TORCH_LIBRARIES} ZLIB::ZLIB nlohmann_json::nlohmann_json vyrn_warnings)
    if(VYRN_PCH)
      target_precompile_headers(vyrn_package PRIVATE <torch/torch.h>)
    endif()
  endif()
endif()
