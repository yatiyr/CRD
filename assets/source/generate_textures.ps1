# Generates checker_512.png and bricks_512.png as procedural CC0 demo textures.
# Run once to refresh; outputs are committed to git so this is not invoked at build.
Add-Type -AssemblyName System.Drawing

$out_dir = $PSScriptRoot
$size = 512

# ---- checker_512.png ---------------------------------------------------------
$bmp = New-Object System.Drawing.Bitmap $size, $size
$cell = 64
for ($y = 0; $y -lt $size; $y++) {
    for ($x = 0; $x -lt $size; $x++) {
        $cx = [int]([Math]::Floor($x / $cell))
        $cy = [int]([Math]::Floor($y / $cell))
        if ((($cx + $cy) % 2) -eq 0) {
            $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(255, 230, 230, 230))
        }
        else {
            $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(255, 64, 64, 64))
        }
    }
}
$bmp.Save("$out_dir\checker_512.png", [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "checker_512.png written"

# ---- bricks_512.png ----------------------------------------------------------
$bmp = New-Object System.Drawing.Bitmap $size, $size
$bw = 96     # brick width
$bh = 32     # brick height
$mortar = 4  # mortar thickness
$brick_color  = [System.Drawing.Color]::FromArgb(255, 145, 70, 50)
$mortar_color = [System.Drawing.Color]::FromArgb(255, 200, 200, 195)

for ($y = 0; $y -lt $size; $y++) {
    $row = [int]([Math]::Floor($y / ($bh + $mortar)))
    $row_y = $y - $row * ($bh + $mortar)
    $offset = if (($row % 2) -eq 0) { 0 } else { [int]($bw / 2) }
    for ($x = 0; $x -lt $size; $x++) {
        $bx = ($x + $offset) % ($bw + $mortar)
        $is_mortar = ($row_y -ge $bh) -or ($bx -ge $bw)
        $c = if ($is_mortar) { $mortar_color } else { $brick_color }
        # Mild deterministic stripe shading along brick height for relief cue.
        $shade = [int]([Math]::Sin(($row_y / [double]$bh) * [Math]::PI) * 16) - 8
        $r = [Math]::Max(0, [Math]::Min(255, [int]$c.R + $shade))
        $g = [Math]::Max(0, [Math]::Min(255, [int]$c.G + $shade))
        $b = [Math]::Max(0, [Math]::Min(255, [int]$c.B + $shade))
        $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(255, $r, $g, $b))
    }
}
$bmp.Save("$out_dir\bricks_512.png", [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "bricks_512.png written"
