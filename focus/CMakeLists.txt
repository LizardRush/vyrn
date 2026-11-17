# vyrn-focus: tiny Wayland client that reports the active window (keyboard safety guard). Linux only: macOS and
# Windows ask the system directly from Python (docs/dev/platforms.md).
if(NOT VYRN_FOCUS OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()
find_program(WAYLAND_SCANNER wayland-scanner)
find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(WAYLAND_CLIENT IMPORTED_TARGET wayland-client)
endif()
if(NOT WAYLAND_SCANNER OR NOT WAYLAND_CLIENT_FOUND)
  message(WARNING "vyrn-focus skipped: needs wayland-scanner and wayland-client (apt: libwayland-dev libwayland-bin). "
                  "Real-game projects cannot press keys without it.")
  return()
endif()

set(xml "${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-foreign-toplevel-management-unstable-v1.xml")
set(gen_h "${CMAKE_CURRENT_BINARY_DIR}/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h")
set(gen_c "${CMAKE_CURRENT_BINARY_DIR}/wlr-foreign-toplevel-management-unstable-v1-protocol.c")
add_custom_command(OUTPUT "${gen_h}" COMMAND "${WAYLAND_SCANNER}" client-header "${xml}" "${gen_h}" DEPENDS "${xml}")
add_custom_command(OUTPUT "${gen_c}" COMMAND "${WAYLAND_SCANNER}" private-code "${xml}" "${gen_c}" DEPENDS "${xml}")

add_executable(vyrn-focus vyrn-focus.c "${gen_c}" "${gen_h}")
set_target_properties(vyrn-focus PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(vyrn-focus PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(vyrn-focus PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(vyrn-focus PRIVATE PkgConfig::WAYLAND_CLIENT)
install(TARGETS vyrn-focus RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
