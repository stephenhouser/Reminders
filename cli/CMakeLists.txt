# The terminal client: `reminders` (CLI commands, or the TUI with none).
find_package(PkgConfig REQUIRED)
pkg_check_modules(NCURSES REQUIRED IMPORTED_TARGET ncursesw)

add_executable(reminders-cli src/cli.cpp src/tui.cpp src/editfile.cpp)
target_link_libraries(reminders-cli PRIVATE reminders_core PkgConfig::NCURSES)
if(TARGET reminders_net)
  target_link_libraries(reminders-cli PRIVATE reminders_net)
  target_compile_definitions(reminders-cli PRIVATE REMINDERS_NETWORK)
endif()
set_target_properties(reminders-cli PROPERTIES OUTPUT_NAME reminders)

include(GNUInstallDirs)
install(TARGETS reminders-cli)
