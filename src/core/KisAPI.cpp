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
#include <QCoreApplication>
#include <QTimeZone>

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
    NetworkUtils::addTimeOut(reply,15000);

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
        if (reply->error() == QNetworkReply::OperationCanceledError)
        {
            qDebug() << "KIS Auth Error:" << "KIS 토큰 인증이 시간 초과로 실패하여 앱을 종료합니다.";
            QCoreApplication::quit();
            return;
        }
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

void KisAPI::reserveSellOrder(const QString& symbol, int quantity, double price, bool marketPrice, const QString& endDate)
{
    if (m_accessToken.isEmpty())
    {
        qDebug() << "토큰이 없습니다. authenticate() 먼저 호출하세요.";
        emit orderReserved(symbol, false, "인증 토큰이 없습니다.");
        return;
    }

    // 방어: 수량은 1주 이상이어야 함
    if (quantity <= 0)
    {
        qDebug() << "예약매도 주문 수량 오류:" << quantity;
        emit orderReserved(symbol, false, "주문 수량은 1주 이상이어야 합니다.");
        return;
    }

    // 주식예약주문 URL (실전투자 전용)
    QUrl url(Config::KIS_BASE_URL + "/uapi/domestic-stock/v1/trading/order-resv");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=utf-8");

    // 한투 주문 필수 헤더
    request.setRawHeader("Authorization", ("Bearer " + m_accessToken).toUtf8());
    request.setRawHeader("appkey", Config::KIS_APP_KEY.toUtf8());
    request.setRawHeader("appsecret", Config::KIS_APP_SECRET.toUtf8());
    request.setRawHeader("tr_id", "CTSC0008U"); // 주식예약주문 등록 (실전 전용)
    request.setRawHeader("custtype", "P");      // 개인고객

    // 요청 바디 (POST는 key를 대문자로 작성해야 함)
    QJsonObject body;
    body["CANO"] = Config::KIS_ACCOUNT_CANO;                     // 종합계좌번호 앞 8자리
    body["ACNT_PRDT_CD"] = Config::KIS_ACCOUNT_PRDT_CD;          // 계좌상품코드 뒤 2자리
    body["PDNO"] = symbol;                                       // 종목코드(6자리)
    body["ORD_QTY"] = QString::number(quantity);                 // 주문수량
    // 시장가는 단가 0, 지정가는 1주당 가격 (원 단위 정수)
    body["ORD_UNPR"] = marketPrice ? "0" : QString::number(price, 'f', 0);
    body["SLL_BUY_DVSN_CD"] = "01";                              // 01: 매도
    body["ORD_DVSN_CD"] = marketPrice ? "01" : "00";            // 00: 지정가, 01: 시장가
    body["ORD_OBJT_CBLC_DVSN_CD"] = "10";                       // 10: 현금
    if (!endDate.isEmpty())
        body["RSVN_ORD_END_DT"] = endDate;                      // 기간예약 종료일 (YYYYMMDD)

    QNetworkReply* reply = manager->post(request, QJsonDocument(body).toJson());

    // 꼬리표 붙이기 (심볼)
    reply->setProperty("TargetSymbol", symbol);
    NetworkUtils::addTimeOut(reply, 15000);

    connect(reply, &QNetworkReply::finished, [this, reply]() { onOrderReservResult(reply); });
}

void KisAPI::onOrderReservResult(QNetworkReply* reply)
{
    reply->deleteLater();
    const QString symbol = reply->property("TargetSymbol").toString();

    const QByteArray responseData = reply->readAll();

    if (reply->error() != QNetworkReply::NoError)
    {
        qDebug() << "KIS 예약주문 통신 오류:" << reply->errorString() << responseData;
        emit orderReserved(symbol, false, reply->errorString());
        return;
    }

    const QJsonObject obj = QJsonDocument::fromJson(responseData).object();

    // rt_cd == "0" 이면 접수 성공, 그 외에는 거부/오류 (msg1에 사유)
    const QString rtCd = obj["rt_cd"].toString();
    const QString msg = obj["msg1"].toString();
    const bool ok = (rtCd == "0");

    if (ok)
        qDebug() << "KIS 예약매도 접수 성공:" << symbol << msg;
    else
        qDebug() << "KIS 예약매도 거부:" << symbol << "rt_cd=" << rtCd << msg;

    emit orderReserved(symbol, ok, msg);
}

