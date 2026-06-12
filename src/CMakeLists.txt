add_library(nv_domain STATIC
  domain/connection/ConnectionStateMachine.cpp
)
target_include_directories(nv_domain PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(nv_domain PRIVATE -Wall -Wextra -Wpedantic)

add_library(nv_app STATIC
  app/ChannelController.cpp
  app/ChannelManager.cpp
)
target_include_directories(nv_app PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(nv_app PUBLIC nv_domain)
target_compile_options(nv_app PRIVATE -Wall -Wextra -Wpedantic)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat libavcodec libavutil libswscale)
find_package(Qt6 6.7 REQUIRED COMPONENTS Widgets Core)

add_library(nv_infra STATIC
  infra/system/ControlExecutor.cpp
  infra/ffmpeg/FfmpegStreamSource.cpp
  infra/ffmpeg/ChannelSourceFactory.cpp
  infra/persist/JsonChannelRepository.cpp
  infra/system/ResourceMonitor.cpp
)
target_include_directories(nv_infra PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(nv_infra PUBLIC nv_app PkgConfig::FFMPEG Qt6::Core)
target_compile_options(nv_infra PRIVATE -Wall -Wextra -Wpedantic)

set(CMAKE_AUTORCC ON)

add_library(nv_ui STATIC
  ui/shell/MainWindow.cpp
  ui/grid/VideoTileWidget.cpp
  ui/grid/GridView.cpp
  ui/channels/ChannelDialog.cpp
  ui/channels/ChannelListPanel.cpp
  ui/shell/LogPanel.cpp
  ui/shell/ControlBridge.h
  ui/shell/RepaintClock.h
  ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
target_include_directories(nv_ui PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(nv_ui PUBLIC nv_infra Qt6::Widgets)
target_compile_options(nv_ui PRIVATE -Wall -Wextra -Wpedantic)

# macOS バンドル アイコン
if(APPLE)
  set(APP_ICON_MACOS "${CMAKE_SOURCE_DIR}/resources/logo.icns")
  set_source_files_properties(${APP_ICON_MACOS}
    PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
  qt_add_executable(new_viewer MACOSX_BUNDLE
    ui/main.cpp
    ${APP_ICON_MACOS}
  )
  set_target_properties(new_viewer PROPERTIES
    MACOSX_BUNDLE_ICON_FILE "logo.icns"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.ziilab.new-viewer"
    MACOSX_BUNDLE_BUNDLE_NAME "영상관리시스템"
  )
else()
  qt_add_executable(new_viewer ui/main.cpp)
endif()

target_link_libraries(new_viewer PRIVATE nv_ui)
