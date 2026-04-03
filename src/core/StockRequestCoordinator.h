#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class FinnhubAPI;
class KisAPI;
struct StockData;
class StockTableModel;

class StockRequestCoordinator : public QObject
{
    Q_OBJECT

public:
    explicit StockRequestCoordinator(FinnhubAPI* usApi, KisAPI* krApi, QObject* parent = nullptr);

    void requestStock(const QString& symbol);
    void requestLogo(const QString& symbol);
    void handleStockData(const StockData& data, StockTableModel* model);
    void refreshStocks(const QStringList& symbols);

private:
    bool isKoreanSymbol(const QString& symbol) const;

private:
    FinnhubAPI* m_usApi = nullptr;
    KisAPI* m_krApi = nullptr;
};