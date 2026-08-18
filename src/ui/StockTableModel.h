#pragma once

#include <QAbstractTableModel>
#include <vector>
#include "core/StockData.h"
#include "core/KisAPI.h"
#include <QHash>


class StockTableModel : public QAbstractTableModel
{
	Q_OBJECT

public:
	explicit StockTableModel(QObject* parent = nullptr);

	int rowCount(const QModelIndex& parent = QModelIndex()) const override;
	int columnCount(const QModelIndex& parent = QModelIndex()) const override;
	bool removeRow(int row, const QModelIndex& parent = QModelIndex());

	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
	QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

	void setStockData(const std::vector<StockData>& data);
	void addStockData(const StockData& data);
	void clear();
	void updateOrInsert(const StockData& data);
	void updateLogo(const QString& symbol, const QPixmap& logo);

	bool isPriceChanged(int row) const;
	QStringList getAllSymbols() const;
	bool hasLogo(const QString& symbol) const;
	bool contains(const QString& symbol) const;

	// 행 단위 조회 (컨텍스트 메뉴/주문 등에서 사용)
	QString symbolAt(int row) const;
	QString displayNameAt(int row) const;
	double currentPriceAt(int row) const;
	double tickSizeAt(int row) const;   // 호가단위(0이면 모름)

	// 보유종목 탭에서만 쓰는 컬럼들. 관심종목 탭은 Change 까지만 보여준다.
	enum Column
	{
		Symbol = 0,
		Price,
		Change,
		Qty,        // 보유수량
		AvgPrice,   // 매입평균가
		Pnl,        // 평가손익 (금액 + 수익률)
		ColumnCount
	};

	// 보유종목 모드로 켜면 수량/매입가/손익 컬럼이 나타난다
	void setHoldingsMode(bool on);
	// 잔고에서 받은 종목별 현황을 넣어준다 (수량/매입가/손익 표시에 사용)
	void setHoldingDetails(const QHash<QString, KisHoldingDetail>& details);

private:
	std::vector<StockData> m_data;
	bool m_holdingsMode = false;
	QHash<QString, KisHoldingDetail> m_details;

	QString formatNumber(double value) const;
};