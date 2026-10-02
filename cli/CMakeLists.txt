# The terminal client: `reminders` (CLI commands, or the TUI with none).
find_package(PkgConfig REQUIRED)
pkg_check_modules(NCURSES REQUIRED IMPORTED_TARGET ncursesw)

add_executable(reminders-cli src/cli.cpp src/tui.cpp)
target_link_libraries(reminders-cli PRIVATE reminders_core PkgConfig::NCURSES)
set_target_properties(reminders-cli PROPERTIES OUTPUT_NAME reminders)

include(GNUInstallDirs)
install(TARGETS reminders-cli)
