#pragma once

#include <QObject>
#include <QString>

class FinnhubAPI;
class KisAPI;

class StockRequestCoordinator : public QObject
{
    Q_OBJECT

public:
    explicit StockRequestCoordinator(FinnhubAPI* usApi, KisAPI* krApi, QObject* parent = nullptr);

    void requestStock(const QString& symbol);
    void requestLogo(const QString& symbol);

private:
    bool isKoreanSymbol(const QString& symbol) const;

private:
    FinnhubAPI* m_usApi = nullptr;
    KisAPI* m_krApi = nullptr;
};