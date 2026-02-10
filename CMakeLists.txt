cmake_minimum_required(VERSION 3.24)

project(StockFlow LANGUAGES CXX)

# C++ 표준 설정
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC 컴파일러 옵션
if (MSVC)
  add_compile_options(/Zc:__cplusplus)
endif()

# Qt 자동 MOC/UIC/RCC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Qt6
find_package(Qt6 REQUIRED COMPONENTS Widgets)

# 하위 모듈 등록 (의존성 없는 것부터)
add_subdirectory(src/core)
add_subdirectory(src/ui)
add_subdirectory(src/app) # 실행 파일

file(COPY "${CMAKE_SOURCE_DIR}/kospi_code.mst" DESTINATION "${CMAKE_BINARY_DIR}/src/app")
file(COPY "${CMAKE_SOURCE_DIR}/kosdaq_code.mst" DESTINATION "${CMAKE_BINARY_DIR}/src/app")