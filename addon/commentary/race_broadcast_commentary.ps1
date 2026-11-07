# Race Broadcast - commentary voices (v3)
# The addon decides what is said and when (it knows the camera, the order, the tyres). This script only gives it voices.
# Recorded voices (the default): every line was recorded once with Google Gemini text to speech (voices.zip next to this
# file, unpacked on first use). The addon names the recording it wants, and the driver's recorded surname plays just
# before lines that name him. Piper voices (the "Commentary voices" setting): Piper (free, offline text to speech)
# reads the lines live with four British voices:
#   Ben Archer (lead)       en_GB-aru-medium, speaker 2
#   Graham Lowe (expert)    en_GB-vctk-medium, speaker p232
#   Tom Kirby (pit lane)    en_GB-aru-medium, speaker 1
#   Erin Doyle (strategy)   en_GB-jenny_dioco-medium
# First run: Piper and the voices are copied from the voice audition folder if it is there, otherwise downloaded once
# (about 250 MB) into %LOCALAPPDATA%\Swofty\addon-storage\apoc.race-broadcast\tts.
# The addon starts this hidden and it stops when the game closes. Settings: commentary_settings.ini next to this file.
param([int]$GamePid = 0, [int]$Chatty = 0, [switch]$Test, [Alias('Voices')][string]$VoiceSet = 'recorded')

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$store = Join-Path $env:LOCALAPPDATA 'Swofty\addon-storage\apoc.race-broadcast'
New-Item -ItemType Directory -Force -Path $store | Out-Null
try { Start-Transcript -Path (Join-Path $store 'commentary.log') -Force | Out-Null } catch {}
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sayFile = Join-Path $store 'say.txt'
$saidFile = Join-Path $store 'said.txt'
if (-not $Test) { try { [IO.File]::WriteAllText($saidFile, '') } catch {} }
function Said([string]$line) {
    if ($Test) { return }
    try { [IO.File]::AppendAllText($saidFile, $line + "`r`n") } catch {}
}
function Status([string]$text) { Write-Host "STATUS $text" -ForegroundColor Cyan; Said "0|status|$text" }

# ---------------------------------------------------------------- settings
$cfg = @{
    volume = 90
    lead_speed = 0.92; expert_speed = 1.0; pit_speed = 0.95; strategy_speed = 0.97
    lead_voice = 'en_GB-aru-medium:2'; expert_voice = 'en_GB-vctk-medium:p232'
    pit_voice = 'en_GB-aru-medium:1'; strategy_voice = 'en_GB-jenny_dioco-medium'
    pause = 0.15; player = 'media'
}
$ini = Join-Path $here 'commentary_settings.ini'
if (Test-Path $ini) {
    foreach ($l in Get-Content $ini) {
        if ($l -match '^\s*([a-z_]+)\s*=\s*(.*?)\s*$') { $cfg[$matches[1]] = $matches[2] }
    }
}
$inv = [Globalization.CultureInfo]::InvariantCulture
function Num($v, [double]$def) { $d = 0.0; if ([double]::TryParse("$v", [Globalization.NumberStyles]::Float, $inv, [ref]$d)) { return $d }; return $def }

