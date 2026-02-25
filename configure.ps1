# configure.ps1 – configure, build, and test the LOB project

$proj  = "C:\Users\DELL\OneDrive - Grambling State University\Desktop\Low-Latency-Limit-Order-Book"
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

# Load VS build environment (ignore non-fatal warnings from vcvars64.bat)
Write-Host "Loading VS 2022 environment..." -ForegroundColor Yellow
$env_lines = cmd /c "`"$vcvars`" 2>nul && set 2>nul"
foreach ($line in $env_lines) {
    if ($line -match "^([A-Za-z_][^=]*)=(.*)$") {
        try {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
        } catch {}
    }
}

# Verify cl.exe is reachable
$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $cl) {
    Write-Host "cl.exe not found in PATH. Adding manually..." -ForegroundColor Yellow
    $msvc_bin = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.42.34433\bin\Hostx64\x64"
    if (-not (Test-Path $msvc_bin)) {
        # Find whichever MSVC version exists
        $msvc_bin = (Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC" |
                     Sort-Object Name -Descending | Select-Object -First 1).FullName + "\bin\Hostx64\x64"
    }
    $env:PATH = "$msvc_bin;$env:PATH"
}

$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if ($cl) {
    Write-Host "cl.exe found: $($cl.Source)" -ForegroundColor Green
} else {
    Write-Host "WARNING: cl.exe still not in PATH. Build may fail." -ForegroundColor Red
}

Set-Location $proj

# ── Configure ─────────────────────────────────────────────────────────────────
Write-Host "`n[1/3] Configuring (NMake Release)..." -ForegroundColor Cyan
& $cmake -S $proj -B "$proj\build" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) {
    Write-Host "NMake configure failed. Trying Visual Studio generator..." -ForegroundColor Yellow
    & $cmake -S $proj -B "$proj\build" -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed"; exit 1 }
    $vs_generator = $true
}

# ── Build ─────────────────────────────────────────────────────────────────────
Write-Host "`n[2/3] Building..." -ForegroundColor Cyan
if ($vs_generator) {
    & $cmake --build "$proj\build" --config Release --parallel
} else {
    & $cmake --build "$proj\build" --parallel
}
if ($LASTEXITCODE -ne 0) { Write-Error "Build failed"; exit 1 }

# ── Test ──────────────────────────────────────────────────────────────────────
Write-Host "`n[3/3] Running unit tests..." -ForegroundColor Cyan
$tests_exe = if ($vs_generator) { "$proj\build\Release\lob_tests.exe" } else { "$proj\build\lob_tests.exe" }
& $tests_exe
if ($LASTEXITCODE -ne 0) { Write-Error "Tests failed"; exit 1 }

Write-Host "`n=== Build successful ===" -ForegroundColor Green
