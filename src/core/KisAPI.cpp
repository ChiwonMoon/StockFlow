#include "KisAPI.h"
#include "Config.h"
#include "NetworkUtils.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrlQuery>
#include <QSettings>
#include <QDateTime>
#include "StockCodeMap.h"
#include <QPixmap>

KisAPI::KisAPI(QObject* parent) : StockAPI(parent)
{
}

void KisAPI::authenticate()
{
    if (loadToken())
    {
        qDebug() << "저장된 토큰을 불러왔습니다. (서버 요청 생략)";
        emit authenticated(); // 바로 성공 신호 보냄
        return;
    }

    qDebug() << "토큰이 없거나 만료됨. 새로 요청합니다...";
    QUrl url(Config::KIS_BASE_URL + "/oauth2/tokenP");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject json;
    json["grant_type"] = "client_credentials";
    json["appkey"] = Config::KIS_APP_KEY;
    json["appsecret"] = Config::KIS_APP_SECRET;

    QNetworkReply* reply = manager->post(request, QJsonDocument(json).toJson());
    NetworkUtils::addTimeOut(reply);

    connect(
        reply, 
        &QNetworkReply::finished, 
        [this, reply]() 
        { onAuthFinished(reply); }
    );
}

void KisAPI::onAuthFinished(QNetworkReply* reply)
{
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError)
    {
        qDebug() << "KIS Auth Error:" << reply->errorString();
        qDebug() << reply->readAll();
        return;
    }

    QByteArray responseData = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(responseData);
    QJsonObject obj = doc.object();

    // 토큰 저장
    m_accessToken = obj["access_token"].toString();

    // 유효기간 가져오기 (초 단위) - 보통 86400초
    int expiresIn = doc.object()["expires_in"].toInt();

    // 현재 시간 + 유효기간 = 만료 시간 계산
    QDateTime expiryTime = QDateTime::currentDateTime().addSecs(expiresIn);

    // 파일에 저장
    saveToken(m_accessToken, expiryTime);

    qDebug() << "KIS Login Success! Token acquired.";

    emit authenticated(); // "이제 주식 조회해도 된다"고 알림
}

void KisAPI::fetchStock(const QString& symbol)
{
    if (m_accessToken.isEmpty())
    {
        qDebug() << "토큰이 없습니다. authenticate() 먼저 호출하세요.";
        return;
    }

    // 주식현재가 시세 URL
    QUrl url(Config::KIS_BASE_URL + "/uapi/domestic-stock/v1/quotations/inquire-price");
    QUrlQuery query;
    query.addQueryItem("fid_cond_mrkt_div_code", "J"); // J: 주식
    query.addQueryItem("fid_input_iscd", symbol);      // 종목코드
    url.setQuery(query);

    QNetworkRequest request(url);

    // 한투 API 필수 헤더 4대장
    request.setRawHeader("Authorization", ("Bearer " + m_accessToken).toUtf8());
    request.setRawHeader("appkey", Config::KIS_APP_KEY.toUtf8());
    request.setRawHeader("appsecret", Config::KIS_APP_SECRET.toUtf8());
    request.setRawHeader("tr_id", "FHKST01010100"); // 현재가 조회용 거래 ID (모의/실전 동일)

    QNetworkReply* reply = manager->get(request);

    // 꼬리표 붙이기 (심볼)
    reply->setProperty("TargetSymbol", symbol);
    NetworkUtils::addTimeOut(reply);

    connect(reply, &QNetworkReply::finished, [this, reply]() { onStockReceived(reply); });
}

void KisAPI::fetchLogo(const QString& symbol)
{
    QString urlStr = QString("https://file.alphasquare.co.kr/media/images/stock_logo/kr/%1.png").arg(symbol);

    downloadLogoFromUrl(symbol, urlStr);
}

void KisAPI::onStockReceived(QNetworkReply* reply)
{
    reply->deleteLater();
    QString symbol = reply->property("TargetSymbol").toString();

    if (reply->error() != QNetworkReply::NoError)
    {
        qDebug() << "KIS Error:" << reply->errorString();
        return;
    }

    // 한투 응답 파싱
    QByteArray responseData = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(responseData);
    QJsonObject output = doc.object()["output"].toObject(); // "output" 안에 데이터 있음

    if (output.isEmpty()) return;

    StockData data;
    data.symbol = symbol;
    // 한투는 이름이 안 옴. 일단 심볼로 대체하거나 별도 매핑 필요.
    data.name = StockCodeMap::getName(symbol);

    // 문자열로 오기 때문에 숫자로 변환 필요
    data.currentPrice = output["stck_prpr"].toString().toDouble(); // 현재가
    data.highPrice = output["stck_hgpr"].toString().toDouble();    // 고가
    data.lowPrice = output["stck_lwpr"].toString().toDouble();     // 저가
    data.openPrice = output["stck_oprc"].toString().toDouble();    // 시가

    // 전일 종가는 계산해서 넣거나 다른 필드 참조 (stck_prpr - prdy_vrss)
    double change = output["prdy_vrss"].toString().toDouble(); // 전일대비
    data.prevClose = data.currentPrice - change;

    // 국내 주식임을 표시 (나중에 원화(₩) 표시할 때 씀)
    // data.currency = "KRW"; // StockData에 currency 필드가 있다면 추가 권장

    //qDebug() << "Received KIS Data:" << symbol << data.currentPrice;
    emit dataReceived(data);
}

void KisAPI::onLogoDownloaded(QNetworkReply* reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        qDebug() << "로고 다운로드 실패:" << reply->errorString();
        return;
    }

    QString symbol = reply->property("symbol").toString();

    // 이미지 데이터 변환 (Binary -> QPixmap)
    QByteArray data = reply->readAll();

    QPixmap logo;
    if (logo.loadFromData(data))
    {
        emit logoReceived(symbol, logo);
    }
}

void KisAPI::saveToken(const QString& token, const QDateTime& expiry)
{
    // 윈도우 레지스트리나 ini 파일에 자동 저장됨
    QSettings settings(Config::SETTINGS_COMPANY, Config::SETTINGS_APP);
    settings.setValue("kis_token", token);
    settings.setValue("kis_expiry", expiry);
}

bool KisAPI::loadToken()
{
    QSettings settings(Config::SETTINGS_COMPANY, Config::SETTINGS_APP);

    // 저장된 게 없으면 실패
    if (!settings.contains("kis_token")) return false;

    QDateTime expiry = settings.value("kis_expiry").toDateTime();
    QString token = settings.value("kis_token").toString();

    // 만료 시간과 현재 시간 비교 (여유 있게 10분 정도 남았는지 확인)
    if (QDateTime::currentDateTime().addSecs(600) < expiry)
    {
        m_accessToken = token; // 멤버 변수에 저장
        return true; // 유효함!
    }

    return false; // 만료됨
}
