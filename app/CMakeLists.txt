# The app layer: what the GNOME app and the terminal interface share above
# the core (views, selection, the rules for editing several reminders).
# Standard C++ only, like the core.
add_library(reminders_app STATIC
  src/view.cpp
  src/selection.cpp
  src/actions.cpp
  src/view_model.cpp
  src/sidebar.cpp
  src/preferences.cpp
  src/import_file.cpp
  src/command_line.cpp)
target_include_directories(reminders_app PUBLIC include)
target_link_libraries(reminders_app PUBLIC reminders_core)

add_executable(app_tests
  ${PROJECT_SOURCE_DIR}/core/tests/main.cpp
  tests/view_test.cpp
  tests/selection_test.cpp
  tests/actions_test.cpp
  tests/view_model_test.cpp
  tests/sidebar_test.cpp
  tests/preferences_test.cpp
  tests/import_file_test.cpp
  tests/command_line_test.cpp)
target_include_directories(app_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests)
target_link_libraries(app_tests PRIVATE reminders_app)
add_test(NAME app_tests COMMAND app_tests)
