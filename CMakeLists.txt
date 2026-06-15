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

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
