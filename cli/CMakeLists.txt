# vyrn-cli: check / train / watch / bench from a terminal, plus (with VYRN_WITH_APP_PARTS) the sharing commands
# (package_cmd.cpp: pack, export-brain, inspect, import, trust), the project lock and `vyrn-cli mcp`.
# Uses only the core's public headers (no torch here). Each optional part comes with a compile definition, so
# vyrn-cli still builds without it: VYRN_HAVE_PROJECT (vyrn_project: .vyrn-lock), VYRN_HAVE_PACKAGE (vyrn_package),
# VYRN_HAVE_MCP (vyrn_mcp, from cli/mcp/, which the top-level CMakeLists.txt adds before this folder).
add_executable(vyrn-cli main.cpp)
set_target_properties(vyrn-cli PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(vyrn-cli PRIVATE VYRN_VERSION="${PROJECT_VERSION}")
target_link_libraries(vyrn-cli PRIVATE vyrn_core vyrn_warnings)
vyrn_executable(vyrn-cli)
if(TARGET vyrn_project)
  target_link_libraries(vyrn-cli PRIVATE vyrn_project)
  target_compile_definitions(vyrn-cli PRIVATE VYRN_HAVE_PROJECT)
endif()
if(TARGET vyrn_package AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/package_cmd.cpp")
  target_sources(vyrn-cli PRIVATE package_cmd.cpp)
  target_link_libraries(vyrn-cli PRIVATE vyrn_package)
  target_compile_definitions(vyrn-cli PRIVATE VYRN_HAVE_PACKAGE)
endif()
if(TARGET vyrn_mcp)
  target_link_libraries(vyrn-cli PRIVATE vyrn_mcp)
  target_compile_definitions(vyrn-cli PRIVATE VYRN_HAVE_MCP)
endif()
install(TARGETS vyrn-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
