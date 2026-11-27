$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$source = (Get-Item -LiteralPath "$PSScriptRoot\..").FullName
$root = $source
if (Test-Path -LiteralPath "$source\.git" -PathType Leaf) {
    $worktrees = @(git -C $source -c core.quotePath=false worktree list --porcelain)
    if ($LASTEXITCODE -ne 0 -or $worktrees[0] -notlike 'worktree *') { throw 'Cannot locate the primary checkout.' }
    $root = (Get-Item -LiteralPath $worktrees[0].Substring(9)).FullName
}

function Get-StreamDigest([IO.Stream]$stream) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash($stream)) } finally { $sha.Dispose() }
}

function Assert-ProjectPath([string]$path) {
    $current = [IO.Path]::GetFullPath($path)
    if (!$current.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Path outside checkout: $current" }
    while ($current -ne $root) {
        $item = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
        if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Refusing linked path: $current" }
        $current = [IO.Path]::GetDirectoryName($current)
    }
}

function Assert-UnlinkedTree([string]$path) {
    Assert-ProjectPath $path
    foreach ($item in (Get-ChildItem -LiteralPath $path -Force)) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing linked item: $($item.FullName)" }
        if ($item.PSIsContainer) { Assert-UnlinkedTree $item.FullName }
    }
}

$dist = Join-Path $root 'dist'
$runs = Join-Path $root 'build\package-runs'
Assert-ProjectPath $dist
Assert-ProjectPath $runs
[void][IO.Directory]::CreateDirectory($dist)
[void][IO.Directory]::CreateDirectory($runs)
$run = Join-Path $runs ([Guid]::NewGuid().ToString('N'))
$package = Join-Path $run 'TokenBucket'
$archive = Join-Path $run 'TokenBucket-windows-x64.zip'
$live = Join-Path $dist 'TokenBucket'
$liveZip = Join-Path $dist 'TokenBucket-windows-x64.zip'
$previous = Join-Path $run 'previous-package'
$previousZip = Join-Path $run 'previous.zip'
$lockPath = Join-Path $runs 'publish.lock'
foreach ($path in @($package, $archive, $live, $liveZip, $previous, $previousZip, $lockPath)) { Assert-ProjectPath $path }
$lock = [IO.File]::Open($lockPath, 'OpenOrCreate', 'ReadWrite', 'None')
try {
    [void][IO.Directory]::CreateDirectory($package)
    $dependencies = Join-Path $source 'build\vcpkg_installed\x64-windows'
    foreach ($file in @('build\Release\TokenBucket.exe', 'build\Release\TokenBucket-agent.exe', 'docs\agent-access.md')) {
        Copy-Item -LiteralPath (Join-Path $source $file) -Destination $package
    }
    & "$dependencies\tools\Qt6\bin\windeployqt.exe" --release --no-translations "$package\TokenBucket.exe"
    if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE." }
    Copy-Item -Path "$dependencies\bin\*.dll" -Destination $package -Force
    Copy-Item -LiteralPath "$source\docs\licenses" -Destination "$package\licenses" -Recurse
    foreach ($file in @('libsodium.dll','Qt6Core.dll','platforms\qwindows.dll','licenses\libsodium-LICENSE.txt','licenses\qtbase-LICENSE.txt')) {
        if (!(Test-Path -LiteralPath (Join-Path $package $file) -PathType Leaf)) { throw "Missing package file: $file" }
    }
    Assert-UnlinkedTree $package
    [IO.Compression.ZipFile]::CreateFromDirectory($package, $archive, [IO.Compression.CompressionLevel]::Optimal, $true)
    $expected = @{}
    foreach ($file in (Get-ChildItem -LiteralPath $package -File -Recurse -Force)) {
        $relative = $file.FullName.Substring($run.Length + 1).Replace('\','/')
        $stream = [IO.File]::OpenRead($file.FullName)
        try { $expected[$relative] = Get-StreamDigest $stream } finally { $stream.Dispose() }
    }
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName.Replace('\','/')
            if ($name.EndsWith('/')) { continue }
            $stream = $entry.Open()
            try { $actual = Get-StreamDigest $stream } finally { $stream.Dispose() }
            if ($expected[$name] -ne $actual) { throw "ZIP verification failed: $name" }
            $expected.Remove($name)
        }
        if ($expected.Count) { throw "ZIP missing $($expected.Count) file(s)." }
    } finally { $zip.Dispose() }

    [void][IO.Directory]::CreateDirectory($dist)
    $savedFolder = $false
    $savedZip = $false
    $installedFolder = $false
    try {
        if ([IO.Directory]::Exists($live)) { [IO.Directory]::Move($live, $previous); $savedFolder = $true }
        if ([IO.File]::Exists($liveZip)) { [IO.File]::Move($liveZip, $previousZip); $savedZip = $true }
        [IO.Directory]::Move($package, $live)
        $installedFolder = $true
        [IO.File]::Move($archive, $liveZip)
    } catch {
        $publishError = $_.Exception.Message
        $undoErrors = @()
        if ($installedFolder) {
            try { [IO.Directory]::Move($live, $package) } catch { $undoErrors += $_.Exception.Message }
        }
        if ($savedFolder) {
            try { [IO.Directory]::Move($previous, $live) } catch { $undoErrors += $_.Exception.Message }
        }
        if ($savedZip) {
            try { [IO.File]::Move($previousZip, $liveZip) } catch { $undoErrors += $_.Exception.Message }
        }
        if ($undoErrors.Count) { throw "Publication failed ($publishError). Recovery files: $run. Rollback: $($undoErrors -join ' / ')" }
        throw "Publication failed ($publishError). Previous outputs preserved. Verified package: $run"
    }
    Write-Host "Published $live and $liveZip"
    $running = Get-Process -Name TokenBucket,TokenBucket-agent -ErrorAction SilentlyContinue
    if ($savedFolder -and $running) {
        Write-Host "Previous package retained for running processes: $run"
    } else {
        try {
            Assert-UnlinkedTree $run
            Remove-Item -LiteralPath $run -Recurse -Force
        } catch { Write-Warning "Published successfully. Previous files retained in $run. $($_.Exception.Message)" }
    }
} finally { $lock.Dispose() }
