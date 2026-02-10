# src/core/CMakeLists.txt

# Qt Network 패키지 찾기
find_package(Qt6 REQUIRED COMPONENTS Core Network)

# Core 라이브러리 생성
add_library(stockflow_core STATIC
    Config.h
    NetworkUtils.h
    StockData.h
    StockAPI.h
    StockAPI.cpp
    KisAPI.h
    KisAPI.cpp
    FinnhubAPI.h
    FinnhubAPI.cpp
    StockCodeMap.h
    StockCodeMap.cpp
)

# 라이브러리 연결
target_link_libraries(stockflow_core
    PUBLIC
        Qt6::Core
        Qt6::Network
        Qt6::Gui
)

# 포함 경로 설정
target_include_directories(stockflow_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)