void KisAPI::fetchBalance()
{
    if (m_accessToken.isEmpty())
    {
        qDebug() << "토큰이 없습니다. authenticate() 먼저 호출하세요.";
        emit holdingsReceived({});
        return;
    }

    // 주식잔고조회 URL
    QUrl url(Config::KIS_BASE_URL + "/uapi/domestic-stock/v1/trading/inquire-balance");
    QUrlQuery query;
    query.addQueryItem("CANO", Config::KIS_ACCOUNT_CANO);
    query.addQueryItem("ACNT_PRDT_CD", Config::KIS_ACCOUNT_PRDT_CD);
    query.addQueryItem("AFHR_FLPR_YN", "N");           // 시간외단일가 여부
    query.addQueryItem("OFL_YN", "");                  // 오프라인 여부
    query.addQueryItem("INQR_DVSN", "02");             // 조회구분 02: 종목별
    query.addQueryItem("UNPR_DVSN", "01");             // 단가구분
    query.addQueryItem("FUND_STTL_ICLD_YN", "N");      // 펀드결제분 포함여부
    query.addQueryItem("FNCG_AMT_AUTO_RDPT_YN", "N");  // 융자금액자동상환 여부
    query.addQueryItem("PRCS_DVSN", "00");             // 처리구분 00: 전일매매포함
    query.addQueryItem("CTX_AREA_FK100", "");          // 연속조회 검색조건 (첫 조회는 공백)
    query.addQueryItem("CTX_AREA_NK100", "");          // 연속조회 키
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("Authorization", ("Bearer " + m_accessToken).toUtf8());
    request.setRawHeader("appkey", Config::KIS_APP_KEY.toUtf8());
    request.setRawHeader("appsecret", Config::KIS_APP_SECRET.toUtf8());
    request.setRawHeader("tr_id", "TTTC8434R"); // 주식잔고조회 (실전)
    request.setRawHeader("custtype", "P");

    QNetworkReply* reply = manager->get(request);
    NetworkUtils::addTimeOut(reply, 15000);

    connect(reply, &QNetworkReply::finished, [this, reply]() { onBalanceReceived(reply); });
}

void KisAPI::onBalanceReceived(QNetworkReply* reply)
{
    reply->deleteLater();

    const QByteArray responseData = reply->readAll();

    if (reply->error() != QNetworkReply::NoError)
    {
        qDebug() << "KIS 잔고조회 통신 오류:" << reply->errorString() << responseData;
        emit holdingsReceived({});
        return;
    }

    const QJsonObject obj = QJsonDocument::fromJson(responseData).object();
    if (obj["rt_cd"].toString() != "0")
    {
        qDebug() << "KIS 잔고조회 실패:" << obj["msg1"].toString();
        emit holdingsReceived({});
        return;
    }

    // output1: 보유종목 배열. 보유수량(hldg_qty) > 0 인 종목코드(pdno)만 수집
    QStringList symbols;
    QHash<QString, int> quantities;
    const QJsonArray holdings = obj["output1"].toArray();
    for (const QJsonValue& v : holdings)
    {
        const QJsonObject item = v.toObject();
        const QString code = item["pdno"].toString();
        const int qty = item["hldg_qty"].toString().toInt();
        if (!code.isEmpty() && qty > 0)
        {
            symbols << code;
            quantities.insert(code, qty);
        }
    }

    qDebug() << "KIS 보유종목 수신:" << symbols.size() << "개" << symbols;
    emit holdingQuantitiesReceived(quantities);
    emit holdingsReceived(symbols);
}

void KisAPI::setTradeHeaders(QNetworkRequest& request, const char* trId) const
{
    request.setRawHeader("Authorization", ("Bearer " + m_accessToken).toUtf8());
    request.setRawHeader("appkey", Config::KIS_APP_KEY.toUtf8());
    request.setRawHeader("appsecret", Config::KIS_APP_SECRET.toUtf8());
    request.setRawHeader("tr_id", trId);
    request.setRawHeader("custtype", "P");
}

