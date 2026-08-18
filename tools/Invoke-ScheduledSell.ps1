<#
.SYNOPSIS
    지정한 시각에 국내주식을 SOR(KRX+NXT 통합)로 매도하는 독립 실행 스크립트.
    StockFlow 앱을 켜두지 않아도 윈도우 작업 스케줄러가 이 스크립트를 실행한다.

.DESCRIPTION
    매도 방식: 호가 추격
      매수호가창을 읽어, 목표가 이상인 최우선 매수호가에 "그 호가 가격 그대로" 지정가
      매도를 낸다. 잠깐 기다렸다 체결 여부를 확인하고, 안 팔린 잔량은 취소한 뒤 호가를
      다시 읽어 새 최우선 호가에 다시 낸다. **수량을 다 팔거나 매수 1호가가 목표가 밑으로
      내려갈 때까지** 계속 반복한다 (ChaseMinutes 는 폭주 방지용 상한일 뿐이다).
      추격이 끝나고 남은 수량은 목표가에 지정가로 걸어두고 끝낸다.

    동작 순서
      1. src/core/Config.h 에서 KIS 키·계좌번호를 읽는다 (비밀값을 복사해두지 않음).
      2. 캐시된 접근토큰이 유효하면 재사용하고, 없거나 만료됐으면 새로 발급받는다.
      3. 잔고로 매도 수량을 확정한다 (All 이면 전량, 숫자면 보유수량 상한).
      4. 지각 실행이면 이미 매도 주문이 올라가 있는지 먼저 확인한다.
      5. 호가 추격 루프를 돌며 매도한다.
      6. 남은 수량은 목표가에 걸어둔다.

.EXAMPLE
    .\Invoke-ScheduledSell.ps1 -Symbol 005930 -Quantity 10 -TargetPrice 85000 -DryRun

.EXAMPLE
    .\Invoke-ScheduledSell.ps1 -Symbol 005930 -Quantity All -TargetPrice 85000
#>
[CmdletBinding()]
param(
    # 종목코드 6자리
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d{6}$')]
    [string]$Symbol,

    # 매도 수량. 정수 또는 'All'(보유 전량)
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^(\d+|[Aa]ll)$')]
    [string]$Quantity,

    # 목표가. 이 가격 미만의 매수호가에는 절대 팔지 않는다.
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 100000000)]
    [double]$TargetPrice,

    # 주문 실패(거부) 시 재시도 간격
    [ValidateRange(10, 3600)]
    [int]$RetryIntervalSec = 60,

    [ValidateRange(0, 100)]
    [int]$MaxRetries = 10,

    # 호가에 주문을 낸 뒤 체결 여부를 확인하기까지 기다리는 시간.
    # 짧을수록 호가 추격이 빠르지만, 너무 짧으면 체결됐는데 미체결로 읽을 수 있다.
    [ValidateRange(1, 60)]
    [int]$FillCheckSec = 3,

    # 통신이 끊겼을 때 재접속을 시도하는 간격(초). 멈추지 않고 계속 두드린다.
    [ValidateRange(1, 300)]
    [int]$RecoverIntervalSec = 10,

    # 호가 추격 시간 상한(분). 정상 종료 조건은 '매수 1호가가 목표가 밑으로 하락'이고,
    # 이 값은 그 조건이 오지 않을 때 무한 루프를 막는 안전장치다.
    [ValidateRange(1, 600)]
    [int]$ChaseMinutes = 60,

    # 예약된 시각(HH:mm). 지각 실행인지 판단하는 데만 쓴다.
    [ValidatePattern('^([01][0-9]|2[0-3]):[0-5][0-9]$')]
    [string]$ScheduledTime,

    # 예약 시각보다 이만큼 늦게 실행되면 '지각'으로 본다.
    # 지각이면 이미 매도 주문이 올라가 있는지 먼저 확인하고, 있으면 발사하지 않는다.
    [ValidateRange(1, 1440)]
    [int]$LateAfterMin = 5,

    [string]$ConfigPath,
    [string]$LogPath,

    # 실제 주문을 보내지 않고 판단 결과만 로그로 남긴다
    [switch]$DryRun
)

# StrictMode 는 켜지 않는다. KIS 응답은 상황에 따라 output / msg1 같은 필드가 통째로
# 빠져서 오므로, 없는 속성 접근이 예외가 아니라 $null 이 되는 편이 판단 로직에 안전하다.
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# 앱(QProcess)에서 호출될 때 한글 출력이 깨지지 않도록 stdout 을 UTF-8 로 고정한다.
# 콘솔이 없는 환경(리다이렉트)에서는 실패할 수 있으므로 조용히 넘어간다.
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

# ---------------------------------------------------------------- 경로 기본값

if (-not $ConfigPath) { $ConfigPath = Join-Path $PSScriptRoot '..\src\core\Config.h' }
if (-not $LogPath) {
    $logDir = Join-Path $PSScriptRoot 'logs'
    if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }
    $LogPath = Join-Path $logDir ('sell-{0}.log' -f (Get-Date -Format 'yyyyMMdd'))
}
$tokenCachePath = Join-Path $PSScriptRoot '.kis-token.json'

# ---------------------------------------------------------------------- 로깅

function Write-Log {
    param([string]$Message, [ValidateSet('INFO', 'WARN', 'ERROR', 'OK')][string]$Level = 'INFO')
    $line = '{0} [{1}] {2}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Level, $Message
    Write-Host $line
    Add-Content -Path $LogPath -Value $line -Encoding utf8
}

# ------------------------------------------------------------------- 설정 로드

function Read-KisConfig {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        throw "Config.h 를 찾을 수 없습니다: $Path"
    }
    $text = Get-Content -Path $Path -Raw -Encoding UTF8

    $get = {
        param($name)
        $m = [regex]::Match($text, ('{0}\s*=\s*"([^"]*)"' -f [regex]::Escape($name)))
        if (-not $m.Success) { throw "Config.h 에서 $name 값을 찾지 못했습니다." }
        return $m.Groups[1].Value
    }

    $cfg = [ordered]@{
        BaseUrl   = & $get 'KIS_BASE_URL'
        AppKey    = & $get 'KIS_APP_KEY'
        AppSecret = & $get 'KIS_APP_SECRET'
        Cano      = & $get 'KIS_ACCOUNT_CANO'
        PrdtCd    = & $get 'KIS_ACCOUNT_PRDT_CD'
    }

    foreach ($k in @('AppKey', 'AppSecret', 'Cano')) {
        if ([string]::IsNullOrWhiteSpace($cfg[$k]) -or $cfg[$k] -match '여기에') {
            throw "Config.h 의 $k 값이 비어 있거나 placeholder 입니다."
        }
    }
    if ($cfg.Cano -notmatch '^\d{8}$') {
        throw "KIS_ACCOUNT_CANO 는 계좌번호 앞 8자리여야 합니다: $($cfg.Cano)"
    }
    return $cfg
}

# -------------------------------------------------------------------- HTTP 헬퍼

