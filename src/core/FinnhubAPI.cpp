#include "FinnhubAPI.h"
#include "Config.h"
#include "NetworkUtils.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>
#include <QDebug>
#include "StockCodeMap.h"
#include <QJsonArray>

FinnhubAPI::FinnhubAPI(QObject* parent) : StockAPI(parent)
{
}

void FinnhubAPI::fetchStock(const QString& symbol)
{
	// URL
	QUrl url(Config::FINNHUB_BASE_URL + "/quote");

	QUrlQuery query;
	query.addQueryItem("symbol", symbol);
	query.addQueryItem("token", Config::FINNHUB_API_KEY);
	url.setQuery(query);

	// 요청
	QNetworkRequest request(url);
	QNetworkReply* reply = manager->get(request);

	// 심볼 기억하기 (꼬리표)
	reply->setProperty("TargetSymbol", symbol);

	NetworkUtils::addTimeOut(reply);

	// 응답
	connect(
		reply,
		&QNetworkReply::finished,
		[this, reply]()
		{this->onStockReceived(reply); }
	);

	//qDebug() << "Requesting stock data for:" << symbol;
}

void FinnhubAPI::fetchLogo(const QString& symbol)
{
	// 기업 정보(Profile2) API 호출
	QUrl url(Config::FINNHUB_BASE_URL + "/stock/profile2");
	QUrlQuery query;
	query.addQueryItem("symbol", symbol);
	query.addQueryItem("token", Config::FINNHUB_API_KEY);
	url.setQuery(query);

	QNetworkRequest request(url);
	QNetworkReply* reply = manager->get(request);

	// 심볼 기억하기 (꼬리표)
	reply->setProperty("TargetSymbol", symbol);

	NetworkUtils::addTimeOut(reply);

	connect(
		reply,
		&QNetworkReply::finished,
		[this, reply]()
		{ this->onProfileLoaded(reply); }
	);
}

void FinnhubAPI::fetchAllUSSymblos()
{
	QUrl url(Config::FINNHUB_BASE_URL + "/stock/symbol");
	QUrlQuery query;
	query.addQueryItem("exchange", "US");
	query.addQueryItem("token", Config::FINNHUB_API_KEY);
	url.setQuery(query);

	QNetworkRequest request(url);
	QNetworkReply* reply = manager->get(request);

	NetworkUtils::addTimeOut(reply);

	connect(
		reply,
		&QNetworkReply::finished,
		[this, reply]()
		{ this->onAllSymbolsReceived(reply); }
	);
}

void FinnhubAPI::onStockReceived(QNetworkReply* reply)
{
	// 메모리 해제 예약
	reply->deleteLater();

	// 에러체크
	if (reply->error() != QNetworkReply::NoError)
	{
		qDebug() << "Network Error:" << reply->errorString();
		return;
	}

	// JSON 파싱
	QByteArray responseData = reply->readAll();
	QJsonDocument jsonDoc = QJsonDocument::fromJson(responseData);
	QJsonObject jsonObj = jsonDoc.object();

	if (!jsonObj.contains("c"))
	{
		qDebug() << "Invalid Data format";
		return;
	}

	// 데이터를 구조체에 담기
	StockData data;

	data.symbol = reply->property("TargetSymbol").toString();
	data.currentPrice = jsonObj["c"].toDouble();
	data.highPrice = jsonObj["h"].toDouble();
	data.lowPrice = jsonObj["l"].toDouble();
	data.openPrice = jsonObj["o"].toDouble();
	data.prevClose = jsonObj["pc"].toDouble();

	data.name = StockCodeMap::getName(data.symbol);

	//qDebug() << "Data Received! Price:" << data.currentPrice;

	// UI
	emit dataReceived(data);
}

void FinnhubAPI::onProfileLoaded(QNetworkReply* reply)
{
	reply->deleteLater();

	if (reply->error() != QNetworkReply::NoError)
	{
		qDebug() << "Profile Error:" << reply->errorString();
		return;
	}

	QString symbol = reply->property("TargetSymbol").toString();
	QByteArray responseData = reply->readAll();
	QJsonDocument jsonDoc = QJsonDocument::fromJson(responseData);
	QJsonObject jsonObj = jsonDoc.object();

	// "logo" 라는 키에 이미지 주소(https://...)가 들어있음
	if (jsonObj.contains("logo"))
	{
		QString logoUrl = jsonObj["logo"].toString();
		if (!logoUrl.isEmpty())
		{
			downloadLogoFromUrl(symbol, logoUrl);
		}
	}
}

void FinnhubAPI::onAllSymbolsReceived(QNetworkReply* reply)
{
	reply->deleteLater();

	if (reply->error() != QNetworkReply::NoError)
	{
		qDebug() << "List Error:" << reply->errorString();
		int waitTime = 2000;
		QTimer::singleShot(waitTime, this, &FinnhubAPI::fetchAllUSSymblos);
		return;
	}

	QByteArray data = reply->readAll();
	QJsonDocument doc = QJsonDocument::fromJson(data);

	if (!doc.isArray()) return;

	QJsonArray array = doc.array();

	// 하나씩 꺼내서 StockCodeMap에 등록
	for (const QJsonValue& val : array)
	{
		QJsonObject obj = val.toObject();

		// Finnhub 데이터 구조:
		// "symbol": "AAPL"
		// "description": "APPLE INC"
		QString symbol = obj["symbol"].toString();
		QString name = obj["description"].toString();

		if (name.isEmpty())
		{
			name = symbol;
		}

		// 맵에 등록!
		StockCodeMap::addStock(symbol, name);
	}
	emit symbolsReceived();
}