void KisAPI::placeOrder(const QString& symbol, bool isBuy, int quantity, double price, bool marketPrice)
{
    if (m_accessToken.isEmpty())
    {
        emit orderPlaced(symbol, isBuy, false, "인증 토큰이 없습니다.");
        return;
    }
    if (quantity <= 0)
    {
        emit orderPlaced(symbol, isBuy, false, "주문 수량은 1주 이상이어야 합니다.");
        return;
    }

    QUrl url(Config::KIS_BASE_URL + "/uapi/domestic-stock/v1/trading/order-cash");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=utf-8");
    setTradeHeaders(request, isBuy ? "TTTC0012U" : "TTTC0011U"); // 매수 / 매도 (실전)

    QJsonObject body;
    body["CANO"] = Config::KIS_ACCOUNT_CANO;
    body["ACNT_PRDT_CD"] = Config::KIS_ACCOUNT_PRDT_CD;
    body["PDNO"] = symbol;
    body["ORD_DVSN"] = marketPrice ? "01" : "00";              // 00 지정가 / 01 시장가
    body["ORD_QTY"] = QString::number(quantity);
    body["ORD_UNPR"] = marketPrice ? "0" : QString::number(price, 'f', 0);
    body["EXCG_ID_DVSN_CD"] = "SOR";                            // 스마트주문(KRX+NXT 통합 최선호가)

    QNetworkReply* reply = manager->post(request, QJsonDocument(body).toJson());
    reply->setProperty("TargetSymbol", symbol);
    reply->setProperty("IsBuy", isBuy);
    NetworkUtils::addTimeOut(reply, 15000);

    connect(reply, &QNetworkReply::finished, [this, reply]() { onOrderResult(reply); });
}

void KisAPI::onOrderResult(QNetworkReply* reply)
{
    reply->deleteLater();
    const QString symbol = reply->property("TargetSymbol").toString();
    const bool isBuy = reply->property("IsBuy").toBool();
    const QByteArray data = reply->readAll();

    if (reply->error() != QNetworkReply::NoError)
    {
        qDebug() << "KIS 주문 통신 오류:" << reply->errorString() << data;
        emit orderPlaced(symbol, isBuy, false, reply->errorString());
        return;
    }

    const QJsonObject obj = QJsonDocument::fromJson(data).object();
    const bool ok = obj["rt_cd"].toString() == "0";
    if (!ok) qDebug() << "KIS 주문 거부:" << symbol << obj["msg1"].toString();
    emit orderPlaced(symbol, isBuy, ok, obj["msg1"].toString());
}

void KisAPI::reviseOrder(const KisOpenOrder& order, int newQty, double newPrice)
{
    sendRvseCncl(order, "01", newQty, newPrice, false); // 01: 정정
}

void KisAPI::cancelOrder(const KisOpenOrder& order)
{
    sendRvseCncl(order, "02", order.possibleQty, 0, true); // 02: 취소 (잔량 전부)
}

void KisAPI::sendRvseCncl(const KisOpenOrder& order, const char* rvseCnclCd, int qty, double price, bool allQty)
{
    if (m_accessToken.isEmpty())
    {
        emit orderModified(false, "인증 토큰이 없습니다.");
        return;
    }

    QUrl url(Config::KIS_BASE_URL + "/uapi/domestic-stock/v1/trading/order-rvsecncl");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=utf-8");
    setTradeHeaders(request, "TTTC0013U"); // 주식주문(정정취소) 실전

    QJsonObject body;
    body["CANO"] = Config::KIS_ACCOUNT_CANO;
    body["ACNT_PRDT_CD"] = Config::KIS_ACCOUNT_PRDT_CD;
    body["KRX_FWDG_ORD_ORGNO"] = order.orgNo;
    body["ORGN_ODNO"] = order.orderNo;
    body["ORD_DVSN"] = order.ordDvsnCd.isEmpty() ? "00" : order.ordDvsnCd;
    body["RVSE_CNCL_DVSN_CD"] = rvseCnclCd;                    // 01 정정 / 02 취소
    body["ORD_QTY"] = QString::number(qty);
    body["ORD_UNPR"] = QString::number(price, 'f', 0);
    body["QTY_ALL_ORD_YN"] = allQty ? "Y" : "N";              // 잔량 전부 여부
    body["EXCG_ID_DVSN_CD"] = "SOR";                          // 원주문과 동일하게 SOR

    QNetworkReply* reply = manager->post(request, QJsonDocument(body).toJson());
    NetworkUtils::addTimeOut(reply, 15000);

    connect(reply, &QNetworkReply::finished, [this, reply]() { onModifyResult(reply); });
}

