find_package(CURL REQUIRED)
find_package(LibXml2 REQUIRED)

add_library(reminders_net
  src/dav.cpp
  src/caldav_client.cpp
  src/webdav_client.cpp
  src/server_sync.cpp
  src/sync_runner.cpp
)
target_include_directories(reminders_net PUBLIC include)
target_link_libraries(reminders_net PUBLIC reminders_core PRIVATE CURL::libcurl LibXml2::LibXml2)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_executable(net_tests tests/net_test.cpp)
  target_include_directories(net_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/tests)
  target_link_libraries(net_tests PRIVATE reminders_net)
  add_test(NAME net_tests COMMAND net_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_dav.py)
endif()
