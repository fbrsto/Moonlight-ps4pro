# Regenerate src/ui/ui_assets.{c,h} from assets-src/. Windows only (System.Drawing).
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
Add-Type -Path (Join-Path $PSScriptRoot 'Baker.cs') -ReferencedAssemblies System.Drawing
[Baker]::Bake((Join-Path $root 'assets-src'))
[Baker]::WriteC((Join-Path $root 'src\ui'))
foreach ($f in [Baker]::Fonts) { "font $($f.Name): $($f.Px)px line=$($f.LineH) glyphs=$($f.Glyphs.Length) bytes=$($f.Data.Length)" }
foreach ($i in [Baker]::Icons) { "icon $($i.Name): $($i.W)x$($i.H)" }
