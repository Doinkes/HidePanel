# ---------------------------------------------------------------------------
#  Генератор иконки HidePanel (assets/icon.ico + assets/icon-preview.png)
#  Не требует интернета и сторонних утилит: только System.Drawing.
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$IcoPath,
    [string]$PreviewPath
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $IcoPath)     { $IcoPath     = Join-Path $root '..\assets\icon.ico' }
if (-not $PreviewPath) { $PreviewPath = Join-Path $root '..\assets\icon-preview.png' }

Add-Type -AssemblyName System.Drawing

function New-RoundedPath {
    param([System.Drawing.RectangleF]$Rect, [single]$Radius)
    $d = $Radius * 2
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddArc($Rect.X,          $Rect.Y,          $d, $d, 180, 90)
    $p.AddArc($Rect.Right - $d, $Rect.Y,          $d, $d, 270, 90)
    $p.AddArc($Rect.Right - $d, $Rect.Bottom - $d, $d, $d,   0, 90)
    $p.AddArc($Rect.X,          $Rect.Bottom - $d, $d, $d,  90, 90)
    $p.CloseFigure()
    return $p
}

function Draw-Icon {
    param([System.Drawing.Graphics]$G, [int]$S)

    $G.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $G.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $G.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $G.Clear([System.Drawing.Color]::Transparent)

    $f = [single]($S / 256.0)
    function Px([double]$v) { [single]([Math]::Max(1.0, $v * $f)) }   # толщина линий

    # --- подложка ---------------------------------------------------------
    $bgRect = New-Object System.Drawing.RectangleF (6*$f), (6*$f), (244*$f), (244*$f)
    $bg     = New-RoundedPath -Rect $bgRect -Radius (48*$f)
    $gb     = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
                  (New-Object System.Drawing.PointF 0, 0),
                  (New-Object System.Drawing.PointF 0, $S),
                  [System.Drawing.Color]::FromArgb(255, 34, 39, 51),
                  [System.Drawing.Color]::FromArgb(255, 14, 16, 21))
    $G.FillPath($gb, $bg)
    $gb.Dispose()
    $border = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(70, 255, 255, 255)), (Px 2.5)
    $G.DrawPath($border, $bg)
    $border.Dispose()

    # --- монитор ----------------------------------------------------------
    $monRect = New-Object System.Drawing.RectangleF (50*$f), (58*$f), (156*$f), (120*$f)
    $mon     = New-RoundedPath -Rect $monRect -Radius (14*$f)
    $monPen  = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 233, 238, 247)), (Px 9)
    $monPen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $G.DrawPath($monPen, $mon)
    $monPen.Dispose()

    # подставка
    $standPen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 233, 238, 247)), (Px 9)
    $standPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $standPen.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
    $G.DrawLine($standPen, (108*$f), (180*$f), (128*$f), (200*$f))
    $G.DrawLine($standPen, (148*$f), (180*$f), (128*$f), (200*$f))
    $G.DrawLine($standPen, ( 88*$f), (204*$f), (168*$f), (204*$f))
    $standPen.Dispose()

    # --- "панель задач" внутри монитора -----------------------------------
    $barRect = New-Object System.Drawing.RectangleF (66*$f), (148*$f), (124*$f), (16*$f)
    $bar     = New-RoundedPath -Rect $barRect -Radius (6*$f)
    $barBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
                    (New-Object System.Drawing.PointF 0, (148*$f)),
                    (New-Object System.Drawing.PointF 0, (164*$f)),
                    [System.Drawing.Color]::FromArgb(255, 96, 214, 255),
                    [System.Drawing.Color]::FromArgb(255, 46, 160, 255))
    $G.FillPath($barBrush, $bar)
    $barBrush.Dispose()

    # --- горячая полоса у нижнего края ------------------------------------
    $hot = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 96, 214, 255)), (Px 8)
    $hot.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $hot.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
    $G.DrawLine($hot, (96*$f), (228*$f), (160*$f), (228*$f))
    $hot.Dispose()

    $bg.Dispose(); $mon.Dispose(); $bar.Dispose()
}

function New-IconBitmap {
    param([int]$Size)
    $bmp = New-Object System.Drawing.Bitmap($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.PageUnit = [System.Drawing.GraphicsUnit]::Pixel
    Draw-Icon -G $g -S $Size
    $g.Dispose()
    return $bmp
}

# --- PNG-предпросмотр для README ------------------------------------------
$preview = New-IconBitmap -Size 256
$preview.Save($PreviewPath, [System.Drawing.Imaging.ImageFormat]::Png)
$preview.Dispose()
Write-Host "preview -> $PreviewPath"

# --- многомасштабный .ico -------------------------------------------------
$sizes = 16, 20, 24, 32, 40, 48, 64, 128, 256
$blobs = @()
foreach ($s in $sizes) {
    $bmp = New-IconBitmap -Size $s
    $ms  = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $blobs += ,@{ Size = $s; Data = $ms.ToArray() }
    $ms.Dispose(); $bmp.Dispose()
}

$fs = [System.IO.File]::Create($IcoPath)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([UInt16]0)                 # reserved
$bw.Write([UInt16]1)                 # type: icon
$bw.Write([UInt16]$blobs.Count)      # count

$offset = 6 + 16 * $blobs.Count
foreach ($b in $blobs) {
    $dim = if ($b.Size -ge 256) { 0 } else { $b.Size }
    $bw.Write([Byte]$dim)            # width
    $bw.Write([Byte]$dim)            # height
    $bw.Write([Byte]0)               # palette
    $bw.Write([Byte]0)               # reserved
    $bw.Write([UInt16]1)             # planes
    $bw.Write([UInt16]32)            # bpp
    $bw.Write([UInt32]$b.Data.Length)
    $bw.Write([UInt32]$offset)
    $offset += $b.Data.Length
}
foreach ($b in $blobs) { $bw.Write($b.Data) }

$bw.Flush(); $bw.Dispose(); $fs.Dispose()
Write-Host "icon    -> $IcoPath ($($blobs.Count) sizes)"