void KisAPI::onModifyResult(QNetworkReply* reply)
{
    reply->deleteLater();
    const QByteArray data = reply->readAll();

    if (reply->error() != QNetworkReply::NoError)
    {
        qDebug() << "KIS 정정취소 통신 오류:" << reply->errorString() << data;
        emit orderModified(false, reply->errorString());
        return;
    }

    const QJsonObject obj = QJsonDocument::fromJson(data).object();
    emit orderModified(obj["rt_cd"].toString() == "0", obj["msg1"].toString());
}

void KisAPI::fetchOpenOrders()
{
    if (m_accessToken.isEmpty())
    {
        emit openOrdersReceived({});
        return;
    }

    QUrl url(Config::KIS_BASE_URL + "/uapi/domestic-stock/v1/trading/inquire-psbl-rvsecncl");
    QUrlQuery query;
    query.addQueryItem("CANO", Config::KIS_ACCOUNT_CANO);
    query.addQueryItem("ACNT_PRDT_CD", Config::KIS_ACCOUNT_PRDT_CD);
    query.addQueryItem("INQR_DVSN_1", "0"); // 0: 주문
    query.addQueryItem("INQR_DVSN_2", "0"); // 0: 전체
    query.addQueryItem("CTX_AREA_FK100", "");
    query.addQueryItem("CTX_AREA_NK100", "");
    url.setQuery(query);

    QNetworkRequest request(url);
    setTradeHeaders(request, "TTTC0084R"); // 주식정정취소가능주문조회

    QNetworkReply* reply = manager->get(request);
    NetworkUtils::addTimeOut(reply, 15000);

    connect(reply, &QNetworkReply::finished, [this, reply]() { onOpenOrdersReceived(reply); });
}

void KisAPI::onOpenOrdersReceived(QNetworkReply* reply)
{
    reply->deleteLater();
    const QByteArray data = reply->readAll();

    if (reply->error() != QNetworkReply::NoError)
    {
        qDebug() << "KIS 미체결 조회 통신 오류:" << reply->errorString() << data;
        emit openOrdersReceived({});
        return;
    }

    const QJsonObject obj = QJsonDocument::fromJson(data).object();
    if (obj["rt_cd"].toString() != "0")
    {
        qDebug() << "KIS 미체결 조회 실패:" << obj["msg1"].toString();
        emit openOrdersReceived({});
        return;
    }

    // output: 정정취소가능 주문 배열. 가능수량(psbl_qty) > 0 인 미체결만 수집
    QList<KisOpenOrder> orders;
    const QJsonArray arr = obj["output"].toArray();
    for (const QJsonValue& v : arr)
    {
        const QJsonObject o = v.toObject();
        KisOpenOrder ord;
        ord.orgNo = o["ord_gno_brno"].toString();
        ord.orderNo = o["odno"].toString();
        ord.symbol = o["pdno"].toString();
        ord.name = o["prdt_name"].toString();
        ord.isBuy = o["sll_buy_dvsn_cd"].toString() == "02";
        ord.orderQty = o["ord_qty"].toString().toInt();
        ord.possibleQty = o["psbl_qty"].toString().toInt();
        ord.orderPrice = o["ord_unpr"].toString().toDouble();
        ord.ordDvsnCd = o["ord_dvsn_cd"].toString();

        if (ord.possibleQty > 0)
            orders << ord;
    }

    qDebug() << "KIS 미체결" << orders.size() << "건 수신";
    emit openOrdersReceived(orders);
}

void KisAPI::fetchOrderableCash(const QString& symbol, double price, bool marketPrice)
{
    if (m_accessToken.isEmpty())
    {
        emit orderableCashReceived(symbol, 0, 0);
        return;
    }

    QUrl url(Config::KIS_BASE_URL + "/uapi/domestic-stock/v1/trading/inquire-psbl-order");
    QUrlQuery query;
    query.addQueryItem("CANO", Config::KIS_ACCOUNT_CANO);
    query.addQueryItem("ACNT_PRDT_CD", Config::KIS_ACCOUNT_PRDT_CD);
    query.addQueryItem("PDNO", symbol);
    query.addQueryItem("ORD_UNPR", marketPrice ? "0" : QString::number(price, 'f', 0));
    query.addQueryItem("ORD_DVSN", marketPrice ? "01" : "00");
    query.addQueryItem("CMA_EVLU_AMT_ICLD_YN", "N");
    query.addQueryItem("OVRS_ICLD_YN", "N");
    url.setQuery(query);

    QNetworkRequest request(url);
    setTradeHeaders(request, "TTTC8908R"); // 매수가능조회

    QNetworkReply* reply = manager->get(request);
    reply->setProperty("TargetSymbol", symbol);
    NetworkUtils::addTimeOut(reply, 15000);

    connect(reply, &QNetworkReply::finished, [this, reply]() { onOrderableCashReceived(reply); });
}

