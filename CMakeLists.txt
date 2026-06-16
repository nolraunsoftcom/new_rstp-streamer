cmake_minimum_required(VERSION 3.24)
project(new_viewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

# 경고 플래그 — MSVC는 GCC/Clang 스타일(-Wall -Wextra -Wpedantic)을 모름(D8021).
# 루트에 정의해 src/tests 서브디렉터리 모두에서 사용.
if(MSVC)
  set(NV_WARNING_FLAGS /W3)
else()
  set(NV_WARNING_FLAGS -Wall -Wextra -Wpedantic)
endif()

# Windows Release에 디버그 심볼(PDB) 생성 — 크래시 덤프(.dmp)를 심볼로 분석 가능하게.
# /Zi는 코드 생성/최적화에 영향 없음(별도 PDB). new_viewer.pdb가 exe 옆에 생성되어 아티팩트 포함.
if(MSVC)
  add_compile_options($<$<CONFIG:Release>:/Zi>)
  add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
