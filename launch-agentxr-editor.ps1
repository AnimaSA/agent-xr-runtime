param(
	[Parameter(Mandatory = $false)]
	[string]$ShortcutPath
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ShortcutPath))
{
	$ShortcutPath = Join-Path (Split-Path $PSScriptRoot -Parent) "Launch VRRaidGame.lnk"
}

$ShortcutPath = (Resolve-Path $ShortcutPath).Path
$ManifestCandidates = @(
	(Join-Path $PSScriptRoot "agent-xr.json"),
	(Join-Path $PSScriptRoot "dist/agent-xr.json")
)
$ManifestPath = $ManifestCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($null -eq $ManifestPath)
{
	throw "AgentXR runtime manifest not found beside launcher or under dist/."
}
$ManifestPath = (Resolve-Path $ManifestPath).Path

$Shell = New-Object -ComObject WScript.Shell
$Shortcut = $Shell.CreateShortcut($ShortcutPath)
if ([string]::IsNullOrWhiteSpace($Shortcut.TargetPath) -or $Shortcut.Arguments -notmatch "VRRaidGame\.uproject")
{
	throw "Shortcut does not target VRRaidGame editor."
}

$HadRuntime = Test-Path Env:XR_RUNTIME_JSON
$PreviousRuntime = $env:XR_RUNTIME_JSON
try
{
	$env:XR_RUNTIME_JSON = $ManifestPath
	$WorkingDirectory = $Shortcut.WorkingDirectory
	if ([string]::IsNullOrWhiteSpace($WorkingDirectory))
	{
		$WorkingDirectory = Split-Path $Shortcut.TargetPath -Parent
	}
	Start-Process -FilePath $Shortcut.TargetPath -ArgumentList $Shortcut.Arguments -WorkingDirectory $WorkingDirectory -PassThru
}
finally
{
	if ($HadRuntime)
	{
		$env:XR_RUNTIME_JSON = $PreviousRuntime
	}
	else
	{
		Remove-Item Env:XR_RUNTIME_JSON -ErrorAction SilentlyContinue
	}
}
