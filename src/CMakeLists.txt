add_library(nv_domain STATIC
  domain/connection/ConnectionStateMachine.cpp
  domain/relay/RelayConfig.cpp
)
target_include_directories(nv_domain PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(nv_domain PRIVATE -Wall -Wextra -Wpedantic)

add_library(nv_app STATIC
  app/ChannelController.cpp
  app/ChannelManager.cpp
  app/RecordingController.cpp
  app/SnapshotService.cpp
)
target_include_directories(nv_app PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(nv_app PUBLIC nv_domain)
target_compile_options(nv_app PRIVATE -Wall -Wextra -Wpedantic)

# ── FFmpeg 탐색 ──────────────────────────────────────────────────────────────
# Windows: vcpkg 툴체인이 find_package(FFMPEG) 경로를 주입한다.
# macOS/Linux: pkg-config 경로.
if(WIN32)
  # vcpkg가 설치한 FFmpeg CMake 패키지 사용.
  # vcpkg.json에서 avcodec/avformat/avutil/swscale 피처만 선언(GPL 피처 제외).
  find_package(FFMPEG REQUIRED COMPONENTS avformat avcodec avutil swscale)
  # vcpkg FFmpeg CMake 패키지는 FFMPEG_INCLUDE_DIRS / FFMPEG_LIBRARIES 변수를 노출한다.
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswscale)
endif()

find_package(Qt6 6.7 REQUIRED COMPONENTS Widgets Core Gui GuiPrivate ShaderTools)

add_library(nv_infra STATIC
  infra/system/ControlExecutor.cpp
  infra/system/SoakLogger.cpp
  infra/ffmpeg/FfmpegStreamSource.cpp
  infra/ffmpeg/FfmpegRecorder.cpp
  infra/ffmpeg/HwContext.cpp
  infra/ffmpeg/ChannelSourceFactory.cpp
  infra/persist/JsonChannelRepository.cpp
  infra/persist/PngSnapshotWriter.cpp
  infra/system/ResourceMonitor.cpp
)
target_include_directories(nv_infra PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(nv_infra PRIVATE -Wall -Wextra -Wpedantic)

if(WIN32)
  # Windows/vcpkg: FFMPEG_INCLUDE_DIRS + FFMPEG_LIBRARIES (FindFFMPEG.cmake 변수).
  # D3D11VA HW 디코딩은 HwContext.cpp의 #ifdef _WIN32 분기에서 활성.
  # d3d11 / dxgi는 FFmpeg avcodec이 내부적으로 요구; 명시 링크로 확실히 해결.
  target_include_directories(nv_infra PUBLIC ${FFMPEG_INCLUDE_DIRS})
  # Qt6::Gui — PngSnapshotWriter의 QImage(PNG 저장). Core는 JsonChannelRepository/RecordingPaths.
  target_link_libraries(nv_infra PUBLIC nv_app Qt6::Core Qt6::Gui ${FFMPEG_LIBRARIES} d3d11 dxgi)
else()
  # macOS/Linux: pkg-config IMPORTED_TARGET 사용.
  # Qt6::Gui — PngSnapshotWriter의 QImage(PNG 저장).
  target_link_libraries(nv_infra PUBLIC nv_app PkgConfig::FFMPEG Qt6::Core Qt6::Gui)
endif()

# macOS: VideoToolbox HW 디코딩 — CVPixelBuffer 수명 관리에 CoreVideo 필요.
# + VtMetalBridge(.mm, Obj-C++): CVPixelBuffer(NV12)→Metal 2-plane 텍스처(zero-copy, M2c).
#   Metal/CoreVideo/Foundation 프레임워크 링크. 비-Apple은 .mm 제외(헤더만 — Obj-C++는
#   Apple 전용이라 컴파일 불가; VtMetalBridge 호출측이 아직 없어 심볼 누락 없음).
if(APPLE)
  target_sources(nv_infra PRIVATE infra/video/VtMetalBridge.mm)
  target_link_libraries(nv_infra PUBLIC
    "-framework CoreVideo"
    "-framework Metal"
    "-framework Foundation")
endif()

set(CMAKE_AUTORCC ON)

add_library(nv_ui STATIC
  ui/shell/MainWindow.cpp
  ui/grid/VideoTileWidget.cpp
  ui/grid/GridView.cpp
  ui/render/SwVideoRenderer.cpp
  ui/render/RhiVideoRenderer.cpp
  ui/channels/ChannelDialog.cpp
  ui/channels/ChannelListPanel.cpp
  ui/shell/LogPanel.cpp
  ui/panels/FilePanel.cpp
  ui/shell/ControlBridge.h
  ui/shell/RepaintClock.h
  ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
target_include_directories(nv_ui PUBLIC ${CMAKE_SOURCE_DIR})
# Qt6::GuiPrivate — QRhi(rhi/qrhi.h) 헤더 접근(QRhiWidget GPU 렌더러).
target_link_libraries(nv_ui PUBLIC nv_infra Qt6::Widgets Qt6::GuiPrivate)
target_compile_options(nv_ui PRIVATE -Wall -Wextra -Wpedantic)

# QRhiWidget 셰이더 — qsb 컴파일 후 :/shaders/*.qsb 로 노출 (RhiVideoRenderer가 QFile 로드).
qt6_add_shaders(nv_ui "nv_ui_shaders"
  BATCHABLE
  PRECOMPILE
  PREFIX "/shaders"
  BASE "${CMAKE_CURRENT_SOURCE_DIR}/ui/render"
  FILES
    ui/render/texture.vert
    ui/render/texture.frag
    ui/render/nv12.frag
)

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

# Windows: windeployqt — Qt DLL을 실행 파일 옆에 복사.
# vcpkg FFmpeg DLL은 vcpkg 툴체인이 자동으로 output 디렉토리로 복사한다.
# 수동 배포 시: windeployqt --release new_viewer.exe
if(WIN32)
  find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${Qt6_DIR}/../../../bin")
  if(WINDEPLOYQT_EXECUTABLE)
    add_custom_command(TARGET new_viewer POST_BUILD
      COMMAND "${WINDEPLOYQT_EXECUTABLE}"
              --no-translations
              --no-system-d3d-compiler
              "$<TARGET_FILE:new_viewer>"
      COMMENT "windeployqt: Qt DLL 배포"
    )
  endif()
endif()
