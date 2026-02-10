#pragma once

#include <QStyledItemDelegate>

class StockItemDelegate : public QStyledItemDelegate
{
	Q_OBJECT

public:
	explicit StockItemDelegate(QObject* parent = nullptr);

	void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
	void drawPriceBorder(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const;
};