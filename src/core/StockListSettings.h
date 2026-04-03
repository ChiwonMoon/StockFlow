#pragma once

#include <QStringList>

class StockListSettings
{
public:
    // 저장된 심볼을 불러옵니다. 저장된 값이 없으면 기본 종목을 반환합니다.
    QStringList load() const;

    // 심볼을 영구 설정에 저장합니다
    void save(const QStringList& symbols) const;

    // 기본 폴백(대체) 종목을 반환합니다
    static QStringList defaultSymbols();
};
