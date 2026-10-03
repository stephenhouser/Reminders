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

option(BUILD_CALDAV "Build the CalDAV back end (needs libcurl and libxml2)" ON)
if(BUILD_CALDAV)
  add_subdirectory(net)
endif()

option(BUILD_TERMINAL_APP "Build the terminal client (needs ncursesw)" ON)
if(BUILD_TERMINAL_APP)
  add_subdirectory(cli)
endif()

option(BUILD_GNOME_APP "Build the GNOME client (needs gtk4 and libadwaita-1)" ON)
if(BUILD_GNOME_APP AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/linux/CMakeLists.txt)
  add_subdirectory(linux)
endif()
