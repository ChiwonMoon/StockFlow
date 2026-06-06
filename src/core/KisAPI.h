#pragma once

#include "StockAPI.h"
#include <QStringList>
#include <QList>
#include <QVector>

class QDateTime;
class QNetworkRequest;

// 미체결(정정취소가능) 주문 한 건
struct KisOpenOrder
{
    QString orgNo;       // 주문채번지점번호 (정정취소 시 KRX_FWDG_ORD_ORGNO)
    QString orderNo;     // 주문번호 (정정취소 시 ORGN_ODNO)
    QString symbol;      // 종목코드
    QString name;        // 종목명
    bool isBuy = false;  // true: 매수(02), false: 매도(01)
    int orderQty = 0;    // 주문수량
    int possibleQty = 0; // 정정취소가능수량
    double orderPrice = 0; // 주문단가
    QString ordDvsnCd;   // 주문구분코드
};

// 호가 한 단계 (가격 + 잔량)
struct KisAskRow
{
    double price = 0;
    int qty = 0;
};

// 10호가 정보
struct KisAskingPrice
{
    QVector<KisAskRow> asks; // 매도호가 1~10
    QVector<KisAskRow> bids; // 매수호가 1~10
};

class KisAPI : public StockAPI
{
    Q_OBJECT

public:
    explicit KisAPI(QObject* parent = nullptr);

    void authenticate();
    void fetchStock(const QString& symbol) override;
    void fetchLogo(const QString& symbol) override;

    // 국내주식 예약매도 주문 (실전투자 전용, order-resv / CTSC0008U)
    // marketPrice=true 면 시장가(가격 무시), endDate 입력 시 기간예약(YYYYMMDD)
    void reserveSellOrder(const QString& symbol,
                          int quantity,
                          double price,
                          bool marketPrice = false,
                          const QString& endDate = QString());

    // 계좌 보유종목(주식잔고) 조회 (실전 TTTC8434R)
    void fetchBalance();

    // 즉시 현금주문 (실전: 매수 TTTC0012U / 매도 TTTC0011U)
    void placeOrder(const QString& symbol, bool isBuy, int quantity, double price, bool marketPrice = false);

    // 주문 정정/취소 (order-rvsecncl, 실전 TTTC0013U)
    void reviseOrder(const KisOpenOrder& order, int newQty, double newPrice);
    void cancelOrder(const KisOpenOrder& order);

    // 정정취소가능(미체결) 주문 조회 (실전 TTTC0084R)
    void fetchOpenOrders();

    // 매수가능/주문가능현금 조회 (실전 TTTC8908R)
    void fetchOrderableCash(const QString& symbol, double price, bool marketPrice = false);

    // 현재가 10호가 조회 (FHKST01010200)
    void fetchAskingPrice(const QString& symbol);

signals:
    void authenticated();
    // 예약주문 접수 결과 (success=true 면 접수 성공, message는 KIS 응답 메시지)
    void orderReserved(const QString& symbol, bool success, const QString& message);
    // 보유종목 종목코드 목록 (보유수량 > 0 인 종목)
    void holdingsReceived(const QStringList& symbols);
    // 즉시주문 접수 결과
    void orderPlaced(const QString& symbol, bool isBuy, bool success, const QString& message);
    // 정정/취소 결과
    void orderModified(bool success, const QString& message);
    // 미체결 주문 목록
    void openOrdersReceived(const QList<KisOpenOrder>& orders);
    // 주문가능현금 + (미수없는) 최대매수수량
    void orderableCashReceived(const QString& symbol, qint64 orderableCash, int maxBuyQty);
    // 10호가
    void askingPriceReceived(const QString& symbol, const KisAskingPrice& data);

private slots:
       void onAuthFinished(QNetworkReply* reply);
       void onStockReceived(QNetworkReply* reply);
       void onLogoDownloaded(QNetworkReply* reply);
       void onOrderReservResult(QNetworkReply* reply);
       void onBalanceReceived(QNetworkReply* reply);
       void onOrderResult(QNetworkReply* reply);
       void onModifyResult(QNetworkReply* reply);
       void onOpenOrdersReceived(QNetworkReply* reply);
       void onOrderableCashReceived(QNetworkReply* reply);
       void onAskingPriceReceived(QNetworkReply* reply);

private:
    QString m_accessToken;
    void saveToken(const QString& token, const QDateTime& expiry);
    bool loadToken();

    // KIS 인증/거래 공통 헤더 설정
    void setTradeHeaders(QNetworkRequest& request, const char* trId) const;
    // 정정/취소 공통 전송 (rvseCnclCd: 01 정정 / 02 취소)
    void sendRvseCncl(const KisOpenOrder& order, const char* rvseCnclCd, int qty, double price, bool allQty);
};