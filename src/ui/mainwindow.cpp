#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "StockItemDelegate.h"
#include "StockTableModel.h"
#include "core/FinnhubAPI.h"
#include "core/KisAPI.h"
#include "core/Config.h"
#include "core/StockCodeMap.h"
#include "core/StartupCoordinator.h"
#include "core/StockRequestCoordinator.h"

#include <QCheckBox>
#include <QColor>
#include <QCompleter>
#include <QCoreApplication>
#include <QCursor>
#include <cmath>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTime>
#include <QUrl>
#include <QDate>
#include <QDateEdit>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QStringListModel>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    setupUiState();
    setupTable();
    createApis();
    setupConnections();
    setupTimers();
    setupSearch();
    startServices();
}

MainWindow::~MainWindow()
{
    // 앱 종료 직전에 현재 테이블의 모든 심볼 저장
    m_stockListSettings.save(m_stockModel->getAllSymbols());

    delete ui;
}

void MainWindow::setupUiState()
{
    // 초기설정
    ui->editSearch->installEventFilter(this);
    ui->editSearch->setEnabled(false);
    ui->btnSearch->setEnabled(false);
    ui->editSearch->setPlaceholderText("데이터 로딩 중... 잠시만 기다려주세요");
}

void MainWindow::setupTable()
{
    // 관심종목 / 보유종목 두 탭 모델 설정
    m_stockModel = new StockTableModel(this);
    m_holdingsModel = new StockTableModel(this);

    ui->tableView->setModel(m_stockModel);
    ui->holdingsView->setModel(m_holdingsModel);
    m_holdingsModel->setHoldingsMode(true);   // 수량/매입가/평가손익 컬럼 표시

    for (QTableView* view : { ui->tableView, ui->holdingsView })
    {
        view->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); // 화면 너비에 맞게 늘리기
        view->setContextMenuPolicy(Qt::CustomContextMenu);                    // 우클릭메뉴 활성화
        view->setItemDelegate(new StockItemDelegate(this));
    }

    m_searchModel = new QStringListModel(this);
}

void MainWindow::createApis()
{
    // API 객체 생성
    m_usApi = new FinnhubAPI(this);
    m_krApi = new KisAPI(this);
    m_startupCoordinator = new StartupCoordinator(m_krApi, &m_stockListSettings, this);
    m_requestCoordinator = new StockRequestCoordinator(m_usApi, m_krApi, this);
}

void MainWindow::setupConnections()
{
    // 버튼 및 입력
    ui->btnRefresh->setShortcut(Qt::Key_F5);
    connect(ui->btnRefresh, &QPushButton::clicked, this, &MainWindow::onRefreshClicked);
    connect(ui->editSearch, &QLineEdit::returnPressed, ui->btnSearch, &QPushButton::click);
    connect(ui->btnSearch, &QPushButton::clicked, this, &MainWindow::onSearchClicked);
    connect(ui->btnOpenOrders, &QPushButton::clicked, this, &MainWindow::onOpenOrdersClicked);
    connect(ui->btnSchedSells, &QPushButton::clicked, this, &MainWindow::onScheduledOrdersClicked);
    connect(ui->btnWinTasks, &QPushButton::clicked, this, &MainWindow::onWindowsTasksClicked);

    // 우클릭 메뉴 (관심종목/보유종목 각 탭의 모델로 동작)
    connect(ui->tableView, &QTableView::customContextMenuRequested, this,
        [this](const QPoint& p) { showStockContextMenu(ui->tableView, m_stockModel, p); });
    connect(ui->holdingsView, &QTableView::customContextMenuRequested, this,
        [this](const QPoint& p) { showStockContextMenu(ui->holdingsView, m_holdingsModel, p); });

    // 보유종목 탭으로 전환하면 자동 로드 (버튼 없이)
    connect(ui->tabWidget, &QTabWidget::currentChanged, this,
        [this](int index) { if (ui->tabWidget->widget(index) == ui->tabHoldings) loadHoldings(); });

    // 데이터 수신
    connect(m_usApi, &StockAPI::dataReceived, this, &MainWindow::updateUI);
    connect(m_krApi, &KisAPI::dataReceived, this, &MainWindow::updateUI);

    // 로고 (두 모델 모두 갱신; 해당 심볼 없으면 no-op)
    auto updateLogos = [this](QString symbol, QPixmap logo)
    {
        m_stockModel->updateLogo(symbol, logo);
        m_holdingsModel->updateLogo(symbol, logo);
    };
    connect(m_usApi, &StockAPI::logoReceived, this, updateLogos);
    connect(m_krApi, &StockAPI::logoReceived, this, updateLogos);

    // 한국투자증권 로그인 토큰 발급
    connect(m_startupCoordinator, &StartupCoordinator::initialReady, this, &MainWindow::onInitialReady);

    // 예약매도 접수 결과 알림
    connect(m_krApi, &KisAPI::orderReserved, this,
        [this](const QString& symbol, bool success, const QString& message)
        {
            if (success)
                QMessageBox::information(this, "예약매도 접수",
                    QString("[%1] 예약매도가 접수되었습니다.\n%2\n\n※ 처리 결과는 통보되지 않으니 주문처리일 장 시작 전 반드시 확인하세요.")
                        .arg(symbol, message));
            else
                QMessageBox::warning(this, "예약매도 실패",
                    QString("[%1] 예약매도 접수에 실패했습니다.\n%2").arg(symbol, message));
        });

    // 보유종목 수신 → 보유종목 탭에 로드
    connect(m_krApi, &KisAPI::holdingsReceived, this,
        [this](const QStringList& symbols)
        {
            m_holdingSymbols = QSet<QString>(symbols.begin(), symbols.end());
            m_holdingsModel->clear();                    // 청산된 종목 제거
            if (!symbols.isEmpty())
                m_requestCoordinator->refreshStocks(symbols); // 실시간 시세 → updateUI가 보유 모델로 라우팅
        });

    // 보유수량 맵 캐시 (전량 버튼/매도 수량 상한용)
    connect(m_krApi, &KisAPI::holdingQuantitiesReceived, this,
        [this](const QHash<QString, int>& quantities) { m_holdingQty = quantities; });

    // 매수/매도 현황 캐시 (잔고조회에 같이 실려 온다) + 보유종목 표에 반영
    connect(m_krApi, &KisAPI::holdingDetailsReceived, this,
        [this](const QHash<QString, KisHoldingDetail>& details)
        {
            m_holdingDetail = details;
            m_holdingsModel->setHoldingDetails(details);
        });

    // 즉시 매수/매도 접수 결과 알림
    connect(m_krApi, &KisAPI::orderPlaced, this,
        [this](const QString& symbol, bool isBuy, bool success, const QString& message)
        {
            const QString kind = isBuy ? "매수" : "매도";
            if (success)
            {
                QMessageBox::information(this, kind + " 접수",
                    QString("[%1] %2 주문이 접수되었습니다.\n%3").arg(symbol, kind, message));
                // 주문이 들어가면 주문가능수량이 바로 줄어든다. 표를 맞춰준다.
                refreshBalanceIfPossible();
            }
            else
                QMessageBox::warning(this, kind + " 실패",
                    QString("[%1] %2 주문 접수에 실패했습니다.\n%3").arg(symbol, kind, message));
        });
}

void MainWindow::setupTimers()
{
    // 자동 갱신
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &MainWindow::onRefreshClicked);
    //connect(m_timer, &QTimer::timeout, []() { qDebug() << "Auto refresh time out"; });

    m_debounceTimer = new QTimer(this);
    m_debounceTimer->setSingleShot(true);
    m_debounceTimer->setInterval(300);
    connect(m_debounceTimer, &QTimer::timeout, this, &MainWindow::performSearch);

    // SOR 예약매도 발사 체크 (5초마다 도달분 확인)
    m_scheduleTimer = new QTimer(this);
    m_scheduleTimer->setInterval(5000);
    connect(m_scheduleTimer, &QTimer::timeout, this, &MainWindow::checkScheduledOrders);
    m_scheduleTimer->start();
}

void MainWindow::setupSearch()
{
    // 검색
    connect(m_usApi, &FinnhubAPI::symbolsReceived, this, &MainWindow::updateSearchCompleter);
    connect(ui->editSearch, &QLineEdit::textEdited, this, &MainWindow::onSearchTextEdited);
}

void MainWindow::startServices()
{
    // 저장된 SOR 예약주문 복원
    loadScheduledOrders();

    // 미국 주식 심볼 전체 가져오기
    m_usApi->fetchAllUSSymblos();

    // 시작 흐름 시작
    m_startupCoordinator->start();
}

void MainWindow::onInitialReady()
{
    const QStringList symbols = m_startupCoordinator->initialSymbols();

    m_requestCoordinator->refreshStocks(symbols);

    // 자동갱신타이머
    m_timer->start(10000);
}

void MainWindow::updateUI(const StockData& data)
{
    const bool isHolding = m_holdingSymbols.contains(data.symbol);

    // 보유종목이면 보유 탭 갱신
    if (isHolding)
        m_requestCoordinator->handleStockData(data, m_holdingsModel);

    // 관심종목 탭: 이미 목록에 있으면 갱신, 보유종목 전용이 아니면 신규 추가
    if (m_stockModel->contains(data.symbol) || !isHolding)
        m_requestCoordinator->handleStockData(data, m_stockModel);
}

void MainWindow::onRefreshClicked()
{
    // 시세 갱신: 관심종목 + 보유종목 심볼 모두 (updateUI가 알맞은 탭으로 라우팅)
    QStringList symbols = m_stockModel->getAllSymbols();
    symbols += QStringList(m_holdingSymbols.begin(), m_holdingSymbols.end());
    symbols.removeDuplicates();
    if (!symbols.isEmpty())
        m_requestCoordinator->refreshStocks(symbols);

    // 잔고 갱신: 수량/주문가능/매입가/평가손익은 시세가 아니라 잔고조회로만 들어온다.
    // 이걸 안 부르면 보유종목 표의 그 칸들이 탭을 다시 누를 때까지 멈춰 있다.
    refreshBalanceIfPossible();
}

