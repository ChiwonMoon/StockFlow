#include "StartupCoordinator.h"
#include "KisAPI.h"
#include "Config.h"

#include <QSettings>

StartupCoordinator::StartupCoordinator(KisAPI* krApi, QObject* parent)
    : m_krApi(krApi), QObject(parent)
{
    if (m_krApi)
    {
        connect(m_krApi, &KisAPI::authenticated, this, &StartupCoordinator::initialReady);
    }
}

void StartupCoordinator::start()
{
    if (m_krApi)
        m_krApi->authenticate();
}

QStringList StartupCoordinator::initialSymbols() const
{
    QSettings settings(Config::SETTINGS_COMPANY, Config::SETTINGS_APP);
    QStringList symbols = settings.value(Config::KEY_FAVORITES).toStringList();

    if (symbols.isEmpty())
    {
        symbols = { "AAPL", "GOOGL", "NVDA", "005930", "000660", "005380" };
    }

    return symbols;
}
