cmake_minimum_required(VERSION 3.24)

project(PortfolioApp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
  add_compile_options(/Zc:__cplusplus)
endif()

set(CMAKE_PREFIX_PATH "E:\\QT\\6.10.1\\msvc2022_64\\lib\\cmake\\Qt6")

# Qt 자동 MOC/UIC/RCC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Qt6
find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(PortfolioApp
    src/main.cpp
    src/mainwindow.cpp
    include/mainwindow.h
    resources/mainwindow.ui
)

target_include_directories(PortfolioApp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(PortfolioApp PRIVATE
    Qt6::Widgets
)

# 콘솔 제거 (Windows GUI 앱)
set_target_properties(PortfolioApp PROPERTIES
    WIN32_EXECUTABLE TRUE
)