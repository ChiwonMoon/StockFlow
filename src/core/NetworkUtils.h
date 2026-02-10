#pragma once

#include <qobject.h>
#include <QNetworkReply>
#include <QTimer>
#include <QDebug>

class NetworkUtils : public QObject
{
	Q_OBJECT

public:

	static void addTimeOut(QNetworkReply* reply, int ms = 5000)
	{
		if (!reply) return;

		QTimer* timer = new QTimer(reply);
		timer->setSingleShot(true);

		QObject::connect(timer, &QTimer::timeout, [reply]()
		{
			if (reply && reply->isRunning())
			{
				qDebug() << "[NetworkUtils] Timeout reached. Aborting request:" << reply->url().toString();
				reply->abort();
			}
		});

		timer->start(ms);
	}
};