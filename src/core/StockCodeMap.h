#pragma once
#include <QString>
#include <QHash>

class StockCodeMap
{
public:
    // MST 파일을 읽어서 메모리에 저장하는 함수
    static void loadFromMstFiles();

    static QString getName(const QString& code);
    static QString getCodeByName(const QString& name);
    static QStringList getAllSearchKeywords();
    static QStringList searchKeywords(const QString& keyword, int limit = 25);
    static void addStock(const QString& code, const QString& name);

private:
    static QHash<QString, QString> m_map;

    // 내부에서만 쓰는 진짜 파싱 함수
    static void parseMstFile(const QString& filePath);
};