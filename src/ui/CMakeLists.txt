# src/ui/CMakeLists.txt

# ¶óÀÌºê·¯¸® »ı¼º
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

# Qt ¿¬°á
target_link_libraries(stockflow_ui
    PUBLIC
        Qt6::Widgets
        stockflow_core
)

# Æ÷ÇÔ °æ·Î ¼³Á¤ º¯°æ
target_include_directories(stockflow_ui
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
)
# ê°œë°œ ë¹Œë“œì—ì„œ tools/*.ps1 ìœ„ì¹˜ë¥¼ ëŸ°íƒ€ì„ì— ì°¾ì„ ìˆ˜ ìˆê²Œ ì†ŒìŠ¤ ê²½ë¡œë¥¼ ë°•ì•„ë‘”ë‹¤
# (ë°°í¬ ì‹œì—ëŠ” ì‹¤í–‰íŒŒì¼ ì˜† tools/ ë¥¼ ë¨¼ì € ì°¾ìœ¼ë¯€ë¡œ ì´ ê°’ì€ í´ë°±ì´ë‹¤)
target_compile_definitions(stockflow_ui PRIVATE STOCKFLOW_TOOLS_DIR="${CMAKE_SOURCE_DIR}/tools")