# ---------------------------------------------------------------- the recorded voices
# voices.zip holds <role>\<bank>_<index>.mp3 for every line, <role>\name_<surname>.mp3 and name_x_<surname>.mp3 (said with
# excitement) for each driver, and index.json: per line [name tag (A, B, C or empty), excited (1/0), seconds], per name
# its seconds. It is unpacked into addon-storage once, and again whenever the zip changes.
$recDir = Join-Path $store 'recorded'
$rec = $null
$recOnly = $false
if ($VoiceSet -ne 'piper') {
    try {
        $zip = Join-Path $here 'voices.zip'
        $stampFile = Join-Path $recDir 'stamp.txt'
        if (Test-Path $zip) {
            $zi = Get-Item $zip
            $stamp = "$($zi.Length)|$($zi.LastWriteTimeUtc.Ticks)"
            $old = if (Test-Path $stampFile) { [IO.File]::ReadAllText($stampFile) } else { '' }
            if ($old -ne $stamp) {
                Status 'unpacking the recorded voices'
                if (Test-Path $recDir) { Remove-Item $recDir -Recurse -Force }
                Add-Type -AssemblyName System.IO.Compression.FileSystem
                [IO.Compression.ZipFile]::ExtractToDirectory($zip, $recDir)
                [IO.File]::WriteAllText($stampFile, $stamp)
            }
        }
        $ix = Join-Path $recDir 'index.json'
        if (Test-Path $ix) {
            $j = [IO.File]::ReadAllText($ix) | ConvertFrom-Json
            $rec = @{ lines = @{}; names = @{} }
            foreach ($pr in $j.lines.PSObject.Properties) { $rec.lines[$pr.Name] = $pr.Value }
            foreach ($pr in $j.names.PSObject.Properties) { $rec.names[$pr.Name] = [double]$pr.Value }
            $recOnly = $true
            Write-Host "Recorded voices: $($rec.lines.Count) lines, $($rec.names.Count) names"
        } else { Status 'the recorded voices are missing: using Piper' }
    } catch { Status "could not load the recorded voices ($($_.Exception.Message)): using Piper"; $rec = $null; $recOnly = $false }
}
# "Pérez" -> "perez", as in the file names
function Name-Key([string]$n) {
    $d = $n.Normalize([Text.NormalizationForm]::FormD)
    $sb = New-Object Text.StringBuilder
    foreach ($ch in $d.ToCharArray()) { if ([Globalization.CharUnicodeInfo]::GetUnicodeCategory($ch) -ne 'NonSpacingMark') { [void]$sb.Append($ch) } }
    return (($sb.ToString().ToLowerInvariant()) -replace '[^a-z]', '')
}
# the recording for a line: "bank;index;A;B;C" -> the files to play in turn (the name first if the line takes one)
function Rec-Files([string]$role, [string]$info) {
    if (-not $rec -or -not $info) { return $null }
    $f = $info -split ';'
    if ($f.Count -lt 2) { return $null }
    $bank = $f[0]; $ix = $f[1]
    $dir = switch ($bank.Substring(0, 1)) { 'L' { 'lead' } 'E' { 'expert' } 'P' { 'pit' } 'S' { 'strategy' } default { if (@('lead', 'expert', 'pit', 'strategy') -contains $role) { $role } else { 'lead' } } }
    $key = "$dir/$($bank)_$ix"
    $meta = $rec.lines[$key]
    if (-not $meta) {
        # conversation parts and history lines live under the voice that speaks them
        foreach ($d2 in @('lead', 'expert', 'strategy', 'pit')) { if ($rec.lines["$d2/$($bank)_$ix"]) { $dir = $d2; $key = "$d2/$($bank)_$ix"; $meta = $rec.lines[$key]; break } }
    }
    if (-not $meta) { return $null }
    $files = New-Object System.Collections.ArrayList
    $secs = New-Object System.Collections.ArrayList
    $tag = "$($meta[0])"
    $nm = switch ($tag) { 'A' { $f[2] } 'B' { $f[3] } 'C' { $f[4] } default { '' } }
    if ($nm) {
        $nk = Name-Key $nm
        $nkey = if ([int]$meta[1]) { "$dir/name_x_$nk" } else { "$dir/name_$nk" }
        if (-not $rec.names.ContainsKey($nkey)) { $nkey = "$dir/name_$nk" }
        if ($rec.names.ContainsKey($nkey)) { [void]$files.Add((Join-Path $recDir "$nkey.mp3")); [void]$secs.Add($rec.names[$nkey] + 0.12) }
    }
    [void]$files.Add((Join-Path $recDir "$key.mp3")); [void]$secs.Add([double]$meta[2])
    return @{ files = @($files); secs = @($secs) }
}

# ---------------------------------------------------------------- Piper and the voices
$tts = Join-Path $store 'tts'
$voiceDir = Join-Path $tts 'voices'
New-Item -ItemType Directory -Force -Path $voiceDir | Out-Null
$need = @()
foreach ($r in @('lead', 'expert', 'pit', 'strategy')) { $m = ("$($cfg["$($r)_voice"])" -split ':')[0]; if ($need -notcontains $m) { $need += $m } }

