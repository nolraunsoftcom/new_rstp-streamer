add_library(nv_domain STATIC
  domain/connection/ConnectionStateMachine.cpp
)
target_include_directories(nv_domain PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(nv_domain PRIVATE -Wall -Wextra -Wpedantic)

add_library(nv_app STATIC
  app/ChannelController.cpp
)
target_include_directories(nv_app PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(nv_app PUBLIC nv_domain)
target_compile_options(nv_app PRIVATE -Wall -Wextra -Wpedantic)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat libavcodec libavutil libswscale)
find_package(Qt6 6.7 REQUIRED COMPONENTS Widgets)

# nv_infra/nv_ui는 Task 2~8에서 소스가 생기면 활성화한다.

qt_add_executable(new_viewer ui/main.cpp)
target_link_libraries(new_viewer PRIVATE nv_app Qt6::Widgets PkgConfig::FFMPEG)
