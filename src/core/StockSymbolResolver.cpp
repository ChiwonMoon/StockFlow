#include "StockSymbolResolver.h"
#include "StockCodeMap.h"

#include <QRegularExpression>

QString StockSymbolResolver::resolve(const QString& input) const
{
    QString normalized = input.trimmed().toUpper();
    if (normalized.isEmpty())
        return {};

    QString targetSymbol = normalized;

    static const QRegularExpression re("\\(([^)]+)\\)$");
    const QRegularExpressionMatch match = re.match(normalized);

    if (match.hasMatch())
    {
        return match.captured(1);
    }

    const QString name = StockCodeMap::getName(targetSymbol);
    const QString code = StockCodeMap::getCodeByName(name);

    if (targetSymbol != code)
    {
        if (code.isEmpty())
            return {};
        targetSymbol = code;
    }

    return targetSymbol;
}