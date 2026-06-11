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

add_library(nv_infra STATIC
  infra/system/ControlExecutor.cpp
  infra/ffmpeg/FfmpegStreamSource.cpp
)
target_include_directories(nv_infra PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(nv_infra PUBLIC nv_app PkgConfig::FFMPEG)
target_compile_options(nv_infra PRIVATE -Wall -Wextra -Wpedantic)

qt_add_executable(new_viewer ui/main.cpp)
target_link_libraries(new_viewer PRIVATE nv_app Qt6::Widgets PkgConfig::FFMPEG)
