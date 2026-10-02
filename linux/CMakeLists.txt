# The GNOME client (GTK4 + libadwaita through their C APIs).
enable_language(C)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ADW REQUIRED IMPORTED_TARGET libadwaita-1>=1.8 gtk4>=4.20)

find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
set(RES_XML ${CMAKE_CURRENT_SOURCE_DIR}/data/resources.gresource.xml)
set(RES_C ${CMAKE_CURRENT_BINARY_DIR}/resources.c)
execute_process(
  COMMAND ${GLIB_COMPILE_RESOURCES} --generate-dependencies ${RES_XML}
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
  OUTPUT_VARIABLE RES_DEPS_RAW)
string(REPLACE "\n" ";" RES_DEPS "${RES_DEPS_RAW}")
list(FILTER RES_DEPS EXCLUDE REGEX "^$")
list(TRANSFORM RES_DEPS PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/data/)
add_custom_command(
  OUTPUT ${RES_C}
  COMMAND ${GLIB_COMPILE_RESOURCES} --generate-source --target=${RES_C} ${RES_XML}
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
  DEPENDS ${RES_XML} ${RES_DEPS})

add_executable(reminders-gnome
  src/main.cpp
  src/window.cpp
  src/dialogs.cpp
  src/support.cpp
  ${RES_C})
target_link_libraries(reminders-gnome PRIVATE reminders_core PkgConfig::ADW)
# The generated resource C file isn't warning-clean under -Wpedantic.
set_source_files_properties(${RES_C} PROPERTIES COMPILE_OPTIONS "-w")

include(GNUInstallDirs)
set_target_properties(reminders-gnome PROPERTIES OUTPUT_NAME Reminders)
install(TARGETS reminders-gnome)
install(FILES data/com.stephenhouser.Reminders.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/icons/hicolor/scalable/apps/com.stephenhouser.Reminders.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
