#pragma once

#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QHash>
#include <QList>
#include <QDateTime>
#include "core/StockSymbolResolver.h"
#include "core/StockListSettings.h"
#include "core/KisAPI.h"

// 앱 자체 SOR 예약주문 한 건 (정한 시각에 order-cash SOR 발사)
struct ScheduledOrder
{
    QString symbol;
    QString name;
    bool isBuy = false;   // true: 매수, false: 매도
    int qty = 0;
    double price = 0;
    bool marketPrice = false;
    QDateTime fireTime;   // 다음 발사 시각
    bool daily = false;   // 매일 반복 여부
};

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
    void onScheduledOrdersClicked();  // SOR 예약주문 목록/취소
    void checkScheduledOrders();      // 발사 시각 도달분 발사
    void onWindowsTasksClicked();     // 윈도우 예약매도(작업 스케줄러) 목록/취소

private:
    Ui::MainWindow* ui = nullptr;;
    FinnhubAPI *m_usApi = nullptr;;
    KisAPI *m_krApi = nullptr;;
    StockTableModel* m_stockModel = nullptr;        // 관심종목 탭
    StockTableModel* m_holdingsModel = nullptr;     // 보유종목 탭
    QSet<QString> m_holdingSymbols;                 // 보유종목 코드 집합 (시세 라우팅용)
    QHash<QString, int> m_holdingQty;               // 종목 → 보유수량 (전량/상한용)
    QHash<QString, KisHoldingDetail> m_holdingDetail; // 종목 → 매수/매도 현황
    QList<ScheduledOrder> m_scheduledOrders;        // 앱 자체 SOR 예약주문 목록(매수/매도)
    QTimer* m_scheduleTimer = nullptr;              // SOR 예약 발사 체크 타이머
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
    void refreshBalanceIfPossible();                                        // 잔고 자동 갱신(경고창 없이)
    void loadScheduledOrders();                                             // SOR 예약 복원 (시작 시)
    void saveScheduledOrders();                                             // SOR 예약 저장 (변경 시)
    // 매도 다이얼로그에 수량행(전량 버튼 + 보유수량 표시 + 보유수량 상한) 추가
    void addSellQtyRow(QDialog* dlg, const QString& symbol, QSpinBox* qtySpin, QFormLayout* form);
    // 매수 다이얼로그에 수량행(최대 버튼 + 주문가능현금/최대매수 표시) 추가
    void addBuyQtyRow(QDialog* dlg, const QString& symbol, QSpinBox* qtySpin, QFormLayout* form, double price);
    void showStockContextMenu(QTableView* view, StockTableModel* model, const QPoint& pos);
    void reserveSellFor(StockTableModel* model, int row);                  // KIS 예약매도(KRX) 다이얼로그
    void scheduleOrderDialog(StockTableModel* model, int row, bool isBuy);  // 앱 SOR 예약주문 등록 다이얼로그
    void tradeDialog(StockTableModel* model, int row, bool isBuy);         // 즉시 매수/매도 다이얼로그
    void showAskingPrice(StockTableModel* model, int row);                 // 10호가 다이얼로그
    void showOrderStatus(StockTableModel* model, int row);                 // 매수매도현황 다이얼로그

    // 다이얼로그용 현재가. 표에 시세가 아직 없으면 잔고조회로 받아둔 값을 쓴다.
    double currentPriceFor(StockTableModel* model, int row, const QString& symbol) const;

    // 가격 입력칸을 그 종목의 호가단위(틱)에 맞춘다.
    // 틱에 안 맞는 가격을 내면 KIS 가 '주식주문 호가단위 오류'로 거부한다.
    static void applyTick(QSpinBox* priceSpin, double tick, double currentPrice);
    // 가격을 틱 배수로 맞춘다. 항상 내림 - 올리면 주문이 안 나갈 수 있어서다.
    static int alignToTick(double price, double tick);
    // 주문 가격이 속한 구간의 호가단위 (일반주식 기준)
    static double tickForPrice(double price);
    // 종목 종류까지 반영한 호가단위. ETF 처럼 구간표를 안 따르는 종목은 실측값을 쓴다.
    static double resolveTick(double orderPrice, double currentPrice, double measuredTick);
    // 종목코드로 호가단위를 찾는다 (정정 다이얼로그처럼 행 정보가 없을 때)
    double tickForSymbol(const QString& symbol) const;

    // --- 윈도우 예약매도 (tools/*.ps1 + 작업 스케줄러) ---------------------
    // 앱 SOR 예약과 달리 앱이 꺼져 있어도, PC 가 절전이어도 발사된다.
    // 예약 정보는 앱이 아니라 윈도우 작업 스케줄러가 보관한다.
    void windowsScheduleSellDialog(StockTableModel* model, int row);       // 등록 다이얼로그
    static QString toolsScriptPath(const QString& fileName);               // tools/*.ps1 경로 해석
    // powershell.exe 로 스크립트 실행. 성공 여부 반환, 출력은 output 에 담는다.
    bool runToolScript(const QString& scriptPath, const QStringList& scriptArgs,
                       QString* output, int timeoutMs = 30000);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    
};