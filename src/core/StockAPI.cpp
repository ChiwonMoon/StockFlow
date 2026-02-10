#include "Config.h"
#include "StockAPI.h"
#include "NetworkUtils.h"

StockAPI::StockAPI(QObject* parent)	: QObject(parent)
{
	// 통신 관리자 생성
	manager = new QNetworkAccessManager(this);
}

StockAPI::~StockAPI()
{
	if (manager)
	{
		delete manager;
		manager = nullptr;
	}
}

void StockAPI::downloadLogoFromUrl(const QString& symbol, const QString& url)
{
	QNetworkRequest request((QUrl(url)));
	QNetworkReply* reply = manager->get(request);
	reply->setProperty("TargetSymbol", symbol);
	NetworkUtils::addTimeOut(reply, 10000);

	connect(reply, &QNetworkReply::finished, this, &StockAPI::onGenericLogoDownloaded);
}

void StockAPI::onGenericLogoDownloaded()
{
	QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
	if (!reply) return;

	reply->deleteLater(); // 메모리 해제 예약

	if (reply->error() != QNetworkReply::NoError)
	{
		qDebug() << "Logo Download Error:" << reply->errorString();
		return;
	}

	// 데이터 -> 이미지 변환
	QByteArray data = reply->readAll();
	QPixmap logo;
	if (logo.loadFromData(data))
	{
		QString symbol = reply->property("TargetSymbol").toString();
		emit logoReceived(symbol, logo);
	}
}