function Invoke-Kis {
    param(
        [ValidateSet('GET', 'POST')][string]$Method,
        [string]$Url,
        [hashtable]$Headers,
        $Body
    )
    $params = @{
        Method          = $Method
        Uri             = $Url
        Headers         = $Headers
        TimeoutSec      = 15
        ErrorAction     = 'Stop'
        UseBasicParsing = $true
    }
    if ($null -ne $Body) {
        $json = $Body | ConvertTo-Json -Depth 5 -Compress
        $params.Body = [Text.Encoding]::UTF8.GetBytes($json)
        $params.ContentType = 'application/json; charset=utf-8'
    }

    # Invoke-RestMethod 대신 직접 디코딩한다. PowerShell 5.1 은 응답 헤더에 charset 이
    # 없으면 본문을 ISO-8859-1 로 읽어버려서, KIS 가 돌려주는 한글 msg1(실패 사유)이
    # 깨진다. 실패 원인을 로그에서 읽는 게 이 스크립트의 핵심이라 UTF-8 로 고정한다.
    $resp = Invoke-WebRequest @params
    $text = [Text.Encoding]::UTF8.GetString($resp.RawContentStream.ToArray())
    if ([string]::IsNullOrWhiteSpace($text)) { return $null }
    return ($text | ConvertFrom-Json)
}

# HTTP 에러 응답에서 본문을 최대한 뽑아낸다 (KIS 는 4xx 에도 msg1 을 담아 보낸다)
function Get-ErrorBody {
    param($ErrorRecord)
    try {
        $resp = $ErrorRecord.Exception.Response
        if ($null -eq $resp) { return $null }
        $stream = $resp.GetResponseStream()
        $reader = New-Object IO.StreamReader($stream, [Text.Encoding]::UTF8)
        $text = $reader.ReadToEnd()
        $reader.Close()
        if ([string]::IsNullOrWhiteSpace($text)) { return $null }
        return ($text | ConvertFrom-Json)
    }
    catch {
        return $null
    }
}

# ---------------------------------------------------------------------- 토큰

function Get-KisToken {
    param($Config, [string]$CachePath)

    # 캐시가 5분 이상 남아 있으면 재사용 (KIS 는 토큰 발급 자체에 빈도 제한이 있음)
    if (Test-Path $CachePath) {
        try {
            $cached = Get-Content $CachePath -Raw -Encoding UTF8 | ConvertFrom-Json
            $expiry = [datetime]$cached.expiry
            if ($cached.token -and $expiry -gt (Get-Date).AddMinutes(5)) {
                Write-Log ('캐시된 토큰 재사용 (만료 {0})' -f $expiry.ToString('yyyy-MM-dd HH:mm:ss'))
                return $cached.token
            }
            Write-Log '캐시된 토큰이 만료되었습니다. 새로 발급받습니다.'
        }
        catch {
            Write-Log ('토큰 캐시를 읽지 못했습니다. 새로 발급받습니다. ({0})' -f $_.Exception.Message) 'WARN'
        }
    }
    else {
        Write-Log '토큰 캐시가 없습니다. 새로 발급받습니다.'
    }

    $body = @{
        grant_type = 'client_credentials'
        appkey     = $Config.AppKey
        appsecret  = $Config.AppSecret
    }
    $res = Invoke-Kis -Method POST -Url ($Config.BaseUrl + '/oauth2/tokenP') -Headers @{} -Body $body

    if (-not $res.access_token) {
        throw ('토큰 발급 실패: {0}' -f ($res | ConvertTo-Json -Compress))
    }

    $expiresIn = 86400
    if ($res.PSObject.Properties.Name -contains 'expires_in' -and $res.expires_in) {
        $expiresIn = [int]$res.expires_in
    }
    $expiry = (Get-Date).AddSeconds($expiresIn)

    $cache = @{ token = $res.access_token; expiry = $expiry.ToString('o') }
    $cache | ConvertTo-Json | Set-Content -Path $CachePath -Encoding utf8
    # 토큰 파일은 본인만 읽도록 권한을 좁힌다
    try {
        icacls $CachePath /inheritance:r /grant:r ("{0}:F" -f $env:USERNAME) | Out-Null
    }
    catch {
        Write-Log '토큰 캐시 파일 권한 설정에 실패했습니다(무시하고 계속).' 'WARN'
    }

    Write-Log ('새 토큰 발급 완료 (만료 {0})' -f $expiry.ToString('yyyy-MM-dd HH:mm:ss')) 'OK'
    return $res.access_token
}

function Get-BaseHeaders {
    param($Config, [string]$Token, [string]$TrId)
    return @{
        'Authorization' = "Bearer $Token"
        'appkey'        = $Config.AppKey
        'appsecret'     = $Config.AppSecret
        'tr_id'         = $TrId
        'custtype'      = 'P'
    }
}

# ------------------------------------------------------------------- 호가 조회