function Find-Piper {
    $p = Join-Path $tts 'piper\piper.exe'
    if (Test-Path $p) { return $p }
    $f = Get-ChildItem $tts -Recurse -Filter piper.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($f) { return $f.FullName }
    return $null
}
function Install-Tts {
    # 1. the voice audition folder, if it is there
    $desks = @([Environment]::GetFolderPath('Desktop'), (Join-Path $env:USERPROFILE 'Desktop'), (Join-Path $env:USERPROFILE 'OneDrive\Desktop')) | Select-Object -Unique
    $aud = $null
    foreach ($d in $desks) {
        if (-not $d) { continue }
        $c = Join-Path $d 'F1 Modding\race_broadcast\voice_audition'
        if ($d -and (Test-Path (Join-Path $c 'piper\piper.exe'))) { $aud = $c; break }
    }
    if (-not (Find-Piper)) {
        if ($aud) {
            Status 'copying Piper from the voice audition folder'
            Copy-Item (Join-Path $aud 'piper') -Destination $tts -Recurse -Force
        } else {
            Status 'downloading Piper (first run only)'
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            $zip = Join-Path $tts 'piper_windows_amd64.zip'
            Invoke-WebRequest 'https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_windows_amd64.zip' -OutFile $zip -UseBasicParsing
            Expand-Archive $zip -DestinationPath $tts -Force
            Remove-Item $zip -ErrorAction SilentlyContinue
        }
    }
    foreach ($m in $need) {
        $onnx = Join-Path $voiceDir "$m.onnx"
        if ((Test-Path $onnx) -and (Test-Path "$onnx.json")) { continue }
        if ($aud -and (Test-Path (Join-Path $aud "voices\$m.onnx")) -and (Test-Path (Join-Path $aud "voices\$m.onnx.json"))) {
            Status "copying voice $m"
            Copy-Item (Join-Path $aud "voices\$m.onnx") $onnx -Force
            Copy-Item (Join-Path $aud "voices\$m.onnx.json") "$onnx.json" -Force
            continue
        }
        if ($m -notmatch '^en_GB-([a-z_]+)-([a-z]+)$') { throw "unknown voice name $m" }
        Status "downloading voice $m (first run only)"
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $base = "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_GB/$($matches[1])/$($matches[2])/$m"
        Invoke-WebRequest "$base.onnx.json" -OutFile "$onnx.json" -UseBasicParsing
        Invoke-WebRequest "$base.onnx" -OutFile "$onnx.part" -UseBasicParsing
        Move-Item "$onnx.part" $onnx -Force
    }
}
if (-not $recOnly) {
    try { Install-Tts } catch { Status "could not get the voices ready: $($_.Exception.Message)" }
    $piper = Find-Piper
    if (-not $piper) { Status 'Piper is missing, commentary is off'; exit 1 }
    Write-Host "Piper: $piper"
}

