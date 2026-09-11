param(
	[Parameter(Mandatory = $true)]
	[string]$ShortcutPath,
	[Parameter(Mandatory = $false)]
	[string]$ExpectedProjectPath,
	[Parameter(Mandatory = $false)]
	[string]$RuntimeManifestPath
)

$ErrorActionPreference = "Stop"

$ShortcutPath = (Resolve-Path $ShortcutPath).Path
if ([string]::IsNullOrWhiteSpace($RuntimeManifestPath))
{
	$ManifestCandidates = @(
		(Join-Path $PSScriptRoot "agent-xr.json"),
		(Join-Path $PSScriptRoot "dist/agent-xr.json")
	)
	$RuntimeManifestPath = $ManifestCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($RuntimeManifestPath))
{
	throw "AgentXR runtime manifest not found beside launcher or under dist/."
}
$RuntimeManifestPath = (Resolve-Path $RuntimeManifestPath).Path

$Manifest = Get-Content -Raw $RuntimeManifestPath | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace($Manifest.runtime.library_path))
{
	throw "AgentXR runtime manifest has no library_path."
}
$RuntimeLibraryPath = $Manifest.runtime.library_path
if (-not [IO.Path]::IsPathRooted($RuntimeLibraryPath))
{
	$RuntimeLibraryPath = Join-Path (Split-Path $RuntimeManifestPath -Parent) $RuntimeLibraryPath
}
$RuntimeLibraryPath = (Resolve-Path $RuntimeLibraryPath).Path
$Manifest.runtime.library_path = $RuntimeLibraryPath
$ResolvedManifestDirectory = Join-Path ([IO.Path]::GetTempPath()) "AgentXR"
New-Item -ItemType Directory -Force $ResolvedManifestDirectory | Out-Null
$ResolvedManifestPath = Join-Path $ResolvedManifestDirectory "agent-xr-runtime-$PID.json"
$Manifest | ConvertTo-Json -Depth 10 | Set-Content -Encoding UTF8 $ResolvedManifestPath

$Shell = New-Object -ComObject WScript.Shell
$Shortcut = $Shell.CreateShortcut($ShortcutPath)
if ([string]::IsNullOrWhiteSpace($Shortcut.TargetPath))
{
	throw "Shortcut target is empty."
}
if (-not [string]::IsNullOrWhiteSpace($ExpectedProjectPath))
{
	$ExpectedProjectPath = (Resolve-Path $ExpectedProjectPath).Path
	$NormalizedArguments = $Shortcut.Arguments.Replace("/", "\")
	if ($NormalizedArguments.IndexOf($ExpectedProjectPath, [System.StringComparison]::OrdinalIgnoreCase) -lt 0)
	{
		throw "Shortcut does not target expected project."
	}
}

$EnvironmentNames = @(
	"XR_RUNTIME_JSON",
	"XR_ENABLE_API_LAYERS",
	"XR_API_LAYER_PATH",
	"DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY"
)
$PreviousEnvironment = @{}
foreach ($Name in $EnvironmentNames)
{
	$PreviousEnvironment[$Name] = @{
		Exists = Test-Path "Env:$Name"
		Value = [Environment]::GetEnvironmentVariable($Name, "Process")
	}
}

try
{
	$env:XR_RUNTIME_JSON = $ResolvedManifestPath
	Remove-Item Env:XR_ENABLE_API_LAYERS -ErrorAction SilentlyContinue
	Remove-Item Env:XR_API_LAYER_PATH -ErrorAction SilentlyContinue
	$env:DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY = "1"

	$WorkingDirectory = $Shortcut.WorkingDirectory
	if ([string]::IsNullOrWhiteSpace($WorkingDirectory))
	{
		$WorkingDirectory = Split-Path $Shortcut.TargetPath -Parent
	}
	Start-Process -FilePath $Shortcut.TargetPath -ArgumentList $Shortcut.Arguments -WorkingDirectory $WorkingDirectory -PassThru
}
finally
{
	foreach ($Name in $EnvironmentNames)
	{
		$Previous = $PreviousEnvironment[$Name]
		if ($Previous.Exists)
		{
			[Environment]::SetEnvironmentVariable($Name, $Previous.Value, "Process")
		}
		else
		{
			[Environment]::SetEnvironmentVariable($Name, $null, "Process")
		}
	}
}
