#pragma once

#include <QMainWindow>
#include <QTimer>
#include "StockTableModel.h"
#include "core/KisAPI.h"
#include "core/FinnhubAPI.h"
#include <QStringListModel>
#include <QEvent>
#include <QInputMethodEvent>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

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

    void updateSearchCompleter();
    void performSearch();
    void requestStock(const QString& sym);
    void requestLogo(const QString& sym);
    bool isKoreanSymbol(const QString& sym) const;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    
};