# ---------------------------------------------------------------- one Piper process per voice
$work = Join-Path $store $(if ($Test) { 'tts\out_test' } else { 'tts\out' })
if (Test-Path $work) { Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path $work | Out-Null

function Speaker-Id([string]$model, [string]$sp) {
    if (-not $sp) { return $null }
    if ($sp -match '^\d+$') { return [int]$sp }
    try {
        $j = [IO.File]::ReadAllText((Join-Path $voiceDir "$model.onnx.json")) | ConvertFrom-Json
        $v = $j.speaker_id_map.$sp
        if ($null -ne $v) { return [int]$v }
    } catch {}
    Status "speaker $sp not found in $model, using its default speaker"
    return $null
}

$voices = @{}
foreach ($r in @('lead', 'expert', 'pit', 'strategy')) {
    if ($recOnly) { $voices[$r] = @{ role = $r; proc = $null; jobs = (New-Object System.Collections.Generic.List[object]) }; continue }
    $parts = "$($cfg["$($r)_voice"])" -split ':'
    $voices[$r] = @{
        role = $r; model = $parts[0]; speaker = (Speaker-Id $parts[0] ($parts | Select-Object -Skip 1 -First 1))
        speed = (Num $cfg["$($r)_speed"] 1.0); proc = $null; dir = (Join-Path $work $r); fallback = $false
        jobs = (New-Object System.Collections.Generic.List[object]); n = 0
        outFile = (Join-Path $work "$($r)_out.txt"); outPos = 0; outBuf = ''; lastProgress = (Get-Date)
    }
    New-Item -ItemType Directory -Force -Path $voices[$r].dir | Out-Null
}

function Piper-Args($v, [string]$out, [string]$outFlag) {
    $a = "--model `"$(Join-Path $voiceDir "$($v.model).onnx")`" $outFlag `"$out`" --length_scale $($v.speed.ToString($inv)) --sentence_silence $((Num $cfg.pause 0.15).ToString($inv))"
    if ($null -ne $v.speaker) { $a += " --speaker $($v.speaker)" }
    return $a
}
# is this voice's Piper process running (written plainly: PS 5.1 can fail on "obj -and bool" mixes)
function Alive($v) {
    if ($null -eq $v.proc) { return $false }
    try { if ($v.proc.HasExited) { return $false } } catch { return $false }
    return $true
}
function Stop-Voice($v) {
    if ($script:recOnly) { return }
    if ($null -eq $v.proc) { return }
    try { $v.proc.StandardInput.Close() } catch {}
    # closing stdin ends Piper; only a process that is still there after a moment is stopped by force
    try { [void]$v.proc.WaitForExit(800) } catch {}
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'SilentlyContinue'
    try { if (Alive $v) { & taskkill.exe /T /F /PID $v.proc.Id 2>&1 | Out-Null } } catch {}
    $ErrorActionPreference = $eap
    $v.proc = $null
}
# Piper prints the path of each finished file on its own line; the cmd window sends that to <role>_out.txt
function Start-Voice($v) {
    if ($script:recOnly) { return }
    if ($v.fallback) { return }
    if (Alive $v) { return }
    if ($null -ne $v.proc) { Write-Host "voice $($v.role): Piper stopped, starting it again" -ForegroundColor Red; Stop-Voice $v }
    # lines asked of a process that died are asked again
    $old = $v.jobs.ToArray(); $v.jobs.Clear()
    foreach ($j in $old) { $cache.Remove($j.key) }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $env:ComSpec
    $err = Join-Path $work "$($v.role)_piper.txt"
    $psi.Arguments = "/c `"`"$piper`" $(Piper-Args $v $v.dir '--output_dir') 1>`"$($v.outFile)`" 2>`"$err`"`""
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.CreateNoWindow = $true
    $psi.WorkingDirectory = Split-Path -Parent $piper
    $v.outPos = 0; $v.outBuf = ''
    $v.proc = [System.Diagnostics.Process]::Start($psi)
    $v.lastProgress = Get-Date
    Write-Host "voice $($v.role): $($v.model) speaker $($v.speaker) speed $($v.speed) (process $($v.proc.Id))"
    foreach ($j in $old) { Render $v.role $j.text }
}

# a request: render text for a role
$cache = @{}   # role|text -> wav path ($null while it is being made)
function Wav-Done([string]$f) {
    try {
        $fs = [IO.File]::Open($f, 'Open', 'Read', 'Read')
        try {
            if ($fs.Length -lt 44) { return $false }
            $b = New-Object byte[] 8
            [void]$fs.Read($b, 0, 8)
            $riff = [BitConverter]::ToInt32($b, 4)
            return ($riff + 8) -le $fs.Length
        } finally { $fs.Dispose() }
    } catch { return $false }
}
function Wav-Secs([string]$f) {
    try {
        $b = [IO.File]::ReadAllBytes($f)
        $rate = [BitConverter]::ToInt32($b, 28)
        if ($rate -gt 0) { return ($b.Length - 44) / $rate }
    } catch {}
    return 2.0
}
function Render([string]$role, [string]$text) {
    if ($script:recOnly) { return }
    $key = "$role|$text"
    if ($cache.ContainsKey($key)) { return }
    $v = $voices[$role]
    if (-not $v) { return }
    $cache[$key] = $null
    $job = @{ key = $key; text = $text; t = (Get-Date); file = $null }
    if (-not $v.fallback) {
        try {
            Start-Voice $v
            $bytes = [Text.Encoding]::UTF8.GetBytes(($text -replace '[\r\n|]', ' ') + "`n")
            $v.proc.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
            $v.proc.StandardInput.BaseStream.Flush()
            if ($v.jobs.Count -eq 0) { $v.lastProgress = Get-Date }
            $v.jobs.Add($job)
            return
        } catch { Write-Host "voice $role process failed: $($_.Exception.Message) $($_.InvocationInfo.PositionMessage) $($_.ScriptStackTrace), using one process per line" -ForegroundColor Red; $v.fallback = $true; Stop-Voice $v }
    }
    # fallback: one Piper run per line (slower to start, always works)
    $v.n = $v.n + 1
    $wav = Join-Path $v.dir ("line{0}.wav" -f $v.n)
    $txt = Join-Path $v.dir ("line{0}.txt" -f $v.n)
    [IO.File]::WriteAllText($txt, $text + "`n")
    $job.file = $wav
    try {
        Start-Process -FilePath $piper -ArgumentList (Piper-Args $v $wav '--output_file') -RedirectStandardInput $txt `
            -RedirectStandardError (Join-Path $v.dir ("line{0}_log.txt" -f $v.n)) -RedirectStandardOutput (Join-Path $v.dir ("line{0}_out.txt" -f $v.n)) -NoNewWindow | Out-Null
        $v.jobs.Add($job)
    } catch { Write-Host "could not run Piper: $($_.Exception.Message)" -ForegroundColor Red; $cache.Remove($key) }
}
function Switch-Fallback($v, [string]$why) {
    Write-Host "voice $($v.role): $why, switching to one process per line" -ForegroundColor Red
    $err = Join-Path $work "$($v.role)_piper.txt"
    try { if (Test-Path $err) { Get-Content $err -Tail 5 | ForEach-Object { Write-Host "  piper: $_" -ForegroundColor DarkGray } } } catch {}
    $v.fallback = $true
    Stop-Voice $v
    $old = @($v.jobs); $v.jobs.Clear()
    foreach ($j in $old) { $cache.Remove($j.key); Render $v.role $j.text }
    if ($script:want -and $script:want.role -eq $v.role) { $script:want.t = Get-Date }   # give the slower way its time
}
function Collect {
    if ($script:recOnly) { return }
    foreach ($v in @($voices.Values)) {
        if ($v.jobs.Count -eq 0) { continue }
        if ($v.fallback) {
            for ($i = $v.jobs.Count - 1; $i -ge 0; $i--) {
                $j = $v.jobs[$i]
                if ($j.file -and (Test-Path $j.file) -and (Wav-Done $j.file)) { $cache[$j.key] = $j.file; $v.jobs.RemoveAt($i) }
                elseif (((Get-Date) - $j.t).TotalSeconds -gt 25) { $cache.Remove($j.key); $v.jobs.RemoveAt($i) }
            }
            continue
        }
        # the paths Piper has printed, in order: each one is the oldest request's audio
        try {
            if (Test-Path $v.outFile) {
                $fs = New-Object IO.FileStream($v.outFile, 'Open', 'Read', 'ReadWrite, Delete')
                try {
                    if ($fs.Length -gt $v.outPos) {
                        [void]$fs.Seek($v.outPos, 'Begin')
                        $bytes = New-Object byte[] ($fs.Length - $v.outPos)
                        $got = $fs.Read($bytes, 0, $bytes.Length)
                        $v.outPos += $got
                        $v.outBuf += [Text.Encoding]::UTF8.GetString($bytes, 0, $got)
                    }
                } finally { $fs.Dispose() }
            }
        } catch {}
        while (($nl = $v.outBuf.IndexOf("`n")) -ge 0) {
            $path = $v.outBuf.Substring(0, $nl).Trim(); $v.outBuf = $v.outBuf.Substring($nl + 1)
            if (-not $path.EndsWith('.wav') -or $v.jobs.Count -eq 0) { continue }
            $j = $v.jobs[0]; $v.jobs.RemoveAt(0)
            $j.file = $path
            $v.lastProgress = Get-Date
            if (Wav-Done $path) { $cache[$j.key] = $path } else { $v.jobs.Insert(0, $j); $v.outBuf = "$path`n" + $v.outBuf; break }
        }
        $dead = ($null -ne $v.proc) -and -not (Alive $v)
        if ($v.jobs.Count -gt 0 -and $dead) { Switch-Fallback $v 'Piper stopped'; continue }
        # no progress for a long time while lines wait: the process is not working
        if ($v.jobs.Count -gt 0 -and ((Get-Date) - $v.lastProgress).TotalSeconds -gt 15) { Switch-Fallback $v 'no audio from Piper' }
    }
}

