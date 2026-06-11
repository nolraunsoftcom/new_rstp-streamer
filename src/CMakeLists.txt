add_library(nv_domain INTERFACE)
target_include_directories(nv_domain INTERFACE ${CMAKE_SOURCE_DIR})
# ConnectionStateMachine.cpp가 생기는 Task 3에서 STATIC으로 전환한다.
