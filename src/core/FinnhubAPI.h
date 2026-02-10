#pragma once
#include "StockAPI.h"

class FinnhubAPI : public StockAPI
{
    Q_OBJECT
public:
    explicit FinnhubAPI(QObject* parent = nullptr);

    void fetchStock(const QString& symbol) override;
    void fetchLogo(const QString& symbol) override;
    void fetchAllUSSymblos();

signals:
    void symbolsReceived();

private slots:
    // 네트워크 응답이 오면 처리
    void onStockReceived(QNetworkReply* reply);
    void onProfileLoaded(QNetworkReply* reply);
    void onAllSymbolsReceived(QNetworkReply* reply);
};