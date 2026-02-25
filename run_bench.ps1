$env_lines = cmd /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" 2>nul && set 2>nul'
foreach ($line in $env_lines) {
    if ($line -match '^([A-Za-z_][^=]*)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
& 'C:\Users\DELL\OneDrive - Grambling State University\Desktop\Low-Latency-Limit-Order-Book\build\lob_bench.exe' 1000000