# ---------------------------------------------------------------- playing
$player = $null; $mode = 'media'
if ("$($cfg.player)" -eq 'sound') { $mode = 'sound'; $player = New-Object System.Media.SoundPlayer }
else { try { Add-Type -AssemblyName PresentationCore; $player = New-Object System.Windows.Media.MediaPlayer; $player.Volume = [Math]::Max(0, [Math]::Min(1, (Num $cfg.volume 90) / 100.0)) }
catch { $mode = 'sound'; $player = New-Object System.Media.SoundPlayer; Write-Host "MediaPlayer not available, playing at full volume" } }
$playing = $null   # @{ id; until }
$want = $null      # the line the addon wants said: @{ id; role; text; t }
$queue = New-Object System.Collections.ArrayList   # the rest of a recorded line (after the driver's name): @{ file; secs }
function Play-File([string]$file, [double]$secs) {
    if ($script:mode -eq 'media') { $script:player.Open([Uri]$file); $script:player.Play() }
    else { $script:player.SoundLocation = $file; $script:player.Play() }
    $script:clipSecs = $secs
    $script:clipT0 = Get-Date
    # the latest it can end; Clip-Done normally sees the real end sooner (the media player starts a moment after Play)
    $tail = 0.05; if ($script:mode -eq 'media') { $tail = 1.5 }
    return (Get-Date).AddSeconds($secs + $tail)
}
# the clip has really finished playing: by the media player's own position (it can start late, so a fixed time would
# cut the end of a word off when the next clip opens), or the time limit
function Clip-Done {
    if (-not $script:playing) { return $true }
    if ((Get-Date) -ge $script:playing.until) { return $true }
    if ($script:mode -ne 'media') { return $false }
    if (((Get-Date) - $script:clipT0).TotalSeconds -lt [math]::Max(0.2, $script:clipSecs - 0.3)) { return $false }
    try {
        $p = $script:player
        if ($p.NaturalDuration.HasTimeSpan) { return ($p.Position.TotalSeconds -ge $p.NaturalDuration.TimeSpan.TotalSeconds - 0.02) }
    } catch {}
    return $false
}
function Play($w, [string]$file) {
    $script:queue.Clear()
    if ($w.files) {
        # a recording: the name (if any), then the line
        $secs = 0.0; foreach ($x in $w.secs) { $secs += $x }
        for ($i = 1; $i -lt $w.files.Count; $i++) { [void]$script:queue.Add(@{ file = $w.files[$i]; secs = $w.secs[$i] }) }
        $until = Play-File $w.files[0] $w.secs[0]
    } else {
        $secs = Wav-Secs $file
        $until = Play-File $file $secs
    }
    $script:playing = @{ id = $w.id; until = $until }
    Said ("{0}|start|{1}" -f $w.id, $secs.ToString('0.00', $inv))
    $tag = @{ lead = 'BEN   '; expert = 'GRAHAM'; pit = 'TOM   '; strategy = 'ERIN  ' }[$w.role]
    Write-Host ("{0} {1}  {2}" -f (Get-Date).ToString('HH:mm:ss'), $tag, $w.text)
}
function Stop-Play {
    $script:queue.Clear()
    if (-not $script:playing) { return }
    try { if ($script:mode -eq 'media') { $script:player.Stop() } else { $script:player.Stop() } } catch {}
    Said ("{0}|end" -f $script:playing.id)
    $script:playing = $null
}

