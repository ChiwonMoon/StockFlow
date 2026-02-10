#pragma once
#include <QString>
#include <QPixmap>

struct StockData
{
	QString symbol;	// 티커 
	QString name;	// 종목명
	QPixmap logo;

	double currentPrice; // 현재가
	double previousPrice; // 직전가
	double openPrice;	// 시가
	double highPrice;	// 고가
	double lowPrice;	// 저가
	double prevClose;	// 전일 종가
	long long volume;	// 거래량

	// 변동률 계산 함수
	double getChangePercentage() const
	{
		if (prevClose == 0) return 0.0;
		return ((currentPrice - prevClose) / prevClose) * 100.0;
	}
};