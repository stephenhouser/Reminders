cmake_minimum_required(VERSION 3.25)
project(Reminders LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

enable_testing()
add_subdirectory(core)
add_subdirectory(app)

add_subdirectory(backends)

# The clients, one directory each under clients/. Their executables all go
# to build/bin/, so commands and docs don't depend on the source layout.
set(REMINDERS_BIN_DIR ${CMAKE_BINARY_DIR}/bin)

option(BUILD_TERMINAL_APP "Build the terminal client (needs ncursesw)" ON)
if(BUILD_TERMINAL_APP)
  add_subdirectory(clients/terminal)
endif()

option(BUILD_GNOME_APP "Build the GNOME client (needs gtk4 and libadwaita-1)" ON)
if(BUILD_GNOME_APP AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/clients/gnome/CMakeLists.txt)
  add_subdirectory(clients/gnome)
endif()

# Formatting with clang-format, in the style of the nearest .clang-format
# above the sources (the user's own, in their home folder):
#   cmake --build build --target format        reformat every C/C++ file
#   cmake --build build --target format-check  list unformatted files, change nothing
# `format` runs twice: InsertBraces can need a second pass for nested
# statements.
find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
  file(GLOB_RECURSE FORMAT_SOURCES CONFIGURE_DEPENDS
       ${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/core/*.hpp
       ${CMAKE_CURRENT_SOURCE_DIR}/app/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/app/*.hpp
       ${CMAKE_CURRENT_SOURCE_DIR}/backends/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/backends/*.hpp
       ${CMAKE_CURRENT_SOURCE_DIR}/clients/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/clients/*.hpp)
  add_custom_target(format
    COMMAND ${CLANG_FORMAT} -i --style=file ${FORMAT_SOURCES}
    COMMAND ${CLANG_FORMAT} -i --style=file ${FORMAT_SOURCES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Formatting C/C++ sources with clang-format"
    VERBATIM)
  add_custom_target(format-check
    COMMAND ${CLANG_FORMAT} --dry-run --Werror --style=file ${FORMAT_SOURCES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Checking C/C++ formatting with clang-format"
    VERBATIM)
endif()
