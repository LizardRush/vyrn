# FindQScintilla — locate QScintilla built for Qt 6. None of the usual installs ship CMake or pkg-config files for
# it, only headers + the library:
#   Debian:   /usr/include/<multiarch>/qt6/Qsci/qsciscintilla.h, /usr/lib/<multiarch>/libqscintilla2_qt6.so
#   Homebrew: <prefix>/include/Qsci/... or <prefix>/opt/qscintilla2/include/Qsci/..., lib/libqscintilla2_qt6.dylib
#             or lib/qscintilla2_qt6.framework (prefix /opt/homebrew on Apple Silicon, /usr/local on Intel)
#   vcpkg:    <installed>/<triplet>/include/Qsci/..., lib/qscintilla2_qt6.lib (found through CMAKE_PREFIX_PATH,
#             which the vcpkg toolchain sets; VCPKG_INSTALLED_DIR + VCPKG_TARGET_TRIPLET are searched too)
# Provides the imported target QScintilla::QScintilla.

set(_qsci_prefixes "")
if(APPLE)
  list(APPEND _qsci_prefixes /opt/homebrew/opt/qscintilla2 /opt/homebrew /usr/local/opt/qscintilla2 /usr/local)
endif()
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
  list(APPEND _qsci_prefixes "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()
# Qt6_DIR is <libdir>/cmake/Qt6: Debian's <libdir> is lib/<multiarch> (four levels up is the prefix); a Qt from the
# online installer or aqtinstall, with QScintilla built into it (install-windows.ps1, CI), has lib/ (three levels).
set(_qsci_include_hints "${Qt6_DIR}/../../../../include" "${Qt6_DIR}/../../../include")
set(_qsci_lib_hints "")
foreach(prefix IN LISTS _qsci_prefixes)
  list(APPEND _qsci_include_hints "${prefix}/include")
  list(APPEND _qsci_lib_hints "${prefix}/lib")
endforeach()

find_library(QScintilla_LIBRARY
  NAMES qscintilla2_qt6 libqscintilla2_qt6 qscintilla2_qt6d
  HINTS ${_qsci_lib_hints})
find_path(QScintilla_INCLUDE_DIR
  NAMES Qsci/qsciscintilla.h
  PATH_SUFFIXES qt6 ${CMAKE_LIBRARY_ARCHITECTURE}/qt6
  HINTS ${_qsci_include_hints})

# A macOS framework (qscintilla2_qt6.framework) keeps its headers in Headers/, with or without the Qsci/ folder the
# sources include: point a folder in the build tree at them in the second case.
if(NOT QScintilla_INCLUDE_DIR AND QScintilla_LIBRARY MATCHES "\\.framework$")
  if(EXISTS "${QScintilla_LIBRARY}/Headers/Qsci/qsciscintilla.h")
    set(QScintilla_INCLUDE_DIR "${QScintilla_LIBRARY}/Headers" CACHE PATH "QScintilla headers" FORCE)
  elseif(EXISTS "${QScintilla_LIBRARY}/Headers/qsciscintilla.h")
    set(_qsci_shim "${CMAKE_BINARY_DIR}/qscintilla-include")
    file(MAKE_DIRECTORY "${_qsci_shim}")
    file(CREATE_LINK "${QScintilla_LIBRARY}/Headers" "${_qsci_shim}/Qsci" SYMBOLIC)
    set(QScintilla_INCLUDE_DIR "${_qsci_shim}" CACHE PATH "QScintilla headers" FORCE)
  endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(QScintilla
  REQUIRED_VARS QScintilla_LIBRARY QScintilla_INCLUDE_DIR)

if(QScintilla_FOUND AND NOT TARGET QScintilla::QScintilla)
  add_library(QScintilla::QScintilla UNKNOWN IMPORTED)
  if(QScintilla_LIBRARY MATCHES "\\.framework$")
    get_filename_component(_qsci_fw_name "${QScintilla_LIBRARY}" NAME_WE)
    set(_qsci_location "${QScintilla_LIBRARY}/${_qsci_fw_name}")
  else()
    set(_qsci_location "${QScintilla_LIBRARY}")
  endif()
  set_target_properties(QScintilla::QScintilla PROPERTIES
    IMPORTED_LOCATION "${_qsci_location}"
    INTERFACE_INCLUDE_DIRECTORIES "${QScintilla_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES Qt6::Widgets)
  if(WIN32)   # the DLL build of QScintilla needs this to import its symbols
    set_property(TARGET QScintilla::QScintilla APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS QSCINTILLA_DLL)
  endif()
endif()

# Windows: the DLL that goes with the import library, for installing next to vyrn.exe (QScintilla_DLL). qmake's
# `nmake install` puts it beside the .lib in Qt's lib folder; vcpkg puts it in bin.
if(WIN32 AND QScintilla_LIBRARY)
  get_filename_component(_qsci_lib_dir "${QScintilla_LIBRARY}" DIRECTORY)
  get_filename_component(_qsci_lib_name "${QScintilla_LIBRARY}" NAME_WE)
  find_file(QScintilla_DLL NAMES "${_qsci_lib_name}.dll"
            HINTS "${_qsci_lib_dir}" "${_qsci_lib_dir}/../bin" NO_DEFAULT_PATH)
  mark_as_advanced(QScintilla_DLL)
endif()
mark_as_advanced(QScintilla_INCLUDE_DIR QScintilla_LIBRARY)