// 계좌 설정이 되어 있을 때만 잔고를 부른다.
// loadHoldings()는 미설정 시 경고창을 띄우므로 타이머에서 부르면 창이 계속 뜬다.
void MainWindow::refreshBalanceIfPossible()
{
    if (Config::KIS_ACCOUNT_CANO.isEmpty() || Config::KIS_ACCOUNT_CANO.startsWith("여기에"))
        return;
    m_krApi->fetchBalance();
}

void MainWindow::loadHoldings()
{
    // 계좌번호 미설정 방어
    if (Config::KIS_ACCOUNT_CANO.isEmpty() || Config::KIS_ACCOUNT_CANO.startsWith("여기에"))
    {
        QMessageBox::warning(this, "설정 필요",
            "Config.h의 KIS_ACCOUNT_CANO(종합계좌번호 앞 8자리)를 먼저 설정하세요.");
        return;
    }

    m_krApi->fetchBalance(); // 응답은 holdingsReceived → 보유 탭에 반영
}

void MainWindow::addSellQtyRow(QDialog* dlg, const QString& symbol, QSpinBox* qtySpin, QFormLayout* form)
{
    // 캐시된 보유수량이 있으면 즉시 상한 적용
    const int cached = m_holdingQty.value(symbol, -1);
    if (cached > 0)
        qtySpin->setMaximum(cached);

    QPushButton* allBtn = new QPushButton("전량", dlg);
    QHBoxLayout* qtyRow = new QHBoxLayout();
    qtyRow->addWidget(qtySpin);
    qtyRow->addWidget(allBtn);
    form->addRow("수량", qtyRow);

    QLabel* heldLabel = new QLabel(dlg);
    form->addRow("", heldLabel);

    auto held = std::make_shared<int>(cached);
    auto applyHeld = [held, heldLabel, qtySpin]()
    {
        if (*held < 0) { heldLabel->setText("보유수량: 조회 중..."); return; }
        heldLabel->setText(QString("보유수량: %1주 (이보다 많이 매도 불가)").arg(*held));
        if (*held > 0)
            qtySpin->setMaximum(*held); // 보유수량 상한 (초과 값은 자동 클램프)
    };
    applyHeld();

    // 잔고 응답이 오면 상한/표시 갱신 (수명은 다이얼로그에 종속)
    connect(m_krApi, &KisAPI::holdingQuantitiesReceived, dlg,
        [held, applyHeld, symbol](const QHash<QString, int>& q)
        {
            *held = q.value(symbol, 0);
            applyHeld();
        });

    // 전량 버튼 → 보유수량으로 채움
    connect(allBtn, &QPushButton::clicked, dlg,
        [held, qtySpin, dlg]()
        {
            if (*held > 0) qtySpin->setValue(*held);
            else QMessageBox::information(dlg, "전량", "보유수량이 없거나 아직 조회되지 않았습니다.");
        });

    m_krApi->fetchBalance(); // 최신 보유수량 갱신
}

void MainWindow::addBuyQtyRow(QDialog* dlg, const QString& symbol, QSpinBox* qtySpin, QFormLayout* form, double price)
{
    QPushButton* maxBtn = new QPushButton("최대", dlg);
    QHBoxLayout* qtyRow = new QHBoxLayout();
    qtyRow->addWidget(qtySpin);
    qtyRow->addWidget(maxBtn);
    form->addRow("수량", qtyRow);

    QLabel* cashLabel = new QLabel("주문가능현금: 조회 중...", dlg);
    form->addRow("", cashLabel);

    // 주문가능현금/최대매수수량 응답 보관 (최대 버튼용). 수명은 다이얼로그에 종속
    auto maxQty = std::make_shared<int>(-1);
    connect(m_krApi, &KisAPI::orderableCashReceived, dlg,
        [cashLabel, symbol, maxQty](const QString& s, qint64 cash, int mq)
        {
            if (s != symbol) return;
            *maxQty = mq;
            cashLabel->setText(QString("주문가능현금: %1원  /  최대매수: %2주")
                .arg(QLocale::system().toString(cash), QString::number(mq)));
        });

    // 최대 버튼 → 최대매수수량으로 채움
    connect(maxBtn, &QPushButton::clicked, dlg,
        [maxQty, qtySpin, dlg]()
        {
            if (*maxQty > 0) qtySpin->setValue(*maxQty);
            else QMessageBox::information(dlg, "최대수량", "최대매수수량이 없거나 아직 조회되지 않았습니다.");
        });

    m_krApi->fetchOrderableCash(symbol, price, false); // 현재가 기준 조회
}

void MainWindow::onSearchClicked()
{
    const QString targetSymbol = m_symbolResolver.resolve(ui->editSearch->text());
    if (targetSymbol.isEmpty())
        return;

    m_requestCoordinator->requestStock(targetSymbol);

    ui->editSearch->clear();
}

void MainWindow::onSearchTextEdited(const QString& text)
{
    m_pendingText = text;
    m_debounceTimer->start();
    //qDebug() << "검색타이머";
}

void MainWindow::showStockContextMenu(QTableView* view, StockTableModel* model, const QPoint& pos)
{
    QModelIndex index = view->indexAt(pos);
    if (!index.isValid()) return;
    const int row = index.row();

    // 매수/매도/예약매도/호가는 국내(6자리 코드) 종목만 (KIS 국내주식 API)
    const QString symbol = model->symbolAt(row);
    static const QRegularExpression krRe("^[0-9]{6}$");
    const bool isKorean = krRe.match(symbol).hasMatch();

    QMenu menu(this);
    QAction* buyAction = isKorean ? menu.addAction("매수 (Buy)") : nullptr;
    QAction* sellAction = isKorean ? menu.addAction("매도 (Sell)") : nullptr;
    QAction* reserveSellAction = isKorean ? menu.addAction("예약매도 (KIS·KRX)") : nullptr;
    QAction* schedBuyAction = isKorean ? menu.addAction("SOR 예약매수 (앱·NXT포함)") : nullptr;
    QAction* schedSellAction = isKorean ? menu.addAction("SOR 예약매도 (앱·NXT포함)") : nullptr;
    QAction* winSchedSellAction = isKorean ? menu.addAction("윈도우 예약매도 (앱꺼도OK·절전깨움)") : nullptr;
    QAction* askingPriceAction = isKorean ? menu.addAction("호가 (10호가)") : nullptr;
    QAction* orderStatusAction = isKorean ? menu.addAction("매수매도현황") : nullptr;

    // 보유종목 탭은 계좌에서 받아오는 목록이라 임의로 지우는 게 의미가 없다.
    // (지워도 다음 조회 때 다시 올라온다) 그래서 삭제는 관심종목에서만 제공한다.
    const bool isHoldingsView = (view == ui->holdingsView);
    QAction* deleteAction = nullptr;
    if (!isHoldingsView)
    {
        if (isKorean) menu.addSeparator();
        deleteAction = menu.addAction("삭제 (delete)");
    }

    // 메뉴 띄우고 기다림
    QAction* selectedItem = menu.exec(view->viewport()->mapToGlobal(pos));
    if (selectedItem == nullptr) return; // 사용자가 메뉴 밖을 클릭해서 취소함

    if (selectedItem == buyAction)
        tradeDialog(model, row, true);
    else if (selectedItem == sellAction)
        tradeDialog(model, row, false);
    else if (selectedItem == reserveSellAction)
        reserveSellFor(model, row);
    else if (selectedItem == schedBuyAction)
        scheduleOrderDialog(model, row, true);
    else if (selectedItem == schedSellAction)
        scheduleOrderDialog(model, row, false);
    else if (selectedItem == winSchedSellAction)
        windowsScheduleSellDialog(model, row);
    else if (selectedItem == askingPriceAction)
        showAskingPrice(model, row);
    else if (selectedItem == orderStatusAction)
        showOrderStatus(model, row);
    else if (deleteAction && selectedItem == deleteAction)
        model->removeRow(row);
}

