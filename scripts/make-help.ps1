# `make help` on Windows: lists the Makefile targets with their `## ` descriptions.
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
# Only the Windows block (from "# ---- Windows" to the next "# ---- " marker).
$inBlock = $false
Get-Content (Join-Path $root "Makefile") | ForEach-Object {
    if ($_ -match '^# ---- (\w+)') {
        $inBlock = $Matches[1] -eq 'Windows'
    } elseif ($inBlock -and $_ -match '^([a-z-]+):.*## (.*)$') {
        "  {0,-12} {1}" -f $Matches[1], $Matches[2]
    }
}
