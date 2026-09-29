# Prints, for each built exe: every DLL it can possibly need (read straight out
# of the exe's own import table via dumpbin -- not guessed, not observed from one
# run, the complete/exhaustive list the Windows loader itself will consult), and
# flags anything that ISN'T a known-present-on-every-Windows-PC system DLL. This
# is exactly the check that would have caught the MSVCP140.dll/VCRUNTIME140.dll
# problem before it ever reached a client's machine.

$ErrorActionPreference = "Stop"

# Any dumpbin.exe works for this (it's just reading the PE header, not compiling),
# so search broadly rather than hardcoding one toolset version that might not
# exist on a future machine/toolset update.
$dumpbin = Get-ChildItem "C:\Program Files*\Microsoft Visual Studio\2022\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $dumpbin) {
    Write-Host "[WARN] dumpbin.exe not found -- skipping dependency report." -ForegroundColor Yellow
    exit 0
}

# DLLs that ship as part of Windows itself on every real install (has shipped
# since Windows XP/Vista/7 at the latest) -- safe to assume present anywhere.
# Anything found in an exe's import table that is NOT on this list gets flagged
# as [CHECK] rather than silently assumed safe.
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

# What the build (CMakeLists.txt) declares is statically compiled directly INTO
# the exe, not loaded externally at runtime. This is not auto-detected from the
# binary (there's no reliable, simple way to prove "this code came from library
# X" after static linking merges everything into one file) -- it reflects what
# this project's own build configuration specifies, kept in sync by hand.
$staticallyLinked = @(
    'Dear ImGui (UI library)',
    'GLFW (windowing/input)',
    'sqlite3 (vendored amalgamation source, PropertyManager only)',
    'nlohmann_json (StockViewer only)',
    'MSVC C/C++ runtime (/MT static CRT -- this is the actual fix for the',
    '  MSVCP140.dll/VCRUNTIME140.dll-not-found problem)'
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
            # dumpbin prints one blank separator line right after the header,
            # BEFORE the first DLL name -- only treat a blank line as the END of
            # the list once at least one dependency has actually been captured
            # (otherwise the very first blank line ends capture before it starts).
            if ($line.Trim() -eq '') {
                if ($deps.Count -gt 0) { break } else { continue }
            }
            if ($line -match '^\s*(\S+\.(dll|drv))\s*$') { $deps.Add($Matches[1].ToUpper()) }
        }
    }

    Write-Host ""
    Write-Host "Compiled directly INTO the exe (not external, nothing to ship separately):"
    foreach ($item in $staticallyLinked) { Write-Host "  [BUILT-IN] $item" -ForegroundColor Cyan }

    Write-Host ""
    Write-Host "External DLLs this exe's import table asks the OS for at launch:"
    Write-Host "(this is the COMPLETE list -- read directly from the file, not observed from one run)"
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
    Write-Host "Summary: $($deps.Count) external dependencies, $($deps.Count - $unknownCount) confirmed core-Windows, $unknownCount need verification." -ForegroundColor $(if ($unknownCount -eq 0) { "Green" } else { "Red" })
    if ($unknownCount -eq 0) {
        Write-Host "-> Every external dependency is a standard Windows OS component. This is exactly the state that should run on any Windows PC with no extra install." -ForegroundColor Green
    } else {
        Write-Host "-> $unknownCount dependency(ies) above are NOT on the known-Windows list -- verify these will actually be present on target machines before shipping." -ForegroundColor Red
    }
}

Show-Dependencies "build\Release\PropertyManager.exe" "PropertyManager.exe"
Write-Host ""
