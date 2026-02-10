#include <QApplication>
#include "ui/mainwindow.h"
#include "core/StockAPI.h"
#include <qdebug.h>
#include "core/StockCodeMap.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    StockCodeMap::loadFromMstFiles();

    MainWindow w;
    w.show();
    return app.exec();
}