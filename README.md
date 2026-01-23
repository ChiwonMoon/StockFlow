# StockFlow 📈

![C++](https://img.shields.io/badge/C++-20-blue.svg?style=flat&logo=c%2B%2B)
![Qt](https://img.shields.io/badge/Qt-6.5+-41cd52.svg?style=flat&logo=qt)
![CMake](https://img.shields.io/badge/CMake-3.24+-064f8c.svg?style=flat&logo=cmake)
![License](https://img.shields.io/badge/License-MIT-yellow.svg)

**StockFlow**는 C++과 Qt 6를 사용하여 개발된 고성능 **실시간 주식 시세 대시보드(Real-time Stock Dashboard)** 애플리케이션입니다.
Finnhub API를 활용하여 실시간 주가 데이터를 수신하고, 이를 효율적인 Model/View 패턴으로 시각화하는 것을 목표로 합니다.

## 🎯 프로젝트 목표 (Project Goal)
- **Modern C++ & Qt6:** 최신 C++ 표준(C++20)과 Qt 6 프레임워크 활용 능력 증명
- **Modular Architecture:** UI, 비즈니스 로직(Core), 실행 파일(App)의 명확한 모듈 분리
- **Network & Concurrency:** REST API 비동기 통신 및 멀티스레딩 데이터 처리
- **Clean Code:** 유지보수 가능한 코드 구조와 CMake 빌드 시스템 설계

## 🛠 사용 기술 (Tech Stack)
- **Language:** C++20
- **Framework:** Qt 6 (Widgets Module)
- **Build System:** CMake
- **Version Control:** Git & GitHub
- **External Library:** - [Finnhub API](https://finnhub.io/) (주식 데이터 제공)
    - (추후 추가 예정: QCustomPlot 등)

## 📂 프로젝트 구조 (Architecture)
이 프로젝트는 **관심사의 분리(Separation of Concerns)** 원칙에 따라 모듈화되어 있습니다.

```text
StockFlow/
├── src/
│   ├── app/      # 실행 파일 진입점 (Main Entry)
│   ├── core/     # 데이터 모델, 네트워크 통신, 비즈니스 로직 (UI 의존성 없음)
│   └── ui/       # 화면 디자인(.ui), 위젯 코드 (View 계층)
├── CMakeLists.txt      # 최상위 빌드 설정
└── CMakePresets.json   # 멀티 플랫폼 빌드 및 경로 설정
