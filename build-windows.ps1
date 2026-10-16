param([string]$ToolsDir = (Join-Path (Split-Path $PSScriptRoot -Parent) 'tools'))
$ErrorActionPreference = 'Stop'
$cmake = Join-Path $ToolsDir 'python-packages\cmake\data\bin\cmake.exe'
$compiler = Join-Path $ToolsDir 'llvm-mingw-20261006-ucrt-x86_64\bin'
$ninja = Join-Path $ToolsDir 'python-packages\bin\ninja.exe'
$dependencies = Join-Path $ToolsDir 'deps-win64'
$build = Join-Path $PSScriptRoot 'build-win64'
foreach ($required in @($cmake, $ninja, $compiler, $dependencies)) {
    if (!(Test-Path -LiteralPath $required)) { throw "Missing build dependency: $required" }
}
$env:PATH = "$compiler;$(Split-Path $ninja -Parent);" + $env:PATH
& $cmake -S $PSScriptRoot -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" '-DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang.exe' '-DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++.exe' '-DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres.exe' '-DCMAKE_BUILD_TYPE=Release' '-DCMAKE_POLICY_VERSION_MINIMUM=3.5' '-DOPTION_USE_ASSERTS=ON' '-DOPTION_NO_LTO=ON' '-DZLIB_USE_STATIC_LIBS=ON' "-DCMAKE_PREFIX_PATH=$dependencies"
if ($LASTEXITCODE) { throw 'Configure failed' }
& $cmake --build $build --target all openttd_test --parallel 12
if ($LASTEXITCODE) { throw 'Build failed' }
& (Join-Path $build 'openttd_test.exe') --reporter compact
if ($LASTEXITCODE) { throw 'Tests failed' }
Write-Output "Windows executable: $(Join-Path $build 'openttd.exe')"
