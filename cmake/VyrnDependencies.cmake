# Vyrn's libraries besides Qt and QScintilla (docs/dev/platforms.md).
#
# VYRN_TORCH_FIND_ARGS        libtorch comes from Torch_DIR or CMAKE_PREFIX_PATH. On macOS and Windows the install
# vyrn_torch_missing()        scripts unpack it into ~/Library/Vyrn/libtorch or %LOCALAPPDATA%\Vyrn\libtorch, which are
#                             searched after those two. When it's missing, configuring stops with what to run.
# vyrn_fetch_missing_deps()   VYRN_FETCH_DEPS (on by default on macOS and Windows): nlohmann_json, GoogleTest and zlib
#                             are looked for first (Homebrew, vcpkg, CMAKE_PREFIX_PATH...). Each one that isn't there is
#                             downloaded (a pinned release, checked against its SHA-256) and built along with Vyrn. The
#                             find_package() calls in the subfolders (core/ asks for ZLIB, tests/ for GTest) then get
#                             the downloaded copy, through CMAKE_FIND_PACKAGE_REDIRECTS_DIR. Nothing downloaded is
#                             installed: zlib and GoogleTest are linked statically, nlohmann_json is headers only.
#
# Qt, QScintilla and libtorch are never downloaded by CMake: they are big, and install-mac.sh / install-windows.ps1
# put them in place.
include_guard(GLOBAL)

set(VYRN_JSON_VERSION 3.11.3)
set(VYRN_JSON_URL "https://github.com/nlohmann/json/releases/download/v${VYRN_JSON_VERSION}/json.tar.xz")
set(VYRN_JSON_SHA256 d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
set(VYRN_GTEST_VERSION 1.17.0)
set(VYRN_GTEST_URL
    "https://github.com/google/googletest/releases/download/v${VYRN_GTEST_VERSION}/googletest-${VYRN_GTEST_VERSION}.tar.gz")
set(VYRN_GTEST_SHA256 65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c)
set(VYRN_ZLIB_VERSION 1.3.1)
set(VYRN_ZLIB_URL "https://github.com/madler/zlib/releases/download/v${VYRN_ZLIB_VERSION}/zlib-${VYRN_ZLIB_VERSION}.tar.gz")
set(VYRN_ZLIB_SHA256 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23)

# libtorch: the top-level CMakeLists.txt calls find_package(Torch ${VYRN_TORCH_FIND_ARGS}) (on Linux, plain
# find_package(Torch)), then vyrn_torch_missing() when it wasn't found.
set(VYRN_TORCH_FIND_ARGS "")
if(APPLE)
  set(VYRN_TORCH_FIND_ARGS HINTS "$ENV{HOME}/Library/Vyrn/libtorch")
elseif(WIN32 AND DEFINED ENV{LOCALAPPDATA})
  file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/Vyrn/libtorch" _vyrn_torch_hint)
  set(VYRN_TORCH_FIND_ARGS HINTS "${_vyrn_torch_hint}")
endif()

function(vyrn_torch_missing)
  if(APPLE)
    set(help "Run ./install-mac.sh --deps: it puts libtorch in ~/Library/Vyrn/libtorch, where this build looks for it.")
  elseif(WIN32)
    set(help "Run .\\install-windows.ps1 -Deps in PowerShell: it puts libtorch in %LOCALAPPDATA%\\Vyrn\\libtorch, where this build looks for it.")
  else()
    set(help "Run ./install.sh --deps (it installs the Debian package libtorch-dev).")
  endif()
  message(FATAL_ERROR
    "Vyrn needs libtorch (the C++ library of PyTorch), and CMake did not find it.\n"
    "${help}\n"
    "To use another copy, unpacked from https://pytorch.org, add this to the cmake command:\n"
    "  -DTorch_DIR=<libtorch folder>/share/cmake/Torch")
endfunction()

function(vyrn_fetch_missing_deps)
  include(FetchContent)

  # ---- nlohmann_json (headers only) ----
  find_package(nlohmann_json 3.11 QUIET)
  if(nlohmann_json_FOUND)
    message(STATUS "nlohmann_json ${nlohmann_json_VERSION}: installed (${nlohmann_json_DIR})")
  else()
    message(STATUS "nlohmann_json: not installed, using release ${VYRN_JSON_VERSION} downloaded from GitHub (VYRN_FETCH_DEPS)")
    FetchContent_Declare(nlohmann_json URL "${VYRN_JSON_URL}" URL_HASH SHA256=${VYRN_JSON_SHA256}
                         OVERRIDE_FIND_PACKAGE)
    FetchContent_MakeAvailable(nlohmann_json)
  endif()

  # ---- zlib: vyrn_project and vyrn_package read and write zip files ----
  if(VYRN_WITH_APP_PARTS)
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
      message(STATUS "zlib: installed (${ZLIB_LIBRARIES})")
    else()
      message(STATUS "zlib: not installed, using release ${VYRN_ZLIB_VERSION} downloaded from GitHub (VYRN_FETCH_DEPS)")
      set(ZLIB_BUILD_EXAMPLES OFF)
      set(SKIP_INSTALL_ALL ON)
      FetchContent_Declare(ZLIB URL "${VYRN_ZLIB_URL}" URL_HASH SHA256=${VYRN_ZLIB_SHA256} OVERRIDE_FIND_PACKAGE)
      FetchContent_MakeAvailable(ZLIB)
      set_property(TARGET zlib PROPERTY EXCLUDE_FROM_ALL ON)   # the shared library (zlib1.dll) isn't used
      add_library(ZLIB::ZLIB ALIAS zlibstatic)
    endif()
  endif()

  # ---- GoogleTest: only for the tests ----
  if(BUILD_TESTING)
    find_package(GTest QUIET)
    if(GTest_FOUND)
      message(STATUS "GoogleTest: installed")
    else()
      message(STATUS "GoogleTest: not installed, using release ${VYRN_GTEST_VERSION} downloaded from GitHub (VYRN_FETCH_DEPS)")
      set(INSTALL_GTEST OFF)
      set(BUILD_GMOCK OFF)
      set(gtest_force_shared_crt ON)   # Windows: the same C runtime (/MD) as the rest of the build
      FetchContent_Declare(googletest URL "${VYRN_GTEST_URL}" URL_HASH SHA256=${VYRN_GTEST_SHA256})
      FetchContent_MakeAvailable(googletest)
      # tests/ calls find_package(GTest): answer it with this copy (it defines GTest::gtest and GTest::gtest_main).
      file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/gtest-config.cmake"
           "# GoogleTest ${VYRN_GTEST_VERSION}, downloaded and built with Vyrn (cmake/VyrnDependencies.cmake)\n"
           "set(GTEST_LIBRARIES GTest::gtest)\n"
           "set(GTEST_MAIN_LIBRARIES GTest::gtest_main)\n"
           "set(GTEST_BOTH_LIBRARIES GTest::gtest GTest::gtest_main)\n")
      file(WRITE "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/gtest-config-version.cmake"
           "set(PACKAGE_VERSION ${VYRN_GTEST_VERSION})\n"
           "set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
           "if(PACKAGE_FIND_VERSION STREQUAL PACKAGE_VERSION)\n"
           "  set(PACKAGE_VERSION_EXACT TRUE)\n"
           "endif()\n")
    endif()
  endif()
endfunction()
