add_library(nv_domain STATIC
  domain/connection/ConnectionStateMachine.cpp
)
target_include_directories(nv_domain PUBLIC ${CMAKE_SOURCE_DIR})
