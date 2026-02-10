#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include "StockData.h"

class StockAPI : public QObject
{
	Q_OBJECT

public:
	explicit StockAPI(QObject* parent = nullptr);
	virtual ~StockAPI();

	// 주식 데이터 요청 함수
	virtual void fetchStock(const QString& symbol) = 0;
	virtual void fetchLogo(const QString& symbol) = 0;

signals:
	// 데이터를 다 받으면
	void dataReceived(const StockData& data);
	void logoReceived(const QString& symbol, const QPixmap& logo);

protected:
	QNetworkAccessManager* manager;	// 통신을 담당하는 qt 객체
	void downloadLogoFromUrl(const QString& symbol, const QString& url);

private slots:
	void onGenericLogoDownloaded();
};