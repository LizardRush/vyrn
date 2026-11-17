# The Qt app (vyrn): IDE + dashboard. Never includes torch headers (see tests/lint_headers.cmake).
# Sources and resources are globbed: add files without editing this file. What differs per system is in
# platform_linux.cpp, platform_mac.cpp and platform_win.cpp (AppPlatform.h): exactly one of them is built.
if(NOT VYRN_GUI OR NOT VYRN_WITH_APP_PARTS OR NOT TARGET vyrn_project)
  return()
endif()
if(NOT TARGET vyrn_package)   # .vrl / .vrlp sharing (core/src/package); built with VYRN_WITH_APP_PARTS
  message(STATUS "vyrn app: skipped, the vyrn_package library isn't there")
  return()
endif()
file(GLOB VYRN_APP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
if(NOT VYRN_APP_SOURCES)
  message(STATUS "vyrn app: no sources yet")
  return()
endif()
list(FILTER VYRN_APP_SOURCES EXCLUDE REGEX "/platform_[a-z]+\\.cpp$")
if(APPLE)
  list(APPEND VYRN_APP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/platform_mac.cpp")
elseif(WIN32)
  list(APPEND VYRN_APP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/platform_win.cpp")
else()
  list(APPEND VYRN_APP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/platform_linux.cpp")
endif()

qt_standard_project_setup()   # AUTOMOC/AUTOUIC for this directory
qt_add_executable(vyrn WIN32 MACOSX_BUNDLE ${VYRN_APP_SOURCES})
vyrn_executable(vyrn)         # Windows: UTF-8 as the ANSI code page (cmake/utf8.manifest)
set_target_properties(vyrn PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON WIN32_EXECUTABLE ON MACOSX_BUNDLE ON)
target_compile_definitions(vyrn PRIVATE
  QT_NO_KEYWORDS                       # QScintilla headers come in through app/Qsci.h only
  VYRN_SOURCE_DIR="${VYRN_SOURCE_DIR}"
  VYRN_VERSION="${PROJECT_VERSION}")
target_link_libraries(vyrn PRIVATE vyrn_package vyrn_project vyrn_core Qt6::Widgets QScintilla::QScintilla vyrn_warnings)

# ---- static resources: everything under app/resources/ except generated files -> ":/<relative path>" -----
file(GLOB_RECURSE app_res CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/resources"
     "${CMAKE_CURRENT_SOURCE_DIR}/resources/*")
list(FILTER app_res EXCLUDE REGEX "^(vyrn\\.api|docs/api\\.html)$")   # generated at build time (below)
if(app_res)
  list(TRANSFORM app_res PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/resources/")
  qt_add_resources(vyrn app_static PREFIX "/" BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources" FILES ${app_res})
endif()

# ---- generated from the vyrn library docstrings: ":/vyrn.api" (autocompletion), ":/docs/api.html" ---------
set(gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(GLOB vyrn_py CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/python/vyrn/*.py")
add_custom_command(
  OUTPUT "${gen_dir}/vyrn.api" "${gen_dir}/docs/api.html"
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/python/tools/gen_api.py" --out-dir "${gen_dir}"
  DEPENDS "${PROJECT_SOURCE_DIR}/python/tools/gen_api.py" ${vyrn_py}
  COMMENT "Generating vyrn.api and docs/api.html from the vyrn library docstrings"
  VERBATIM)
qt_add_resources(vyrn app_generated PREFIX "/" BASE "${gen_dir}"
                 FILES "${gen_dir}/vyrn.api" "${gen_dir}/docs/api.html")

# ---- hand-written user docs (Markdown, shown in the Docs tab): ":/guide/<file>.md" ----------------------
file(GLOB user_docs CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/docs/user/*.md")
if(user_docs)
  qt_add_resources(vyrn app_guide PREFIX "/guide" BASE "${PROJECT_SOURCE_DIR}/docs/user" FILES ${user_docs})
endif()

# ---- macOS: Vyrn.app ------------------------------------------------------------------------------------------
if(APPLE)
  # The icon, made at configure time from packaging/vyrn.png with sips + iconutil (both come with macOS); a 1024 px
  # master when Pillow can draw one (packaging/make_icon.py). Without them the app has no icon of its own.
  set(vyrn_png "${PROJECT_SOURCE_DIR}/packaging/vyrn.png")
  set(vyrn_icns "${CMAKE_CURRENT_BINARY_DIR}/vyrn.icns")
  find_program(VYRN_SIPS sips)
  find_program(VYRN_ICONUTIL iconutil)
  if(NOT EXISTS "${vyrn_icns}" AND EXISTS "${vyrn_png}" AND VYRN_SIPS AND VYRN_ICONUTIL)
    set(iconset "${CMAKE_CURRENT_BINARY_DIR}/vyrn.iconset")
    file(REMOVE_RECURSE "${iconset}")
    file(MAKE_DIRECTORY "${iconset}")
    set(master "${vyrn_png}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/packaging/make_icon.py" --size 1024
                            --out "${CMAKE_CURRENT_BINARY_DIR}/vyrn-1024.png"
                    RESULT_VARIABLE drawn OUTPUT_QUIET ERROR_QUIET)
    if(drawn EQUAL 0)
      set(master "${CMAKE_CURRENT_BINARY_DIR}/vyrn-1024.png")
    endif()
    set(made TRUE)
    foreach(size 16 32 128 256 512)
      math(EXPR double "${size} * 2")
      execute_process(COMMAND "${VYRN_SIPS}" -z ${size} ${size} "${master}" --out "${iconset}/icon_${size}x${size}.png"
                      RESULT_VARIABLE one OUTPUT_QUIET ERROR_QUIET)
      execute_process(COMMAND "${VYRN_SIPS}" -z ${double} ${double} "${master}"
                              --out "${iconset}/icon_${size}x${size}@2x.png"
                      RESULT_VARIABLE two OUTPUT_QUIET ERROR_QUIET)
      if(NOT one EQUAL 0 OR NOT two EQUAL 0)
        set(made FALSE)
      endif()
    endforeach()
    if(made)
      execute_process(COMMAND "${VYRN_ICONUTIL}" -c icns "${iconset}" -o "${vyrn_icns}"
                      RESULT_VARIABLE packed OUTPUT_QUIET ERROR_QUIET)
      if(NOT packed EQUAL 0)
        set(made FALSE)
      endif()
    endif()
    if(NOT made)
      file(REMOVE "${vyrn_icns}")
      message(STATUS "vyrn app: no macOS icon (sips or iconutil failed)")
    endif()
  endif()
  if(EXISTS "${vyrn_icns}")
    set_source_files_properties("${vyrn_icns}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(vyrn PRIVATE "${vyrn_icns}")
    set_target_properties(vyrn PROPERTIES MACOSX_BUNDLE_ICON_FILE vyrn.icns)
  endif()
  # The bundle id ends in ".vyrn": the focus guard knows Vyrn's own window by it (docs/dev/protocol.md section 6).
  if(NOT VYRN_APP_BUNDLE_NAME)   # the top-level CMakeLists.txt installs vyrn-cli into <this>.app/Contents/MacOS
    set(VYRN_APP_BUNDLE_NAME Vyrn)
  endif()
  set_target_properties(vyrn PROPERTIES
    OUTPUT_NAME "${VYRN_APP_BUNDLE_NAME}"
    MACOSX_BUNDLE_BUNDLE_NAME Vyrn
    MACOSX_BUNDLE_GUI_IDENTIFIER org.vyrn.vyrn
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_LONG_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_INFO_STRING "Vyrn ${PROJECT_VERSION}")
  # Carbon: the panic hotkey (RegisterEventHotKey); ApplicationServices: AXIsProcessTrusted, CGPreflightScreenCaptureAccess.
  target_link_libraries(vyrn PRIVATE "-framework ApplicationServices" "-framework Carbon")
endif()

# ---- Windows: the icon (packaging/vyrn.ico, else one Pillow makes from packaging/vyrn.png at configure time) ----
if(WIN32)
  set(vyrn_ico "${PROJECT_SOURCE_DIR}/packaging/vyrn.ico")
  if(NOT EXISTS "${vyrn_ico}")
    set(vyrn_ico "${CMAKE_CURRENT_BINARY_DIR}/vyrn.ico")
    if(NOT EXISTS "${vyrn_ico}" AND EXISTS "${PROJECT_SOURCE_DIR}/packaging/vyrn.png")
      execute_process(
        COMMAND "${Python3_EXECUTABLE}" -c
                "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2], sizes=[(s, s) for s in (16, 20, 24, 32, 40, 48, 64, 128, 256)])"
                "${PROJECT_SOURCE_DIR}/packaging/vyrn.png" "${vyrn_ico}"
        RESULT_VARIABLE made OUTPUT_QUIET ERROR_QUIET)
      if(NOT made EQUAL 0)
        file(REMOVE "${vyrn_ico}")
        message(STATUS "vyrn app: no Windows icon (Pillow couldn't make vyrn.ico)")
      endif()
    endif()
  endif()
  if(EXISTS "${vyrn_ico}")
    set(vyrn_rc "${CMAKE_CURRENT_BINARY_DIR}/vyrn.rc")
    set(rc_text "IDI_ICON1 ICON \"${vyrn_ico}\"\n")   # forward slashes: nothing to escape
    set(rc_old "")
    if(EXISTS "${vyrn_rc}")
      file(READ "${vyrn_rc}" rc_old)
    endif()
    if(NOT rc_old STREQUAL rc_text)   # rewritten only when it changes, so it isn't compiled again each time
      file(WRITE "${vyrn_rc}" "${rc_text}")
    endif()
    target_sources(vyrn PRIVATE "${vyrn_rc}")
  endif()
  target_link_libraries(vyrn PRIVATE user32)   # RegisterHotKey: the panic hotkey
endif()

install(TARGETS vyrn RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} BUNDLE DESTINATION .)
if(APPLE)
  # What find_data_dir() looks for in <exe>/../Resources/vyrn (docs/dev/platforms.md): python/, templates/, the user
  # docs and tools/link, copied into the installed bundle (a build folder's Vyrn.app finds the source tree instead).
  set(bundle_data "${VYRN_APP_BUNDLE_NAME}.app/Contents/Resources/vyrn")
  if(COMMAND vyrn_install_data)   # the top-level CMakeLists.txt: the same files as <prefix>/share/vyrn elsewhere
    vyrn_install_data("${bundle_data}")
  else()
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/python/" DESTINATION "${bundle_data}/python"
            PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/templates/" DESTINATION "${bundle_data}/templates"
            PATTERN "__pycache__" EXCLUDE PATTERN "brains" EXCLUDE PATTERN "runs" EXCLUDE)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/docs/user/" DESTINATION "${bundle_data}/docs")
  endif()
endif()

# ---- unit test: Pick area's coordinate conversion (pure functions in PickTool.h) --------------------------------
if(BUILD_TESTING AND EXISTS "${PROJECT_SOURCE_DIR}/tests/gui/pick_area_test.cpp")
  add_executable(test_pick_area "${PROJECT_SOURCE_DIR}/tests/gui/pick_area_test.cpp")
  set_target_properties(test_pick_area PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON AUTOMOC OFF)
  target_include_directories(test_pick_area PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(test_pick_area PRIVATE Qt6::Core vyrn_warnings)
  vyrn_executable(test_pick_area)
  add_test(NAME gui_pick_area COMMAND test_pick_area)
  set_tests_properties(gui_pick_area PROPERTIES LABELS "unit;gui" TIMEOUT 60)
endif()