function Get-OrderBook {
    param($Config, [string]$Token, [string]$Symbol)

    # UN(통합) 우선 - SOR 로 주문하므로 KRX+NXT 통합 호가가 실제 체결 기준이다.
    foreach ($divCode in @('UN', 'J')) {
        $url = '{0}/uapi/domestic-stock/v1/quotations/inquire-asking-price-exp-ccn?fid_cond_mrkt_div_code={1}&fid_input_iscd={2}' -f `
            $Config.BaseUrl, $divCode, $Symbol
        try {
            $res = Invoke-Kis -Method GET -Url $url -Headers (Get-BaseHeaders $Config $Token 'FHKST01010200')
            if ($res.rt_cd -ne '0' -or -not $res.output1) {
                Write-Log ('호가 조회 응답 이상 [{0}]: {1}' -f $divCode, $res.msg1) 'WARN'
                continue
            }

            # 호가단위(틱) 실측: 인접한 호가 레벨의 최소 간격이 곧 틱이다.
            # 수량 0인 레벨을 걸러내기 '전에' 계산해야 레벨이 연속으로 유지된다.
            $raw = @()
            foreach ($i in 1..10) { $raw += [double]$res.output1."bidp$i" }
            foreach ($i in 1..10) { $raw += [double]$res.output1."askp$i" }
            $raw = @($raw | Where-Object { $_ -gt 0 } | Sort-Object -Unique)

            $tick = 0
            for ($i = 1; $i -lt $raw.Count; $i++) {
                $d = $raw[$i] - $raw[$i - 1]
                if ($d -gt 0 -and ($tick -eq 0 -or $d -lt $tick)) { $tick = $d }
            }

            # 매수호가(bidp1~10)는 높은 가격부터 내려온다. 우리가 팔아넣을 대상이다.
            $bids = @()
            foreach ($i in 1..10) {
                $price = [double]$res.output1."bidp$i"
                $qty = [int]$res.output1."bidp_rsqn$i"
                if ($price -gt 0 -and $qty -gt 0) {
                    $bids += [pscustomobject]@{ Price = $price; Qty = $qty }
                }
            }

            $cur = 0
            if ($res.output2 -and $res.output2.stck_prpr) { $cur = [double]$res.output2.stck_prpr }

            return [pscustomobject]@{ Source = $divCode; Current = $cur; Bids = $bids; Tick = $tick }
        }
        catch {
            Write-Log ('호가 조회 실패 [{0}]: {1}' -f $divCode, $_.Exception.Message) 'WARN'
        }
    }
    return $null
}

# KIS 가 호가단위 때문에 거부했는지 본다.
# 문구가 바뀔 수 있어 '호가단위' / '호가 단위' 를 모두 받는다.
function Test-TickError {
    param([string]$Message)
    if ([string]::IsNullOrWhiteSpace($Message)) { return $false }
    return ($Message -match '호가\s*단위')
}

# 호가단위는 '현재가'가 아니라 '주문 가격'으로 정해진다.
# 현재가 163,100원(틱 100)인 종목이라도 210,200원에 주문하면 20만원 구간이라 틱이 500이고,
# 210,200 은 500 의 배수가 아니라서 거부된다. 그래서 호가창에서 잰 틱이 아니라
# 주문 가격으로 구간을 따져야 한다.
#
# 실제 API(aspr_unit)로 확인한 구간 (2026-08 기준):
#   12,960/14,940 -> 10 | 20,350/32,450 -> 50 | 78,000/194,800 -> 100
#   208,500/432,500 -> 500 | 1,527,000/1,660,000 -> 1,000
function Get-TickForPrice {
    param([double]$Price)

    if ($Price -lt 2000) { return 1 }
    if ($Price -lt 5000) { return 5 }
    if ($Price -lt 20000) { return 10 }
    if ($Price -lt 50000) { return 50 }
    if ($Price -lt 200000) { return 100 }
    if ($Price -lt 500000) { return 500 }
    return 1000
}

# 주문 가격의 호가단위를 정한다.
#
# 종목 종류마다 규칙이 다르다. 일반주식은 가격 구간표를 따르지만 ETF/ETN 은 가격과
# 무관하게 5원(2,000원 미만은 1원)이다. 종류를 코드로 판별할 방법이 마땅치 않으므로,
# '현재가에서 실측한 틱'이 주식 구간표의 예측과 다르면 주식이 아니라고 보고
# 실측값을 그대로 쓴다. (ETF 는 구간이 없으니 실측값이 모든 가격에 유효하다)
#
# 실측 예: KODEX200 107,865원 -> 틱 5 (주식이면 100). TIGER 26,975원 -> 5 (주식이면 50).
function Resolve-Tick {
    param([double]$OrderPrice, [double]$CurrentPrice, [double]$MeasuredTick)

    if ($MeasuredTick -gt 0 -and $CurrentPrice -gt 0) {
        $predicted = Get-TickForPrice -Price $CurrentPrice
        if ($MeasuredTick -ne $predicted) {
            return $MeasuredTick   # ETF/ETN 등 구간표를 안 따르는 종목
        }
    }
    return Get-TickForPrice -Price $OrderPrice
}

# 목표가를 호가단위(틱)에 맞춘다.
# 틱을 안 맞추면 KIS 가 '주식주문 호가단위 오류'로 거부한다.
# 방향은 '내림'이다. 올리면 주문이 안 나가거나 체결이 어려워질 수 있어서,
# 한 틱 이내로 싸지더라도 주문이 들어가는 쪽을 택한다.
function Get-TickAlignedPrice {
    param([double]$Price, [double]$Tick)

    # 내림 한 번으로 끝낸다. 틱을 제대로 골랐다면 이걸로 유효한 가격이 되고,
    # 벗어나는 폭도 한 틱 미만이다.
    if ($Tick -le 0) { return [math]::Round($Price) }
    $down = [math]::Floor($Price / $Tick) * $Tick
    if ($down -le 0) { $down = $Tick }   # 목표가가 한 틱보다 작으면 최소 한 틱
    return [double]$down
}

# ------------------------------------------------------------------- 잔고 조회

# 잔고에서 이 종목의 수량 관련 값을 한 번에 가져온다.
#   Hldg    : 보유수량
#   OrdPsbl : 주문가능수량 (미체결 매도 주문이 있으면 그만큼 줄어든다)
#   ThdtSll : 당일 매도체결 수량  <- 체결 여부 판정의 기준
function Get-HoldingSnapshot {
    param($Config, [string]$Token, [string]$Symbol)

    $q = @(
        "CANO=$($Config.Cano)"
        "ACNT_PRDT_CD=$($Config.PrdtCd)"
        'AFHR_FLPR_YN=N'
        'OFL_YN='
        'INQR_DVSN=02'
        'UNPR_DVSN=01'
        'FUND_STTL_ICLD_YN=N'
        'FNCG_AMT_AUTO_RDPT_YN=N'
        'PRCS_DVSN=00'
        'CTX_AREA_FK100='
        'CTX_AREA_NK100='
    ) -join '&'

    $url = '{0}/uapi/domestic-stock/v1/trading/inquire-balance?{1}' -f $Config.BaseUrl, $q
    $res = Invoke-Kis -Method GET -Url $url -Headers (Get-BaseHeaders $Config $Token 'TTTC8434R')

    if ($res.rt_cd -ne '0') {
        throw ('잔고조회 실패: {0}' -f $res.msg1)
    }
    foreach ($item in $res.output1) {
        if ($item.pdno -eq $Symbol) {
            return [pscustomobject]@{
                Hldg    = [int]$item.hldg_qty
                OrdPsbl = [int]$item.ord_psbl_qty
                ThdtSll = [int]$item.thdt_sll_qty
            }
        }
    }
    return [pscustomobject]@{ Hldg = 0; OrdPsbl = 0; ThdtSll = 0 }
}

function Get-HoldingQuantity {
    param($Config, [string]$Token, [string]$Symbol)

    $q = @(
        "CANO=$($Config.Cano)"
        "ACNT_PRDT_CD=$($Config.PrdtCd)"
        'AFHR_FLPR_YN=N'
        'OFL_YN='
        'INQR_DVSN=02'
        'UNPR_DVSN=01'
        'FUND_STTL_ICLD_YN=N'
        'FNCG_AMT_AUTO_RDPT_YN=N'
        'PRCS_DVSN=00'
        'CTX_AREA_FK100='
        'CTX_AREA_NK100='
    ) -join '&'

    $url = '{0}/uapi/domestic-stock/v1/trading/inquire-balance?{1}' -f $Config.BaseUrl, $q
    $res = Invoke-Kis -Method GET -Url $url -Headers (Get-BaseHeaders $Config $Token 'TTTC8434R')

    if ($res.rt_cd -ne '0') {
        throw ('잔고조회 실패: {0}' -f $res.msg1)
    }
    foreach ($item in $res.output1) {
        if ($item.pdno -eq $Symbol) {
            return [int]$item.hldg_qty
        }
    }
    return 0
}

# ----------------------------------------------------------------- 미체결 조회

# 이 종목의 미체결(정정취소 가능) 매도 주문 목록
function Get-OpenSellOrders {
    param($Config, [string]$Token, [string]$Symbol)

    $q = @(
        "CANO=$($Config.Cano)"
        "ACNT_PRDT_CD=$($Config.PrdtCd)"
        'CTX_AREA_FK100='
        'CTX_AREA_NK100='
        'INQR_DVSN_1=0'
        'INQR_DVSN_2=0'
    ) -join '&'

    $url = '{0}/uapi/domestic-stock/v1/trading/inquire-psbl-rvsecncl?{1}' -f $Config.BaseUrl, $q
    $res = Invoke-Kis -Method GET -Url $url -Headers (Get-BaseHeaders $Config $Token 'TTTC0084R')

    if ($res.rt_cd -ne '0') {
        throw ('미체결 조회 실패: {0}' -f $res.msg1)
    }

    $orders = @()
    foreach ($o in $res.output) {
        if ($o.pdno -ne $Symbol) { continue }
        if ($o.sll_buy_dvsn_cd -eq '02') { continue }   # 02 = 매수. 매도만 본다
        $psbl = [int]$o.psbl_qty
        if ($psbl -le 0) { continue }
        $orders += [pscustomobject]@{
            OrgNo   = $o.ord_gno_brno
            Odno    = $o.odno
            PsblQty = $psbl
            OrdDvsn = $o.ord_dvsn_cd
            Price   = [double]$o.ord_unpr
        }
    }
    return $orders
}

# 특정 주문번호의 미체결 잔량 정보. 목록에 없으면 $null (= 전량 체결).
function Get-UnfilledOrder {
    param($Config, [string]$Token, [string]$Symbol, [string]$Odno)
    foreach ($o in @(Get-OpenSellOrders -Config $Config -Token $Token -Symbol $Symbol)) {
        if ($o.Odno -eq $Odno) { return $o }
    }
    return $null
}

# 걸어둔 주문의 체결 상태를 확인해서 최신 상태로 갱신한다.
# 반환: @{ Aborted; Filled; Live }  - Live 가 $null 이면 전량 체결된 것이다.
function Sync-LiveOrder {
    param($Config, [string]$Token, [string]$Symbol, $Live,
        [int]$RecoverIntervalSec, [datetime]$Deadline)

    $res = Invoke-WithRecovery -What '체결 확인' -IntervalSec $RecoverIntervalSec -Deadline $Deadline `
        -Action { Get-UnfilledOrder -Config $Config -Token $Token -Symbol $Symbol -Odno $Live.Odno }
    if (-not $res.Ok) {
        return @{ Aborted = $true; Filled = 0; Live = $Live }
    }

    $open = $res.Value
    if ($null -eq $open) {
        return @{ Aborted = $false; Filled = $Live.Qty; Live = $null }
    }

    $filled = $Live.Qty - $open.PsblQty
    $newLive = @{
        Odno = $open.Odno; OrgNo = $open.OrgNo; OrdDvsn = $open.OrdDvsn
        Price = $Live.Price; Qty = $open.PsblQty
    }
    return @{ Aborted = $false; Filled = $filled; Live = $newLive }
}

# ------------------------------------------------------------------ 통신 복구

# 조회성 작업은 통신이 끊겨도 멈추지 않는다. 성공할 때까지 IntervalSec 마다 다시 두드린다.
# (주문 '전송'은 이 래퍼를 쓰면 안 된다. 중복 주문이 될 수 있으므로 별도 경로로 처리한다.)
function Invoke-WithRecovery {
    param([scriptblock]$Action, [string]$What, [int]$IntervalSec, [datetime]$Deadline)

    $tries = 0
    while ($true) {
        try {
            $value = & $Action
            if ($tries -gt 0) { Write-Log ('{0} 통신 복구됨 ({1}회 재시도 후)' -f $What, $tries) 'OK' }
            return @{ Ok = $true; Value = $value }
        }
        catch {
            $tries++
            if ((Get-Date) -ge $Deadline) {
                Write-Log ('{0} 실패 - 시간 상한에 도달해 포기합니다: {1}' -f $What, $_.Exception.Message) 'ERROR'
                return @{ Ok = $false; Value = $null }
            }
            Write-Log ('{0} 실패 ({1}회차). {2}초 후 다시 시도합니다: {3}' -f `
                    $What, $tries, $IntervalSec, $_.Exception.Message) 'WARN'
            Start-Sleep -Seconds $IntervalSec
        }
    }
}

# -------------------------------------------------------------------- 주문 취소

function Stop-SellOrder {
    param($Config, [string]$Token, $Order)

    $body = [ordered]@{
        CANO               = $Config.Cano
        ACNT_PRDT_CD       = $Config.PrdtCd
        KRX_FWDG_ORD_ORGNO = $Order.OrgNo
        ORGN_ODNO          = $Order.Odno
        ORD_DVSN           = $(if ($Order.OrdDvsn) { $Order.OrdDvsn } else { '00' })
        RVSE_CNCL_DVSN_CD  = '02'                       # 02 = 취소
        ORD_QTY            = [string]$Order.PsblQty
        ORD_UNPR           = '0'
        QTY_ALL_ORD_YN     = 'Y'                        # 잔량 전부
        EXCG_ID_DVSN_CD    = 'SOR'
    }

    $url = $Config.BaseUrl + '/uapi/domestic-stock/v1/trading/order-rvsecncl'
    $res = Invoke-Kis -Method POST -Url $url -Headers (Get-BaseHeaders $Config $Token 'TTTC0013U') -Body $body
    if ($res.rt_cd -eq '0') { return $true }
    Write-Log ('취소 거부 [{0}] {1}' -f $res.msg_cd, $res.msg1) 'WARN'
    return $false
}

# -------------------------------------------------------------------- 매도 주문

function Send-SellOrder {
    param($Config, [string]$Token, [string]$Symbol, [int]$Qty, [double]$Price)

    # 항상 지정가(00). 시장가는 호가가 얇을 때 목표가 아래로 미끄러지므로 쓰지 않는다.
    $body = [ordered]@{
        CANO            = $Config.Cano
        ACNT_PRDT_CD    = $Config.PrdtCd
        PDNO            = $Symbol
        ORD_DVSN        = '00'
        ORD_QTY         = [string]$Qty
        ORD_UNPR        = [string][int][math]::Round($Price)
        EXCG_ID_DVSN_CD = 'SOR'                         # KRX+NXT 통합
    }

    $url = $Config.BaseUrl + '/uapi/domestic-stock/v1/trading/order-cash'
    return Invoke-Kis -Method POST -Url $url -Headers (Get-BaseHeaders $Config $Token 'TTTC0011U') -Body $body
}

# 주문 1건을 보낸다. 통신이 끊겨도 멈추지 않되, 끊긴 뒤에는 반드시 '그 주문이 실제로
# 들어갔는지'를 먼저 확인하고 나서만 재전송한다. 확인 없이 재전송하면 이중 매도가 된다.
#
# 반환: @{ Ok; Odno; Filled; Aborted }
#   Odno   : 접수된 주문번호 (통신 끊김 뒤 복구로 찾아낸 경우도 포함)
#   Filled : 통신이 끊긴 사이 이미 체결된 것으로 확인된 수량
#   Aborted: 시간 상한까지 통신이 안 돌아와 판단 불가 (더 진행하면 안 됨)
function Invoke-SellWithRetry {
    param($Config, [string]$Token, [string]$Symbol, [int]$Qty, [double]$Price,
        [int]$MaxRetries, [int]$RetryIntervalSec, [int]$RecoverIntervalSec, [datetime]$Deadline,
        [double]$Tick = 0)

    # 호가단위 오류로 거부당하면 가격을 한 틱씩 낮춰가며 '즉시' 다시 시도한다.
    # 같은 가격으로 60초 뒤에 재시도해봐야 똑같이 거부당하기 때문이다.
    $price = $Price
    # 호가단위 보정은 딱 한 번만 한다. 틱 구간표가 정확하므로 한 번이면 맞아야 하고,
    # 그래도 거부당한다면 가격을 더 내릴 게 아니라 원인을 봐야 하는 상황이다.
    $tickFixes = 0
    $maxTickFixes = 1
    $immediate = $false

    $attempt = 0
    while ($attempt -le $MaxRetries) {
        if ($immediate) {
            $immediate = $false
        }
        elseif ($attempt -gt 0) {
            Write-Log ('{0}초 후 재시도 ({1}/{2})' -f $RetryIntervalSec, $attempt, $MaxRetries)
            Start-Sleep -Seconds $RetryIntervalSec
        }

        # 보내기 직전 상태를 찍어둔다. 통신이 끊겼을 때 '내 주문이 들어갔는가'를
        # 판별하는 기준점이 된다.
        $beforeOdnos = @()
        $snapBefore = $null
        try {
            $beforeOdnos = @((Get-OpenSellOrders -Config $Config -Token $Token -Symbol $Symbol) |
                ForEach-Object { $_.Odno })
            $snapBefore = Get-HoldingSnapshot -Config $Config -Token $Token -Symbol $Symbol
        }
        catch {
            Write-Log ('주문 전 상태 확인 실패: {0}' -f $_.Exception.Message) 'WARN'
        }

        $rejectMsg = $null
        try {
            $res = Send-SellOrder -Config $Config -Token $Token -Symbol $Symbol -Qty $Qty -Price $price
            if ($res.rt_cd -eq '0') {
                $odno = ''
                if ($res.output -and $res.output.ODNO) { $odno = $res.output.ODNO }
                Write-Log ('주문 접수 | {0}주 @ {1:N0}원 | 주문번호 {2}' -f $Qty, $price, $odno) 'OK'
                return @{ Ok = $true; Odno = $odno; Filled = 0; Aborted = $false; Price = $price }
            }
            Write-Log ('주문 거부 [{0}] {1}' -f $res.msg_cd, $res.msg1) 'ERROR'
            $rejectMsg = [string]$res.msg1
        }
        catch {
            $errBody = Get-ErrorBody $_
            if ($null -ne $errBody -and $errBody.PSObject.Properties.Name -contains 'rt_cd') {
                Write-Log ('주문 거부 [{0}] {1}' -f $errBody.msg_cd, $errBody.msg1) 'ERROR'
                $rejectMsg = [string]$errBody.msg1
            }
        }

        # --- 서버가 명시적으로 거부한 경우 --------------------------------------
        # 주문이 만들어지지 않았으므로 재시도해도 중복 위험이 없다.
        if ($null -ne $rejectMsg) {
            if ((Test-TickError $rejectMsg) -and $tickFixes -lt $maxTickFixes) {
                # 틱을 모르면(호가 조회 실패 등) 호가창을 다시 읽어 알아낸다
                # 낮추는 도중 가격 구간이 바뀔 수 있으므로 매번 다시 판단한다.
                # 전달받은 $Tick 이 이미 종목 종류까지 반영된 값이라 그대로 존중한다.
                $useTick = $Tick
                if ($useTick -le 0) { $useTick = Get-TickForPrice -Price $price }

                if ($useTick -gt 0) {
                    $Tick = $useTick
                    $tickFixes++
                    $next = [math]::Floor($price / $Tick) * $Tick
                    if ($next -ge $price) { $next = $price - $Tick }   # 이미 배수였으면 한 틱 아래로
                    if ($next -le 0) {
                        Write-Log '가격을 더 낮출 수 없습니다.' 'ERROR'
                        return @{ Ok = $false; Odno = ''; Filled = 0; Aborted = $false; Price = $price }
                    }
                    Write-Log ('호가단위 오류. {0:N0}원 -> {1:N0}원(호가단위 {2}원)으로 한 번만 낮춰 재시도합니다.' -f `
                            $price, $next, $Tick) 'WARN'
                    $price = $next
                    $immediate = $true
                    continue    # 재시도 횟수를 소모하지 않고 바로 다시
                }
                Write-Log '호가단위를 알 수 없어 가격을 보정하지 못했습니다.' 'ERROR'
            }
            $attempt++
            continue
        }

        # --- 여기부터는 응답을 못 받은 경우 -------------------------------------
        # 주문이 들어갔는지 알 수 없다. 멈추지 않고 통신을 되살린 뒤 실제 상태로 판정한다.
        Write-Log '주문 응답을 받지 못했습니다. 통신을 되살려 실제 접수 여부를 확인합니다.' 'WARN'

        if ($null -eq $snapBefore) {
            Write-Log '주문 전 상태를 못 찍어둬서 접수 여부를 판정할 수 없습니다. 중복을 피해 중단합니다.' 'ERROR'
            return @{ Ok = $false; Odno = ''; Filled = 0; Aborted = $true; Price = $price }
        }

        Start-Sleep -Seconds $RecoverIntervalSec
        $openRes = Invoke-WithRecovery -What '미체결 조회' -IntervalSec $RecoverIntervalSec -Deadline $Deadline `
            -Action { Get-OpenSellOrders -Config $Config -Token $Token -Symbol $Symbol }
        if (-not $openRes.Ok) {
            return @{ Ok = $false; Odno = ''; Filled = 0; Aborted = $true; Price = $price }
        }

        # 보내기 전에 없던 주문번호가 생겼으면 그게 내 주문이다
        $newOrders = @($openRes.Value | Where-Object { $beforeOdnos -notcontains $_.Odno })
        if ($newOrders.Count -gt 0) {
            Write-Log ('주문이 접수돼 있었습니다 (주문번호 {0}). 재전송하지 않습니다.' -f $newOrders[0].Odno) 'OK'
            return @{ Ok = $true; Odno = $newOrders[0].Odno; Filled = 0; Aborted = $false; Price = $price }
        }

        # 미체결에 없다면 (a) 접수 자체가 안 됐거나 (b) 들어가서 전량 체결됐거나 둘 중 하나다.
        # 판정 기준은 '당일매도수량(thdt_sll_qty)'이다. 보유수량(hldg_qty)은 결제 기준으로
        # 늦게 반영될 수 있어 체결 판정에 쓰면 안 된다(안 줄어든 걸 미접수로 오판 -> 이중매도).
        $snapRes = Invoke-WithRecovery -What '잔고 조회' -IntervalSec $RecoverIntervalSec -Deadline $Deadline `
            -Action { Get-HoldingSnapshot -Config $Config -Token $Token -Symbol $Symbol }
        if (-not $snapRes.Ok) {
            return @{ Ok = $false; Odno = ''; Filled = 0; Aborted = $true; Price = $price }
        }

        $soldMeanwhile = $snapRes.Value.ThdtSll - $snapBefore.ThdtSll
        if ($soldMeanwhile -gt 0) {
            Write-Log ('통신이 끊긴 사이 {0}주가 체결됐습니다(당일매도 {1}->{2}). 재전송하지 않습니다.' -f `
                    $soldMeanwhile, $snapBefore.ThdtSll, $snapRes.Value.ThdtSll) 'OK'
            return @{ Ok = $true; Odno = ''; Filled = $soldMeanwhile; Aborted = $false; Price = $price }
        }

        # 주문가능수량이 줄었다면 어딘가에 미체결 주문이 잡고 있다는 뜻이다.
        if ($snapRes.Value.OrdPsbl -lt $snapBefore.OrdPsbl) {
            Write-Log ('주문가능수량이 {0}->{1} 로 줄었습니다. 주문이 어딘가 잡혀 있어 재전송하지 않습니다.' -f `
                    $snapBefore.OrdPsbl, $snapRes.Value.OrdPsbl) 'WARN'
            return @{ Ok = $false; Odno = ''; Filled = 0; Aborted = $true; Price = $price }
        }

        Write-Log '주문이 접수되지 않은 것으로 확인됐습니다(당일매도·주문가능수량 변화 없음). 재전송합니다.' 'OK'
        $attempt++
    }

    Write-Log ('{0}회 시도했지만 주문을 접수시키지 못했습니다.' -f ($MaxRetries + 1)) 'ERROR'
    return @{ Ok = $false; Odno = ''; Filled = 0; Aborted = $false; Price = $price }
}

