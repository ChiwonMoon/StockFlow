#pragma once

#include <QString>

class StockSymbolResolver
{
public:
    QString resolve(const QString& input) const;
};