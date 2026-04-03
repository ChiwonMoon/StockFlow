#pragma once

#include <QMainWindow>
#include <QString>
#include <QStringList>
#include "core/StockSymbolResolver.h"
#include "core/StockListSettings.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

struct StockData;

class FinnhubAPI;
class KisAPI;
class QEvent;
class QPoint;
class QTimer;
class QStringListModel;
class StockTableModel;
class StartupCoordinator;
class StockRequestCoordinator;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

private:
    void setupUiState();
    void setupTable();
    void createApis();
    void setupConnections();
    void setupTimers();
    void setupSearch();
    void startServices();
    void onInitialReady();

private slots:
    void onRefreshClicked();
    void updateUI(const StockData& data);
    void onSearchClicked();
    void onSearchTextEdited(const QString &text);
    void onTableContextMenu(const QPoint& pos);

private:
    Ui::MainWindow* ui = nullptr;;
    FinnhubAPI *m_usApi = nullptr;;
    KisAPI *m_krApi = nullptr;;
    StockTableModel* m_stockModel = nullptr;
    QTimer* m_timer = nullptr;                    // 갱신타이머
    QStringListModel* m_searchModel = nullptr;
    QTimer* m_debounceTimer = nullptr;            // 검색지연타이머
    QString m_pendingText;
    QString m_lastSearchText;
    StartupCoordinator* m_startupCoordinator = nullptr;
    StockRequestCoordinator* m_requestCoordinator = nullptr;
    StockSymbolResolver m_symbolResolver;
    StockListSettings m_stockListSettings;

    void updateSearchCompleter();
    void performSearch();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    
};