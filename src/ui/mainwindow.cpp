#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "StockItemDelegate.h"
#include "core/FinnhubAPI.h"
#include "core/KisAPI.h"
#include <QMessageBox>
#include <QRegularExpression>
#include <QPushButton>
#include <QHeaderView>
#include "core/StockCodeMap.h"
#include <QCompleter>
#include <QStringListModel>
#include <QMenu>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    ui->editSearch->installEventFilter(this);
    ui->editSearch->setEnabled(false);
    ui->btnSearch->setEnabled(false);
    ui->editSearch->setPlaceholderText("데이터 로딩 중... 잠시만 기다려주세요 ⏳");

    m_stockModel = new StockTableModel(this);
    ui->tableView->setModel(m_stockModel);
    ui->tableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->tableView->setContextMenuPolicy(Qt::CustomContextMenu); // 우클릭메뉴

    StockItemDelegate* delegate = new StockItemDelegate(this);
    ui->tableView->setItemDelegate(delegate);

    m_searchModel = new QStringListModel(this);

    // API 객체 생성
    m_usApi = new FinnhubAPI(this);
    m_krApi = new KisAPI(this);

    m_usApi->fetchAllUSSymblos();

    // 버튼 클릭 시
    ui->btnRefresh->setShortcut(Qt::Key_F5);
    connect(ui->btnRefresh, &QPushButton::clicked, this, &MainWindow::onRefreshClicked);
    connect(ui->editSearch, &QLineEdit::returnPressed, ui->btnSearch, &QPushButton::click);
    connect(ui->btnSearch, &QPushButton::clicked, this, &MainWindow::onSearchClicked);
    connect(ui->tableView, &QTableView::customContextMenuRequested, this, &MainWindow::onTableContextMenu);
    // 데이터 도착 시
    connect(m_usApi, &StockAPI::dataReceived, this, &MainWindow::updataUI);
    connect(m_krApi, &KisAPI::dataReceived, this, &MainWindow::updataUI);
    // 로고
    connect(m_usApi, &StockAPI::logoReceived, this, 
        [this](QString symbol, QPixmap logo) { m_stockModel->updateLogo(symbol, logo); }
    );
    connect(m_krApi, &StockAPI::logoReceived, this,
        [this](QString symbol, QPixmap logo) { m_stockModel->updateLogo(symbol, logo); }
    );
    // 한국 로그인 토큰 발급
    connect(m_krApi, &KisAPI::authenticated, this, [this]() { this->onRefreshClicked(); }
    );

    m_krApi->authenticate();
    // 자동 갱신
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &MainWindow::onRefreshClicked);
    connect(m_timer, &QTimer::timeout, []() { qDebug() << "time out"; });
    m_timer->start(10000);

    // 검색
    connect(m_usApi, &FinnhubAPI::symbolsReceived, this, &MainWindow::updateSearchCompleter);
    m_debounceTimer = new QTimer(this);
    m_debounceTimer->setSingleShot(true);
    m_debounceTimer->setInterval(300);
    connect(m_debounceTimer, &QTimer::timeout, this, &MainWindow::performSearch);
    connect(ui->editSearch, &QLineEdit::textEdited, this, &MainWindow::onSearchTextEdited);

    // 시작하자마자 한번 가져오기
    onRefreshClicked();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::updataUI(const StockData& data)
{
    m_stockModel->updataOrInsert(data);
}

void MainWindow::onRefreshClicked()
{
    // 버튼 누르면 데이터 + 로고 요청
    QStringList symbols = m_stockModel->getAllSymbols();
    if(symbols.isEmpty())
        symbols = { "AAPL", "GOOGL", "NVDA" , "005930", "000660", "005380"};
    
    for (const QString& sym : symbols)
    {
        QRegularExpression re("^[0-9]{6}$");

        if (re.match(sym).hasMatch())
        {
            m_krApi->fetchStock(sym);
            m_krApi->fetchLogo(sym);
        }
        else
        {
            m_usApi->fetchStock(sym); // 가격 요청
            m_usApi->fetchLogo(sym);  // 로고 요청 (비동기라 순서 상관 없음)
        }
    }
}

void MainWindow::onSearchClicked()
{
    // 공백제거 대문자 변환
    QString input = ui->editSearch->text().trimmed().toUpper();
    if (input.isEmpty()) return;

    QString targetSymbol = input;

    static QRegularExpression re("\\(([^)]+)\\)$");
    QRegularExpressionMatch match = re.match(input);

    if (match.hasMatch())
    {
        // 괄호 안의 내용(코드)만 추출해서 타겟으로 설정
        targetSymbol = match.captured(1);
    }
    else {
        // 2. 괄호가 없다면?
        targetSymbol = targetSymbol.toUpper();

        // 혹시 한글 이름인가? -> 코드로 변환 시도
        QString code = StockCodeMap::getCodeByName(targetSymbol);
        if (!code.isEmpty())
        {
            targetSymbol = code;
        }
    }

    bool isKorean = QRegularExpression("^[0-9]{6}$").match(targetSymbol).hasMatch();

    if (isKorean)
    {
        m_krApi->fetchStock(targetSymbol);
        m_krApi->fetchLogo(targetSymbol);
    }
    else
    {
        m_usApi->fetchStock(targetSymbol);
        m_usApi->fetchLogo(targetSymbol);
    }

    ui->editSearch->clear();
}

void MainWindow::onSearchTextEdited(const QString& text)
{
    m_pendingText.clear();
    m_debounceTimer->start();
    qDebug() << "검색타이머";
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

        // 5. 모델에서 삭제
        m_stockModel->removeRow(row);
    }
}

void MainWindow::updateSearchCompleter()
{
    //QStringList wordList = StockCodeMap::getAllSearchKeywords();
    //QCompleter* completer = new QCompleter(wordList, this);
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
    QString text;
    if (m_pendingText.isEmpty())
        m_pendingText = ui->editSearch->text();

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
    // 검색창 입력
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
        qDebug() << "한글검색타이머시작";

        // 주의: return false를 해야 QLineEdit 본체도 이벤트를 받아서 글자를 화면에 그립니다.
        return false;
    }

    // 그 외 다른 이벤트는 건드리지 않고 통과
    return QMainWindow::eventFilter(obj, event);
}
