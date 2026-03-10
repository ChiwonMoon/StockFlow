#pragma once

#include <QObject>
#include <QStringList>

class KisAPI;

class StartupCoordinator : public QObject
{
	Q_OBJECT

public:
	explicit StartupCoordinator(KisAPI* krApi, QObject* parent = nullptr);

	void start();
	QStringList initialSymbols() const;

signals:
	void initialReady();

private:
	KisAPI* m_krApi = nullptr;
};