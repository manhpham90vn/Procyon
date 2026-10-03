# `make help` on Windows: lists the Makefile targets with their `## ` descriptions.
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$seen = @{}
Get-Content (Join-Path $root "Makefile") | ForEach-Object {
    if ($_ -match '^([a-z-]+):.*## (.*)$' -and -not $seen.ContainsKey($Matches[1])) {
        $seen[$Matches[1]] = $true
        "  {0,-12} {1}" -f $Matches[1], $Matches[2]
    }
}
