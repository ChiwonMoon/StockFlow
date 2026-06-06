# StockFlow 📈

![C++](https://img.shields.io/badge/C++-20-blue.svg?style=flat&logo=c%2B%2B)
![Qt](https://img.shields.io/badge/Qt-6.5+-41cd52.svg?style=flat&logo=qt)
![CMake](https://img.shields.io/badge/CMake-3.24+-064f8c.svg?style=flat&logo=cmake)
![License](https://img.shields.io/badge/License-MIT-yellow.svg)

**StockFlow**는 C++과 Qt 6를 사용하여 개발된 **실시간 주식 데이터 시각화 및 거래 데스크톱 애플리케이션** 입니다.
한국투자증권(KIS) API와 Finnhub API를 연동하여 국내/해외 주식 시세를 수집·시각화하고, 한 발 더 나아가 **국내주식 주문·예약매도·잔고/미체결 관리**까지 데스크톱에서 처리하는 것을 목표로 합니다.

## 🎯 프로젝트 목표 (Project Goal)
- **Modern C++ & Qt6:** 최신 C++ 표준(C++20)과 Qt 6 프레임워크 활용 능력 증명
- **Modular Architecture:** UI, 비즈니스 로직(Core), 실행 파일(App)의 명확한 모듈 분리
- **Network & Concurrency:** REST API 비동기 통신 및 OAuth2 인증 처리
- **Clean Code:** 유지보수 가능한 코드 구조와 CMake 빌드 시스템 설계

## ✨ 주요 기능 (Features)

### 📊 시세 & 화면
- **관심종목 / 보유종목 탭 분리** — 보유종목 탭은 계좌 잔고를 자동 조회해 표시
- **실시간 시세 갱신** 및 종목 로고 표시 (국내 KIS / 해외 Finnhub)
- **빠른 종목 검색 + 자동완성** (디바운스 처리로 부드러운 입력)
- **실시간 10호가** 조회 (매도/매수 10단계 + 잔량)

### 💰 국내주식 거래 (KIS, 실전)
- **즉시 매수 / 매도** — `SOR` 라우팅으로 **KRX + NXT(넥스트레이드) 통합** 최선호가 전송
- **매수가능금액(예수금) 조회** 및 **보유수량 기반 전량 버튼 · 수량 상한**
- **미체결 조회 + 주문 정정/취소**

### ⏰ 예약매도 (2가지 방식)
- **KIS 네이티브 예약매도** — 증권사 서버가 보관, PC를 꺼도 다음 영업일 장 시작에 자동 처리 (KRX 정규장)
- **앱 자체 SOR 예약매도** — 지정한 **발사 시각**(예: NXT 프리마켓 08:00)에 `SOR` 주문을 자동 발사. **매일 반복** 지원, **NXT 연장세션 포함**. 등록 내역은 **저장되어 재시작·재부팅에도 유지**

## 🛠 사용 기술 (Tech Stack)
- **Language:** C++20
- **Framework:** Qt 6 (Widgets, Network)
- **Build System:** CMake + CMakePresets (Ninja / MSVC)
- **Version Control:** Git & GitHub
- **External API:**
    - **한국투자증권(KIS) OpenAPI** — OAuth2 인증, 국내 시세·호가, 주문(현금/정정취소)·예약주문, 잔고·매수가능 조회
    - **Finnhub API** — 해외(미국) 주식 시세 및 심볼 데이터
    - **AlphaSquare** — 종목 로고 이미지 파싱

## 📂 프로젝트 구조 (Architecture)
**관심사의 분리(Separation of Concerns)** 원칙에 따라 모듈화되어 있습니다.

```text
StockFlow/
├── src/
│   ├── app/          # Main Entry (실행 파일 진입점)
│   ├── core/         # Business Logic & Data Layer
│   │   ├── StockAPI / KisAPI / FinnhubAPI      # API 통신 (인증·시세·주문)
│   │   ├── *Coordinator / *Resolver            # 요청 흐름·심볼 해석 분리
│   │   └── StockCodeMap / StockListSettings    # 종목코드·설정 관리
│   └── ui/           # View Layer (MainWindow, StockTableModel, Delegate)
├── CMakeLists.txt    # 최상위 빌드 설정
└── CMakePresets.json # 멀티 플랫폼 빌드 및 경로 설정
```

## ⚙️ 설정 (Setup)
API 키와 계좌 정보는 git에 포함되지 않습니다. 아래 절차로 직접 채워주세요.

1. `src/core/Config.h.example` 를 복사해 **`src/core/Config.h`** 로 이름 변경
2. 발급받은 키/계좌 정보를 입력
   - `FINNHUB_API_KEY` — Finnhub 발급 키
   - `KIS_BASE_URL` — 실전 `https://openapi.koreainvestment.com:9443` / 모의 `...vts...:29443`
   - `KIS_APP_KEY` / `KIS_APP_SECRET` — KIS 개발자센터 발급 (실전/모의 환경 일치)
   - `KIS_ACCOUNT_CANO` (계좌 앞 8자리) / `KIS_ACCOUNT_PRDT_CD` (뒤 2자리, 보통 `01`)
3. `Config.h` 변경 후에는 **반드시 다시 빌드**해야 값이 반영됩니다.

## 🎬 주요 기능 데모 (Demo)
**빠르고 부드러운 종목 검색 및 자동완성 기능**
https://github.com/user-attachments/assets/2b1e2405-f44e-4c5b-8002-c63465ae8c99

## ⚠️ 면책 (Disclaimer)
본 프로젝트는 **학습·포트폴리오 목적**으로 제작되었습니다. 거래 기능은 **실전 계좌에서 실제 체결**되며, 주문 실수는 금전적 손실로 이어질 수 있습니다. 사용에 따른 책임은 전적으로 사용자 본인에게 있습니다.
