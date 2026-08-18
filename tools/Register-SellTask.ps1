<#
.SYNOPSIS
    Invoke-ScheduledSell.ps1 을 윈도우 작업 스케줄러에 등록한다.
    StockFlow 앱을 켜두지 않아도, PC 가 절전 상태여도 지정 시각에 깨어나 매도를 실행한다.

.EXAMPLE
    # 2026-08-17 08:00 (NXT 프리마켓 시작)에 삼성전자 10주, 목표가 85,000원
    .\Register-SellTask.ps1 -At '2026-08-17 08:00' -Symbol 005930 -Quantity 10 -TargetPrice 85000

.EXAMPLE
    # 08:00 에 5일 동안 매일 반복 (체결되든 안 되든 5일치 발사 후 종료)
    .\Register-SellTask.ps1 -At '2026-08-17 08:00' -Symbol 005930 -Quantity All -TargetPrice 85000 -RepeatDays 5

.EXAMPLE
    # 등록된 작업 확인 / 삭제
    .\Register-SellTask.ps1 -List
    .\Register-SellTask.ps1 -Remove -TaskName 'StockFlow-Sell-005930'
#>
[CmdletBinding(DefaultParameterSetName = 'Register')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Register')]
    [datetime]$At,

    [Parameter(Mandatory = $true, ParameterSetName = 'Register')]
    [ValidatePattern('^\d{6}$')]
    [string]$Symbol,

    [Parameter(Mandatory = $true, ParameterSetName = 'Register')]
    [ValidatePattern('^(\d+|[Aa]ll)$')]
    [string]$Quantity,

    [Parameter(Mandatory = $true, ParameterSetName = 'Register')]
    [double]$TargetPrice,

    [Parameter(ParameterSetName = 'Register')]
    [ValidateRange(10, 3600)]
    [int]$RetryIntervalSec = 60,

    [Parameter(ParameterSetName = 'Register')]
    [ValidateRange(0, 100)]
    [int]$MaxRetries = 10,

    # 호가 추격 시간 상한(분). 매수 1호가가 목표가 밑으로 가면 그전에 알아서 끝난다.
    [Parameter(ParameterSetName = 'Register')]
    [ValidateRange(1, 600)]
    [int]$ChaseMinutes = 60,

    # 며칠 동안 반복할지. 1 = 지정한 시각에 한 번만.
    # 2 이상이면 그날부터 N일 동안 매일 같은 시각에 발사하고 그 뒤로는 실행되지 않는다.
    [Parameter(ParameterSetName = 'Register')]
    [ValidateRange(1, 365)]
    [int]$RepeatDays = 1,

    [Parameter(ParameterSetName = 'Register')]
    [switch]$DryRun,

    [Parameter(ParameterSetName = 'Register')]
    [Parameter(Mandatory = $true, ParameterSetName = 'Remove')]
    [string]$TaskName,

    [Parameter(Mandatory = $true, ParameterSetName = 'Remove')]
    [switch]$Remove,

    [Parameter(Mandatory = $true, ParameterSetName = 'List')]
    [switch]$List,

    # 목록을 사람이 읽는 표 대신 JSON 으로 출력한다 (StockFlow 앱이 파싱용으로 사용)
    [Parameter(ParameterSetName = 'List')]
    [switch]$Json
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 앱(QProcess)에서 호출될 때 한글 출력이 깨지지 않도록 stdout 을 UTF-8 로 고정한다.
# 콘솔이 없는 환경(리다이렉트)에서는 실패할 수 있으므로 조용히 넘어간다.
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

$taskPrefix = 'StockFlow-Sell'

if ($List) {
    # 등록할 때 우리가 만든 인자 문자열을 되읽어 종목·수량·목표가를 복원한다.
    # (작업 스케줄러가 보관하는 값이 유일한 출처이므로 별도 상태 파일이 필요 없다)
    $rows = @()
    foreach ($t in @(Get-ScheduledTask | Where-Object { $_.TaskName -like "$taskPrefix*" })) {
        $info = Get-ScheduledTaskInfo -TaskName $t.TaskName -TaskPath $t.TaskPath
        $argStr = (@($t.Actions | ForEach-Object { $_.Arguments })) -join ' '

        $grab = {
            param($pattern)
            $m = [regex]::Match($argStr, $pattern)
            if ($m.Success) { return $m.Groups[1].Value } else { return '' }
        }

        # 반복 예약이면 종료 경계(EndBoundary)에서 '언제까지'를 복원한다
        $isDaily = $false
        $repeatUntil = ''
        foreach ($trg in @($t.Triggers)) {
            if ($trg.CimClass.CimClassName -eq 'MSFT_TaskDailyTrigger') { $isDaily = $true }
            if ($trg.EndBoundary) {
                try { $repeatUntil = ([datetime]$trg.EndBoundary).ToString('yyyy-MM-dd') } catch { }
            }
        }

        $next = ''
        if ($info.NextRunTime) { $next = $info.NextRunTime.ToString('yyyy-MM-dd HH:mm') }
        $last = ''
        if ($info.LastRunTime -and $info.LastRunTime.Year -gt 2000) {
            $last = $info.LastRunTime.ToString('yyyy-MM-dd HH:mm')
        }

        # 작업 스케줄러의 State 는 '사용 가능/실행 중'만 뜻해서, 1회 예약이 끝난 뒤에도
        # 계속 Ready 로 남는다. 그대로 보여주면 아직 안 나간 것처럼 읽히므로,
        # 다음 실행 시각과 마지막 실행 결과를 합쳐 사람이 읽을 상태를 따로 만든다.
        $status = '대기'
        if ([string]$t.State -eq 'Running') {
            $status = '실행 중'
        }
        elseif ([string]$t.State -eq 'Disabled') {
            $status = '사용 안 함'
        }
        elseif (-not $next) {
            if ($last) {
                if ($info.LastTaskResult -eq 0) { $status = '완료' }
                else { $status = '완료(실패 0x{0:X})' -f $info.LastTaskResult }
            }
            else {
                $status = '실행 안 됨'
            }
        }

        $rows += [pscustomobject]@{
            TaskName       = $t.TaskName
            State          = [string]$t.State
            Status         = $status
            Symbol         = (& $grab '-Symbol\s+(\S+)')
            Quantity       = (& $grab '-Quantity\s+(\S+)')
            TargetPrice    = (& $grab '-TargetPrice\s+(\S+)')
            DryRun         = [bool]($argStr -match '-DryRun')
            Daily          = $isDaily
            RepeatUntil    = $repeatUntil
            NextRunTime    = $next
            LastRunTime    = $last
            LastTaskResult = [int]$info.LastTaskResult
        }
    }

    if ($Json) {
        # PS 5.1 은 원소가 1개면 배열을 객체로 접어버린다. 앱 쪽에서 둘 다 받아준다.
        if ($rows.Count -eq 0) { Write-Output '[]' }
        else { ConvertTo-Json -InputObject @($rows) -Depth 4 -Compress }
    }
    else {
        if ($rows.Count -eq 0) {
            Write-Host '등록된 윈도우 예약이 없습니다.'
        }
        else {
            $rows | Select-Object @{n='이름';e={$_.TaskName}}, @{n='종목';e={$_.Symbol}},
            @{n='수량';e={$_.Quantity}}, @{n='목표가';e={$_.TargetPrice}},
            @{n='반복';e={ if ($_.Daily) { '~' + $_.RepeatUntil } else { '1회' } }},
            @{n='상태';e={$_.Status}}, @{n='다음실행';e={$_.NextRunTime}},
            @{n='마지막실행';e={$_.LastRunTime}} | Format-Table -AutoSize
        }
    }
    return
}

if ($Remove) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Host "작업을 삭제했습니다: $TaskName"
    return
}

