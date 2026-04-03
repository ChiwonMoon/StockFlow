#include "StartupCoordinator.h"
#include "KisAPI.h"
#include "StockListSettings.h"

StartupCoordinator::StartupCoordinator(KisAPI* krApi, StockListSettings* settings, QObject* parent)
    : QObject(parent), m_krApi(krApi), m_settings(settings)
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
    if (m_settings)
    {
        return m_settings->load();
    }

    return StockListSettings::defaultSymbols();
}