void KisAPI::onOrderableCashReceived(QNetworkReply* reply)
{
    reply->deleteLater();
    const QString symbol = reply->property("TargetSymbol").toString();

    if (reply->error() != QNetworkReply::NoError)
    {
        qDebug() << "KIS 주문가능조회 오류:" << reply->errorString();
        emit orderableCashReceived(symbol, 0, 0);
        return;
    }

    const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
    const QJsonObject output = obj["output"].toObject();
    const qint64 cash = output["ord_psbl_cash"].toString().toLongLong();  // 주문가능현금
    const int maxQty = output["nrcvb_buy_qty"].toString().toInt();        // 미수없는 매수가능수량
    emit orderableCashReceived(symbol, cash, maxQty);
}

void KisAPI::fetchAskingPrice(const QString& symbol)
{
    if (m_accessToken.isEmpty())
    {
        emit askingPriceReceived(symbol, {});
        return;
    }

    QUrl url(Config::KIS_BASE_URL + "/uapi/domestic-stock/v1/quotations/inquire-asking-price-exp-ccn");
    QUrlQuery query;
    query.addQueryItem("FID_COND_MRKT_DIV_CODE", "J"); // J: 주식
    query.addQueryItem("FID_INPUT_ISCD", symbol);
    url.setQuery(query);

    QNetworkRequest request(url);
    setTradeHeaders(request, "FHKST01010200"); // 주식현재가 호가/예상체결

    QNetworkReply* reply = manager->get(request);
    reply->setProperty("TargetSymbol", symbol);
    NetworkUtils::addTimeOut(reply);

    connect(reply, &QNetworkReply::finished, [this, reply]() { onAskingPriceReceived(reply); });
}

void KisAPI::onAskingPriceReceived(QNetworkReply* reply)
{
    reply->deleteLater();
    const QString symbol = reply->property("TargetSymbol").toString();

    if (reply->error() != QNetworkReply::NoError)
    {
        qDebug() << "KIS 호가조회 오류:" << reply->errorString();
        emit askingPriceReceived(symbol, {});
        return;
    }

    const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
    const QJsonObject o1 = obj["output1"].toObject();

    // askp1~10 매도호가 / bidp1~10 매수호가, *_rsqn 잔량
    KisAskingPrice data;
    for (int i = 1; i <= 10; ++i)
    {
        KisAskRow ask;
        ask.price = o1[QString("askp%1").arg(i)].toString().toDouble();
        ask.qty = o1[QString("askp_rsqn%1").arg(i)].toString().toInt();
        data.asks << ask;

        KisAskRow bid;
        bid.price = o1[QString("bidp%1").arg(i)].toString().toDouble();
        bid.qty = o1[QString("bidp_rsqn%1").arg(i)].toString().toInt();
        data.bids << bid;
    }

    emit askingPriceReceived(symbol, data);
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
    if (!settings.contains("kis_token") || !settings.contains("kis_expiry")) {
        return false;
    }

    // QSettings에서 시간과 토큰을 꺼냄
    QDateTime rawExpiry = settings.value("kis_expiry").toDateTime();
    QString token = settings.value("kis_token").toString();

    QTimeZone kst("Asia/Seoul");

    // 현재 시간을 한국시간으로 강제
    const QDateTime nowKst = QDateTime::currentDateTime(kst);

    // 저장된 만료시간도 한국시간으로 맞춤
    const QDateTime expiryKst = rawExpiry.toTimeZone(kst);

    qDebug() << "현재 시간(KST):"
        << nowKst.toString("yyyy-MM-dd HH:mm:ss t")
        << "(Epoch:" << nowKst.toSecsSinceEpoch() << ")";
    qDebug() << "토큰 만료 시간(KST):"
        << expiryKst.toString("yyyy-MM-dd HH:mm:ss t")
        << "(Epoch:" << expiryKst.toSecsSinceEpoch() << ")";

    if (expiryKst.isValid() && nowKst < expiryKst) {
        m_accessToken = token;
        return true;
    }

    return false;
}