void MainWindow::reserveSellFor(StockTableModel* model, int row)
{
    const QString symbol = model->symbolAt(row);
    const QString name = model->displayNameAt(row);
    const double curPrice = currentPriceFor(model, row, symbol);

    // 계좌번호 미설정 방어 (placeholder 그대로면 주문 불가)
    if (Config::KIS_ACCOUNT_CANO.isEmpty() || Config::KIS_ACCOUNT_CANO.startsWith("여기에"))
    {
        QMessageBox::warning(this, "설정 필요",
            "Config.h의 KIS_ACCOUNT_CANO(종합계좌번호 앞 8자리)를 먼저 설정하세요.");
        return;
    }

    // 입력 다이얼로그 구성
    QDialog dlg(this);
    dlg.setWindowTitle("예약매도 주문");

    QSpinBox* qtySpin = new QSpinBox(&dlg);
    qtySpin->setRange(1, 1000000);
    qtySpin->setValue(1);
    qtySpin->setSuffix(" 주");

    QCheckBox* marketCheck = new QCheckBox("시장가 주문", &dlg);

    QSpinBox* priceSpin = new QSpinBox(&dlg);
    priceSpin->setRange(0, 1000000000);
    priceSpin->setGroupSeparatorShown(true);
    priceSpin->setSuffix(" 원");
    priceSpin->setValue(static_cast<int>(curPrice));
    applyTick(priceSpin, model->tickSizeAt(row), curPrice);

    QCheckBox* periodCheck = new QCheckBox("기간예약 종료일 지정", &dlg);
    QDateEdit* endDateEdit = new QDateEdit(QDate::currentDate().addDays(1), &dlg);
    endDateEdit->setCalendarPopup(true);
    endDateEdit->setDisplayFormat("yyyy-MM-dd");
    endDateEdit->setEnabled(false);

    // 시장가 체크 시 가격 비활성화 / 기간예약 체크 시 날짜 활성화
    connect(marketCheck, &QCheckBox::toggled, priceSpin, &QSpinBox::setDisabled);
    connect(periodCheck, &QCheckBox::toggled, endDateEdit, &QWidget::setEnabled);

    QFormLayout* form = new QFormLayout();
    form->addRow("종목", new QLabel(QString("%1 (%2)").arg(name, symbol), &dlg));
    addSellQtyRow(&dlg, symbol, qtySpin, form); // 수량 + 전량 버튼 + 보유수량 상한
    form->addRow("", marketCheck);
    form->addRow("지정가", priceSpin);
    form->addRow("", periodCheck);
    form->addRow("종료일", endDateEdit);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    layout->addLayout(form);
    layout->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const int qty = qtySpin->value();
    const bool isMarket = marketCheck->isChecked();
    const double price = priceSpin->value();
    const QString endDate = periodCheck->isChecked() ? endDateEdit->date().toString("yyyyMMdd") : QString();

    // 실전 계좌 — 실제 체결되므로 최종 확인
    const QString priceText = isMarket ? "시장가" : (QLocale::system().toString(price, 'f', 0) + "원");
    const QString periodText = endDate.isEmpty() ? "" : ("\n기간예약 종료일: " + endDateEdit->date().toString("yyyy-MM-dd"));
    const QString confirmMsg =
        QString("⚠️ 실전 계좌 예약매도\n\n종목: %1 (%2)\n수량: %3주\n가격: %4%5\n\n정말 예약하시겠습니까?")
            .arg(name, symbol).arg(qty).arg(priceText, periodText);

    if (QMessageBox::warning(this, "예약매도 확인", confirmMsg,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    m_krApi->reserveSellOrder(symbol, qty, price, isMarket, endDate);
}

void MainWindow::scheduleOrderDialog(StockTableModel* model, int row, bool isBuy)
{
    const QString symbol = model->symbolAt(row);
    const QString name = model->displayNameAt(row);
    const double curPrice = currentPriceFor(model, row, symbol);
    const QString kind = isBuy ? "매수" : "매도";

    // 계좌번호 미설정 방어
    if (Config::KIS_ACCOUNT_CANO.isEmpty() || Config::KIS_ACCOUNT_CANO.startsWith("여기에"))
    {
        QMessageBox::warning(this, "설정 필요",
            "Config.h의 KIS_ACCOUNT_CANO(종합계좌번호 앞 8자리)를 먼저 설정하세요.");
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle("SOR 예약" + kind + " (앱)");

    QSpinBox* qtySpin = new QSpinBox(&dlg);
    qtySpin->setRange(1, 1000000);
    qtySpin->setValue(1);
    qtySpin->setSuffix(" 주");

    QCheckBox* marketCheck = new QCheckBox("시장가 주문", &dlg);

    QSpinBox* priceSpin = new QSpinBox(&dlg);
    priceSpin->setRange(0, 1000000000);
    priceSpin->setGroupSeparatorShown(true);
    priceSpin->setSuffix(" 원");
    priceSpin->setValue(static_cast<int>(curPrice));
    applyTick(priceSpin, model->tickSizeAt(row), curPrice);
    connect(marketCheck, &QCheckBox::toggled, priceSpin, &QSpinBox::setDisabled);

    // 발사 시각 기본값: 다음 08:00 (NXT 프리마켓 시작)
    const QDateTime now = QDateTime::currentDateTime();
    QDateTime next8(now.date(), QTime(8, 0));
    if (now.time() >= QTime(8, 0))
        next8 = next8.addDays(1);
    QDateTimeEdit* fireEdit = new QDateTimeEdit(next8, &dlg);
    fireEdit->setDisplayFormat("yyyy-MM-dd HH:mm");
    fireEdit->setCalendarPopup(true);

    QCheckBox* dailyCheck = new QCheckBox("매일 반복 (체결/취소 전까지)", &dlg);

    QFormLayout* form = new QFormLayout();
    form->addRow("종목", new QLabel(QString("%1 (%2)").arg(name, symbol), &dlg));
    if (isBuy)
        addBuyQtyRow(&dlg, symbol, qtySpin, form, curPrice);   // 매수: 최대 버튼 + 주문가능현금
    else
        addSellQtyRow(&dlg, symbol, qtySpin, form);            // 매도: 전량 버튼 + 보유수량 상한
    form->addRow("", marketCheck);
    form->addRow("지정가", priceSpin);
    form->addRow("발사 시각", fireEdit);
    form->addRow("", dailyCheck);

    QLabel* hint = new QLabel("※ 발사 시각에 PC(이 앱)가 켜져 있어야 주문이 나갑니다.\n   거래소는 SOR(KRX+NXT 통합)로 전송됩니다.", &dlg);
    hint->setStyleSheet("color: gray;");

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    layout->addLayout(form);
    layout->addWidget(hint);
    layout->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    ScheduledOrder s;
    s.symbol = symbol;
    s.name = name;
    s.isBuy = isBuy;
    s.qty = qtySpin->value();
    s.marketPrice = marketCheck->isChecked();
    s.price = priceSpin->value();
    s.fireTime = fireEdit->dateTime();
    s.daily = dailyCheck->isChecked();

    // 1회 예약인데 발사 시각이 과거면 거부
    if (!s.daily && s.fireTime <= QDateTime::currentDateTime())
    {
        QMessageBox::warning(this, "시각 오류", "발사 시각이 이미 지났습니다. 미래 시각으로 지정하세요.");
        return;
    }

    const QString priceText = s.marketPrice ? "시장가" : (QLocale::system().toString(s.price, 'f', 0) + "원");
    const QString confirmMsg =
        QString("⚠️ 앱 SOR 예약%1 등록 (실전 계좌)\n\n종목: %2 (%3)\n수량: %4주\n가격: %5\n발사: %6%7\n\n등록할까요?")
            .arg(kind, name, symbol).arg(s.qty)
            .arg(priceText, s.fireTime.toString("yyyy-MM-dd HH:mm"), s.daily ? " (매일 반복)" : "");

    if (QMessageBox::warning(this, "SOR 예약 확인", confirmMsg,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    m_scheduledOrders.append(s);
    saveScheduledOrders();
}

void MainWindow::checkScheduledOrders()
{
    if (m_scheduledOrders.isEmpty())
        return;

    const QDateTime now = QDateTime::currentDateTime();
    bool changed = false;
    for (int i = m_scheduledOrders.size() - 1; i >= 0; --i)
    {
        ScheduledOrder& s = m_scheduledOrders[i];
        if (s.fireTime > now)
            continue;

        qDebug() << "[SOR예약]" << (s.isBuy ? "매수" : "매도") << "발사:" << s.symbol << s.qty << "주"
                 << (s.marketPrice ? "시장가" : QString::number(s.price));
        m_krApi->placeOrder(s.symbol, s.isBuy, s.qty, s.price, s.marketPrice); // SOR, 결과는 orderPlaced로 표시

        if (s.daily)
        {
            // 다음 영업/달력일로 이월 (과거 누락분 방지)
            do { s.fireTime = s.fireTime.addDays(1); } while (s.fireTime <= now);
        }
        else
        {
            m_scheduledOrders.removeAt(i);
        }
        changed = true;
    }

    if (changed)
        saveScheduledOrders();
}

void MainWindow::saveScheduledOrders()
{
    QSettings settings(Config::SETTINGS_COMPANY, Config::SETTINGS_APP);
    settings.remove("sched_orders"); // 기존 배열 제거 후 재기록 (잔여 항목 방지)
    settings.beginWriteArray("sched_orders");
    for (int i = 0; i < m_scheduledOrders.size(); ++i)
    {
        const ScheduledOrder& s = m_scheduledOrders[i];
        settings.setArrayIndex(i);
        settings.setValue("symbol", s.symbol);
        settings.setValue("name", s.name);
        settings.setValue("isBuy", s.isBuy);
        settings.setValue("qty", s.qty);
        settings.setValue("price", s.price);
        settings.setValue("marketPrice", s.marketPrice);
        settings.setValue("fireTime", s.fireTime);
        settings.setValue("daily", s.daily);
    }
    settings.endArray();
}

void MainWindow::loadScheduledOrders()
{
    QSettings settings(Config::SETTINGS_COMPANY, Config::SETTINGS_APP);
    const int n = settings.beginReadArray("sched_orders");
    const QDateTime now = QDateTime::currentDateTime();

    for (int i = 0; i < n; ++i)
    {
        settings.setArrayIndex(i);
        ScheduledOrder s;
        s.symbol = settings.value("symbol").toString();
        s.name = settings.value("name").toString();
        s.isBuy = settings.value("isBuy").toBool();
        s.qty = settings.value("qty").toInt();
        s.price = settings.value("price").toDouble();
        s.marketPrice = settings.value("marketPrice").toBool();
        s.fireTime = settings.value("fireTime").toDateTime();
        s.daily = settings.value("daily").toBool();

        if (s.daily)
        {
            // 앱이 꺼져 지나간 발사는 다음 시각으로 이월
            while (s.fireTime.isValid() && s.fireTime <= now)
                s.fireTime = s.fireTime.addDays(1);
        }
        else if (s.fireTime <= now)
        {
            // 1회 예약인데 발사 시각이 이미 지났으면(앱 꺼져 있던 사이) 폐기
            qDebug() << "[SOR예약] 놓친 1회 예약 폐기:" << s.symbol << s.fireTime;
            continue;
        }

        m_scheduledOrders.append(s);
    }
    settings.endArray();

    // 이월/폐기 반영분 다시 저장
    saveScheduledOrders();
}

void MainWindow::onScheduledOrdersClicked()
{
    QDialog dlg(this);
    dlg.setWindowTitle("SOR 예약목록 (앱)");
    dlg.resize(640, 320);

    QTableWidget* table = new QTableWidget(0, 6, &dlg);
    table->setHorizontalHeaderLabels({ "종목", "구분", "수량", "가격", "발사시각", "반복" });
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    auto refresh = [this, table]()
    {
        QLocale loc = QLocale::system();
        table->setRowCount(m_scheduledOrders.size());
        for (int i = 0; i < m_scheduledOrders.size(); ++i)
        {
            const ScheduledOrder& s = m_scheduledOrders[i];
            table->setItem(i, 0, new QTableWidgetItem(QString("%1 (%2)").arg(s.name, s.symbol)));
            QTableWidgetItem* kindItem = new QTableWidgetItem(s.isBuy ? "매수" : "매도");
            kindItem->setForeground(s.isBuy ? QColor(Qt::red) : QColor(Qt::blue));
            table->setItem(i, 1, kindItem);
            table->setItem(i, 2, new QTableWidgetItem(loc.toString(s.qty)));
            table->setItem(i, 3, new QTableWidgetItem(s.marketPrice ? "시장가" : loc.toString(s.price, 'f', 0)));
            table->setItem(i, 4, new QTableWidgetItem(s.fireTime.toString("yyyy-MM-dd HH:mm")));
            table->setItem(i, 5, new QTableWidgetItem(s.daily ? "매일" : "1회"));
        }
    };
    refresh();

    QPushButton* cancelBtn = new QPushButton("선택 취소", &dlg);
    connect(cancelBtn, &QPushButton::clicked, &dlg, [this, table, refresh]()
    {
        const int r = table->currentRow();
        if (r < 0 || r >= m_scheduledOrders.size())
        {
            QMessageBox::information(table->window(), "취소", "예약을 선택하세요.");
            return;
        }
        m_scheduledOrders.removeAt(r);
        saveScheduledOrders();
        refresh();
    });

    QPushButton* closeBtn = new QPushButton("닫기", &dlg);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(closeBtn);

    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    layout->addWidget(table);
    layout->addLayout(btnRow);

    dlg.exec();
}

// ==================== 윈도우 예약매도 (작업 스케줄러 연동) ====================
// 앱 SOR 예약(m_scheduledOrders)은 이 앱이 떠 있어야 발사되지만, 이쪽은 등록만 해두면
// 윈도우 작업 스케줄러가 tools/Invoke-ScheduledSell.ps1 을 직접 실행한다.
// 따라서 앱을 꺼도, PC 가 절전 상태여도(-WakeToRun) 발사된다.
// 예약 정보는 앱이 보관하지 않는다. 작업 스케줄러가 유일한 저장소다.

QString MainWindow::toolsScriptPath(const QString& fileName)
{
    QStringList candidates;
    const QString appDir = QCoreApplication::applicationDirPath();

    // 배포 레이아웃: 실행파일 옆 tools/
    candidates << appDir + "/tools/" + fileName;

    // 빌드 트리에서 실행하는 경우: 상위로 올라가며 저장소 루트의 tools/ 를 찾는다
    QDir dir(appDir);
    for (int i = 0; i < 6; ++i)
    {
        candidates << dir.absoluteFilePath("tools/" + fileName);
        if (!dir.cdUp())
            break;
    }

#ifdef STOCKFLOW_TOOLS_DIR
    candidates << QString(STOCKFLOW_TOOLS_DIR) + "/" + fileName; // 빌드 시점에 박아둔 소스 경로
#endif

    for (const QString& c : candidates)
    {
        if (QFileInfo::exists(c))
            return QDir::cleanPath(c);
    }
    return QString();
}

bool MainWindow::runToolScript(const QString& scriptPath, const QStringList& scriptArgs,
                               QString* output, int timeoutMs)
{
    QStringList args;
    args << "-NoProfile" << "-NonInteractive" << "-ExecutionPolicy" << "Bypass"
         << "-File" << QDir::toNativeSeparators(scriptPath);
    args += scriptArgs;

    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start("powershell.exe", args);

    if (!proc.waitForStarted(5000))
    {
        if (output) *output = "powershell.exe 를 실행할 수 없습니다.";
        return false;
    }
    if (!proc.waitForFinished(timeoutMs))
    {
        proc.kill();
        proc.waitForFinished(2000);
        if (output) *output = "스크립트 응답이 시간 내에 오지 않았습니다.";
        return false;
    }

    // 스크립트가 첫머리에서 stdout 을 UTF-8 로 고정해둔다
    if (output)
        *output = QString::fromUtf8(proc.readAll()).trimmed();

    return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

void MainWindow::windowsScheduleSellDialog(StockTableModel* model, int row)
{
    const QString symbol = model->symbolAt(row);
    const QString name = model->displayNameAt(row);
    const double curPrice = currentPriceFor(model, row, symbol);

    // 계좌번호 미설정 방어
    if (Config::KIS_ACCOUNT_CANO.isEmpty() || Config::KIS_ACCOUNT_CANO.startsWith("여기에"))
    {
        QMessageBox::warning(this, "설정 필요",
            "Config.h의 KIS_ACCOUNT_CANO(종합계좌번호 앞 8자리)를 먼저 설정하세요.");
        return;
    }

    const QString script = toolsScriptPath("Register-SellTask.ps1");
    if (script.isEmpty())
    {
        QMessageBox::warning(this, "스크립트 없음",
            "tools/Register-SellTask.ps1 을 찾을 수 없습니다.\n"
            "저장소의 tools 폴더가 실행파일 근처에 있어야 합니다.");
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle("윈도우 예약매도 (앱 꺼도 실행)");

    QSpinBox* qtySpin = new QSpinBox(&dlg);
    qtySpin->setRange(1, 1000000);
    qtySpin->setValue(1);
    qtySpin->setSuffix(" 주");

    const double tick = model->tickSizeAt(row);

    QSpinBox* targetSpin = new QSpinBox(&dlg);
    targetSpin->setRange(1, 1000000000);
    targetSpin->setGroupSeparatorShown(true);
    targetSpin->setSuffix(" 원");
    if (curPrice > 0)
        targetSpin->setValue(static_cast<int>(curPrice));
    applyTick(targetSpin, tick, curPrice);

    // 아직 시세가 없으면 잔고조회 응답이 올 때 현재가로 채워준다
    // (addSellQtyRow 가 이미 fetchBalance 를 호출해 둔다)
    if (curPrice <= 0)
    {
        targetSpin->setSpecialValueText("시세 조회 중...");
        connect(m_krApi, &KisAPI::holdingDetailsReceived, &dlg,
            [targetSpin, symbol, tick](const QHash<QString, KisHoldingDetail>& d)
            {
                if (!d.contains(symbol)) return;
                const double px = d.value(symbol).curPrice;
                if (px <= 0) return;
                targetSpin->setSpecialValueText(QString());
                targetSpin->setValue(static_cast<int>(px));
                applyTick(targetSpin, tick, px);
            });
    }

    // 발사 시각 기본값: 다음 08:00 (NXT 프리마켓 시작)
    const QDateTime now = QDateTime::currentDateTime();
    QDateTime next8(now.date(), QTime(8, 0));
    if (now.time() >= QTime(8, 0))
        next8 = next8.addDays(1);
    QDateTimeEdit* fireEdit = new QDateTimeEdit(next8, &dlg);
    fireEdit->setDisplayFormat("yyyy-MM-dd HH:mm");
    fireEdit->setCalendarPopup(true);

    // 호가 추격은 '매수 1호가 < 목표가'가 되면 알아서 끝난다. 이 값은 그 조건이
    // 오지 않을 때를 대비한 상한이다.
    QSpinBox* chaseSpin = new QSpinBox(&dlg);
    chaseSpin->setRange(1, 600);
    chaseSpin->setValue(60);
    chaseSpin->setSuffix(" 분");
    chaseSpin->setToolTip("매수 1호가가 목표가 이상인 동안 계속 추격합니다.\n"
                          "이 시간이 지나면 남은 수량을 목표가에 걸어두고 끝냅니다.");

    QSpinBox* retrySpin = new QSpinBox(&dlg);
    retrySpin->setRange(0, 100);
    retrySpin->setValue(10);
    retrySpin->setSuffix(" 회 (1분 간격)");

    // 발사 시각 기준 전량. 지금 수량을 고정하는 대신 그때 보유수량을 다시 조회한다.
    QCheckBox* allCheck = new QCheckBox("발사 시각의 보유 전량", &dlg);
    connect(allCheck, &QCheckBox::toggled, qtySpin, &QSpinBox::setDisabled);
    allCheck->setChecked(true);   // 전량 매도가 기본. 수량칸은 자동으로 비활성화된다

    // 며칠 동안 매일 같은 시각에 반복할지. 1이면 그날 한 번만.
    QSpinBox* repeatSpin = new QSpinBox(&dlg);
    repeatSpin->setRange(1, 365);
    repeatSpin->setValue(1);
    repeatSpin->setSuffix(" 일 (1이면 1회만)");

    QCheckBox* dryRunCheck = new QCheckBox("테스트 실행 (주문 없이 판단만 로그로 기록)", &dlg);

    QFormLayout* form = new QFormLayout();
    form->addRow("종목", new QLabel(QString("%1 (%2)").arg(name, symbol), &dlg));
    addSellQtyRow(&dlg, symbol, qtySpin, form);   // 수량 + 전량 버튼 + 보유수량 상한
    form->addRow("", allCheck);
    targetSpin->setToolTip("이 가격 미만의 매수호가에는 팔지 않습니다.\n"
                           "목표가 이상 호가에는 그 호가 가격으로 매도합니다.");
    form->addRow("목표가", targetSpin);
    form->addRow("발사 시각", fireEdit);
    form->addRow("반복 일수", repeatSpin);
    form->addRow("호가 추격 상한", chaseSpin);
    form->addRow("주문거부 재시도", retrySpin);
    form->addRow("", dryRunCheck);

    QLabel* hint = new QLabel(
        "※ 앱을 꺼도 실행됩니다. PC 가 절전이면 깨워서 발사합니다.\n"
        "   단, PC 전원이 완전히 꺼져 있으면 발사되지 않습니다.\n"
        "   목표가 이상인 매수호가에 그 호가 가격 그대로 매도를 냅니다.\n"
        "   안 팔리면 취소하고 호가를 다시 읽어 재주문하며, 매수 1호가가\n"
        "   목표가 밑으로 내려갈 때까지 계속 추격합니다.\n"
        "   추격이 끝나고 남은 수량은 목표가에 걸어둡니다.\n"
        "   통신이 끊기면 10초마다 재접속하며, 주문 접수 여부를 확인한 뒤에만\n"
        "   재전송합니다(이중 매도 방지).\n"
        "   SOR(KRX+NXT 통합)로 나가므로 NXT 프리마켓에서도 체결됩니다.", &dlg);
    hint->setStyleSheet("color: gray;");

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    layout->addLayout(form);
    layout->addWidget(hint);
    layout->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const bool sellAll = allCheck->isChecked();
    const int repeatDays = repeatSpin->value();
    const bool dryRun = dryRunCheck->isChecked();
    const QDateTime fire = fireEdit->dateTime();

    // 발사 시각이 과거면 거부 (스크립트도 막지만 여기서 먼저 알려준다)
    if (fire <= QDateTime::currentDateTime())
    {
        QMessageBox::warning(this, "시각 오류", "발사 시각이 이미 지났습니다. 미래 시각으로 지정하세요.");
        return;
    }

    const QLocale loc = QLocale::system();
    const QString qtyText = sellAll ? QString("보유 전량") : QString("%1주").arg(qtySpin->value());
    const QString repeatText = (repeatDays > 1)
        ? QString(" (매일 반복, %1일간 ~ %2)")
              .arg(repeatDays).arg(fire.date().addDays(repeatDays - 1).toString("yyyy-MM-dd"))
        : QString(" (1회)");
    const QString confirmMsg =
        QString("[확인] 윈도우 예약매도 등록 (실전 계좌)\n\n"
                "종목: %1 (%2)\n수량: %3\n목표가: %4원\n발사: %5%6\n"
                "\n목표가 이상 매수호가에 그 호가 가격으로 매도하고,\n"
                "매수 1호가가 목표가 밑으로 갈 때까지 추격합니다(최대 %7분).\n"
                "목표가 아래로는 절대 팔지 않습니다.\n%8\n등록할까요?")
            .arg(name, symbol, qtyText,
                 loc.toString(targetSpin->value()),
                 fire.toString("yyyy-MM-dd HH:mm"),
                 repeatText)
            .arg(chaseSpin->value())
            .arg(dryRun ? "\n※ 테스트 실행이라 실제 주문은 나가지 않습니다.\n" : "");

    if (QMessageBox::warning(this, "윈도우 예약 확인", confirmMsg,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    QStringList args;
    args << "-At" << fire.toString("yyyy-MM-dd HH:mm")
         << "-Symbol" << symbol
         << "-Quantity" << (sellAll ? QString("All") : QString::number(qtySpin->value()))
         << "-TargetPrice" << QString::number(
                alignToTick(targetSpin->value(), resolveTick(targetSpin->value(), curPrice, tick)))
         << "-MaxRetries" << QString::number(retrySpin->value())
         << "-ChaseMinutes" << QString::number(chaseSpin->value())
         << "-RepeatDays" << QString::number(repeatDays);
    if (dryRun) args << "-DryRun";

    QString out;
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = runToolScript(script, args, &out);
    QGuiApplication::restoreOverrideCursor();

    if (ok)
        QMessageBox::information(this, "등록 완료", out.isEmpty() ? QString("예약이 등록되었습니다.") : out);
    else
        QMessageBox::critical(this, "등록 실패", out.isEmpty() ? QString("예약 등록에 실패했습니다.") : out);
}

void MainWindow::onWindowsTasksClicked()
{
    const QString script = toolsScriptPath("Register-SellTask.ps1");
    if (script.isEmpty())
    {
        QMessageBox::warning(this, "스크립트 없음", "tools/Register-SellTask.ps1 을 찾을 수 없습니다.");
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle("윈도우 예약목록 (작업 스케줄러)");
    dlg.resize(820, 340);

    QTableWidget* table = new QTableWidget(0, 7, &dlg);
    table->setHorizontalHeaderLabels({ "종목", "수량", "목표가", "반복", "다음실행", "상태", "비고" });
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    // 행 인덱스 → 작업 이름 (취소할 때 필요). 다이얼로그 수명 동안만 유지된다.
    auto taskNames = std::make_shared<QStringList>();

    auto refresh = [this, table, taskNames, script]()
    {
        QString out;
        if (!runToolScript(script, QStringList() << "-List" << "-Json", &out, 20000))
        {
            QMessageBox::warning(table->window(), "조회 실패",
                out.isEmpty() ? QString("예약 목록을 불러오지 못했습니다.") : out);
            return;
        }

        // PowerShell 5.1 은 원소가 1개면 배열을 객체로 접어버리므로 둘 다 받아준다
        const QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8());
        QJsonArray arr;
        if (doc.isArray())        arr = doc.array();
        else if (doc.isObject())  arr.append(doc.object());

        const QLocale loc = QLocale::system();
        taskNames->clear();
        table->setRowCount(arr.size());

        for (int i = 0; i < arr.size(); ++i)
        {
            const QJsonObject o = arr.at(i).toObject();
            const QString sym = o["Symbol"].toString();
            QString nm = StockCodeMap::getName(sym);
            if (nm.isEmpty()) nm = sym;
            taskNames->append(o["TaskName"].toString());

            const QString qty = o["Quantity"].toString();
            const bool isAll = (qty.compare("All", Qt::CaseInsensitive) == 0);

            table->setItem(i, 0, new QTableWidgetItem(QString("%1 (%2)").arg(nm, sym)));
            table->setItem(i, 1, new QTableWidgetItem(isAll ? QString("전량") : qty + "주"));
            table->setItem(i, 2, new QTableWidgetItem(loc.toString(o["TargetPrice"].toString().toDouble(), 'f', 0)));
            const QString until = o["RepeatUntil"].toString();
            table->setItem(i, 3, new QTableWidgetItem(
                o["Daily"].toBool() ? (until.isEmpty() ? QString("매일") : QString("~%1").arg(until))
                                    : QString("1회")));
            const QString nextRun = o["NextRunTime"].toString();
            table->setItem(i, 4, new QTableWidgetItem(nextRun.isEmpty() ? QString("-") : nextRun));

            // 스크립트가 계산해준 사람이 읽을 상태를 쓴다.
            // (스케줄러 원본 State 는 1회 예약이 끝나도 Ready 로 남아 오해를 준다)
            QString status = o["Status"].toString();
            if (status.isEmpty()) status = o["State"].toString();
            QTableWidgetItem* statusItem = new QTableWidgetItem(status);
            if (status.startsWith("완료(실패")) statusItem->setForeground(QColor(Qt::red));
            else if (status.startsWith("완료"))  statusItem->setForeground(QColor(0, 128, 0));
            table->setItem(i, 5, statusItem);

            QStringList notes;
            if (o["DryRun"].toBool()) notes << "테스트";
            const QString lastRun = o["LastRunTime"].toString();
            if (!lastRun.isEmpty())
            {
                notes << QString("최근 %1 %2")
                    .arg(lastRun, o["LastTaskResult"].toInt() == 0 ? "성공" : "실패");
            }
            table->setItem(i, 6, new QTableWidgetItem(notes.join(" / ")));
        }
    };
    refresh();

    QPushButton* cancelBtn = new QPushButton("선택 취소", &dlg);
    connect(cancelBtn, &QPushButton::clicked, &dlg, [this, table, taskNames, refresh, script]()
    {
        const int r = table->currentRow();
        if (r < 0 || r >= taskNames->size())
        {
            QMessageBox::information(table->window(), "취소", "예약을 선택하세요.");
            return;
        }
        const QString taskName = taskNames->at(r);
        if (QMessageBox::question(table->window(), "예약 취소",
                QString("이 예약을 삭제할까요?\n\n%1").arg(taskName),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;

        QString out;
        if (!runToolScript(script, QStringList() << "-Remove" << "-TaskName" << taskName, &out, 20000))
        {
            QMessageBox::warning(table->window(), "취소 실패",
                out.isEmpty() ? QString("예약을 삭제하지 못했습니다.") : out);
        }
        refresh();
    });

    QPushButton* refreshBtn = new QPushButton("새로고침", &dlg);
    connect(refreshBtn, &QPushButton::clicked, &dlg, [refresh]() { refresh(); });

    // 끝난 1회 예약은 스케줄러에 그대로 남는다. 쌓이면 목록을 읽기 어려우니 한 번에 지운다.
    QPushButton* purgeBtn = new QPushButton("완료 항목 정리", &dlg);
    connect(purgeBtn, &QPushButton::clicked, &dlg, [this, table, refresh, script]()
    {
        QStringList done;
        for (int r = 0; r < table->rowCount(); ++r)
        {
            QTableWidgetItem* st = table->item(r, 5);
            QTableWidgetItem* nm = table->item(r, 0);
            if (st && nm && st->text().startsWith("완료"))
                done << nm->text();
        }
        if (done.isEmpty())
        {
            QMessageBox::information(table->window(), "정리", "완료된 예약이 없습니다.");
            return;
        }
        if (QMessageBox::question(table->window(), "완료 항목 정리",
                QString("완료된 예약 %1건을 목록에서 지울까요?\n(이미 나간 주문은 취소되지 않습니다)")
                    .arg(done.size()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
            return;

        // 표에는 종목명이 그려져 있으므로 작업 이름은 다시 조회해서 지운다
        QString listOut;
        if (runToolScript(script, QStringList() << "-List" << "-Json", &listOut, 20000))
        {
            const QJsonDocument doc = QJsonDocument::fromJson(listOut.toUtf8());
            QJsonArray arr;
            if (doc.isArray())       arr = doc.array();
            else if (doc.isObject()) arr.append(doc.object());

            for (const QJsonValue& v : arr)
            {
                const QJsonObject o = v.toObject();
                if (!o["Status"].toString().startsWith("완료")) continue;
                QString out;
                runToolScript(script, QStringList() << "-Remove" << "-TaskName" << o["TaskName"].toString(),
                              &out, 20000);
            }
        }
        refresh();
    });

    // 조용히 실패했을 때 원인을 볼 수 있는 유일한 통로라 바로 열 수 있게 해둔다
    QPushButton* logBtn = new QPushButton("로그 폴더", &dlg);
    connect(logBtn, &QPushButton::clicked, &dlg, [this, script]()
    {
        const QString logDir = QFileInfo(script).absolutePath() + "/logs";
        if (!QFileInfo::exists(logDir))
        {
            QMessageBox::information(this, "로그", "아직 실행 기록이 없습니다.");
            return;
        }
        QDesktopServices::openUrl(QUrl::fromLocalFile(logDir));
    });

    QPushButton* closeBtn = new QPushButton("닫기", &dlg);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    QLabel* note = new QLabel(
        "※ 이 목록은 윈도우 작업 스케줄러가 보관합니다. 앱을 꺼도 유지되고 발사됩니다.\n"
        "※ '완료'는 예약이 발사됐다는 뜻입니다. 주문 체결 여부는 매수매도현황에서 확인하세요.", &dlg);
    note->setStyleSheet("color: gray;");

    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(purgeBtn);
    btnRow->addWidget(refreshBtn);
    btnRow->addWidget(logBtn);
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);

    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    layout->addWidget(table);
    layout->addWidget(note);
    layout->addLayout(btnRow);

    dlg.exec();
}

// 가격 입력칸을 호가단위(틱)에 맞춰준다. 스텝을 틱으로 놓고 현재 값도 틱 배수로 스냅한다.
// 틱에 안 맞는 가격으로 주문하면 KIS 가 '주식주문 호가단위 오류'로 거부한다.
void MainWindow::applyTick(QSpinBox* priceSpin, double tick, double currentPrice)
{
    if (!priceSpin) return;
    if (tick <= 0)
    {
        priceSpin->setSingleStep(10);   // 호가단위를 모르면 기존 동작 유지
        return;
    }

    // 값이 가격 구간을 넘나들면 호가단위도 바뀌므로 스텝/표시를 따라가게 한다
    // (ETF 처럼 구간이 없는 종목은 resolveTick 이 항상 같은 값을 돌려준다)
    auto sync = [priceSpin, tick, currentPrice]()
    {
        const int step = static_cast<int>(resolveTick(priceSpin->value(), currentPrice, tick));
        if (priceSpin->singleStep() != step)
        {
            priceSpin->setSingleStep(step);
            priceSpin->setSuffix(QString(" 원 (호가 %1)").arg(step));
        }
    };
    QObject::connect(priceSpin, QOverload<int>::of(&QSpinBox::valueChanged), priceSpin,
                     [sync](int) { sync(); });

    priceSpin->setValue(alignToTick(priceSpin->value(), resolveTick(priceSpin->value(), currentPrice, tick)));
    sync();
}

// 호가단위는 '현재가'가 아니라 '주문 가격'으로 정해진다.
// 현재가 163,100원(틱 100)인 종목도 210,200원에 주문하면 20만원 구간이라 틱이 500이다.
// 실제 API(aspr_unit)로 확인한 구간: ~20,000=10 / ~50,000=50 / ~200,000=100
//                                   / ~500,000=500 / 그 이상=1,000
double MainWindow::tickForPrice(double price)
{
    if (price < 2000)   return 1;
    if (price < 5000)   return 5;
    if (price < 20000)  return 10;
    if (price < 50000)  return 50;
    if (price < 200000) return 100;
    if (price < 500000) return 500;
    return 1000;
}

// 주문 가격의 호가단위를 정한다.
// 일반주식은 가격 구간표를 따르지만 ETF/ETN 은 가격과 무관하게 5원이다.
// 그래서 '현재가에서 API 가 알려준 틱'이 구간표 예측과 다르면 주식이 아니라고 보고
// 그 값을 그대로 쓴다. (ETF 는 구간이 없으니 모든 가격에 같은 틱이 유효하다)
double MainWindow::resolveTick(double orderPrice, double currentPrice, double measuredTick)
{
    if (measuredTick > 0 && currentPrice > 0 && measuredTick != tickForPrice(currentPrice))
        return measuredTick;    // ETF/ETN 등 구간표를 안 따르는 종목
    return tickForPrice(orderPrice);
}

// 내림 한 번으로 끝낸다. 틱을 제대로 골랐다면 이걸로 유효한 가격이 되고,
// 입력값에서 벗어나는 폭도 한 틱 미만이다.
int MainWindow::alignToTick(double price, double tick)
{
    if (tick <= 0) return static_cast<int>(qRound(price));
    double down = std::floor(price / tick) * tick;
    if (down <= 0) down = tick;   // 가격이 한 틱보다 작으면 최소 한 틱
    return static_cast<int>(qRound(down));
}

// 정정 다이얼로그처럼 행 정보가 없는 곳에서 종목코드로 호가단위를 찾는다
double MainWindow::tickForSymbol(const QString& symbol) const
{
    for (StockTableModel* m : { m_stockModel, m_holdingsModel })
    {
        if (!m) continue;
        for (int r = 0; r < m->rowCount(); ++r)
        {
            if (m->symbolAt(r) != symbol) continue;
            const double tk = m->tickSizeAt(r);
            if (tk > 0) return tk;
        }
    }
    return 0.0;
}

// 다이얼로그에 채울 현재가를 구한다.
// 관심종목 표는 시세를 받아야 값이 차는데, 보유종목 탭을 막 열었거나 시세가 아직
// 도착하지 않았으면 0 이 나온다. 그대로 스핀박스에 넣으면 범위 최솟값(1원)으로
// 눌려버려서 "목표가가 1원"으로 보인다. 잔고조회로 받아둔 현재가를 대신 쓴다.
double MainWindow::currentPriceFor(StockTableModel* model, int row, const QString& symbol) const
{
    const double fromModel = model ? model->currentPriceAt(row) : 0.0;
    if (fromModel > 0) return fromModel;

    if (m_holdingDetail.contains(symbol))
    {
        const double fromBalance = m_holdingDetail.value(symbol).curPrice;
        if (fromBalance > 0) return fromBalance;
    }
    return 0.0;
}

void MainWindow::showOrderStatus(StockTableModel* model, int row)
{
    const QString symbol = model->symbolAt(row);
    const QString name = model->displayNameAt(row);

    if (Config::KIS_ACCOUNT_CANO.isEmpty() || Config::KIS_ACCOUNT_CANO.startsWith("여기에"))
    {
        QMessageBox::warning(this, "설정 필요",
            "Config.h의 KIS_ACCOUNT_CANO(종합계좌번호 앞 8자리)를 먼저 설정하세요.");
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QString("매수매도현황 - %1 (%2)").arg(name, symbol));
    dlg.resize(660, 400);

    QLabel* summary = new QLabel(&dlg);
    summary->setTextFormat(Qt::RichText);
    summary->setText("잔고 조회 중...");

    QTableWidget* table = new QTableWidget(0, 5, &dlg);
    table->setHorizontalHeaderLabels({ "구분", "주문번호", "주문수량", "미체결", "주문단가" });
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    QLabel* ordersLabel = new QLabel("미체결 주문 (조회 중...)", &dlg);

    // 표에 그려진 순서 그대로 보관해야 '선택한 행 -> 주문'을 짚을 수 있다
    auto shown = std::make_shared<QList<KisOpenOrder>>();

    // 잔고에서 온 현황을 요약 줄로 그린다
    auto applySummary = [this, symbol, summary]()
    {
        if (!m_holdingDetail.contains(symbol))
        {
            summary->setText("<b>이 종목의 잔고 정보가 없습니다.</b> (보유하고 있지 않거나 조회 전)");
            return;
        }
        const KisHoldingDetail& d = m_holdingDetail.value(symbol);
        const QLocale loc = QLocale::system();
        const QString pnlColor = d.evalPnl >= 0 ? "red" : "blue";

        summary->setText(QString(
            "<table cellspacing='6'>"
            "<tr><td><b>보유수량</b></td><td>%1주</td>"
            "    <td><b>주문가능</b></td><td>%2주</td></tr>"
            "<tr><td><b>당일 매수</b></td><td>%3주</td>"
            "    <td><b>당일 매도</b></td><td>%4주</td></tr>"
            "<tr><td><b>전일 매수</b></td><td>%5주</td>"
            "    <td><b>전일 매도</b></td><td>%6주</td></tr>"
            "<tr><td><b>매입평균</b></td><td>%7원</td>"
            "    <td><b>현재가</b></td><td>%8원</td></tr>"
            "<tr><td><b>평가손익</b></td>"
            "    <td colspan='3'><font color='%9'>%10원 (%11%)</font></td></tr>"
            "</table>")
            .arg(loc.toString(d.holdQty)).arg(loc.toString(d.ordPsblQty))
            .arg(loc.toString(d.todayBuyQty)).arg(loc.toString(d.todaySellQty))
            .arg(loc.toString(d.prevBuyQty)).arg(loc.toString(d.prevSellQty))
            .arg(loc.toString(d.avgPrice, 'f', 0)).arg(loc.toString(d.curPrice, 'f', 0))
            .arg(pnlColor).arg(loc.toString(d.evalPnl, 'f', 0)).arg(d.pnlRate, 0, 'f', 2));

        // 보유수량과 주문가능수량이 다르면 그 차이만큼 주문이 물려 있다는 뜻이라 짚어준다
        if (d.ordPsblQty < d.holdQty)
        {
            summary->setText(summary->text() +
                QString("<font color='gray'>※ %1주는 아래 미체결 주문에 묶여 있어 추가 매도가 불가합니다.</font>")
                    .arg(d.holdQty - d.ordPsblQty));
        }
    };
    applySummary();

    // 이 종목의 미체결 주문만 추린다 (매수/매도 모두)
    auto applyOrders = [table, ordersLabel, symbol, shown](const QList<KisOpenOrder>& orders)
    {
        const QLocale loc = QLocale::system();
        QList<KisOpenOrder> mine;
        for (const KisOpenOrder& o : orders)
        {
            if (o.symbol == symbol)
                mine << o;
        }
        *shown = mine;

        table->setRowCount(mine.size());
        for (int i = 0; i < mine.size(); ++i)
        {
            const KisOpenOrder& o = mine.at(i);
            QTableWidgetItem* kind = new QTableWidgetItem(o.isBuy ? "매수" : "매도");
            kind->setForeground(o.isBuy ? QColor(Qt::red) : QColor(Qt::blue));
            table->setItem(i, 0, kind);
            table->setItem(i, 1, new QTableWidgetItem(o.orderNo));
            table->setItem(i, 2, new QTableWidgetItem(loc.toString(o.orderQty)));
            table->setItem(i, 3, new QTableWidgetItem(loc.toString(o.possibleQty)));
            table->setItem(i, 4, new QTableWidgetItem(loc.toString(o.orderPrice, 'f', 0)));
        }
        ordersLabel->setText(mine.isEmpty() ? "미체결 주문 없음"
                                            : QString("미체결 주문 %1건").arg(mine.size()));
    };

    // 다이얼로그가 살아있는 동안만 갱신을 받는다
    connect(m_krApi, &KisAPI::holdingDetailsReceived, &dlg,
        [this, applySummary](const QHash<QString, KisHoldingDetail>& details)
        {
            m_holdingDetail = details;
            applySummary();
        });
    connect(m_krApi, &KisAPI::openOrdersReceived, &dlg, applyOrders);

    // 정정/취소 결과 -> 알림 후 목록 갱신
    connect(m_krApi, &KisAPI::orderModified, &dlg,
        [this, &dlg](bool success, const QString& message)
        {
            if (success)
                QMessageBox::information(&dlg, "정정/취소", "처리되었습니다.\n" + message);
            else
                QMessageBox::warning(&dlg, "정정/취소 실패", message);
            m_krApi->fetchOpenOrders();
            m_krApi->fetchBalance();
        });

    auto selectedOrder = [table, shown]() -> const KisOpenOrder*
    {
        const int r = table->currentRow();
        if (r < 0 || r >= shown->size()) return nullptr;
        return &(*shown)[r];
    };

    QPushButton* cancelOrderBtn = new QPushButton("주문 취소", &dlg);
    connect(cancelOrderBtn, &QPushButton::clicked, &dlg, [this, &dlg, selectedOrder]()
    {
        const KisOpenOrder* o = selectedOrder();
        if (!o) { QMessageBox::information(&dlg, "주문 취소", "주문을 선택하세요."); return; }
        if (QMessageBox::warning(&dlg, "주문 취소",
                QString("[%1] %2 %3주 (%4원) 주문을 취소할까요?")
                    .arg(o->name, o->isBuy ? "매수" : "매도")
                    .arg(o->possibleQty)
                    .arg(QLocale::system().toString(o->orderPrice, 'f', 0)),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
            m_krApi->cancelOrder(*o);
    });

    QPushButton* reviseOrderBtn = new QPushButton("주문 정정", &dlg);
    connect(reviseOrderBtn, &QPushButton::clicked, &dlg, [this, &dlg, selectedOrder]()
    {
        const KisOpenOrder* o = selectedOrder();
        if (!o) { QMessageBox::information(&dlg, "주문 정정", "주문을 선택하세요."); return; }

        QDialog rd(&dlg);
        rd.setWindowTitle("주문 정정");

        QSpinBox* q = new QSpinBox(&rd);
        q->setRange(1, o->possibleQty);
        q->setValue(o->possibleQty);
        q->setSuffix(" 주");

        const double rtick = tickForSymbol(o->symbol);
        QSpinBox* pr = new QSpinBox(&rd);
        pr->setRange(0, 1000000000);
        pr->setGroupSeparatorShown(true);
        pr->setSuffix(" 원");
        pr->setValue(static_cast<int>(o->orderPrice));
        applyTick(pr, rtick, o->orderPrice);

        QFormLayout* f = new QFormLayout();
        f->addRow("주문", new QLabel(QString("%1 %2주 @ %3원")
            .arg(o->isBuy ? "매수" : "매도")
            .arg(o->possibleQty)
            .arg(QLocale::system().toString(o->orderPrice, 'f', 0)), &rd));
        f->addRow("새 수량", q);
        f->addRow("새 가격", pr);

        QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &rd);
        connect(bb, &QDialogButtonBox::accepted, &rd, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &rd, &QDialog::reject);

        QVBoxLayout* l = new QVBoxLayout(&rd);
        l->addLayout(f);
        l->addWidget(bb);

        if (rd.exec() == QDialog::Accepted)
            m_krApi->reviseOrder(*o, q->value(), alignToTick(pr->value(), resolveTick(pr->value(), o->orderPrice, rtick)));
    });

    QPushButton* refreshBtn = new QPushButton("새로고침", &dlg);
    connect(refreshBtn, &QPushButton::clicked, &dlg, [this]()
    {
        m_krApi->fetchBalance();
        m_krApi->fetchOpenOrders();
    });

    QPushButton* closeBtn = new QPushButton("닫기", &dlg);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    QLabel* note = new QLabel(
        "※ 정정/취소는 표에서 주문을 선택한 뒤 누르세요.\n"
        "※ 당일 매수/매도 수량은 잔고 기준이며, 체결 단가별 내역은 표시하지 않습니다.", &dlg);
    note->setStyleSheet("color: gray;");

    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addWidget(reviseOrderBtn);
    btnRow->addWidget(cancelOrderBtn);
    btnRow->addWidget(refreshBtn);
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);

    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    layout->addWidget(summary);
    layout->addWidget(ordersLabel);
    layout->addWidget(table);
    layout->addWidget(note);
    layout->addLayout(btnRow);

    // 최신 상태로 갱신 (응답은 위 시그널로 들어온다)
    m_krApi->fetchBalance();
    m_krApi->fetchOpenOrders();

    dlg.exec();
}

void MainWindow::tradeDialog(StockTableModel* model, int row, bool isBuy)
{
    const QString symbol = model->symbolAt(row);
    const QString name = model->displayNameAt(row);
    const double curPrice = currentPriceFor(model, row, symbol);
    const QString kind = isBuy ? "매수" : "매도";

    // 계좌번호 미설정 방어
    if (Config::KIS_ACCOUNT_CANO.isEmpty() || Config::KIS_ACCOUNT_CANO.startsWith("여기에"))
    {
        QMessageBox::warning(this, "설정 필요",
            "Config.h의 KIS_ACCOUNT_CANO(종합계좌번호 앞 8자리)를 먼저 설정하세요.");
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(kind + " 주문");

    QSpinBox* qtySpin = new QSpinBox(&dlg);
    qtySpin->setRange(1, 1000000);
    qtySpin->setValue(1);
    qtySpin->setSuffix(" 주");

    QCheckBox* marketCheck = new QCheckBox("시장가 주문", &dlg);

    QSpinBox* priceSpin = new QSpinBox(&dlg);
    priceSpin->setRange(0, 1000000000);
    priceSpin->setGroupSeparatorShown(true);
    priceSpin->setSuffix(" 원");
    priceSpin->setValue(static_cast<int>(curPrice));
    applyTick(priceSpin, model->tickSizeAt(row), curPrice);

    connect(marketCheck, &QCheckBox::toggled, priceSpin, &QSpinBox::setDisabled);

    QFormLayout* form = new QFormLayout();
    form->addRow("종목", new QLabel(QString("%1 (%2)").arg(name, symbol), &dlg));
    if (isBuy)
        addBuyQtyRow(&dlg, symbol, qtySpin, form, curPrice);   // 최대 버튼 + 주문가능현금
    else
        addSellQtyRow(&dlg, symbol, qtySpin, form);            // 전량 버튼 + 보유수량 상한
    form->addRow("", marketCheck);
    form->addRow("지정가", priceSpin);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    layout->addLayout(form);
    layout->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const int qty = qtySpin->value();
    const bool isMarket = marketCheck->isChecked();
    const double price = priceSpin->value();

    // 실전 계좌 — 실제 체결되므로 최종 확인
    const QString priceText = isMarket ? "시장가" : (QLocale::system().toString(price, 'f', 0) + "원");
    const QString confirmMsg =
        QString("⚠️ 실전 계좌 %1\n\n종목: %2 (%3)\n수량: %4주\n가격: %5\n\n정말 주문하시겠습니까?")
            .arg(kind, name, symbol).arg(qty).arg(priceText);

    if (QMessageBox::warning(this, kind + " 확인", confirmMsg,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    m_krApi->placeOrder(symbol, isBuy, qty, price, isMarket);
}

void MainWindow::showAskingPrice(StockTableModel* model, int row)
{
    const QString symbol = model->symbolAt(row);
    const QString name = model->displayNameAt(row);

    QDialog dlg(this);
    dlg.setWindowTitle(QString("호가 - %1 (%2)").arg(name, symbol));
    dlg.resize(280, 480);

    // 20행(매도 10 + 매수 10) x [구분, 호가, 잔량]
    QTableWidget* table = new QTableWidget(20, 3, &dlg);
    table->setHorizontalHeaderLabels({ "구분", "호가", "잔량" });
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    auto fillTable = [table](const KisAskingPrice& data)
    {
        QLocale loc = QLocale::system();
        // 위쪽 0~9행: 매도호가 10 → 1 (가격 높은 순)
        for (int i = 0; i < 10; ++i)
        {
            const KisAskRow& ask = data.asks.value(9 - i);
            table->setItem(i, 0, new QTableWidgetItem("매도"));
            table->setItem(i, 1, new QTableWidgetItem(loc.toString(ask.price, 'f', 0)));
            table->setItem(i, 2, new QTableWidgetItem(loc.toString(ask.qty)));
            for (int c = 0; c < 3; ++c) table->item(i, c)->setForeground(QColor(Qt::blue));
        }
        // 아래쪽 10~19행: 매수호가 1 → 10 (가격 높은 순)
        for (int i = 0; i < 10; ++i)
        {
            const KisAskRow& bid = data.bids.value(i);
            const int r = 10 + i;
            table->setItem(r, 0, new QTableWidgetItem("매수"));
            table->setItem(r, 1, new QTableWidgetItem(loc.toString(bid.price, 'f', 0)));
            table->setItem(r, 2, new QTableWidgetItem(loc.toString(bid.qty)));
            for (int c = 0; c < 3; ++c) table->item(r, c)->setForeground(QColor(Qt::red));
        }
    };

    connect(m_krApi, &KisAPI::askingPriceReceived, &dlg,
        [symbol, fillTable](const QString& s, const KisAskingPrice& data)
        {
            if (s == symbol) fillTable(data);
        });

    QPushButton* refreshBtn = new QPushButton("새로고침", &dlg);
    connect(refreshBtn, &QPushButton::clicked, this, [this, symbol]() { m_krApi->fetchAskingPrice(symbol); });

    QPushButton* closeBtn = new QPushButton("닫기", &dlg);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addWidget(refreshBtn);
    btnRow->addWidget(closeBtn);

    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    layout->addWidget(table);
    layout->addLayout(btnRow);

    m_krApi->fetchAskingPrice(symbol);
    dlg.exec();
}

void MainWindow::onOpenOrdersClicked()
{
    // 계좌번호 미설정 방어
    if (Config::KIS_ACCOUNT_CANO.isEmpty() || Config::KIS_ACCOUNT_CANO.startsWith("여기에"))
    {
        QMessageBox::warning(this, "설정 필요",
            "Config.h의 KIS_ACCOUNT_CANO(종합계좌번호 앞 8자리)를 먼저 설정하세요.");
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle("미체결 / 정정·취소");
    dlg.resize(560, 360);

    QTableWidget* table = new QTableWidget(0, 5, &dlg);
    table->setHorizontalHeaderLabels({ "종목", "구분", "주문가", "가능수량", "주문번호" });
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    // 현재 표시 중인 미체결 목록 보관 (행 인덱스 = 주문)
    auto orders = std::make_shared<QList<KisOpenOrder>>();

    connect(m_krApi, &KisAPI::openOrdersReceived, &dlg,
        [table, orders](const QList<KisOpenOrder>& list)
        {
            *orders = list;
            QLocale loc = QLocale::system();
            table->setRowCount(list.size());
            for (int i = 0; i < list.size(); ++i)
            {
                const KisOpenOrder& o = list[i];
                table->setItem(i, 0, new QTableWidgetItem(QString("%1 (%2)").arg(o.name, o.symbol)));
                table->setItem(i, 1, new QTableWidgetItem(o.isBuy ? "매수" : "매도"));
                table->setItem(i, 2, new QTableWidgetItem(loc.toString(o.orderPrice, 'f', 0)));
                table->setItem(i, 3, new QTableWidgetItem(loc.toString(o.possibleQty)));
                table->setItem(i, 4, new QTableWidgetItem(o.orderNo));
            }
        });

    // 정정/취소 결과 → 메시지 후 목록 갱신
    connect(m_krApi, &KisAPI::orderModified, &dlg,
        [this, &dlg](bool success, const QString& message)
        {
            if (success)
                QMessageBox::information(&dlg, "정정/취소", "처리되었습니다.\n" + message);
            else
                QMessageBox::warning(&dlg, "정정/취소 실패", message);
            m_krApi->fetchOpenOrders(); // 갱신
        });

    auto selectedOrder = [table, orders]() -> const KisOpenOrder*
    {
        const int r = table->currentRow();
        if (r < 0 || r >= orders->size()) return nullptr;
        return &(*orders)[r];
    };

    QPushButton* refreshBtn = new QPushButton("새로고침", &dlg);
    connect(refreshBtn, &QPushButton::clicked, this, [this]() { m_krApi->fetchOpenOrders(); });

    QPushButton* cancelBtn = new QPushButton("취소", &dlg);
    connect(cancelBtn, &QPushButton::clicked, &dlg, [this, &dlg, selectedOrder]()
    {
        const KisOpenOrder* o = selectedOrder();
        if (!o) { QMessageBox::information(&dlg, "취소", "주문을 선택하세요."); return; }
        if (QMessageBox::warning(&dlg, "주문 취소",
                QString("[%1] %2주 주문을 취소하시겠습니까?").arg(o->name).arg(o->possibleQty),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
            m_krApi->cancelOrder(*o);
    });

    QPushButton* reviseBtn = new QPushButton("정정", &dlg);
    connect(reviseBtn, &QPushButton::clicked, &dlg, [this, &dlg, selectedOrder]()
    {
        const KisOpenOrder* o = selectedOrder();
        if (!o) { QMessageBox::information(&dlg, "정정", "주문을 선택하세요."); return; }

        // 새 수량/가격 입력
        QDialog rd(&dlg);
        rd.setWindowTitle("주문 정정");
        QSpinBox* q = new QSpinBox(&rd);
        q->setRange(1, o->possibleQty);
        q->setValue(o->possibleQty);
        q->setSuffix(" 주");
        const double rtick = tickForSymbol(o->symbol);
        QSpinBox* p = new QSpinBox(&rd);
        p->setRange(0, 1000000000);
        p->setGroupSeparatorShown(true);
        p->setSuffix(" 원");
        p->setValue(static_cast<int>(o->orderPrice));
        applyTick(p, rtick, o->orderPrice);
        QFormLayout* f = new QFormLayout();
        f->addRow("새 수량", q);
        f->addRow("새 가격", p);
        QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &rd);
        connect(bb, &QDialogButtonBox::accepted, &rd, &QDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, &rd, &QDialog::reject);
        QVBoxLayout* l = new QVBoxLayout(&rd);
        l->addLayout(f);
        l->addWidget(bb);
        if (rd.exec() == QDialog::Accepted)
            m_krApi->reviseOrder(*o, q->value(), alignToTick(p->value(), resolveTick(p->value(), o->orderPrice, rtick)));
    });

    QPushButton* closeBtn = new QPushButton("닫기", &dlg);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->addWidget(refreshBtn);
    btnRow->addWidget(reviseBtn);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(closeBtn);

    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    layout->addWidget(table);
    layout->addLayout(btnRow);

    m_krApi->fetchOpenOrders();
    dlg.exec();
}

void MainWindow::updateSearchCompleter()
{
    // 검색어 모델 연결
    QCompleter* completer = new QCompleter(m_searchModel, this);
    completer->setCaseSensitivity(Qt::CaseInsensitive); // 대소문자 구분x
    completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);

    ui->editSearch->setCompleter(completer);
    ui->editSearch->setEnabled(true);
    ui->btnSearch->setEnabled(true);
    ui->editSearch->setPlaceholderText("종목명 또는 코드 검색");
    ui->editSearch->setFocus();
}

void MainWindow::performSearch()
{
    QString text = m_pendingText.isEmpty() ? ui->editSearch->text() : m_pendingText;
    if (text.trimmed().isEmpty()) return;

    if (text == m_lastSearchText)
        return;
    m_lastSearchText = text;

    QStringList filteredList = StockCodeMap::searchKeywords(m_pendingText);
    m_searchModel->setStringList(filteredList);

    QCompleter* completer = ui->editSearch->completer();
    if (!filteredList.isEmpty())
    {
        completer->setCompletionPrefix(text);
        // 팝업 띄우기
        completer->complete();
    }
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    // 한글 입력 처리
    if (obj == ui->editSearch && event->type() == QEvent::InputMethod)
    {
        QInputMethodEvent* imeEvent = static_cast<QInputMethodEvent*>(event);

        // 현재 화면에 확정된 글자
        QString committed = ui->editSearch->text();

        // 지금 치고 있는 조합 중인 글자
        QString preedit = imeEvent->preeditString();

        m_pendingText = committed + preedit;

        // 타이머 리셋
        m_debounceTimer->start();
        //qDebug() << "한글 입력 감지:" << m_pendingText;

        // 주의: return false를 해야 QLineEdit 본체도 이벤트를 받아서 글자를 화면에 그립니다.
        return false;
    }

    return QMainWindow::eventFilter(obj, event);
}
