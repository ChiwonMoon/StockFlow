# src/ui/CMakeLists.txt

# 라이브러리 생성
add_library(stockflow_ui
    STATIC
        mainwindow.h
        mainwindow.cpp
        mainwindow.ui
        StockTableModel.h
        StockTableModel.cpp
        StockItemDelegate.h
        StockItemDelegate.cpp
 )

# Qt 연결
target_link_libraries(stockflow_ui
    PUBLIC
        Qt6::Widgets
        stockflow_core
)

# 포함 경로 설정 변경
target_include_directories(stockflow_ui
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
)