#include "StockRequestCoordinator.h"
#include "FinnhubAPI.h"
#include "KisAPI.h"

#include <QRegularExpression>

StockRequestCoordinator::StockRequestCoordinator(FinnhubAPI* usApi, KisAPI* krApi, QObject* parent)
    : QObject(parent), m_usApi(usApi), m_krApi(krApi)
{
}

void StockRequestCoordinator::requestStock(const QString& symbol)
{
    if (isKoreanSymbol(symbol))
        m_krApi->fetchStock(symbol);
    else
        m_usApi->fetchStock(symbol);
}

void StockRequestCoordinator::requestLogo(const QString& symbol)
{
    if (isKoreanSymbol(symbol))
        m_krApi->fetchLogo(symbol);
    else
        m_usApi->fetchLogo(symbol);
}

bool StockRequestCoordinator::isKoreanSymbol(const QString& symbol) const
{
    static const QRegularExpression re("^[0-9]{6}$");
    return re.match(symbol).hasMatch();
}