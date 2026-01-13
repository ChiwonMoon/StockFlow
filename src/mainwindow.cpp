#include "mainwindow.h"
#include <QWidget>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    // 기본 크기만 설정
    resize(800, 600);
    setWindowTitle("PortfolioApp");
}

MainWindow::~MainWindow() = default;