# ------------------------------------------------------------------ 등록 처리

$scriptPath = Join-Path $PSScriptRoot 'Invoke-ScheduledSell.ps1'
if (-not (Test-Path $scriptPath)) {
    throw "매도 스크립트를 찾을 수 없습니다: $scriptPath"
}

if (-not $TaskName) { $TaskName = "$taskPrefix-$Symbol" }

if ($At -lt (Get-Date)) {
    throw "실행 시각이 이미 지났습니다: $($At.ToString('yyyy-MM-dd HH:mm'))"
}

# 실행 인자 구성
$argList = @(
    '-NoProfile'
    '-NonInteractive'
    '-ExecutionPolicy Bypass'
    ('-File "{0}"' -f $scriptPath)
    ('-Symbol {0}' -f $Symbol)
    ('-Quantity {0}' -f $Quantity)
    ('-TargetPrice {0}' -f $TargetPrice)
    ('-RetryIntervalSec {0}' -f $RetryIntervalSec)
    ('-MaxRetries {0}' -f $MaxRetries)
    ('-ChaseMinutes {0}' -f $ChaseMinutes)
    # 예약 시각을 넘겨두면, PC 가 꺼져 있어 늦게 실행됐을 때 스크립트가 스스로
    # '지각'을 알아채고 중복 주문 여부를 먼저 확인한다.
    ('-ScheduledTime {0}' -f $At.ToString('HH:mm'))
)
if ($DryRun) { $argList += '-DryRun' }

$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ($argList -join ' ')

