#include "StockTableModel.h"
#include <QColor>
#include <QLocale>

StockTableModel::StockTableModel(QObject* parent)
	: QAbstractTableModel(parent)
{

}

int StockTableModel::rowCount(const QModelIndex& parent) const
{
	if (parent.isValid()) return 0;
	return static_cast<int>(m_data.size());
}

int StockTableModel::columnCount(const QModelIndex& parent) const
{
	if (parent.isValid()) return 0;
	return Column::ColumnCount;
}

bool StockTableModel::removeRow(int row, const QModelIndex& parent)
{
    if (row < 0 || row >= m_data.size())
        return false;

    beginRemoveRows(parent, row, row);
    m_data.erase(m_data.begin() + row);
    endRemoveRows();
    return true;
}

QVariant StockTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    // 가로 방향 헤더(맨 윗줄)이고, 텍스트를 요청할 때
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
    {
        switch (section)
        {
        case Column::Symbol: return "Symbol";
        case Column::Price:  return "Price ($)";
        case Column::Change: return "Change (%)";
        default: return QVariant();
        }
    }
    return QVariant();
}

QVariant StockTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_data.size())
        return QVariant();

    const StockData& stock = m_data[index.row()];

    // 텍스트 보여주기 (DisplayRole)
    if (role == Qt::DisplayRole)
    {
        switch (index.column())
        {
        case Symbol:
            return stock.name != stock.symbol ? stock.name : stock.symbol;
        case Price:
            return formatNumber(stock.currentPrice);
        case Change:
            double change = stock.getChangePercentage();
            return QString("%1%2%").arg(change > 0 ? "+" : "").arg(change, 0, 'f', 2);
        }
    }
    else if (role == Qt::DecorationRole)
    {
        // Symbol 컬럼(0번 열)에만 이미지를 띄웁니다.
        if (index.column() == Symbol)
        {
            // 로고가 비어있지 않다면 리턴
            if (!stock.logo.isNull()) {
                return stock.logo;
            }
        }
    }
    // 글자 색상 입히기 (ForegroundRole)
    else if (role == Qt::ForegroundRole)
    {
        if (index.column() == Change || index.column() == Price)
        {
            double change = stock.getChangePercentage();
            if (change > 0) return QColor(Qt::red);      // 상승: 빨강
            else if (change < 0) return QColor(Qt::blue); // 하락: 파랑
            // (미국 주식은 보통 상승이 초록, 하락이 빨강이지만 한국식으로 맞춤)
        }
    }
    // 텍스트 정렬 (TextAlignmentRole)
    else if (role == Qt::TextAlignmentRole)
    {
        if (index.column() == Symbol) return Qt::AlignCenter; // 심볼은 가운데 정렬
        return int(Qt::AlignRight | Qt::AlignVCenter); // 숫자는 우측 정렬
    }

    return QVariant();
}

void StockTableModel::setStockData(const std::vector<StockData>&data)
{
    beginResetModel();  // ui 알림
    m_data = data;
    endResetModel();
}

void StockTableModel::addStockData(const StockData& data)
{
    // 데이터 하나 추가할 때 효율적으로 갱신 (전체 새로고침 X)
    int row = m_data.size();
    beginInsertRows(QModelIndex(), row, row);
    m_data.push_back(data);
    endInsertRows();
}

void StockTableModel::clear()
{
    beginResetModel();
    m_data.clear();
    endResetModel();
}

void StockTableModel::updataOrInsert(const StockData& data)
{
    for (int i = 0; i < m_data.size(); ++i)
    {
        // 이미 있는 종목
        if (m_data[i].symbol == data.symbol)
        {
            // 로고 유지
            QPixmap existingLogo = m_data[i].logo;
            // 직전가 비교
            double oldPrice = m_data[i].currentPrice;

            m_data[i] = data;
            m_data[i].previousPrice = oldPrice;

            if (data.logo.isNull() && !existingLogo.isNull())
            {
                m_data[i].logo = existingLogo;
            }

            QModelIndex topLeft = index(i, 0);
            QModelIndex bottomRight = index(i, ColumnCount - 1);
            emit dataChanged(topLeft, bottomRight);

            return;
        }
    }
    addStockData(data);
}

void StockTableModel::updateLogo(const QString& symbol, const QPixmap& logo)
{
    for (int i = 0; i < m_data.size(); ++i)
    {
        if (m_data[i].symbol == symbol)
        {
            m_data[i].logo = logo.scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation); // 크기 24x24로 조절

            // 다시 그림
            QModelIndex idx = index(i, 0);
            emit dataChanged(idx, idx, { Qt::DecorationRole });
            return;
        }
    }
}

bool StockTableModel::isPriceChanged(int row) const
{
    if (row < 0 || row >= m_data.size()) return false;
    return !qFuzzyCompare(m_data[row].currentPrice, m_data[row].previousPrice);
}

QStringList StockTableModel::getAllSymbols() const
{
    QStringList symbols;
    for (const StockData &item : m_data)
    {
        symbols << item.symbol;
    }
    return symbols;
}

QString StockTableModel::formatNumber(double value) const
{
    // 정수인지 확인 (한국 주식은 보통 소수점이 없음)
    // 예: 74500.0 == 74500 (True)
    if (value == (int)value)
    {
        // 소수점 0자리 (예: 74,500)
        return QLocale().toString(value, 'f', 0);
    }
    else
    {
        // 소수점 2자리 (예: 185.50)
        return QLocale().toString(value, 'f', 2);
    }
}
