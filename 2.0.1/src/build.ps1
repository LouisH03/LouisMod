$ErrorActionPreference = "Stop"

$SourceDirectory = $PSScriptRoot
$BuildDirectory = Join-Path $SourceDirectory "build"
$Output = Join-Path $BuildDirectory "LouisMod.dll"
$LocalAppData = [Environment]::GetFolderPath(
    [Environment+SpecialFolder]::LocalApplicationData)
if ([string]::IsNullOrWhiteSpace($LocalAppData)) {
    throw "The local application-data directory could not be resolved."
}
$CacheDirectory = Join-Path $LocalAppData "LouisMod\BuildCache"
$GlobalCache = Join-Path $CacheDirectory "zig-global"
$LocalCache = Join-Path $CacheDirectory "zig-local"
$TemporaryRoot = [System.IO.Path]::GetFullPath(
    [System.IO.Path]::GetTempPath()).TrimEnd('\')
$IntermediateDirectory = Join-Path $TemporaryRoot (
    "LouisMod-build-{0}" -f [Guid]::NewGuid().ToString("N"))
$IntermediateDirectory = [System.IO.Path]::GetFullPath($IntermediateDirectory)
$TemporaryPrefix = $TemporaryRoot + [System.IO.Path]::DirectorySeparatorChar
if (-not $IntermediateDirectory.StartsWith(
        $TemporaryPrefix,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "The intermediate build directory is outside the temporary directory."
}
$StagedOutput = Join-Path $IntermediateDirectory "LouisMod.dll"

$ZigCandidates = @(
    (Join-Path $SourceDirectory `
        "toolchain\zig-x86_64-windows-0.16.0\zig.exe"),
    "C:\Users\louis\Documents\Codex\2026-08-07\hi-gpt-i\work\toolchain\zig-x86_64-windows-0.16.0\zig.exe"
)
$Zig = $ZigCandidates |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Select-Object -First 1

if ($null -eq $Zig) {
    $ZigCommand = Get-Command zig -ErrorAction SilentlyContinue
    if ($null -ne $ZigCommand) {
        $Zig = $ZigCommand.Source
    }
}

if ($null -eq $Zig) {
    throw "Zig 0.16.0 was not found in the project toolchain, the portable Codex toolchain, or PATH."
}

New-Item -ItemType Directory -Force -Path $BuildDirectory | Out-Null
New-Item -ItemType Directory -Force -Path $IntermediateDirectory | Out-Null
New-Item -ItemType Directory -Force -Path $GlobalCache | Out-Null
New-Item -ItemType Directory -Force -Path $LocalCache | Out-Null

$PreviousGlobalCache = $env:ZIG_GLOBAL_CACHE_DIR
$PreviousLocalCache = $env:ZIG_LOCAL_CACHE_DIR
$env:ZIG_GLOBAL_CACHE_DIR = $GlobalCache
$env:ZIG_LOCAL_CACHE_DIR = $LocalCache

$CObject = Join-Path $IntermediateDirectory "louis_mod.o"
$AssemblyObject = Join-Path $IntermediateDirectory "button_stub.o"
$RendererObject = Join-Path $IntermediateDirectory "renderer.o"
$ImGuiDirectory = Join-Path $SourceDirectory "third_party\imgui"
$ImGuiSources = @(
    (Join-Path $ImGuiDirectory "imgui.cpp")
    (Join-Path $ImGuiDirectory "imgui_draw.cpp")
    (Join-Path $ImGuiDirectory "imgui_tables.cpp")
    (Join-Path $ImGuiDirectory "imgui_widgets.cpp")
    (Join-Path $ImGuiDirectory "backends\imgui_impl_dx9.cpp")
)
$ImGuiObjects = @()

try {
    $CCompilerArgs = @(
        "cc"
        "-target", "x86-windows-gnu"
        "-std=c11"
        "-Wall"
        "-Wextra"
        "-Werror"
        "-O2"
        "-fno-ident"
        "-fno-asynchronous-unwind-tables"
        "-fno-unwind-tables"
        "-DLOUISMOD_STANDALONE_RENDERER"
        "-c"
        "-o", $CObject
        (Join-Path $SourceDirectory "louis_mod.c")
    )

    & $Zig @CCompilerArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Zig C compilation failed with exit code $LASTEXITCODE."
    }

    $AssemblyCompilerArgs = @(
        "cc"
        "-target", "x86-windows-gnu"
        "-c"
        "-o", $AssemblyObject
        (Join-Path $SourceDirectory "button_stub.S")
    )

    & $Zig @AssemblyCompilerArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Zig assembly compilation failed with exit code $LASTEXITCODE."
    }

    $CppCommonArgs = @(
        "-target", "x86-windows-gnu"
        "-std=c++17"
        "-O2"
        "-fno-exceptions"
        "-fno-rtti"
        "-fno-ident"
        "-fno-asynchronous-unwind-tables"
        "-fno-unwind-tables"
        "-DIMGUI_USE_BGRA_PACKED_COLOR"
        "-DIMGUI_DISABLE_DEBUG_TOOLS"
        "-I", $SourceDirectory
        "-I", $ImGuiDirectory
    )

    $RendererCompilerArgs = @(
        "c++"
    ) + $CppCommonArgs + @(
        "-Wall"
        "-Wextra"
        "-Werror"
        "-c"
        "-o", $RendererObject
        (Join-Path $SourceDirectory "renderer.cpp")
    )

    & $Zig @RendererCompilerArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Zig renderer compilation failed with exit code $LASTEXITCODE."
    }

    foreach ($ImGuiSource in $ImGuiSources) {
        if (-not (Test-Path -LiteralPath $ImGuiSource -PathType Leaf)) {
            throw "Required Dear ImGui source is missing: $ImGuiSource"
        }
        $ObjectName = [System.IO.Path]::GetFileNameWithoutExtension(
            $ImGuiSource) + ".o"
        $ImGuiObject = Join-Path $IntermediateDirectory $ObjectName
        $ImGuiObjects += $ImGuiObject
        $ImGuiCompilerArgs = @(
            "c++"
        ) + $CppCommonArgs + @(
            "-c"
            "-o", $ImGuiObject
            $ImGuiSource
        )

        & $Zig @ImGuiCompilerArgs
        if ($LASTEXITCODE -ne 0) {
            throw "Zig Dear ImGui compilation failed for $ImGuiSource with exit code $LASTEXITCODE."
        }
    }

    $AllObjects = @(
        $CObject
        $AssemblyObject
        $RendererObject
    ) + $ImGuiObjects

    $LinkerArgs = @(
        "c++"
        "-target", "x86-windows-gnu"
        "-shared"
        "-Wl,--dynamicbase"
        "-Wl,--nxcompat"
        "-Wl,--subsystem,windows"
        "-s"
        "-o", $StagedOutput
    ) + $AllObjects + @(
        "-ld3d9"
        "-luser32"
        "-lkernel32"
    )

    & $Zig @LinkerArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Zig link failed with exit code $LASTEXITCODE."
    }

    Copy-Item -LiteralPath $StagedOutput -Destination $Output -Force
    Get-Item -LiteralPath $Output
}
finally {
    $env:ZIG_GLOBAL_CACHE_DIR = $PreviousGlobalCache
    $env:ZIG_LOCAL_CACHE_DIR = $PreviousLocalCache
    if (Test-Path -LiteralPath $IntermediateDirectory -PathType Container) {
        Remove-Item -LiteralPath $IntermediateDirectory -Recurse -Force
    }
}
