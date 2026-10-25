# Runs a test scenario of explorer-lite in the user's session, on a desktop of its own: nothing shows on the
# user's screens, nothing takes the focus, and the test's tray icon never reaches the real tray (the tray lives on
# the default desktop). Started by tools/pc-test.sh through a scheduled task. The app runs as --instance test (its
# own data folder %LOCALAPPDATA%\explorer-lite-test, mutex and window classes), never the installed one.
#
# Steps file, one per line:
#   conf <key=value>   a line for the test instance's explorer-lite.conf (before start)
#   start <args>       starts the resident (explorer-lite.exe --instance test <args>), not waited for
#   send <args>        runs explorer-lite.exe --instance test <args> and waits for it (a command for the resident)
#   cmd <verb> <arg>   = send --cmd "<verb> <arg>"
#   shot <name>        = cmd shot <out>\<name>.png, then waits for the file
#   sleep <ms>
#   mem <label>        the resident's working set and private bytes, into results.json
#   shell <args>       starts the shell (explorer-lite.exe --instance test --shell <args>), not waited for
#   shellcmd <verb> <arg>   = send --shell --cmd "<verb> <arg>" (a command for the shell)
#   shellshot <name>   the shell's main taskbar into <out>\<name>.png; startshot <name>: Start (open) the same way
#   shellpng <verb> <name>   the same through any of the shell's shot verbs (overflowshot, balloonshot, switchershot)
#   shellmem <label>   the shell's memory, like mem
#   logcheck <text>    an error unless the log has a line with <text> (what the app checked itself, e.g. filtertest)
#   stop               ends the resident and the shell (and what they started)
#   make <dir>         a folder with file1.txt..file3.txt (a sandbox for file operations)
#   exists <path> / missing <path>   checks, failures go to errors
#   probe <args>       runs explorer-lite.exe --instance test <args> on the user's desktop (no window: --everything)
# {dir} in a step is this test's folder.
param([string]$Dir = $PSScriptRoot, [string]$Steps = "$PSScriptRoot\steps.txt")
$ErrorActionPreference = 'Stop'
$out = Join-Path $Dir 'out'
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $Dir 'explorer-lite.exe'
$data = Join-Path $env:LOCALAPPDATA 'explorer-lite-test'
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $data
New-Item -ItemType Directory -Force $data | Out-Null

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class TestDesk {
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateDesktopW(string name, IntPtr dev, IntPtr mode, uint flags, uint access, IntPtr sa);
    [DllImport("user32.dll")] public static extern bool CloseDesktop(IntPtr h);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct SI {
        public int cb; public string reserved; public string desktop; public string title;
        public int x, y, xs, ys, xc, yc, fill, flags; public short show, r2; public IntPtr r3, i, o, e;
    }
    [StructLayout(LayoutKind.Sequential)] public struct PI { public IntPtr hp, ht; public int pid, tid; }
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env, string dir, ref SI si, out PI pi);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    // starts it on that desktop; waits up to waitMs (0: not at all); returns the pid
    public static int Run(string exe, string args, string desktop, uint waitMs) {
        SI si = new SI(); si.cb = Marshal.SizeOf(typeof(SI)); si.desktop = desktop;
        PI pi;
        if (!CreateProcessW(exe, new StringBuilder("\"" + exe + "\" " + args), IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, null, ref si, out pi))
            throw new Exception("CreateProcess " + Marshal.GetLastWin32Error());
        if (waitMs > 0) WaitForSingleObject(pi.hp, waitMs);
        CloseHandle(pi.ht); CloseHandle(pi.hp);
        return pi.pid;
    }
}
'@

