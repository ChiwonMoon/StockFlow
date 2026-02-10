#pragma once

#include "StockAPI.h"
#include <QDateTime>

class KisAPI : public StockAPI
{
    Q_OBJECT

public:
    explicit KisAPI(QObject* parent = nullptr);

    void authenticate();
    void fetchStock(const QString& symbol) override;
    void fetchLogo(const QString& symbol) override;

signals:
    void authenticated();

private slots:
       void onAuthFinished(QNetworkReply* reply);
       void onStockReceived(QNetworkReply* reply);
       void onLogoDownloaded(QNetworkReply* reply);

private:
    QString m_accessToken;
    void saveToken(const QString& token, const QDateTime& expiry);
    bool loadToken();
};