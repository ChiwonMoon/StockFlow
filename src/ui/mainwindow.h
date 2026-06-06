#pragma once

#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QHash>
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
class QTableView;
class QDialog;
class QSpinBox;
class QFormLayout;
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
    void onOpenOrdersClicked();

private:
    Ui::MainWindow* ui = nullptr;;
    FinnhubAPI *m_usApi = nullptr;;
    KisAPI *m_krApi = nullptr;;
    StockTableModel* m_stockModel = nullptr;        // 관심종목 탭
    StockTableModel* m_holdingsModel = nullptr;     // 보유종목 탭
    QSet<QString> m_holdingSymbols;                 // 보유종목 코드 집합 (시세 라우팅용)
    QHash<QString, int> m_holdingQty;               // 종목 → 보유수량 (전량/상한용)
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
    void loadHoldings();                                                    // 보유종목 탭 로드
    // 매도 다이얼로그에 수량행(전량 버튼 + 보유수량 표시 + 보유수량 상한) 추가
    void addSellQtyRow(QDialog* dlg, const QString& symbol, QSpinBox* qtySpin, QFormLayout* form);
    void showStockContextMenu(QTableView* view, StockTableModel* model, const QPoint& pos);
    void reserveSellFor(StockTableModel* model, int row);                  // 예약매도 다이얼로그
    void tradeDialog(StockTableModel* model, int row, bool isBuy);         // 즉시 매수/매도 다이얼로그
    void showAskingPrice(StockTableModel* model, int row);                 // 10호가 다이얼로그

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    
};