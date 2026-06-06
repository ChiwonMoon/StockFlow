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

    // 즉시 매수/매도 접수 결과 알림
    connect(m_krApi, &KisAPI::orderPlaced, this,
        [this](const QString& symbol, bool isBuy, bool success, const QString& message)
        {
            const QString kind = isBuy ? "매수" : "매도";
            if (success)
                QMessageBox::information(this, kind + " 접수",
                    QString("[%1] %2 주문이 접수되었습니다.\n%3").arg(symbol, kind, message));
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
    // 관심종목 + 보유종목 심볼 모두 갱신 (updateUI가 알맞은 탭으로 라우팅)
    QStringList symbols = m_stockModel->getAllSymbols();
    symbols += QStringList(m_holdingSymbols.begin(), m_holdingSymbols.end());
    symbols.removeDuplicates();
    if (symbols.isEmpty())
        return;

    m_requestCoordinator->refreshStocks(symbols);
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
    QAction* askingPriceAction = isKorean ? menu.addAction("호가 (10호가)") : nullptr;
    if (isKorean) menu.addSeparator();
    QAction* deleteAction = menu.addAction("삭제 (delete)");

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
    else if (selectedItem == askingPriceAction)
        showAskingPrice(model, row);
    else if (selectedItem == deleteAction)
        model->removeRow(row);
}

void MainWindow::reserveSellFor(StockTableModel* model, int row)
{
    const QString symbol = model->symbolAt(row);
    const QString name = model->displayNameAt(row);
    const double curPrice = model->currentPriceAt(row);

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
    priceSpin->setSingleStep(10);
    priceSpin->setGroupSeparatorShown(true);
    priceSpin->setSuffix(" 원");
    priceSpin->setValue(static_cast<int>(curPrice));

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
    const double curPrice = model->currentPriceAt(row);
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
    priceSpin->setSingleStep(10);
    priceSpin->setGroupSeparatorShown(true);
    priceSpin->setSuffix(" 원");
    priceSpin->setValue(static_cast<int>(curPrice));
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
    {
        form->addRow("수량", qtySpin);
        // 매수: 주문가능현금/최대매수수량 표시 (라이브 갱신)
        QLabel* cashLabel = new QLabel("주문가능현금: 조회 중...", &dlg);
        connect(m_krApi, &KisAPI::orderableCashReceived, &dlg,
            [cashLabel, symbol](const QString& s, qint64 cash, int maxQty)
            {
                if (s != symbol) return;
                cashLabel->setText(QString("주문가능현금: %1원  /  최대매수: %2주")
                    .arg(QLocale::system().toString(cash), QString::number(maxQty)));
            });
        m_krApi->fetchOrderableCash(symbol, curPrice, false);
        form->addRow("", cashLabel);
    }
    else
    {
        addSellQtyRow(&dlg, symbol, qtySpin, form); // 매도: 전량 버튼 + 보유수량 상한
    }
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

void MainWindow::tradeDialog(StockTableModel* model, int row, bool isBuy)
{
    const QString symbol = model->symbolAt(row);
    const QString name = model->displayNameAt(row);
    const double curPrice = model->currentPriceAt(row);
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
    priceSpin->setSingleStep(10);
    priceSpin->setGroupSeparatorShown(true);
    priceSpin->setSuffix(" 원");
    priceSpin->setValue(static_cast<int>(curPrice));

    connect(marketCheck, &QCheckBox::toggled, priceSpin, &QSpinBox::setDisabled);

    // 매수일 때만 주문가능현금/최대매수수량 표시
    QLabel* cashLabel = nullptr;
    if (isBuy)
    {
        cashLabel = new QLabel("주문가능현금: 조회 중...", &dlg);
        // 응답이 오면(다이얼로그 exec 이벤트루프에서 처리) 라벨 갱신. 컨텍스트=dlg 로 수명 관리
        connect(m_krApi, &KisAPI::orderableCashReceived, &dlg,
            [cashLabel, symbol](const QString& s, qint64 cash, int maxQty)
            {
                if (s != symbol) return;
                cashLabel->setText(QString("주문가능현금: %1원  /  최대매수: %2주")
                    .arg(QLocale::system().toString(cash), QString::number(maxQty)));
            });
        m_krApi->fetchOrderableCash(symbol, curPrice, false);
    }

    QFormLayout* form = new QFormLayout();
    form->addRow("종목", new QLabel(QString("%1 (%2)").arg(name, symbol), &dlg));
    if (isBuy)
        form->addRow("수량", qtySpin);             // 매수: 상한 없음 (주문가능현금 표시)
    else
        addSellQtyRow(&dlg, symbol, qtySpin, form); // 매도: 전량 버튼 + 보유수량 상한
    form->addRow("", marketCheck);
    form->addRow("지정가", priceSpin);
    if (cashLabel) form->addRow("", cashLabel);

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
        QSpinBox* p = new QSpinBox(&rd);
        p->setRange(0, 1000000000);
        p->setSingleStep(10);
        p->setGroupSeparatorShown(true);
        p->setSuffix(" 원");
        p->setValue(static_cast<int>(o->orderPrice));
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
            m_krApi->reviseOrder(*o, q->value(), p->value());
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
