param(
    [switch]$PrintInstall,
    [switch]$Install,
    [switch]$Json
)

$ErrorActionPreference = 'Stop'

$args = @('scripts/check_vulkan_dev.py')
if ($PrintInstall)
{
    $args += '--print-install'
}
if ($Install)
{
    $args += '--install'
}
if ($Json)
{
    $args += '--json'
}

python @args
