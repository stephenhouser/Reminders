add_library(reminders_core
  src/model.cpp
  src/format.cpp
  src/recurrence.cpp
  src/merge.cpp
  src/store.cpp
  src/history.cpp
  src/syncthing.cpp
  src/settings.cpp
  src/dates.cpp
)
target_include_directories(reminders_core PUBLIC include)

add_executable(core_tests
  tests/main.cpp
  tests/format_test.cpp
  tests/recurrence_test.cpp
  tests/merge_test.cpp
  tests/store_test.cpp
  tests/history_test.cpp
  tests/syncthing_test.cpp
  tests/dates_test.cpp
)
target_link_libraries(core_tests PRIVATE reminders_core)
add_test(NAME core_tests COMMAND core_tests)
