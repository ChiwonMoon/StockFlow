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

#include <QCompleter>
#include <QEvent>
#include <QHeaderView>
#include <QInputMethodEvent>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringListModel>
#include <QTimer>

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
    // 모델 및 테이블 설정
    m_stockModel = new StockTableModel(this);
    ui->tableView->setModel(m_stockModel);
    ui->tableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);  // 화면 너비에 맞게 늘리기
    ui->tableView->setContextMenuPolicy(Qt::CustomContextMenu); // 우클릭메뉴 활성화

    StockItemDelegate* delegate = new StockItemDelegate(this);
    ui->tableView->setItemDelegate(delegate);

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
    connect(ui->tableView, &QTableView::customContextMenuRequested, this, &MainWindow::onTableContextMenu);

    // 데이터 수신
    connect(m_usApi, &StockAPI::dataReceived, this, &MainWindow::updateUI);
    connect(m_krApi, &KisAPI::dataReceived, this, &MainWindow::updateUI);

    // 로고
    connect(m_usApi, &StockAPI::logoReceived, this,
        [this](QString symbol, QPixmap logo) { m_stockModel->updateLogo(symbol, logo); });
    connect(m_krApi, &StockAPI::logoReceived, this,
        [this](QString symbol, QPixmap logo) { m_stockModel->updateLogo(symbol, logo); });

    // 한국투자증권 로그인 토큰 발급
    connect(m_startupCoordinator, &StartupCoordinator::initialReady, this, &MainWindow::onInitialReady);
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
}

void MainWindow::setupSearch()
{
    // 검색
    connect(m_usApi, &FinnhubAPI::symbolsReceived, this, &MainWindow::updateSearchCompleter);
    connect(ui->editSearch, &QLineEdit::textEdited, this, &MainWindow::onSearchTextEdited);
}

void MainWindow::startServices()
{
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
    m_requestCoordinator->handleStockData(data, m_stockModel);
}

void MainWindow::onRefreshClicked()
{
    QStringList symbols = m_stockModel->getAllSymbols();
    if (symbols.isEmpty())
    {
        return;
    }
    
    m_requestCoordinator->refreshStocks(symbols);
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

void MainWindow::onTableContextMenu(const QPoint& pos)
{
    QModelIndex index = ui->tableView->indexAt(pos);
    if (!index.isValid()) return;

    QMenu menu(this);
    QAction* deleteAction = menu.addAction("삭제 (delete)");
    // 메뉴 띄우고 기다림
    QAction* selectedItem = menu.exec(ui->tableView->viewport()->mapToGlobal(pos));
    if (selectedItem == nullptr) return; // 사용자가 메뉴 밖을 클릭해서 취소함
    
    if (selectedItem == deleteAction)
    {
        int row = index.row();

        // 모델에서 삭제
        m_stockModel->removeRow(row);
    }
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
