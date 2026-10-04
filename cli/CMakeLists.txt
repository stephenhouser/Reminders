# The terminal client: `reminders` (CLI commands, or the TUI with none).
find_package(PkgConfig REQUIRED)
pkg_check_modules(NCURSES REQUIRED IMPORTED_TARGET ncursesw)

add_executable(reminders-cli
  src/cli/main.cpp
  src/cli/app.cpp
  src/cli/helpers.cpp
  src/cli/commands_read.cpp
  src/cli/commands_edit.cpp
  src/cli/commands_io.cpp
  src/editfile.cpp
  src/tui/tui.cpp
  src/tui/sidebar.cpp
  src/tui/items.cpp
  src/tui/editing.cpp
  src/tui/help.cpp
  src/tui/keys.cpp
  src/tui/marks.cpp
  src/tui/util.cpp)
target_link_libraries(reminders-cli PRIVATE reminders_app PkgConfig::NCURSES)
if(TARGET reminders_net)
  target_link_libraries(reminders-cli PRIVATE reminders_net)
  target_compile_definitions(reminders-cli PRIVATE REMINDERS_NETWORK)
endif()
set_target_properties(reminders-cli PROPERTIES OUTPUT_NAME reminders)

include(GNUInstallDirs)
install(TARGETS reminders-cli)
