cmake_minimum_required(VERSION 3.20)

project(Patchwork LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(
  patchwork_lib
  src/app.cpp
  src/buffer.cpp
  src/build.cpp
  src/command.cpp
  src/cursor.cpp
  src/diff.cpp
  src/editor_state.cpp
  src/json.cpp
  src/patch.cpp
  src/screen.cpp
  src/selection.cpp
  src/syntax/cpp_highlighter.cpp
  src/syntax/language.cpp
  src/syntax/plain_text_highlighter.cpp
  src/syntax/registry.cpp
  src/syntax/theme.cpp
  src/terminal.cpp
  src/ai/codex_app_server_client.cpp
  src/ai/mock_client.cpp
  src/ai/codex_client.cpp
  src/ai/openai_client.cpp
  src/ai/prompt.cpp
)

target_include_directories(patchwork_lib PUBLIC src)
target_compile_definitions(patchwork_lib PUBLIC PATCHWORK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(patchwork_lib PUBLIC Threads::Threads)

add_executable(patchwork src/main.cpp)
target_link_libraries(patchwork PRIVATE patchwork_lib)

enable_testing()

add_executable(
  patchwork_tests
  tests/test_main.cpp
)

target_link_libraries(patchwork_tests PRIVATE patchwork_lib)

add_test(NAME patchwork_tests COMMAND patchwork_tests)
