#include "StockTableModel.h"
#include <QColor>
#include <QLocale>

StockTableModel::StockTableModel(QObject* parent) : QAbstractTableModel(parent)
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
	// 관심종목 탭에는 수량/매입가/손익이 의미 없으므로 숨긴다
	return m_holdingsMode ? Column::ColumnCount : (Column::Change + 1);
}

void StockTableModel::setHoldingsMode(bool on)
{
	if (m_holdingsMode == on) return;
	beginResetModel();
	m_holdingsMode = on;
	endResetModel();
}

void StockTableModel::setHoldingDetails(const QHash<QString, KisHoldingDetail>& details)
{
	m_details = details;
	if (!m_holdingsMode || m_data.empty()) return;

	// 수량/매입가/손익 컬럼만 다시 그리게 한다
	const QModelIndex topLeft = index(0, Column::Qty);
	const QModelIndex bottomRight = index(static_cast<int>(m_data.size()) - 1, Column::Pnl);
	emit dataChanged(topLeft, bottomRight);
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
        case Column::Symbol:   return "Symbol";
        case Column::Price:    return "Price ($)";
        case Column::Change:   return "Change (%)";
        case Column::Qty:      return "수량";
        case Column::AvgPrice: return "매입가";
        case Column::Pnl:      return "평가손익";
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
        {
            double change = stock.getChangePercentage();
            return QString("%1%2%").arg(change > 0 ? "+" : "").arg(change, 0, 'f', 2);
        }
        case Qty:
        case AvgPrice:
        case Pnl:
        {
            if (!m_details.contains(stock.symbol))
                return "-";
            const KisHoldingDetail& d = m_details.value(stock.symbol);
            const QLocale loc = QLocale::system();

            if (index.column() == Qty)
            {
                // 걸어둔 주문 때문에 주문가능수량이 적으면 같이 보여준다
                if (d.ordPsblQty < d.holdQty)
                    return QString("%1 (가능 %2)").arg(loc.toString(d.holdQty)).arg(loc.toString(d.ordPsblQty));
                return loc.toString(d.holdQty);
            }
            if (index.column() == AvgPrice)
                return loc.toString(d.avgPrice, 'f', 0);

            return QString("%1%2 (%3%4%)")
                .arg(d.evalPnl > 0 ? "+" : "")
                .arg(loc.toString(d.evalPnl, 'f', 0))
                .arg(d.pnlRate > 0 ? "+" : "")
                .arg(d.pnlRate, 0, 'f', 2);
        }
        }
    }
    // 이미지 표시 (DecorationRole - 로고)
    else if (role == Qt::DecorationRole)
    {
        // Symbol 컬럼(0번 열)에만 이미지를 띄웁니다.
        if (index.column() == Symbol && !stock.logo.isNull())
        {
            return stock.logo;
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
        }
        else if (index.column() == Pnl && m_details.contains(stock.symbol))
        {
            const double pnl = m_details.value(stock.symbol).evalPnl;
            if (pnl > 0) return QColor(Qt::red);         // 이익: 빨강
            else if (pnl < 0) return QColor(Qt::blue);   // 손실: 파랑
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
    beginResetModel();
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

void StockTableModel::updateOrInsert(const StockData& data)
{
    for (int i = 0; i < m_data.size(); ++i)
    {
        // 이미 있는 종목
        if (m_data[i].symbol == data.symbol)
        {
            // 로고 유지
            QPixmap existingLogo = m_data[i].logo;
            // 이전가격 저장
            double oldPrice = m_data[i].currentPrice;

            // 데이터 갱신
            m_data[i] = data;
            // 이전가격 복구
            m_data[i].previousPrice = oldPrice;

            if (data.logo.isNull() && !existingLogo.isNull())
                m_data[i].logo = existingLogo;

            // 업데이트 알림
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
            // 크기 24x24로 조절
            m_data[i].logo = logo.scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation);

            // 업데이트 알림
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

QString StockTableModel::symbolAt(int row) const
{
	if (row < 0 || row >= static_cast<int>(m_data.size())) return QString();
	return m_data[row].symbol;
}

QString StockTableModel::displayNameAt(int row) const
{
	if (row < 0 || row >= static_cast<int>(m_data.size())) return QString();
	const StockData& stock = m_data[row];
	return stock.name != stock.symbol ? stock.name : stock.symbol;
}

double StockTableModel::currentPriceAt(int row) const
{
	if (row < 0 || row >= static_cast<int>(m_data.size())) return 0.0;
	return m_data[row].currentPrice;
}

double StockTableModel::tickSizeAt(int row) const
{
    if (row < 0 || row >= static_cast<int>(m_data.size())) return 0.0;
    return m_data[row].tickSize;
}

bool StockTableModel::hasLogo(const QString& symbol) const
{
    for (const auto& stock : m_data)
    {
        if (stock.symbol == symbol)
            return !stock.logo.isNull();
    }

    return false;
}

bool StockTableModel::contains(const QString& symbol) const
{
    for (const auto& stock : m_data)
    {
        if (stock.symbol == symbol)
            return true;
    }

    return false;
}

QString StockTableModel::formatNumber(double value) const
{
    // 정수인지 확인 (한국 주식은 보통 소수점이 없음)
    return QLocale::system().toString(value, 'f', (value == (int)value) ? 0 : 2);
}
