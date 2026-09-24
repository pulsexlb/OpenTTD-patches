# Verify the "which carrier parts may carry road vehicles" setting (vehicle.rv_transport_carrier_parts).
#
# The setting has three values:
#   0  any part with cargo capacity
#   1  only parts whose cargo is in the 'oversized' class
#   2  only parts whose cargo is bulk, 'oversized', or the NewGRF "Vehicles" cargo (label VEHC)
#      -- the default
#
# A part which may not carry is reported as rv_capacity=0t by `rvtransport parts` (that figure goes
# through exactly the same check), and loading such a part (without `force`) fails. The test savegame's
# carrier wagon carries the default cargo Wood (piece goods), so it is allowed by value 0 and refused by
# values 1 and 2 (wood is neither oversized nor bulk).
param([string]$saveName = 'Wunfingley Market Transport, 1950-03-14.sav')

. (Join-Path $PSScriptRoot '_common.ps1')

$root = Get-RoRoRoot
$exe  = Join-Path $root 'build\openttd.exe'
$cfg  = Get-RoRoTestConfig -Root $root
$sav  = Join-Path $root ("build\save\" + $saveName)
if (-not (Test-Path $sav)) { Write-Output "savegame not found: $sav"; exit 1 }

$cmds = @(
    'pause',
    'setting vehicle.rv_transport_carrier_parts',       # the savegame has no value stored: the default applies
    'setting vehicle.rv_transport_carrier_parts 0',
    'rvtransport parts firsttrain',
    'rvtransport setwaiting firsttrain firstrv',
    'rvtransport loadfrom 7 firstrv',                  # allowed: attaches
    'rvtransport detach firsttrain current force',
    'setting vehicle.rv_transport_carrier_parts 1',     # only 'oversized' cargo
    'rvtransport parts firsttrain',
    'rvtransport setwaiting firsttrain firstrv',
    'rvtransport loadfrom 7 firstrv',                  # refused: Wood is not oversized
    'setting vehicle.rv_transport_carrier_parts 2',     # the default: bulk / oversized / VEHC only
    'rvtransport parts firsttrain',
    'rvtransport setwaiting firsttrain firstrv',
    'rvtransport loadfrom 7 firstrv',                  # refused: Wood is piece goods
    'setting vehicle.rv_transport_enabled false',       # the master switch off
    'rvtransport parts firsttrain',
    'rvtransport setwaiting firsttrain firstrv',
    'rvtransport loadfrom 7 firstrv',                  # refused: nothing is loaded at all
    'setting vehicle.rv_transport_enabled true',        # and back on (with carrier parts = 0 from above)
    'setting vehicle.rv_transport_carrier_parts 0',
    'rvtransport parts firsttrain',
    'quit'
)
$txt = Invoke-RoRoTest -Tag 'carrierparts' -Exe $exe -Config $cfg -Savegame $sav -Commands $cmds -CarrierParts -1 -LogName 'log_carrier_parts.txt'
$txt -split "`r?`n" | Where-Object { $_ -match 'rv_capacity|loadfrom: part|Current value for|Assertion|crash' } | Select-Object -First 25 | ForEach-Object { Write-Output $_ }

if ($txt -match 'Assertion failed|crash encountered') { Write-Output 'RESULT: FAIL (crash/assertion)'; exit 1 }

# The setting's default value must be 2 (bulk / 'oversized' / 'VEHC' cargo only).
$defaultOk = ($txt -match "Current value for 'vehicle\.rv_transport_carrier_parts' is: '2' \(min: 0, max: 2, def: 2\)")

# The carrier's own wagon (part 1) capacity per setting value, and whether loading worked.
$caps = @()
foreach ($l in ($txt -split "`r?`n")) {
    if ($l -match 'part 1: #\d+ cargo=\d+ cap=\d+ stored=\d+ rv_capacity=(\d+)t') { $caps += [int]$Matches[1] }
}
$attaches = @()
foreach ($l in ($txt -split "`r?`n")) {
    if ($l -match 'loadfrom: part #\d+ -> carrier #\d+ .*attached=(\w+)') { $attaches += $Matches[1] }
}
Write-Output ("default value 2 = {0} ; rv_capacity readings: {1}t ; attach verdicts: {2}" -f `
    $defaultOk, ($caps -join 't / '), ($attaches -join ', '))

$positionsOk = ($caps.Count -ge 5) -and ($caps[0] -gt 0) -and ($caps[1] -eq 0) -and ($caps[2] -eq 0) -and ($caps[3] -eq 0) -and ($caps[4] -gt 0)
$verdictsOk = ($attaches.Count -ge 4) -and ($attaches[0] -eq 'true') -and ($attaches[1] -eq 'false') -and ($attaches[2] -eq 'false') -and ($attaches[3] -eq 'false')
$ok = $defaultOk -and $positionsOk -and $verdictsOk
if ($ok) {
    Write-Output 'RESULT: PASS (default is value 2; value 0 allows an ordinary piece-goods wagon, values 1 and 2 refuse it, and the master switch off refuses everything)'
} else {
    Write-Output 'RESULT: CHECK (see the readings above)'
}