# ---------------------------------------------------------------- a voice check without the game (Test voices.bat)
if ($Test -and $recOnly) {
    # one recorded line from each commentator, the first with a name
    $samples = @(@('lead', 'L_pass_lead;0;Verstappen;Norris;'), @('expert', 'E_pass_good;0;Norris;;'), @('pit', 'P_out_quick;0;Hamilton;;'), @('strategy', 'S_pit_generic;0;;;'))
    $n = 0
    foreach ($sm in $samples) {
        $n++
        $rf = Rec-Files $sm[0] $sm[1]
        if (-not $rf) { Write-Host "  no recording for $($sm[1])" -ForegroundColor Red; continue }
        Play @{ id = $n; role = $sm[0]; text = $sm[1]; files = $rf.files; secs = $rf.secs } $null
        while ($true) {
            Start-Sleep -Milliseconds 50
            if (Clip-Done) { if ($queue.Count) { $nx = $queue[0]; $queue.RemoveAt(0); $playing.until = Play-File $nx.file $nx.secs } else { break } }
        }
        $playing = $null
        Start-Sleep -Milliseconds 400
    }
    Write-Host 'Voice check done (recorded voices).' -ForegroundColor Green
    exit 0
}
if ($Test) {
    $lines = [ordered]@{
        lead = "Ben Archer here with the lead commentary. And he's gone for it down the inside, they're side by side, and he's through!"
        expert = "Graham Lowe, the expert view. Look at the front left there, he's locked it up, and that's opened the door."
        pit = "Tom Kirby in the pit lane. Quick stop, two point four seconds, and he's out on the hard tyre."
        strategy = "Erin Doyle on strategy. Most of the front runners started on the medium, so watch for the undercut around lap twelve."
    }
    $n = 0
    foreach ($r in $lines.Keys) {
        $n++
        Write-Host "Rendering $r ..."
        Render $r $lines[$r]
        $t0 = Get-Date
        while (-not $cache["$r|$($lines[$r])"] -and ((Get-Date) - $t0).TotalSeconds -lt 40) { Collect; Start-Sleep -Milliseconds 100 }
        $file = $cache["$r|$($lines[$r])"]
        if (-not $file) { Write-Host "  no audio for $r (see the log)" -ForegroundColor Red; continue }
        Write-Host ("  ready in {0:0.0} s" -f ((Get-Date) - $t0).TotalSeconds)
        Play @{ id = $n; role = $r; text = $lines[$r] } $file
        while ((Get-Date) -lt $playing.until) { Start-Sleep -Milliseconds 100 }
        $playing = $null
    }
    foreach ($v in @($voices.Values)) { Stop-Voice $v }
    Write-Host 'Voice check done.' -ForegroundColor Green
    exit 0
}

