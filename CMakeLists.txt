cmake_minimum_required(VERSION 3.20)
project(hyprlodr VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
include(CTest)

add_library(hyprlodr_loader STATIC native/src/loader.c)
target_include_directories(hyprlodr_loader PUBLIC native/include)
target_compile_options(hyprlodr_loader PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(UNIX AND NOT APPLE)
  target_link_libraries(hyprlodr_loader PUBLIC dl)
endif()

add_executable(hyprlodr-host native/src/host.c)
target_link_libraries(hyprlodr-host PRIVATE hyprlodr_loader)
target_compile_options(hyprlodr-host PRIVATE -Wall -Wextra -Wpedantic -Werror)

set(MOD_IDS z.foundation a.palette b.audio m.overlay 00.final)
set(MOD_SOURCES foundation palette audio overlay final)
list(LENGTH MOD_IDS MOD_COUNT)
math(EXPR MOD_LAST "${MOD_COUNT} - 1")
foreach(I RANGE ${MOD_LAST})
  list(GET MOD_IDS ${I} MOD_ID)
  list(GET MOD_SOURCES ${I} MOD_SOURCE)
  add_library(mod_${MOD_SOURCE} SHARED samples/${MOD_SOURCE}.c)
  target_include_directories(mod_${MOD_SOURCE} PRIVATE native/include)
  target_compile_options(mod_${MOD_SOURCE} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  set_target_properties(mod_${MOD_SOURCE} PROPERTIES PREFIX "" OUTPUT_NAME "${MOD_ID}")
endforeach()

if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME five_mod_lifecycle
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/native/tests/assert_lifecycle.py
      $<TARGET_FILE:hyprlodr-host> $<TARGET_FILE_DIR:mod_foundation>)
endif()
