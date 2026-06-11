add_library(nv_domain STATIC
  domain/connection/ConnectionStateMachine.cpp
)
target_include_directories(nv_domain PUBLIC ${CMAKE_SOURCE_DIR})

add_library(nv_app STATIC
  app/ChannelController.cpp
)
target_include_directories(nv_app PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(nv_app PUBLIC nv_domain)