# ---------------------------------------------------------------- the addon's lines
# one copy only
$mtx = New-Object Threading.Mutex($false, 'Local\race_broadcast_commentary')
try { if (-not $mtx.WaitOne(0)) { Write-Host 'Commentary is already running.'; exit 0 } } catch [Threading.AbandonedMutexException] {}
function Game-Gone { return ($GamePid -gt 0 -and -not (Get-Process -Id $GamePid -ErrorAction SilentlyContinue)) }
$waitT = Get-Date
while (-not (Test-Path $sayFile)) {
    if (Game-Gone) { exit 0 }
    if (((Get-Date) - $waitT).TotalSeconds -gt 3) { try { [IO.File]::WriteAllText($sayFile, '') } catch {}; Write-Host 'say.txt made by the voice side' }
    Start-Sleep -Milliseconds 300
}
$pos = (Get-Item $sayFile).Length   # only what is said from now on
$buf = ''
$dec = [Text.Encoding]::UTF8.GetDecoder()
Status 'ready'
Write-Host "Listening to $sayFile"
$lastPidCheck = Get-Date
foreach ($r in @('lead', 'expert')) { try { Start-Voice $voices[$r] } catch { Write-Host "could not start $r voice: $($_.Exception.Message) $($_.InvocationInfo.PositionMessage) $($_.ScriptStackTrace)" -ForegroundColor Red } }
while ($true) {
    $now = Get-Date
    if ($GamePid -gt 0 -and ($now - $lastPidCheck).TotalSeconds -gt 2) {
        $lastPidCheck = $now
        if (Game-Gone) { Write-Host 'The game has closed: commentary off.'; break }
    }
    # new lines
    try {
        $len = (Get-Item $sayFile).Length
        if ($len -lt $pos) { $pos = 0; $buf = ''; $dec = [Text.Encoding]::UTF8.GetDecoder() }   # a new game started the file again
        if ($len -gt $pos) {
            $fs = New-Object IO.FileStream($sayFile, 'Open', 'Read', 'ReadWrite, Delete')
            try {
                [void]$fs.Seek($pos, 'Begin')
                $bytes = New-Object byte[] ($len - $pos)
                $got = $fs.Read($bytes, 0, $bytes.Length)
            } finally { $fs.Dispose() }
            $pos += $got
            $chars = New-Object char[] ($got + 4)
            $nc = $dec.GetChars($bytes, 0, $got, $chars, 0)
            $buf += [string]::new($chars, 0, $nc)
        }
    } catch {}
    try {
        while (($nl = $buf.IndexOf("`n")) -ge 0) {
            $line = $buf.Substring(0, $nl).TrimEnd("`r"); $buf = $buf.Substring($nl + 1)
            $f = @($line -split '\|', 4)
            if ($f.Length -lt 4) { continue }
            $id = $f[0]; $cmd = $f[1]; $role = $f[2]; $text = $f[3]
            if ($cmd -eq 'warm') { foreach ($r in ($text -split ',')) { if ($voices[$r]) { Start-Voice $voices[$r] } } }
            elseif ($cmd -eq 'prep') { if ($text -and $voices[$role]) { Render $role (($text -split "`t")[0]) } }
            elseif ($cmd -eq 'say') {
                # text, then after a tab which recording it is (bank;index;names)
                $tp = $text -split "`t", 2
                $plain = $tp[0]; $info = if ($tp.Count -gt 1) { $tp[1] } else { '' }
                $rf = Rec-Files $role $info
                if ($voices[$role] -and $plain -and ($rf -or -not $recOnly)) {
                    if ($playing) { Stop-Play }
                    if ($want) { Said ("{0}|end" -f $want.id) }
                    $want = @{ id = $id; role = $role; text = $plain; t = $now }
                    if ($rf) { $want.files = $rf.files; $want.secs = $rf.secs }
                    else { Render $role $plain }
                } else {
                    if ($recOnly -and $plain) { Write-Host "no recording for: $plain ($info)" -ForegroundColor DarkGray }
                    Said "$id|end"
                }
            }
            elseif ($cmd -eq 'cut') {
                Stop-Play
                if ($want) { Said ("{0}|end" -f $want.id); $want = $null }
            }
        }
    } catch { Write-Host "error: $($_.Exception.Message) $($_.InvocationInfo.PositionMessage) $($_.ScriptStackTrace)" -ForegroundColor Red }
    try {
        Collect
        if ($playing -and (Clip-Done)) {
            if ($queue.Count) {   # the next part of a recorded line
                $nx = $queue[0]; $queue.RemoveAt(0)
                $playing.until = Play-File $nx.file $nx.secs
            } else { Said ("{0}|end" -f $playing.id); $playing = $null }
        }
        if ($want -and -not $playing) {
            $key = "$($want.role)|$($want.text)"
            if ($want.files) { $w = $want; $want = $null; Play $w $null }
            elseif ($cache[$key]) { $w = $want; $want = $null; Play $w $cache[$key] }
            elseif (((Get-Date) - $want.t).TotalSeconds -gt 25) { Write-Host "line dropped (no audio in time): $($want.text)" -ForegroundColor DarkGray; Said ("{0}|end" -f $want.id); $want = $null }
        }
    } catch {
        Write-Host "error: $($_.Exception.Message) $($_.InvocationInfo.PositionMessage) $($_.ScriptStackTrace)" -ForegroundColor Red
        if ($want) { Said ("{0}|end" -f $want.id); $want = $null }
    }
    Start-Sleep -Milliseconds 30
}
foreach ($v in @($voices.Values)) { Stop-Voice $v }
try { Stop-Transcript | Out-Null } catch {}
