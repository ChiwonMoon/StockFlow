#pragma once

#include <QAbstractTableModel>
#include <vector>
#include "core/StockData.h"

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

	enum Column
	{
		Symbol = 0,
		Price,
		Change,
		ColumnCount
	};

private:
	std::vector<StockData> m_data;

	QString formatNumber(double value) const;
};