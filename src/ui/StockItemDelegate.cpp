#include "StockItemDelegate.h"
#include "StockTableModel.h"
#include <QPainter>
#include <QPen>

StockItemDelegate::StockItemDelegate(QObject* parent) : QStyledItemDelegate(parent)
{
}

void StockItemDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
	// 기본 그리기
	QStyledItemDelegate::paint(painter, option, index);

	switch (index.column())
	{
	case StockTableModel::Price:
		drawPriceBorder(painter, option, index);
		break;
	default:
		break;
	}
}

void StockItemDelegate::drawPriceBorder(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
	const StockTableModel* model = static_cast<const StockTableModel*>(index.model());
	if (!model) return;
	if (!model->isPriceChanged(index.row())) return;

	// 모델에서 글자 색상(빨강/파랑)을 가져옴
	QVariant colorVariant = index.data(Qt::ForegroundRole);

	if (colorVariant.isValid() && colorVariant.canConvert<QColor>())
	{
		painter->save();

		QColor borderColor = colorVariant.value<QColor>();

		QPen pen(borderColor);
		pen.setWidth(2);	// 테두리 두께
		painter->setPen(pen);

		// 테두리가 짤리지 않게 약간 안쪽으로
		QRect rect = option.rect.adjusted(1, 1, -1, -1);
		painter->drawRect(rect);

		painter->restore();
	}
}
