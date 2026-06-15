add_library(nv_domain STATIC
  domain/connection/ConnectionStateMachine.cpp
  domain/relay/RelayConfig.cpp
)
target_include_directories(nv_domain PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(nv_domain PRIVATE ${NV_WARNING_FLAGS})

add_library(nv_app STATIC
  app/ChannelController.cpp
  app/ChannelManager.cpp
  app/RecordingController.cpp
  app/RelaySupervisor.cpp
  app/SnapshotService.cpp
)
target_include_directories(nv_app PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(nv_app PUBLIC nv_domain)
target_compile_options(nv_app PRIVATE ${NV_WARNING_FLAGS})

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

find_package(Qt6 6.7 REQUIRED COMPONENTS Widgets Core Gui ShaderTools Network)
# GuiPrivate(QRhi rhi/qrhi.h)는 brew/소스빌드 Qt엔 cmake 패키지가 있지만, aqtinstall 바이너리 Qt엔
# 모듈 private cmake 설정(Qt6GuiPrivate 등)이 빠져 있다. 헤더 자체는 설치되므로 OPTIONAL로 찾고,
# 없으면 아래 nv_ui에서 private include 경로를 수동 지정한다.
find_package(Qt6 6.7 QUIET OPTIONAL_COMPONENTS GuiPrivate)

add_library(nv_infra STATIC
  infra/system/ControlExecutor.cpp
  infra/system/SoakLogger.cpp
  infra/ffmpeg/FfmpegStreamSource.cpp
  infra/ffmpeg/FfmpegRecorder.cpp
  infra/ffmpeg/HwContext.cpp
  infra/ffmpeg/ChannelSourceFactory.cpp
  infra/persist/JsonChannelRepository.cpp
  infra/persist/PngSnapshotWriter.cpp
  infra/relay/MediaMtxConfigWriter.cpp
  infra/relay/HttpRelayControlApi.cpp
  infra/relay/RelayServiceManagerFactory.cpp
  infra/system/ResourceMonitor.cpp
)
target_include_directories(nv_infra PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_options(nv_infra PRIVATE ${NV_WARNING_FLAGS})

if(WIN32)
  # Windows/vcpkg: FFMPEG_INCLUDE_DIRS + FFMPEG_LIBRARIES (FindFFMPEG.cmake 변수).
  # D3D11VA HW 디코딩은 HwContext.cpp의 #ifdef _WIN32 분기에서 활성.
  # d3d11 / dxgi는 FFmpeg avcodec이 내부적으로 요구; 명시 링크로 확실히 해결.
  target_include_directories(nv_infra PUBLIC ${FFMPEG_INCLUDE_DIRS})
  # Qt6::Gui — PngSnapshotWriter의 QImage(PNG 저장). Core는 JsonChannelRepository/RecordingPaths.
  target_link_libraries(nv_infra PUBLIC nv_app Qt6::Core Qt6::Gui Qt6::Network ${FFMPEG_LIBRARIES} d3d11 dxgi)
else()
  # macOS/Linux: pkg-config IMPORTED_TARGET 사용.
  # Qt6::Gui — PngSnapshotWriter의 QImage(PNG 저장).
  # Qt6::Network — HttpRelayControlApi(Control API HTTP 조회).
  target_link_libraries(nv_infra PUBLIC nv_app PkgConfig::FFMPEG Qt6::Core Qt6::Gui Qt6::Network)
endif()

# macOS: VideoToolbox HW 디코딩 — CVPixelBuffer 수명 관리에 CoreVideo 필요.
# + VtMetalBridge(.mm, Obj-C++): CVPixelBuffer(NV12)→Metal 2-plane 텍스처(zero-copy, M2c).
#   Metal/CoreVideo/Foundation 프레임워크 링크. 비-Apple은 .mm 제외(헤더만 — Obj-C++는
#   Apple 전용이라 컴파일 불가; VtMetalBridge 호출측이 아직 없어 심볼 누락 없음).
if(APPLE)
  target_sources(nv_infra PRIVATE
    infra/video/VtMetalBridge.mm
    infra/relay/LaunchdRelayService.cpp)
  target_link_libraries(nv_infra PUBLIC
    "-framework CoreVideo"
    "-framework Metal"
    "-framework Foundation")
elseif(WIN32)
  target_sources(nv_infra PRIVATE
    infra/relay/WindowsRelayService.cpp)
endif()

set(CMAKE_AUTORCC ON)

add_library(nv_ui STATIC
  ui/shell/MainWindow.cpp
  ui/shell/Toast.cpp
  ui/grid/VideoTileWidget.cpp
  ui/grid/GridView.cpp
  ui/render/SwVideoRenderer.cpp
  ui/render/RhiVideoRenderer.cpp
  ui/channels/ChannelDialog.cpp
  ui/channels/ChannelListPanel.cpp
  ui/channels/ChannelInfoDialog.cpp
  ui/shell/LogPanel.cpp
  ui/panels/FilePanel.cpp
  ui/relay/RelayCoordinator.cpp
  ui/shell/ControlBridge.h
  ui/shell/RepaintClock.h
  ui/shell/Toast.h
  ${CMAKE_SOURCE_DIR}/resources/resources.qrc
)
target_include_directories(nv_ui PUBLIC ${CMAKE_SOURCE_DIR})
# QRhi(rhi/qrhi.h) 헤더 접근(QRhiWidget GPU 렌더러).
target_link_libraries(nv_ui PUBLIC nv_infra Qt6::Widgets)
if(TARGET Qt6::GuiPrivate)
  target_link_libraries(nv_ui PUBLIC Qt6::GuiPrivate)
else()
  # aqtinstall 바이너리 Qt: Qt6GuiPrivate cmake 패키지 없음. private 헤더는 설치돼 있으므로
  # rhi/qrhi.h 가 들어있는 versioned private include 경로를 직접 추가한다.
  # 경로: <QtPrefix>/include/Qt{Gui,Core}/<ver>[/Qt{Gui,Core}]
  get_filename_component(_qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
  set(_qt_priv_cands
    "${_qt_prefix}/include/QtGui/${Qt6_VERSION}/QtGui"
    "${_qt_prefix}/include/QtGui/${Qt6_VERSION}"
    "${_qt_prefix}/include/QtCore/${Qt6_VERSION}/QtCore"
    "${_qt_prefix}/include/QtCore/${Qt6_VERSION}"
  )
  set(_qt_rhi_found FALSE)
  foreach(_c ${_qt_priv_cands})
    if(EXISTS "${_c}")
      target_include_directories(nv_ui PRIVATE "${_c}")
    endif()
    if(EXISTS "${_c}/rhi/qrhi.h")
      set(_qt_rhi_found TRUE)
    endif()
  endforeach()
  if(NOT _qt_rhi_found)
    message(FATAL_ERROR
      "Qt6::GuiPrivate 미존재 + rhi/qrhi.h 미발견 (QtPrefix=${_qt_prefix}, ver=${Qt6_VERSION}). "
      "Qt private 헤더 레이아웃 확인 필요.")
  endif()
  target_link_libraries(nv_ui PUBLIC Qt6::Gui)
  message(STATUS "Qt6::GuiPrivate 없음(aqt) → private include 수동 추가 완료")
endif()
target_compile_options(nv_ui PRIVATE ${NV_WARNING_FLAGS})

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
