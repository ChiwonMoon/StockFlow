#include "StockListSettings.h"
#include "Config.h"

#include <QSettings>

// 저장된 심볼을 불러옵니다. 저장된 값이 없으면 defaultSymbols를 반환합니다.
QStringList StockListSettings::load() const
{
    QSettings settings(Config::SETTINGS_COMPANY, Config::SETTINGS_APP);
    QStringList symbols = settings.value(Config::KEY_FAVORITES).toStringList();

    if (symbols.isEmpty())
    {
        symbols = defaultSymbols();
    }

    return symbols;
}

// 심볼 목록을 QSettings에 저장합니다.
void StockListSettings::save(const QStringList& symbols) const
{
    QSettings settings(Config::SETTINGS_COMPANY, Config::SETTINGS_APP);
    settings.setValue(Config::KEY_FAVORITES, symbols);
}

// 기본 폴백 종목 목록을 반환합니다.
QStringList StockListSettings::defaultSymbols()
{
    return { "AAPL", "GOOGL", "NVDA", "005930", "000660", "005380" };
}
