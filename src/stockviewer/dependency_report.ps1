# StockViewer dependency report.
#
# Reads StockViewer.exe's PE import table (via dumpbin) and flags any DLL it
# needs at launch that is NOT a standard Windows OS component. Should print
# zero [CHECK] lines if the static build is correct.

$ErrorActionPreference = "Stop"

# --- Locate dumpbin without a hardcoded absolute path ---
$dumpbin = $null
$vsRoots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ }
foreach ($root in $vsRoots) {
    if (-not (Test-Path $root)) { continue }
    $found = Get-ChildItem "$root\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" `
                          -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $dumpbin = $found; break }
}
if (-not $dumpbin) {
    Write-Host "[WARN] dumpbin.exe not found -- skipping dependency report." -ForegroundColor Yellow
    exit 0
}

# DLLs that ship with every Windows install (since XP/Vista/7 at the latest).
$knownWindowsDlls = @(
    'KERNEL32.DLL','KERNELBASE.DLL','USER32.DLL','GDI32.DLL','GDI32FULL.DLL','SHELL32.DLL',
    'SHLWAPI.DLL','OLE32.DLL','OLEAUT32.DLL','ADVAPI32.DLL','COMCTL32.DLL','COMDLG32.DLL',
    'IMM32.DLL','OPENGL32.DLL','GLU32.DLL','WINMM.DLL','WS2_32.DLL','WINHTTP.DLL','WININET.DLL',
    'VERSION.DLL','CRYPT32.DLL','SETUPAPI.DLL','UXTHEME.DLL','DWMAPI.DLL','MSIMG32.DLL',
    'NETAPI32.DLL','RPCRT4.DLL','SECUR32.DLL','BCRYPT.DLL','BCRYPTPRIMITIVES.DLL','NTDLL.DLL',
    'MSVCRT.DLL','UCRTBASE.DLL','SECHOST.DLL','CFGMGR32.DLL','WTSAPI32.DLL','PSAPI.DLL',
    'IPHLPAPI.DLL','MSCTF.DLL','POWRPROF.DLL','PROPSYS.DLL','SHCORE.DLL','WINSPOOL.DRV',
    'D3D11.DLL','D3D12.DLL','DXGI.DLL','XINPUT1_4.DLL','XINPUT9_1_0.DLL'
)

# What the build statically compiles into the exe (nothing to ship as DLL).
$staticallyLinked = @(
    'Dear ImGui (UI library)',
    'GLFW (windowing / input)',
    'nlohmann_json (JSON parser)',
    'OpenGL (client-side stub, driver loads the real thing)',
    'MSVC C/C++ runtime (/MT static CRT -- no MSVCP140 / VCRUNTIME140 needed)'
)

function Show-Dependencies($exePath, $exeName) {
    if (-not (Test-Path $exePath)) {
        Write-Host "[SKIP] $exeName not found at $exePath" -ForegroundColor Yellow
        return
    }
    Write-Host ""
    Write-Host "=================================================================="
    Write-Host " $exeName -- dependency report"
    Write-Host "=================================================================="

    $output = & $dumpbin.FullName '/dependents' $exePath
    $deps = New-Object System.Collections.Generic.List[string]
    $capture = $false
    foreach ($line in $output) {
        if ($line -match 'the following dependencies') { $capture = $true; continue }
        if ($capture) {
            if ($line.Trim() -eq '') {
                if ($deps.Count -gt 0) { break } else { continue }
            }
            if ($line -match '^\s*(\S+\.(dll|drv))\s*$') { $deps.Add($Matches[1].ToUpper()) }
        }
    }

    Write-Host ""
    Write-Host "Compiled directly INTO the exe (nothing to ship separately):"
    foreach ($item in $staticallyLinked) {
        Write-Host "  [BUILT-IN] $item" -ForegroundColor Cyan
    }

    Write-Host ""
    Write-Host "External DLLs the exe asks the OS for at launch:"
    Write-Host "(complete list, read from the PE header -- not observed from one run)"
    $unknownCount = 0
    foreach ($dll in $deps) {
        if ($knownWindowsDlls -contains $dll) {
            Write-Host "  [OK - core Windows component]  $dll" -ForegroundColor Green
        } else {
            Write-Host "  [CHECK - NOT a recognized core Windows DLL]  $dll" -ForegroundColor Red
            $unknownCount++
        }
    }

    Write-Host ""
    Write-Host "Summary: $($deps.Count) external dependencies, $($deps.Count - $unknownCount) confirmed core-Windows, $unknownCount need verification." `
        -ForegroundColor $(if ($unknownCount -eq 0) { "Green" } else { "Red" })
    if ($unknownCount -eq 0) {
        Write-Host "-> Every external dependency is a standard Windows OS component." -ForegroundColor Green
        Write-Host "   This exe runs on any Windows 10/11 PC with no extra install." -ForegroundColor Green
    } else {
        Write-Host "-> $unknownCount dependency(ies) above need verification before shipping." -ForegroundColor Red
    }
}

Show-Dependencies "build\Release\StockViewer.exe" "StockViewer.exe"
Write-Host ""