if ($RepeatDays -gt 1) {
    $trigger = New-ScheduledTaskTrigger -Daily -At $At
    # 첫날 포함 RepeatDays 일까지만 발사하고 그 뒤로는 실행되지 않도록 종료 경계를 둔다
    $lastFire = $At.AddDays($RepeatDays - 1)
    $trigger.EndBoundary = $lastFire.AddMinutes(5).ToString('yyyy-MM-ddTHH:mm:ss')
}
else {
    $trigger = New-ScheduledTaskTrigger -Once -At $At
}

# 호가 추격 + 주문 재시도까지 감안한 최대 실행 시간 (+30분 여유).
# 이 값이 추격 시간보다 짧으면 작업 스케줄러가 추격 도중에 프로세스를 죽여버린다.
$limitMinutes = $ChaseMinutes + [int](($RetryIntervalSec * $MaxRetries) / 60) + 30

$settings = New-ScheduledTaskSettingsSet `
    -WakeToRun `
    -StartWhenAvailable `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Minutes $limitMinutes) `
    -MultipleInstances IgnoreNew

$qtyText = $Quantity
if ($Quantity -match '^[Aa]ll$') { $qtyText = '보유 전량' } else { $qtyText = '{0}주' -f $Quantity }

$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited

if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    Write-Host "기존 작업을 덮어씁니다: $TaskName"
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
}

Register-ScheduledTask -TaskName $TaskName `
    -Action $action -Trigger $trigger -Settings $settings -Principal $principal `
    -Description ('StockFlow 예약매도: {0} {1} 목표가 {2} 지정가' -f $Symbol, $qtyText, $TargetPrice) | Out-Null

Write-Host ''
Write-Host "등록 완료: $TaskName"
$repeatText = ''
if ($RepeatDays -gt 1) {
    $repeatText = ' (매일 반복, {0}일간 ~ {1})' -f $RepeatDays, $At.AddDays($RepeatDays - 1).ToString('yyyy-MM-dd')
}
Write-Host ("  실행 시각 : {0}{1}" -f $At.ToString('yyyy-MM-dd HH:mm'), $repeatText)
Write-Host ("  종목/수량 : {0} / {1}" -f $Symbol, $qtyText)
Write-Host ("  목표가    : {0:N0}원 (하한선. 이 가격 미만 매수호가에는 팔지 않음)" -f $TargetPrice)
Write-Host ("  호가 추격 : 매수 1호가가 목표가 밑으로 갈 때까지 (최대 {0}분)" -f $ChaseMinutes)
Write-Host ("  재시도    : {0}초 간격 최대 {1}회" -f $RetryIntervalSec, $MaxRetries)
Write-Host ("  로그      : {0}" -f (Join-Path $PSScriptRoot 'logs'))
if ($DryRun) { Write-Host '  ** DRY-RUN 모드: 실제 주문은 나가지 않습니다 **' -ForegroundColor Yellow }
Write-Host ''

# -WakeToRun 은 전원 옵션의 '절전 모드 해제 타이머 허용'이 켜져 있어야 실제로 동작한다.
# 꺼져 있으면 PC 가 잠든 사이 작업이 그냥 건너뛰어지므로 등록 시점에 확인해준다.
Write-Host '절전 모드 해제 타이머 확인:'
try {
    $sleepCfg = (powercfg /query SCHEME_CURRENT SUB_SLEEP 2>$null | Out-String)
    $pos = $sleepCfg.IndexOf('bd3b718a')          # RTCWAKE 설정 GUID
    if ($pos -lt 0) {
        Write-Host '  확인 불가: 전원 옵션에서 "절전 모드 해제 타이머 허용"을 직접 확인하세요.' -ForegroundColor Yellow
    }
    else {
        $section = $sleepCfg.Substring($pos)
        # 한국어판은 '설정 색인', 영문판은 'Setting Index'
        $m = [regex]::Match($section, 'AC[^\r\n]*?(?:색인|Index)\s*:\s*(0x[0-9a-fA-F]+)')
        if ($m.Success -and [Convert]::ToInt32($m.Groups[1].Value, 16) -ge 1) {
            Write-Host ('  사용 중 ({0}) - 절전 상태에서도 깨어나 실행됩니다.' -f $m.Groups[1].Value) -ForegroundColor Green
        }
        else {
            Write-Host '  꺼져 있습니다. PC 가 절전이면 이 작업은 실행되지 않습니다.' -ForegroundColor Red
            Write-Host '  켜기: powercfg /setacvalueindex SCHEME_CURRENT SUB_SLEEP RTCWAKE 1 ; powercfg /setactive SCHEME_CURRENT'
        }
    }
}
catch {
    Write-Host '  확인 중 오류가 발생했습니다(등록 자체는 정상).' -ForegroundColor Yellow
}
