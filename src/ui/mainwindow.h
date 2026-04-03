#pragma once

#include <QMainWindow>

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
    Ui::MainWindow* ui;
    FinnhubAPI *m_usApi;
    KisAPI *m_krApi;
    StockTableModel* m_stockModel;
    QStringList m_symbols;
    QTimer* m_timer;                    // 갱신타이머
    QStringListModel* m_searchModel;
    QTimer* m_debounceTimer;            // 검색지연타이머
    QString m_pendingText;
    QString m_lastSearchText;
    StartupCoordinator* m_startupCoordinator = nullptr;
    StockRequestCoordinator* m_requestCoordinator = nullptr;

    void updateSearchCompleter();
    void performSearch();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    
};