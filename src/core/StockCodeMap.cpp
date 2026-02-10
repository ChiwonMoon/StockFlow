#include "StockCodeMap.h"
#include <QFile>
#include <QDebug>
#include <QStringDecoder>
#include <QDir>

QHash<QString, QString> StockCodeMap::m_map;

void StockCodeMap::loadFromMstFiles()
{
    qDebug() << "증권사 마스터 파일 로딩 시작...";

    // 코스피, 코스닥 둘 다 읽습니다.
    qDebug() << "현재 실행 위치:" << QDir::currentPath();
    parseMstFile("kospi_code.mst");
    parseMstFile("kosdaq_code.mst");

    qDebug() << "로딩 완료! 총" << m_map.size() << "개 종목 등록됨.";
}

void StockCodeMap::parseMstFile(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
    {
        qDebug() << "파일을 찾을 수 없음:" << filePath;
        return;
    }

    // ★ 핵심 1: EUC-KR (CP949) 디코더 생성
    // MST 파일은 옛날 방식이라 UTF-8이 아닙니다.
    auto toUtf16 = QStringDecoder(QStringDecoder::System);

    // 한 줄씩 읽기
    while (!file.atEnd()) {
        QByteArray line = file.readLine();

        // 데이터가 너무 짧으면 패스 (빈 줄 등 방지)
        if (line.size() < 30) continue;

        // ★ 핵심 2: 바이트 단위로 자르기 (한글 때문에 바이트로 잘라야 안전함)
        // [0~9]: 종목코드 (FullCode 아님, 단축코드 A포함)
        QByteArray codeBytes = line.mid(0, 9);

        // [21~61]: 한글 종목명 (40바이트 할당됨)
        QByteArray nameBytes = line.mid(21, 40);

        // ★ 핵심 3: 변환 및 가공
        QString code = QString(codeBytes).trimmed(); // 공백 제거
        QString name = QString(toUtf16(nameBytes)).trimmed(); // EUC-KR -> UTF-16 변환 후 공백 제거

        // 코드는 보통 "A005930" 처럼 옴 -> 맨 앞 'A' 제거해야 우리가 쓰는 "005930"이 됨
        // (가끔 Q로 시작하는 것도 있음, 길이는 7자리여야 정상)
        if (code.length() >= 7)
        {
            code = code.mid(1); // 맨 앞 글자 자르기
        }

        // 맵에 저장 (비어있지 않은 것만)
        if (!code.isEmpty() && !name.isEmpty())
        {
            m_map.insert(code, name);
        }
    }

    file.close();
    qDebug() << filePath << "파싱 완료";
}

QString StockCodeMap::getName(const QString& code)
{
    return m_map.value(code, code);
}

QString StockCodeMap::getCodeByName(const QString& name)
{
    return m_map.key(name, "없음");
}

QStringList StockCodeMap::getAllSearchKeywords()
{
    QStringList list;
    list.reserve(m_map.size() * 2);

    QHashIterator<QString, QString> i(m_map);
    while (i.hasNext())
    {
        i.next();
        list << i.value();
        list << i.key();
    }

    list.sort();

    return list;
}

QStringList StockCodeMap::searchKeywords(const QString& keyword, int limit)
{
    QStringList list;
    if (keyword.isEmpty()) return list;

    QString lowerKey = keyword.toLower();
    QHashIterator<QString, QString> i(m_map);
    QStringList highPriority; // 일치
    QStringList midPriority; // 코드, 이름검색
    QStringList lowPriority; // 그냥 포함

    while (i.hasNext())
    {
        i.next();

        QString code = i.key();
        QString name = i.value();

        QString lowerCode = code.toLower();
        QString lowerName = name.toLower();

        QString display = QString("%1 (%2)").arg(name, code);

        if ((lowerCode == lowerKey || lowerName ==lowerKey) && highPriority.size() < limit)
        {
            highPriority.append(display);
        }
        else if ((lowerCode.startsWith(lowerKey) || lowerName.startsWith(lowerKey))
            && midPriority.size() < limit)
        {
            midPriority.append(display);
        }
        else if (lowerCode.contains(lowerKey) || lowerName.contains(lowerKey))
        {
            if(highPriority.size() + midPriority.size() + lowPriority.size() < limit)
                lowPriority.append(display);
        }
    }

    highPriority.sort();
    midPriority.sort();
    lowPriority.sort();

    list = highPriority + midPriority + lowPriority;

    return list;
}

void StockCodeMap::addStock(const QString& code, const QString& name)
{
    if (!code.isEmpty() && !name.isEmpty())
    {
        m_map.insert(code, name);
    }
}