add_library(reminders_core
  src/model.cpp
  src/format.cpp
  src/recurrence.cpp
  src/merge.cpp
  src/store.cpp
  src/history.cpp
  src/settings.cpp
  src/dates.cpp
  src/clipboard.cpp
  src/backend.cpp
  src/backend_module.cpp
  src/sync_runner.cpp
  src/sources.cpp
  src/library.cpp
  src/ical.cpp
  src/vtodo.cpp
  src/importer.cpp
  src/exporter.cpp
  src/zip.cpp
  src/paths.cpp
  src/profile.cpp
)
target_include_directories(reminders_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(reminders_core PUBLIC Threads::Threads)  # SyncRunner
# zlib compresses exported .zip archives; without it they're stored as is.
find_package(ZLIB)
if(ZLIB_FOUND)
  target_link_libraries(reminders_core PRIVATE ZLIB::ZLIB)
  target_compile_definitions(reminders_core PRIVATE REMINDERS_ZLIB)
endif()

add_executable(core_tests
  tests/main.cpp
  tests/format_test.cpp
  tests/recurrence_test.cpp
  tests/merge_test.cpp
  tests/store_test.cpp
  tests/history_test.cpp
  tests/dates_test.cpp
  tests/settings_test.cpp
  tests/clipboard_test.cpp
  tests/sources_test.cpp
  tests/library_test.cpp
  tests/vtodo_test.cpp
  tests/importer_test.cpp
  tests/exporter_test.cpp
  tests/paths_test.cpp
  tests/profile_test.cpp
)
target_link_libraries(core_tests PRIVATE reminders_core)
add_test(NAME core_tests COMMAND core_tests)
