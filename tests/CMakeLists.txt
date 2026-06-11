include(FetchContent)
FetchContent_Declare(
  catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.5.4
)
FetchContent_MakeAvailable(catch2)

add_executable(nv_unit_tests
  unit/SmokeTest.cpp
  unit/StreamHealthTest.cpp
  unit/ConnectionStateMachineTest.cpp
  unit/ChannelControllerTest.cpp
  unit/ControlExecutorTest.cpp
  unit/LatestFrameSlotTest.cpp
  unit/MarshallingTest.cpp
  unit/SnapshotObserverTest.cpp
)
target_include_directories(nv_unit_tests PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nv_unit_tests PRIVATE nv_domain nv_app nv_infra Catch2::Catch2WithMain)
target_compile_options(nv_unit_tests PRIVATE -Wall -Wextra -Wpedantic)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)
catch_discover_tests(nv_unit_tests)

add_executable(nv_integration_tests
  integration/FfmpegSourceIT.cpp
)
target_include_directories(nv_integration_tests PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nv_integration_tests PRIVATE nv_infra Catch2::Catch2WithMain)
target_compile_definitions(nv_integration_tests PRIVATE PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
catch_discover_tests(nv_integration_tests PROPERTIES LABELS integration)