$deskName = 'explorer-lite-test'
$desk = [TestDesk]::CreateDesktopW($deskName, [IntPtr]::Zero, [IntPtr]::Zero, 0, 0x10000000, [IntPtr]::Zero)
if ($desk -eq [IntPtr]::Zero) { throw 'CreateDesktop failed' }
$results = [ordered]@{ mem = @(); shots = @(); errors = @() }
$resident = 0
$shell = 0
function Send([string]$a) { [void][TestDesk]::Run($exe, "--instance test $a", $deskName, 20000) }
# a PNG drawn by the app itself (a --cmd verb that takes a file name: it writes only into its data folder's shots),
# waited for and moved to the results
function Shot([string]$prefix, [string]$verb, [string]$name) {
    $f = Join-Path $out "$name.png"
    $made = Join-Path $data "shots\$name.png"
    Remove-Item -Force -ErrorAction SilentlyContinue $f, $made
    Send "$prefix--cmd `"$verb $name.png`""
    for ($t = 0; $t -lt 50 -and -not (Test-Path $made); $t++) { Start-Sleep -Milliseconds 100 }
    Start-Sleep -Milliseconds 200
    if (Test-Path $made) { Move-Item -Force $made $f; $script:results.shots += "$name.png" } else { $script:results.errors += "no shot $name" }
}
function Mem([int]$id, [string]$label, [string]$who) {
    $p = Get-Process -Id $id -ErrorAction SilentlyContinue
    if ($p) {
        $p.Refresh()
        # Task Manager's «Memory»: the private working set
        $pf = Get-CimInstance Win32_PerfFormattedData_PerfProc_Process -Filter "IDProcess=$id" -ErrorAction SilentlyContinue
        $script:results.mem += [ordered]@{ label = $label; ws_mb = [math]::Round($p.WorkingSet64 / 1MB, 1); private_ws_mb = [math]::Round($pf.WorkingSetPrivate / 1MB, 1); private_mb = [math]::Round($p.PrivateMemorySize64 / 1MB, 1);
            threads = $p.Threads.Count; handles = $p.HandleCount; cpu_s = [math]::Round($p.TotalProcessorTime.TotalSeconds, 2) }
    } else { $script:results.errors += "mem $label`: the $who is not running" }
}
function Mine { @(Get-CimInstance Win32_Process -Filter "Name='explorer-lite.exe'" | Where-Object { $_.ExecutablePath -eq $exe }) }
try {
    foreach ($line in Get-Content -Encoding UTF8 $Steps) {
        $line = $line.Trim().Replace('{dir}', $Dir)
        if (-not $line -or $line.StartsWith('#')) { continue }
        $verb, $rest = $line -split ' ', 2
        switch ($verb) {
            'conf' { Add-Content -Encoding UTF8 (Join-Path $data 'explorer-lite.conf') $rest }
            'start' { $script:resident = [TestDesk]::Run($exe, "--instance test $rest", $deskName, 0) }
            'send' { Send $rest }
            'cmd' { Send "--cmd `"$rest`"" }
            'shot' { Shot '' 'shot' $rest }
            'sleep' { Start-Sleep -Milliseconds ([int]$rest) }
            'mem' { Mem $resident $rest 'resident' }
            'shell' { $script:shell = [TestDesk]::Run($exe, "--instance test --shell $rest", $deskName, 0) }
            'shellcmd' { Send "--shell --cmd `"$rest`"" }
            'shellshot' { Shot '--shell ' 'shot' $rest }
            'startshot' { Shot '--shell ' 'startshot' $rest }
            'shellpng' { $v, $n = $rest -split ' ', 2; Shot '--shell ' $v $n }
            'shellmem' { Mem $shell $rest 'shell' }
            'logcheck' {
                $log = Join-Path $data 'explorer-lite.log'
                if (-not ((Test-Path $log) -and (Select-String -Path $log -SimpleMatch -Quiet $rest))) { $results.errors += "not in the log: $rest" }
            }
            'stop' {
                Send '--quit'
                Start-Sleep -Milliseconds 500
                foreach ($p in Mine) { & taskkill.exe /PID $p.ProcessId /T /F 2>&1 | Out-Null }
            }
            'probe' {
                $p = Start-Process -FilePath $exe -ArgumentList "--instance test $rest" -PassThru -Wait -WindowStyle Hidden
                if ($p.ExitCode -ne 0) { $results.errors += "probe $rest`: exit $($p.ExitCode)" }
            }
            'make' {
                New-Item -ItemType Directory -Force $rest | Out-Null
                1..3 | ForEach-Object { Set-Content -Encoding UTF8 (Join-Path $rest "file$_.txt") "explorer-lite test $_" }
            }
            'exists' { if (-not (Test-Path -LiteralPath $rest)) { $results.errors += "missing: $rest" } }
            'missing' { if (Test-Path -LiteralPath $rest) { $results.errors += "still there: $rest" } }
            default { $results.errors += "unknown step: $line" }
        }
    }
} catch {
    $results.errors += "$_"
} finally {
    # nothing of ours stays alive
    foreach ($p in Mine) { & taskkill.exe /PID $p.ProcessId /T /F 2>&1 | Out-Null }
    Start-Sleep -Milliseconds 300
    [void][TestDesk]::CloseDesktop($desk)
    if (Test-Path (Join-Path $data 'explorer-lite.log')) { Copy-Item (Join-Path $data 'explorer-lite.log') $out }
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $data
    $results | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $out 'results.json')
    New-Item -ItemType File -Force (Join-Path $Dir 'done.txt') | Out-Null
}
