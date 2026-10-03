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
  src/clipboard.cpp
  src/backend.cpp
  src/sources.cpp
  src/library.cpp
  src/ical.cpp
  src/vtodo.cpp
  src/caldav.cpp
  src/webdav.cpp
  src/ics_import.cpp
  src/paths.cpp
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
  tests/clipboard_test.cpp
  tests/sources_test.cpp
  tests/library_test.cpp
  tests/vtodo_test.cpp
  tests/caldav_test.cpp
  tests/webdav_test.cpp
  tests/ics_import_test.cpp
  tests/paths_test.cpp
)
target_link_libraries(core_tests PRIVATE reminders_core)
add_test(NAME core_tests COMMAND core_tests)
