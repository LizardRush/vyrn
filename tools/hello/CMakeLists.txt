# M0 toolchain proof: libtorch, Qt 6 and QScintilla all link into one program,
# with torch and Qt kept in separate translation units (their macros clash).
add_library(hello_torch STATIC hello_torch.cpp)
set_target_properties(hello_torch PROPERTIES CXX_STANDARD 17)
target_link_libraries(hello_torch PRIVATE ${TORCH_LIBRARIES})

add_executable(hello main.cpp)
target_link_libraries(hello PRIVATE hello_torch)
if(VYRN_GUI)
  add_library(hello_qt STATIC hello_qt.cpp)
  target_link_libraries(hello_qt PRIVATE Qt6::Widgets QScintilla::QScintilla)
  target_compile_definitions(hello PRIVATE VYRN_HELLO_QT=1)
  target_link_libraries(hello PRIVATE hello_qt)
endif()
