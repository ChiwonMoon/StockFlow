#pragma once

#include <QObject>
#include <QStringList>

class KisAPI;
class StockListSettings;

class StartupCoordinator : public QObject
{
	Q_OBJECT

public:
	explicit StartupCoordinator(KisAPI* krApi, StockListSettings* settings, QObject* parent = nullptr);

	void start();
	QStringList initialSymbols() const;

signals:
	void initialReady();

private:
	KisAPI* m_krApi = nullptr;
	StockListSettings* m_settings = nullptr;
};