# ==================================================================== 메인 흐름

$exitCode = 1
try {
    Write-Log ('===== 예약매도 시작 | 종목 {0} | 수량 {1} | 목표가 {2:N0} | 호가추격{3} =====' -f `
            $Symbol, $Quantity, $TargetPrice, $(if ($DryRun) { ' | DRY-RUN' } else { '' }))

    $config = Read-KisConfig -Path $ConfigPath
    if ($config.BaseUrl -notmatch 'openapivts') {
        Write-Log '실전 서버로 동작합니다. 실제 계좌에서 체결됩니다.' 'WARN'
    }

    # 전체 작업의 마감 시각. 통신이 끊겨도 이 시각까지는 포기하지 않고 계속 두드린다.
    $chaseDeadline = (Get-Date).AddMinutes($ChaseMinutes)

    $tokenRes = Invoke-WithRecovery -What '토큰 발급' -IntervalSec $RecoverIntervalSec -Deadline $chaseDeadline `
        -Action { Get-KisToken -Config $config -CachePath $tokenCachePath }
    if (-not $tokenRes.Ok) {
        Write-Log '토큰을 끝내 발급받지 못했습니다.' 'ERROR'
        $exitCode = 6
        exit $exitCode
    }
    $token = $tokenRes.Value

    # --- 매도 수량 확정 ------------------------------------------------------
    $snapRes0 = Invoke-WithRecovery -What '잔고 조회' -IntervalSec $RecoverIntervalSec -Deadline $chaseDeadline `
        -Action { Get-HoldingSnapshot -Config $config -Token $token -Symbol $Symbol }
    if (-not $snapRes0.Ok) {
        Write-Log '잔고를 끝내 조회하지 못했습니다.' 'ERROR'
        $exitCode = 6
        exit $exitCode
    }
    # 기준은 보유수량(hldg_qty)이 아니라 주문가능수량(ord_psbl_qty)이다.
    # 이미 걸어둔 매도 주문이 있으면 그만큼은 다시 팔 수 없는데, 보유수량으로 잡으면
    # 그걸 못 보고 초과 주문을 내서 거부당한다.
    $held = $snapRes0.Value.OrdPsbl
    if ($snapRes0.Value.Hldg -ne $held) {
        Write-Log ('보유 {0}주 중 {1}주만 주문 가능합니다 (나머지는 이미 걸린 주문 등).' -f `
                $snapRes0.Value.Hldg, $held) 'WARN'
    }
    if ($Quantity -match '^[Aa]ll$') {
        $sellQty = $held
        Write-Log ('전량 매도: {0}주 (주문가능수량 기준)' -f $sellQty)
    }
    else {
        $sellQty = [int]$Quantity
        Write-Log ('주문가능수량 {0}주 / 요청수량 {1}주' -f $held, $sellQty)
        if ($sellQty -gt $held) {
            Write-Log ('요청수량이 주문가능수량을 초과합니다. {0}주로 조정합니다.' -f $held) 'WARN'
            $sellQty = $held
        }
    }

    if ($sellQty -le 0) {
        Write-Log '매도할 수량이 없습니다. 종료합니다.' 'ERROR'
        $exitCode = 2
        exit $exitCode
    }

    # --- 지각 실행 판정 ------------------------------------------------------
    # 예약 시각에 PC 가 꺼져 있었으면 작업 스케줄러가 부팅 후 뒤늦게 실행한다.
    # 그 경우 이미 매도 주문이 올라가 있을 수 있으므로 먼저 확인하고,
    # 올라가 있으면 중복 발사하지 않는다. 없으면 늦더라도 그대로 발사한다.
    if ($ScheduledTime) {
        $planned = [datetime]::ParseExact(
            ((Get-Date).ToString('yyyy-MM-dd') + ' ' + $ScheduledTime), 'yyyy-MM-dd HH:mm', $null)
        $lateMin = [int]((Get-Date) - $planned).TotalMinutes

        if ($lateMin -ge $LateAfterMin) {
            Write-Log ('예약 시각({0})보다 {1}분 늦게 실행되었습니다. 중복 여부를 먼저 확인합니다.' -f `
                    $ScheduledTime, $lateMin) 'WARN'
            # 중복 방지는 여기서 하지 않는다. 위에서 매도 수량을 '주문가능수량'으로
            # 잡았기 때문에, 이미 매도 주문이 올라가 있으면 그 수량만큼은 애초에
            # 팔 수량에서 빠진다(전부 걸려 있으면 0주가 되어 그냥 종료된다).
            # 여기서 '미체결이 하나라도 있으면 종료'로 막으면, 손으로 걸어둔 주문이
            # 있다는 이유만으로 발사가 통째로 취소되어버린다.
            $dupRes = Invoke-WithRecovery -What '미체결 조회' -IntervalSec $RecoverIntervalSec -Deadline $chaseDeadline `
                -Action { Get-OpenSellOrders -Config $config -Token $token -Symbol $Symbol }
            if ($dupRes.Ok) {
                $already = @($dupRes.Value)
                if ($already.Count -gt 0) {
                    $alreadyQty = 0
                    foreach ($o in $already) { $alreadyQty += $o.PsblQty }
                    Write-Log ('이 종목에 이미 매도 주문 {0}건({1}주)이 올라가 있습니다. 그 수량은 위에서 이미 제외되었습니다.' -f `
                            $already.Count, $alreadyQty) 'WARN'
                }
                else {
                    Write-Log '올라간 주문이 없습니다. 늦었지만 그대로 발사합니다.'
                }
            }
            else {
                Write-Log '미체결 조회에 실패했지만, 수량은 주문가능수량 기준이라 그대로 진행합니다.' 'WARN'
            }
        }
    }

    # --- 호가 추격 루프 ------------------------------------------------------
    # 끝나는 조건은 '다 팔림' 또는 '매수 1호가 < 목표가'. 시간 상한은 안전장치일 뿐이다.
    #
    # 핵심: 이미 같은 호가에 우리 주문이 걸려 있으면 건드리지 않는다.
    # 취소하고 같은 가격에 다시 걸면 시간우선순위만 잃고 체결이 더 늦어진다.
    # 취소·재주문은 '최우선 매수호가가 다른 가격으로 바뀌었을 때'만 한다.
    $remaining = $sellQty
    $filledTotal = 0
    $aborted = $false
    $timedOut = $false
    # 통신이 끊겨 '주문이 살아있는지 체결됐는지' 확정할 수 없는 수량.
    $unaccounted = 0
    # 지금 시장에 걸려 있는 우리 주문 @{ Odno; OrgNo; OrdDvsn; Price; Qty }
    $live = $null
    # 마지막으로 읽은 호가단위(틱)와 현재가. 목표가의 호가단위를 정할 때 쓴다.
    $lastTick = 0
    $lastPrice = 0

    Write-Log ('호가 추격 시작 (매수 1호가가 목표가 밑으로 갈 때까지, 최대 {0}분 / {1} 까지)' -f `
            $ChaseMinutes, $chaseDeadline.ToString('HH:mm:ss'))

    $round = 0
    while ($remaining -gt 0) {
        if ((Get-Date) -ge $chaseDeadline) {
            Write-Log ('추격 시간 상한({0}분)에 도달했습니다.' -f $ChaseMinutes) 'WARN'
            $timedOut = $true
            break
        }
        $round++
        Write-Log ('--- 라운드 {0} | 남은 수량 {1}주 | 마감까지 {2}분 ---' -f `
                $round, $remaining, [int]($chaseDeadline - (Get-Date)).TotalMinutes)

        # 호가 조회는 실패해도 멈추지 않는다. 통신이 돌아올 때까지 계속 두드린다.
        $book = Get-OrderBook -Config $config -Token $token -Symbol $Symbol
        if ($null -eq $book -or $book.Bids.Count -eq 0) {
            if ((Get-Date) -ge $chaseDeadline) {
                Write-Log '호가를 읽지 못한 채 시간 상한에 도달했습니다.' 'WARN'
                $timedOut = $true
                break
            }
            Write-Log ('호가를 읽지 못했습니다. {0}초 후 다시 시도합니다.' -f $RecoverIntervalSec) 'WARN'
            Start-Sleep -Seconds $RecoverIntervalSec
            continue
        }

        if ($book.Tick -gt 0) { $lastTick = $book.Tick }
        if ($book.Current -gt 0) { $lastPrice = $book.Current }

        $best = @($book.Bids | Where-Object { $_.Price -ge $TargetPrice })[0]
        if ($null -eq $best) {
            Write-Log ('매수 1호가 {0:N0} < 목표가 {1:N0} [{2}, 호가단위 {3}원]. 호가 추격을 멈춥니다.' -f `
                    $book.Bids[0].Price, $TargetPrice, $book.Source, $book.Tick)
            break
        }

        # (A) 같은 호가에 이미 걸려 있으면 그대로 두고 체결만 기다린다.
        #     단 부분체결 등으로 '아직 안 건 수량'이 남았고 그 호가에 여유 물량도 있으면,
        #     순위를 잃더라도 전량을 다시 걸어 물량을 채운다(안 그러면 일부만 일하게 된다).
        $underworked = ($null -ne $live) -and ($remaining -gt $live.Qty) -and ($best.Qty -gt $live.Qty)
        if ($underworked) {
            Write-Log ('같은 호가지만 {0}주 중 {1}주만 걸려 있습니다. 전량으로 다시 겁니다.' -f `
                    $remaining, $live.Qty) 'WARN'
        }

        if ($null -ne $live -and $live.Price -eq $best.Price -and -not $underworked) {
            Write-Log ('같은 호가 {0:N0}원에 {1}주가 이미 걸려 있습니다. 순위를 지키려 그대로 둡니다.' -f `
                    $live.Price, $live.Qty)
            Start-Sleep -Seconds $FillCheckSec

            $sync = Sync-LiveOrder -Config $config -Token $token -Symbol $Symbol -Live $live `
                -RecoverIntervalSec $RecoverIntervalSec -Deadline $chaseDeadline
            if ($sync.Aborted) {
                Write-Log ('체결 확인이 끝내 실패했습니다. 그 {0}주는 상태 불명으로 둡니다.' -f $live.Qty) 'ERROR'
                $unaccounted = $live.Qty
                $live = $null
                $aborted = $true
                break
            }
            if ($sync.Filled -gt 0) {
                Write-Log ('{0}주 체결 @ {1:N0}원' -f $sync.Filled, $live.Price) 'OK'
                $remaining -= $sync.Filled
                $filledTotal += $sync.Filled
            }
            $live = $sync.Live
            continue
        }

        # (B) 호가가 바뀌었거나 물량을 덜 걸어뒀으면 기존 주문을 취소하고 다시 낸다
        if ($null -ne $live) {
            if ($underworked) {
                Write-Log ('물량을 채우기 위해 기존 주문({0}주)을 취소합니다.' -f $live.Qty)
            }
            else {
                Write-Log ('최우선 호가가 {0:N0} -> {1:N0} 으로 바뀌었습니다. 기존 주문을 옮깁니다.' -f `
                        $live.Price, $best.Price)
            }
            $cancelRes = Invoke-WithRecovery -What '주문 취소' -IntervalSec $RecoverIntervalSec -Deadline $chaseDeadline `
                -Action { Stop-SellOrder -Config $config -Token $token -Order $live }
            if (-not $cancelRes.Ok) {
                Write-Log ('취소가 끝내 실패했습니다. 그 {0}주는 주문이 살아있을 수 있어 건드리지 않습니다.' -f $live.Qty) 'ERROR'
                $unaccounted = $live.Qty
                $live = $null
                $aborted = $true
                break
            }
            if (-not $cancelRes.Value) {
                # 취소 거부 = 그 사이 체결된 경우가 대부분이다
                Write-Log ('취소되지 않았습니다 (주문번호 {0}). 그 사이 체결된 것으로 보고 마칩니다.' -f $live.Odno) 'WARN'
                $remaining -= $live.Qty
                $filledTotal += $live.Qty
                $live = $null
                break
            }
            Write-Log ('취소 완료 (주문번호 {0})' -f $live.Odno)
            $live = $null
        }

        # (C) 새 호가에 주문을 낸다
        $qty = [Math]::Min($remaining, $best.Qty)
        Write-Log ('매수호가 {0:N0}원 x {1}주 [{2}] -> {3}주를 {0:N0}원에 매도' -f `
                $best.Price, $best.Qty, $book.Source, $qty)

        if ($DryRun) {
            # 실제로 팔 수 없으니 이 호가가 전부 체결됐다고 보고 같은 스냅샷의 다음 호가로 내려간다
            Write-Log '[DRY-RUN] 주문 생략 (체결됐다고 가정하고 다음 호가로 진행)' 'OK'
            $remaining -= $qty
            $filledTotal += $qty
            $book.Bids = @($book.Bids | Where-Object { $_.Price -lt $best.Price })
            if ($book.Bids.Count -eq 0) { break }
            continue
        }

        # 시계가 이상하거나 응답이 즉시 돌아오는 상황에서 폭주하지 않도록 하는 최후의 방어선
        if ($round -ge 10000) {
            Write-Log '라운드 수가 비정상적으로 많습니다. 중단합니다.' 'ERROR'
            $timedOut = $true
            break
        }

        $sent = Invoke-SellWithRetry -Config $config -Token $token -Symbol $Symbol `
            -Qty $qty -Price $best.Price -MaxRetries $MaxRetries -RetryIntervalSec $RetryIntervalSec `
            -RecoverIntervalSec $RecoverIntervalSec -Deadline $chaseDeadline -Tick $book.Tick
        if ($sent.Aborted) {
            # 이 수량은 주문이 나갔는지 알 수 없다. 나머지만 목표가에 건다.
            $unaccounted = $qty
            $aborted = $true
            break
        }
        if (-not $sent.Ok) {
            Write-Log '주문을 접수시키지 못했습니다. 호가 추격을 멈춥니다.' 'ERROR'
            break
        }

        # 통신이 끊긴 사이 이미 체결된 것으로 확인된 경우 (주문번호를 모른다)
        if ($sent.Filled -gt 0) {
            $remaining -= $sent.Filled
            $filledTotal += $sent.Filled
            continue
        }

        # 호가단위 보정으로 가격이 바뀌었을 수 있으니 실제 접수된 가격을 기록한다
        $livePrice = $best.Price
        if ($sent.Price) { $livePrice = $sent.Price }
        $live = @{ Odno = $sent.Odno; OrgNo = ''; OrdDvsn = '00'; Price = $livePrice; Qty = $qty }

        Start-Sleep -Seconds $FillCheckSec

        $sync = Sync-LiveOrder -Config $config -Token $token -Symbol $Symbol -Live $live `
            -RecoverIntervalSec $RecoverIntervalSec -Deadline $chaseDeadline
        if ($sync.Aborted) {
            Write-Log ('체결 확인이 끝내 실패했습니다. 그 {0}주는 상태 불명으로 둡니다.' -f $live.Qty) 'ERROR'
            $unaccounted = $live.Qty
            $live = $null
            $aborted = $true
            break
        }
        if ($sync.Filled -gt 0) {
            Write-Log ('{0}주 체결 @ {1:N0}원' -f $sync.Filled, $best.Price) 'OK'
            $remaining -= $sync.Filled
            $filledTotal += $sync.Filled
        }
        else {
            Write-Log ('아직 미체결 {0}주 (같은 호가면 다음 라운드에도 그대로 둡니다)' -f $qty)
        }
        $live = $sync.Live
    }

    # 목표가의 호가단위를 정한다. 일반주식이면 목표가 구간표, ETF 등이면 실측값.
    $parkTick = Resolve-Tick -OrderPrice $TargetPrice -CurrentPrice $lastPrice -MeasuredTick $lastTick
    $parkPrice = Get-TickAlignedPrice -Price $TargetPrice -Tick $parkTick
    if ($parkPrice -ne $TargetPrice) {
        Write-Log ('목표가 {0:N0}원은 호가단위({1}원)에 맞지 않아 {2:N0}원으로 내립니다.' -f `
                $TargetPrice, $parkTick, $parkPrice) 'WARN'
    }

    # 추격이 끝났는데 아직 살아있는 주문이 있으면 그대로 둔다.
    # 그 주문은 목표가 이상 가격에 걸려 있으므로 목표가로 다시 거는 것보다 유리하다.
    $workingQty = 0
    if ($null -ne $live) {
        $workingQty = $live.Qty
        Write-Log ('{0}주는 {1:N0}원(목표가 이상)에 걸린 채로 둡니다. 목표가로 낮추지 않습니다.' -f `
                $workingQty, $live.Price) 'OK'
    }

    # --- 남은 수량은 목표가에 걸어두기 ---------------------------------------
    # 추격이 어떻게 끝났든(정상 종료·시간 상한·통신 장애) 남은 수량은 목표가에 건다.
    # 단, 주문 상태를 확정하지 못한 수량($unaccounted)만 빼놓는다. 그건 이미 시장에
    # 나가 있을 수 있어서 다시 걸면 이중 매도가 된다.
    $parkedQty = 0        # 실제로 접수된 목표가 대기 수량
    $parkFailed = $false  # 걸어야 하는데 못 건 경우
    $parkQty = $remaining - $unaccounted - $workingQty
    if ($unaccounted -gt 0) {
        Write-Log ('{0}주는 주문 상태 불명이라 목표가에 걸지 않습니다. HTS 로 확인하세요.' -f $unaccounted) 'WARN'
    }

    if ($parkQty -gt 0) {
        Write-Log ('남은 {0}주를 목표가 {1:N0}원에 걸어둡니다.' -f $parkQty, $parkPrice)
        if ($DryRun) {
            Write-Log ('[DRY-RUN] 주문 생략 | {0}주 @ {1:N0}원 지정가 대기' -f $parkQty, $parkPrice) 'OK'
            $parkedQty = $parkQty
        }
        else {
            # 파킹은 추격 마감 이후이고 통신이 끊겨 있을 수도 있으므로 기한을 따로 준다
            $parkDeadline = (Get-Date).AddMinutes(10)
            $park = Invoke-SellWithRetry -Config $config -Token $token -Symbol $Symbol `
                -Qty $parkQty -Price $parkPrice -MaxRetries $MaxRetries -RetryIntervalSec $RetryIntervalSec `
                -RecoverIntervalSec $RecoverIntervalSec -Deadline $parkDeadline -Tick $parkTick
            if ($park.Ok) {
                $finalPrice = $parkPrice
                if ($park.Price) { $finalPrice = $park.Price }
                Write-Log ('목표가 대기 주문 접수 완료 ({0}주 @ {1:N0}원)' -f $parkQty, $finalPrice) 'OK'
                $parkedQty = $parkQty
            }
            else {
                Write-Log '목표가 대기 주문 접수에 실패했습니다. HTS 로 직접 걸어주세요.' 'ERROR'
                $parkFailed = $true
            }
        }
    }

    # 실제로 시장에 나가 있는 수량만 집계한다. 접수 실패분을 '대기'로 세면
    # 로그도 종료코드도 성공처럼 보여서, 목록에 '완료'로 떠버린다.
    $failedQty = 0
    if ($parkFailed) { $failedQty = $parkQty }

    $summaryLevel = 'OK'
    if ($aborted -or $parkFailed) { $summaryLevel = 'ERROR' }

    Write-Log ('요약 | 요청 {0}주 | 체결 {1}주 | 호가대기 {2}주 | 목표가대기 {3}주 | 접수실패 {4}주 | 상태불명 {5}주 | 라운드 {6}회{7}' -f `
            $sellQty, $filledTotal, $workingQty, $parkedQty, $failedQty, $unaccounted, $round,
            $(if ($timedOut) { ' (시간상한 도달)' } else { '' })) $summaryLevel

    if ($aborted) { $exitCode = 5 }
    elseif ($parkFailed) { $exitCode = 7 }   # 주문을 끝내 접수시키지 못함
    else { $exitCode = 0 }
}
catch {
    Write-Log ('치명적 오류: {0}' -f $_.Exception.Message) 'ERROR'
    Write-Log ($_.ScriptStackTrace) 'ERROR'
    $exitCode = 3
}
finally {
    Write-Log ('===== 종료 (exit {0}) =====' -f $exitCode)
}

exit $exitCode
