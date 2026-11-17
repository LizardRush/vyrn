# Tests. Labels: unit (fast), e2e (learning runs, minutes), slow (long learning runs), gui (app smoke test).
if(NOT BUILD_TESTING)
  return()
endif()
find_package(GTest REQUIRED)
# Python3_EXECUTABLE (the tests' interpreter, /usr/bin/python3 by default) comes from the top-level CMakeLists.txt.

# ---- C++ unit tests (GoogleTest): one binary per area, tests/cpp/<area>/*.cpp -> test_<area> ----
# ipc tests link only vyrn_ipc (no torch); other areas link vyrn_core (+ torch for internals).
include(GoogleTest)
file(GLOB VYRN_TEST_AREAS LIST_DIRECTORIES true CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cpp/*")
foreach(dir IN LISTS VYRN_TEST_AREAS)
  if(NOT IS_DIRECTORY "${dir}")
    continue()
  endif()
  get_filename_component(area "${dir}" NAME)
  set(VYRN_APP_TEST_AREAS project mcp package)   # built only with VYRN_WITH_APP_PARTS (see the top-level CMakeLists.txt)
  if(area IN_LIST VYRN_APP_TEST_AREAS AND NOT VYRN_WITH_APP_PARTS)
    continue()
  endif()
  file(GLOB srcs CONFIGURE_DEPENDS "${dir}/*.cpp")
  if(NOT srcs)
    continue()
  endif()
  if(area STREQUAL "project")
    if(NOT TARGET vyrn_project)
      continue()
    endif()
    set(libs vyrn_project)
  elseif(area STREQUAL "package")
    if(NOT TARGET vyrn_package)
      continue()
    endif()
    set(libs vyrn_package ${TORCH_LIBRARIES})
  elseif(area STREQUAL "mcp")
    if(NOT TARGET vyrn_mcp)
      continue()
    endif()
    set(libs vyrn_mcp)
  elseif(area STREQUAL "ipc")
    if(NOT TARGET vyrn_ipc)
      continue()
    endif()
    set(libs vyrn_ipc)
  elseif(area STREQUAL "link")
    if(NOT TARGET vyrn_link)
      continue()
    endif()
    set(libs vyrn_link)
  elseif(area STREQUAL "platform")   # core/src/platform/ alone (docs/dev/platforms.md)
    set(libs vyrn_platform)
  else()
    if(NOT TARGET vyrn_core)
      continue()
    endif()
    set(libs vyrn_core ${TORCH_LIBRARIES})
  endif()
  add_executable(test_${area} ${srcs})
  set_target_properties(test_${area} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  target_include_directories(test_${area} PRIVATE "${PROJECT_SOURCE_DIR}/core/src" "${CMAKE_CURRENT_SOURCE_DIR}/cpp")
  target_compile_definitions(test_${area} PRIVATE VYRN_SOURCE_DIR="${VYRN_SOURCE_DIR}")
  if(area STREQUAL "link")   # the reference game client, tools/link/link_fake_game.py, runs with this Python
    target_compile_definitions(test_${area} PRIVATE VYRN_PYTHON="${Python3_EXECUTABLE}")
  endif()
  target_link_libraries(test_${area} PRIVATE ${libs} GTest::gtest GTest::gtest_main vyrn_warnings)
  vyrn_executable(test_${area})
  gtest_discover_tests(test_${area} DISCOVERY_MODE PRE_TEST TEST_PREFIX "${area}."
                       PROPERTIES LABELS unit TIMEOUT 600)
endforeach()

# ---- lint: torch never reaches the Qt app or the public headers ---------------------------------
add_test(NAME lint_no_torch_in_app
         COMMAND "${CMAKE_COMMAND}" -DSRC=${PROJECT_SOURCE_DIR} -P "${CMAKE_CURRENT_SOURCE_DIR}/lint_headers.cmake")
set_tests_properties(lint_no_torch_in_app PROPERTIES LABELS unit)

# ---- Python side (library + worker), fast tests only -------------------------------------------
add_test(NAME python_unit
         COMMAND "${Python3_EXECUTABLE}" -B -m pytest "${PROJECT_SOURCE_DIR}/tests/python" -q -m "not slow" -p no:cacheprovider
         WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
set_tests_properties(python_unit PROPERTIES LABELS unit TIMEOUT 600)
add_test(NAME python_slow
         COMMAND "${Python3_EXECUTABLE}" -B -m pytest "${PROJECT_SOURCE_DIR}/tests/python" -q -m slow -p no:cacheprovider
         WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
set_tests_properties(python_slow PROPERTIES LABELS slow TIMEOUT 1800)

# ---- standalone runtime (vyrn.runtime): golden parity with the C++ brain (docs/dev/standalone.md section 5) --
# A fixture pair: standalone_golden (C++) writes .vrl files, inputs, Q-values and to_frame() outputs; the pytest
# loads them with the numpy runtime and needs the same bytes and Q-values within 1e-4.
if(VYRN_WITH_APP_PARTS AND TARGET vyrn_package)
  add_executable(standalone_golden golden/standalone_golden.cpp cpp/package/helpers.cpp)
  set_target_properties(standalone_golden PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  target_include_directories(standalone_golden PRIVATE "${PROJECT_SOURCE_DIR}/core/src" "${CMAKE_CURRENT_SOURCE_DIR}/cpp"
                             "${CMAKE_CURRENT_SOURCE_DIR}/cpp/package")
  target_link_libraries(standalone_golden PRIVATE vyrn_package ${TORCH_LIBRARIES} GTest::gtest vyrn_warnings)
  vyrn_executable(standalone_golden)
  set(VYRN_GOLDEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/standalone_golden_data")
  add_test(NAME standalone_golden_fixtures COMMAND standalone_golden "${VYRN_GOLDEN_DIR}")
  set_tests_properties(standalone_golden_fixtures PROPERTIES LABELS unit TIMEOUT 300
                       FIXTURES_SETUP standalone_golden)
  add_test(NAME python_standalone_golden
           COMMAND "${Python3_EXECUTABLE}" -B -m pytest "${PROJECT_SOURCE_DIR}/tests/python/test_runtime_golden.py"
                   -q -p no:cacheprovider
           WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(python_standalone_golden PROPERTIES LABELS unit TIMEOUT 300
                       FIXTURES_REQUIRED standalone_golden ENVIRONMENT "VYRN_GOLDEN_DIR=${VYRN_GOLDEN_DIR}")
endif()

# ---- end-to-end learning (needs vyrn-cli) -----------------------------------------------------
if(TARGET vyrn-cli AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_learning.py")
  foreach(world catch_numbers catch_pictures cartpole)
    add_test(NAME e2e_${world}
             COMMAND "${Python3_EXECUTABLE}" -B -m pytest "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_learning.py"
                     -q -k ${world} -p no:cacheprovider
             WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(e2e_${world} PROPERTIES LABELS e2e TIMEOUT 3600
                         ENVIRONMENT "VYRN_CLI=$<TARGET_FILE:vyrn-cli>;VYRN_DATA=${PROJECT_SOURCE_DIR}")
  endforeach()
endif()
# Ctrl+C / SIGTERM while vyrn-cli trains (and prints): it saves the brain and exits 0, 13 times (half a minute).
if(TARGET vyrn-cli AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_cli_signals.py")
  add_test(NAME e2e_cli_signals
           COMMAND "${Python3_EXECUTABLE}" -B -m pytest "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_cli_signals.py"
                   -q -p no:cacheprovider
           WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(e2e_cli_signals PROPERTIES LABELS e2e TIMEOUT 1800
                       ENVIRONMENT "VYRN_CLI=$<TARGET_FILE:vyrn-cli>;VYRN_DATA=${PROJECT_SOURCE_DIR}")
endif()
# Real time (no world.py): rt-catch at 20 decisions per second, up to 20 minutes per seed. It measures the machine
# too (tick lateness), so it runs alone.
if(TARGET vyrn-cli AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_realtime.py")
  add_test(NAME e2e_rt_catch
           COMMAND "${Python3_EXECUTABLE}" -B -m pytest "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_realtime.py"
                   -q -p no:cacheprovider
           WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(e2e_rt_catch PROPERTIES LABELS slow TIMEOUT 5400 RUN_SERIAL TRUE
                       ENVIRONMENT "VYRN_CLI=$<TARGET_FILE:vyrn-cli>;VYRN_DATA=${PROJECT_SOURCE_DIR}")
endif()

# Vyrn Link: vyrn-cli trains templates/link-runner while tools/link/link_fake_game.py plays the game, in real time and
# in lockstep, then watches it (Vyrn's own Link server, on a random high port). Real time measures the machine too.
if(TARGET vyrn-cli AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_link_e2e.py")
  add_test(NAME e2e_link
           COMMAND "${Python3_EXECUTABLE}" -B -m pytest "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_link_e2e.py"
                   -q -p no:cacheprovider
           WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(e2e_link PROPERTIES LABELS e2e TIMEOUT 2400 RUN_SERIAL TRUE
                       ENVIRONMENT "VYRN_CLI=$<TARGET_FILE:vyrn-cli>;VYRN_DATA=${PROJECT_SOURCE_DIR}")
endif()

# ---- app parts (VYRN_WITH_APP_PARTS): MCP end to end, GUI smoke test --------------------------------------
if(VYRN_WITH_APP_PARTS AND TARGET vyrn-mcp AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_mcp.py")
  add_test(NAME e2e_mcp
           COMMAND "${Python3_EXECUTABLE}" -B -m pytest "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_mcp.py" -q -p no:cacheprovider
           WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(e2e_mcp PROPERTIES LABELS e2e TIMEOUT 1800
                       ENVIRONMENT "VYRN_MCP=$<TARGET_FILE:vyrn-mcp>;VYRN_DATA=${PROJECT_SOURCE_DIR}")
  if(TARGET vyrn-cli)   # the stop-signal cases run the server as `vyrn-cli mcp` too
    set_property(TEST e2e_mcp APPEND PROPERTY ENVIRONMENT "VYRN_CLI=$<TARGET_FILE:vyrn-cli>")
  endif()
endif()
if(VYRN_WITH_APP_PARTS AND TARGET vyrn AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gui/smoke_test.py")
  add_test(NAME gui_smoke
           COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/gui/smoke_test.py"
           WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(gui_smoke PROPERTIES LABELS gui TIMEOUT 900
                       ENVIRONMENT "VYRN_APP=$<TARGET_FILE:vyrn>;VYRN_DATA=${PROJECT_SOURCE_DIR};QT_QPA_PLATFORM=offscreen")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gui/test_link_gui.py")   # Vyrn Link: the Link bar, with the fake game
    add_test(NAME gui_link
             COMMAND "${Python3_EXECUTABLE}" -B -m pytest "${CMAKE_CURRENT_SOURCE_DIR}/gui/test_link_gui.py" -q -p no:cacheprovider
             WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(gui_link PROPERTIES LABELS gui TIMEOUT 900
                         ENVIRONMENT "VYRN_APP=$<TARGET_FILE:vyrn>;VYRN_DATA=${PROJECT_SOURCE_DIR};QT_QPA_PLATFORM=offscreen")
  endif()
  # Settings → Start program, with a harmless stand-in that never opens a window (never demo_game.py)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gui/test_updater.py")   # self-update check against a file:// URL
    add_test(NAME gui_updater
             COMMAND "${Python3_EXECUTABLE}" -B -m pytest "${CMAKE_CURRENT_SOURCE_DIR}/gui/test_updater.py" -q -p no:cacheprovider
             WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(gui_updater PROPERTIES LABELS gui TIMEOUT 300
                         ENVIRONMENT "VYRN_APP=$<TARGET_FILE:vyrn>;QT_QPA_PLATFORM=offscreen")
  endif()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gui/test_start_program.py")
    add_test(NAME gui_start_program
             COMMAND "${Python3_EXECUTABLE}" -B -m pytest "${CMAKE_CURRENT_SOURCE_DIR}/gui/test_start_program.py" -q -p no:cacheprovider
             WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(gui_start_program PROPERTIES LABELS gui TIMEOUT 900
                         ENVIRONMENT "VYRN_APP=$<TARGET_FILE:vyrn>;VYRN_DATA=${PROJECT_SOURCE_DIR};QT_QPA_PLATFORM=offscreen")
  endif()
  if(TARGET vyrn-cli AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gui/test_open_package.py")
    add_test(NAME gui_open_package
             COMMAND "${Python3_EXECUTABLE}" -B -m pytest "${CMAKE_CURRENT_SOURCE_DIR}/gui/test_open_package.py" -q -p no:cacheprovider
             WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(gui_open_package PROPERTIES LABELS gui TIMEOUT 900
                         ENVIRONMENT "VYRN_APP=$<TARGET_FILE:vyrn>;VYRN_PKG_TOOL=$<TARGET_FILE:vyrn-cli>;VYRN_DATA=${PROJECT_SOURCE_DIR};QT_QPA_PLATFORM=offscreen")
  endif()
endif()

# ---- Compile: train, `vyrn-cli compile`, play the folder with plain Python (docs/dev/standalone.md section 5) --
if(TARGET vyrn-cli AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_compile.py")
  add_test(NAME e2e_compile
           COMMAND "${Python3_EXECUTABLE}" -B -m pytest "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_compile.py" -q -p no:cacheprovider
           WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(e2e_compile PROPERTIES LABELS e2e TIMEOUT 1800
                       ENVIRONMENT "VYRN_CLI=$<TARGET_FILE:vyrn-cli>;VYRN_DATA=${PROJECT_SOURCE_DIR}")
endif()

# ---- sharing files (.vrl / .vrlp) through vyrn-cli (skips itself until vyrn-cli has the package commands) ----
if(VYRN_WITH_APP_PARTS AND TARGET vyrn-cli AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_package.py")
  add_test(NAME e2e_package
           COMMAND "${Python3_EXECUTABLE}" -B -m pytest "${CMAKE_CURRENT_SOURCE_DIR}/e2e/test_package.py" -q -p no:cacheprovider
           WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(e2e_package PROPERTIES LABELS e2e TIMEOUT 1800
                       ENVIRONMENT "VYRN_CLI=$<TARGET_FILE:vyrn-cli>;VYRN_DATA=${PROJECT_SOURCE_DIR}")
endif()
