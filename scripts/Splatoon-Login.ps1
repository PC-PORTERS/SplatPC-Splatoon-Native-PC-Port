param([string]$Shot = '')
# Online sign-in screen. Asks for a pseudo, saves it in Game\settings.ini (pseudo=, every other
# line kept), then starts the game online (PLAY) or offline ("Play offline"). The identity file
# online_identity.ini created by the port keeps the account linked to this PC: no password.
#
# Server: scripts\server.txt (optional, never committed) with lines
#   server=<host>:<port>   nncs1=<ip>   nncs2=<ip>
# Without it, PLAY uses the local lab server started by Launch-Online.ps1.
# -Shot <png>: writes a picture of the screen and exits (checking the layout).
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()
$Root = Split-Path -Parent $PSScriptRoot
$Game = Join-Path $Root 'Game'
$Settings = Join-Path $Game 'settings.ini'
$Executable = Join-Path $Game 'Splatoon-v288.exe'
# New version on GitHub (update.ini)? Offered before the sign-in screen; silent otherwise.
if (-not $Shot) { & (Join-Path $PSScriptRoot 'Update-Check.ps1') }

function Get-SavedPseudo {
    if (-not (Test-Path -LiteralPath $Settings)) { return '' }
    foreach ($line in Get-Content -LiteralPath $Settings) { if ($line -match '^\s*pseudo\s*=\s*(.*)$') { return $Matches[1].Trim() } }
    return ''
}
function Get-Setting([string]$Key) {
    if (-not (Test-Path -LiteralPath $Settings)) { return '' }
    foreach ($line in Get-Content -LiteralPath $Settings) { if ($line -match "^\s*$Key\s*=\s*(.*)$") { return $Matches[1].Trim() } }
    return ''
}
function Save-Setting([string]$Key, [string]$Value) {
    $lines = @()
    if (Test-Path -LiteralPath $Settings) { $lines = @(Get-Content -LiteralPath $Settings | Where-Object { $_ -notmatch "^\s*$Key\s*=" }) }
    $lines += "$Key=$Value"
    [System.IO.File]::WriteAllLines($Settings, [string[]]$lines, (New-Object System.Text.UTF8Encoding($false)))
}
function Save-Pseudo([string]$Name) { Save-Setting 'pseudo' $Name }

# Game languages: the console language code the port reads from settings.ini language= (the Wii U
# system language numbers). Only the languages whose text files the player's own game dump holds
# (content\Message\CommonMsg_<region><lang>.szs, for example EUfr or USes) are offered.
function U([int[]]$Codes) { return -join ($Codes | ForEach-Object { [char]$_ }) }
$LanguageTable = @{
    ja = @(0,  (U 0x65E5, 0x672C, 0x8A9E))
    en = @(1,  'English')
    fr = @(2,  ('Fran' + [char]0xE7 + 'ais'))
    de = @(3,  'Deutsch')
    it = @(4,  'Italiano')
    es = @(5,  ('Espa' + [char]0xF1 + 'ol'))
    ko = @(7,  (U 0xD55C, 0xAD6D, 0xC5B4))
    nl = @(8,  'Nederlands')
    pt = @(9,  ('Portugu' + [char]0xEA + 's'))
    ru = @(10, (U 0x420, 0x443, 0x441, 0x441, 0x43A, 0x438, 0x439))
}
function Get-InstalledLanguages {
    $found = @{}
    $dir = Join-Path $Game 'content\Message'
    if (Test-Path -LiteralPath $dir) {
        foreach ($file in Get-ChildItem -LiteralPath $dir -Filter 'CommonMsg_*.szs') {
            if ($file.Name -match '^CommonMsg_[A-Z]{2}([a-z]{2})\.szs$' -and $LanguageTable.ContainsKey($Matches[1])) {
                $entry = $LanguageTable[$Matches[1]]
                $found[$entry[0]] = [pscustomobject]@{ Tag = $Matches[1]; Code = $entry[0]; Name = $entry[1] }
            }
        }
    }
    return @($found.Keys | Sort-Object | ForEach-Object { $found[$_] })
}
$Languages = Get-InstalledLanguages
# Saved choice, else the Windows display language, else English, else the first one installed.
function Get-StartLanguage {
    if ($Languages.Count -eq 0) { return $null }
    $saved = Get-Setting 'language'
    foreach ($l in $Languages) { if ("$($l.Code)" -eq $saved) { return $l } }
    $culture = [System.Globalization.CultureInfo]::CurrentUICulture.TwoLetterISOLanguageName
    foreach ($l in $Languages) { if ($l.Tag -eq $culture) { return $l } }
    foreach ($l in $Languages) { if ($l.Tag -eq 'en') { return $l } }
    return $Languages[0]
}
$CurrentLanguage = Get-StartLanguage
# The sign-in screen itself is in English or French (French when that language is chosen).
$French = @{
    'PLAY' = 'JOUER'
    'Play offline ' = 'Jouer hors ligne '
    'Up to 10 characters. It stays linked to this PC. No password needed.' = '10 caract' + [char]0xE8 + 'res maximum. Il reste li' + [char]0xE9 + ' ' + [char]0xE0 + ' ce PC. Aucun mot de passe.'
    '2 to 10 characters: letters, digits, space, _ - .' = '2 ' + [char]0xE0 + ' 10 caract' + [char]0xE8 + 'res : lettres, chiffres, espace, _ - .'
    'The server cannot be reached. Check your Internet connection.' = 'Serveur injoignable. V' + [char]0xE9 + 'rifiez votre connexion Internet.'
    'Too many sign-ups from your connection. Try again later.' = 'Trop d''inscriptions depuis votre connexion. R' + [char]0xE9 + 'essayez plus tard.'
    'This server needs version {0} or newer. Update the game, then try again.' = 'Ce serveur demande la version {0} ou plus r' + [char]0xE9 + 'cente. Mettez le jeu ' + [char]0xE0 + ' jour, puis r' + [char]0xE9 + 'essayez.'
    'The server file has an invalid value ({0}). Fix or delete scripts\server.txt.' = 'Le fichier du serveur a une valeur invalide ({0}). Corrigez ou supprimez scripts\server.txt.'
    'You are about to sign up on another server ({0}). Your player number and its secret will be sent to it. Only continue if you trust its owner.`n`nContinue?' = 'Vous allez vous inscrire sur un autre serveur ({0}). Votre num' + [char]0xE9 + 'ro de joueur et son secret lui seront envoy' + [char]0xE9 + 's. Continuez seulement si vous faites confiance ' + [char]0xE0 + ' son propri' + [char]0xE9 + 'taire.`n`nContinuer ?'
    'Server: {0}' = 'Serveur : {0}'
    'This pseudo is already taken by another player. Choose a different one.' = 'Ce pseudo est d' + [char]0xE9 + 'j' + [char]0xE0 + ' pris par un autre joueur. Choisissez-en un autre.'
    'This account is banned from the server.' ='Ce compte est banni du serveur.'
    'This account exists with another password. Keep your old online_identity.ini.' = 'Ce compte existe avec un autre mot de passe. Gardez votre ancien online_identity.ini.'
}
function T([string]$Text) {
    if ($CurrentLanguage -and $CurrentLanguage.Tag -eq 'fr' -and $French.ContainsKey($Text)) { return $French[$Text] }
    return $Text
}
# Letters, digits, space, _ - . : what a Mii name takes without surprises.
function Test-Pseudo([string]$Name) { return $Name.Length -ge 2 -and $Name.Length -le 10 -and $Name -match '^[A-Za-z0-9 _.\-]+$' }

# A plain system dialog: system colours and font, left-aligned, nothing decorative.
$font = New-Object System.Drawing.Font('Segoe UI', 9)
$form = New-Object System.Windows.Forms.Form
$form.Text = 'Splatoon'
$form.Font = $font
$form.ClientSize = New-Object System.Drawing.Size(340, 244)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.MinimizeBox = $false

$label = New-Object System.Windows.Forms.Label
$label.Text = 'Pseudo'
$label.Location = New-Object System.Drawing.Point(20, 18)
$label.AutoSize = $true
$form.Controls.Add($label)

$box = New-Object System.Windows.Forms.TextBox
$box.Location = New-Object System.Drawing.Point(20, 38)
$box.Size = New-Object System.Drawing.Size(300, 23)
$box.MaxLength = 10
$box.Text = Get-SavedPseudo
$form.Controls.Add($box)

$hint = New-Object System.Windows.Forms.Label
$hint.Text = 'Up to 10 characters. It stays linked to this PC. No password needed.'
$hint.Location = New-Object System.Drawing.Point(20, 66)
$hint.Size = New-Object System.Drawing.Size(300, 32)
$hint.ForeColor = [System.Drawing.SystemColors]::GrayText
$form.Controls.Add($hint)

$problem = New-Object System.Windows.Forms.Label
$problem.Location = New-Object System.Drawing.Point(20, 100)
$problem.Size = New-Object System.Drawing.Size(300, 30)
$problem.ForeColor = [System.Drawing.Color]::Firebrick
$form.Controls.Add($problem)

$play = New-Object System.Windows.Forms.Button
$play.Text = 'PLAY'
$play.Location = New-Object System.Drawing.Point(20, 140)
$play.Size = New-Object System.Drawing.Size(100, 32)
$form.Controls.Add($play)

$offline = New-Object System.Windows.Forms.LinkLabel
$offline.Text = 'Play offline ' + [char]0x2192
$offline.Location = New-Object System.Drawing.Point(136, 148)
$offline.AutoSize = $true
$form.Controls.Add($offline)

# Small language list, bottom right; hidden when the game holds a single language only.
$languageBox = New-Object System.Windows.Forms.ComboBox
$languageBox.DropDownStyle = 'DropDownList'
$languageBox.Location = New-Object System.Drawing.Point(224, 190)
$languageBox.Size = New-Object System.Drawing.Size(96, 23)
foreach ($l in $Languages) { [void]$languageBox.Items.Add($l.Name) }
if ($CurrentLanguage) { $languageBox.SelectedIndex = [array]::IndexOf(@($Languages | ForEach-Object { $_.Code }), $CurrentLanguage.Code) }
$languageBox.Visible = $Languages.Count -gt 1
$form.Controls.Add($languageBox)
function Set-Texts {
    $hint.Text = T 'Up to 10 characters. It stays linked to this PC. No password needed.'
    $play.Text = T 'PLAY'
    $offline.Text = (T 'Play offline ') + [char]0x2192
    if ($problem.Text) { $problem.Text = '' }
    if (Get-Command Set-ServerLabel -ErrorAction SilentlyContinue) { Set-ServerLabel }
}
$languageBox.Add_SelectedIndexChanged({
    $script:CurrentLanguage = $Languages[$languageBox.SelectedIndex]
    Save-Setting 'language' "$($script:CurrentLanguage.Code)"
    Set-Texts
})
Set-Texts

# Dark theme: a neutral charcoal, soft white text, one blue for the link. The title bar goes dark
# too where Windows allows it (build 18985 and later; ignored otherwise).
$dark = @{
    Back   = [System.Drawing.Color]::FromArgb(30, 31, 34)
    Input  = [System.Drawing.Color]::FromArgb(43, 45, 49)
    Edge   = [System.Drawing.Color]::FromArgb(74, 77, 84)
    Text   = [System.Drawing.Color]::FromArgb(230, 232, 235)
    Muted  = [System.Drawing.Color]::FromArgb(150, 155, 163)
    Link   = [System.Drawing.Color]::FromArgb(122, 167, 255)
    Error  = [System.Drawing.Color]::FromArgb(255, 123, 114)
    Button = [System.Drawing.Color]::FromArgb(58, 61, 68)
    Hover  = [System.Drawing.Color]::FromArgb(74, 78, 87)
    Down   = [System.Drawing.Color]::FromArgb(48, 51, 57)
}
$form.BackColor = $dark.Back; $form.ForeColor = $dark.Text
$label.ForeColor = $dark.Text
$hint.ForeColor = $dark.Muted
$problem.ForeColor = $dark.Error
$box.BackColor = $dark.Input; $box.ForeColor = $dark.Text; $box.BorderStyle = 'FixedSingle'
foreach ($control in @($play, $languageBox)) {
    $control.FlatStyle = 'Flat'; $control.BackColor = $dark.Button; $control.ForeColor = $dark.Text
}
$play.FlatAppearance.BorderColor = $dark.Edge
$play.FlatAppearance.MouseOverBackColor = $dark.Hover
$play.FlatAppearance.MouseDownBackColor = $dark.Down
$offline.LinkColor = $dark.Link; $offline.ActiveLinkColor = $dark.Text; $offline.VisitedLinkColor = $dark.Link
try {
    Add-Type -Namespace Native -Name Dwm -MemberDefinition '[System.Runtime.InteropServices.DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(System.IntPtr window, int attribute, ref int value, int size);'
    $form.Add_HandleCreated({ $on = 1; [void][Native.Dwm]::DwmSetWindowAttribute($form.Handle, 20, [ref]$on, 4) })
} catch { }

$form.AcceptButton = $play
$script:choice = ''

# scripts\server.txt (local, never committed) wins over scripts\server.public.txt (the public server).
function Test-IPv4([string]$Text) {
    if ($Text -notmatch '^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$') { return $false }
    foreach ($i in 1..4) { if ([int]$Matches[$i] -gt 255) { return $false } }
    return $true
}
# Values go into command lines and DNS overrides of the game: only plain, strictly valid ones pass
# (a bad file is refused, not half-used). '_custom' is true when scripts\server.txt replaces the public file.
function Read-ServerFile {
    $cfg = @{}
    $custom = $false
    foreach ($name in 'server.txt', 'server.public.txt') {
        $file = Join-Path $PSScriptRoot $name
        if (-not (Test-Path -LiteralPath $file)) { continue }
        foreach ($line in Get-Content -LiteralPath $file) { if ($line -match '^\s*(\w+)\s*=\s*(\S+)\s*$') { $cfg[$Matches[1].ToLower()] = $Matches[2] } }
        $custom = ($name -eq 'server.txt')
        break
    }
    $bad = @()
    if ($cfg['server'] -and $cfg['server'] -notmatch '^[A-Za-z0-9.\-]{1,253}:\d{1,5}$') { $bad += 'server' }
    foreach ($key in 'nncs1', 'nncs2') { if ($cfg[$key] -and -not (Test-IPv4 $cfg[$key])) { $bad += $key } }
    if ($cfg['register'] -and $cfg['register'] -notmatch '^https://[A-Za-z0-9.\-]+(:\d{1,5})?(/\S*)?$' -and $cfg['register'] -notmatch '^http://(127\.0\.0\.1|localhost)(:\d{1,5})?(/\S*)?$') { $bad += 'register' }
    $cfg['_custom'] = $custom
    $cfg['_bad'] = $bad
    return $cfg
}
# The player number and its secret go to whatever register= says: ask before sending them to a
# server other than the public one shipped with the game (once: the marker remembers it).
function Confirm-ForeignRegister($cfg) {
    if (-not $cfg['register']) { return $true }
    $marker = Join-Path $Game 'online_registered.txt'
    if ((Test-Path -LiteralPath $marker) -and ((Get-Content -LiteralPath $marker -Raw).Trim() -eq $cfg['register'])) { return $true }
    $official = ''
    $publicFile = Join-Path $PSScriptRoot 'server.public.txt'
    if (Test-Path -LiteralPath $publicFile) {
        foreach ($line in Get-Content -LiteralPath $publicFile) { if ($line -match '^\s*register\s*=\s*(\S+)\s*$') { $official = $Matches[1] } }
    }
    if ($official -and $cfg['register'] -eq $official) { return $true }
    $answer = [System.Windows.Forms.MessageBox]::Show(
        ((T 'You are about to sign up on another server ({0}). Your player number and its secret will be sent to it. Only continue if you trust its owner.`n`nContinue?') -f ([Uri]$cfg['register']).Authority),
        'Splatoon', 'YesNo', 'Warning')
    return $answer -eq 'Yes'
}
# Which server PLAY will use, shown under the button (amber when scripts\server.txt replaces the public one).
$serverLabel = New-Object System.Windows.Forms.Label
$serverLabel.Location = New-Object System.Drawing.Point(20, 194)
$serverLabel.Size = New-Object System.Drawing.Size(196, 18)
$serverLabel.AutoEllipsis = $true
$form.Controls.Add($serverLabel)
function Set-ServerLabel {
    $cfg = Read-ServerFile
    $name = 'local'
    if ($cfg['server']) { $name = ($cfg['server'] -split ':')[0] }
    $serverLabel.Text = (T 'Server: {0}') -f $name
    $serverLabel.ForeColor = if ($cfg['_custom']) { [System.Drawing.Color]::FromArgb(255, 200, 87) } else { $dark.Muted }
}
Set-ServerLabel
# "0.2" and "0.2.0" are the same version (System.Version would call the first one older).
function ConvertTo-GameVersion([string]$Text) {
    $parsed = $null
    if (-not ($Text.Trim().TrimStart('v') -match '^\d+\.\d+(\.\d+)?$') -or -not [version]::TryParse($Matches[0], [ref]$parsed)) { return $null }
    return [version]::new($parsed.Major, $parsed.Minor, [Math]::Max($parsed.Build, 0))
}
# The server publishes the oldest game version it accepts in /health (min_client_version). Returns ''
# when this game is recent enough or the server cannot be asked (that case shows up at sign-up).
function Test-ServerVersion($cfg) {
    $uri = $null
    if (-not $cfg['register'] -or -not [Uri]::TryCreate($cfg['register'], 'Absolute', [ref]$uri)) { return '' }
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $reply = Invoke-RestMethod -Uri "$($uri.Scheme)://$($uri.Authority)/health" -TimeoutSec 4
        $minimum = ConvertTo-GameVersion ([string]$reply.min_client_version)
        if (-not $minimum) { return '' }
        $installed = $null
        foreach ($file in @((Join-Path $Root 'build\installed-version.txt'), (Join-Path $Root 'VERSION'))) {
            if (Test-Path -LiteralPath $file) { $installed = ConvertTo-GameVersion (Get-Content -LiteralPath $file -Raw); if ($installed) { break } }
        }
        if ($installed -and $installed -lt $minimum) { return (T 'This server needs version {0} or newer. Update the game, then try again.') -f $minimum.ToString(3) }
    } catch { }
    return ''
}
# Friends are found by pseudo, so a pseudo belongs to one account: claim it on the server. Only "taken"
# stops the player; any other problem (older server, no Internet) lets the game start without friends.
function Claim-Pseudo($cfg, [string]$Name) {
    if (-not $cfg['register']) { return '' }
    $identity = Join-Path $Game 'online_identity.ini'
    if (-not (Test-Path -LiteralPath $identity)) { return '' }
    $fields = @{}
    foreach ($line in Get-Content -LiteralPath $identity) { if ($line -match '^\s*(pid|password)\s*=\s*(\S+)\s*$') { $fields[$Matches[1]] = $Matches[2] } }
    if (-not $fields['pid'] -or -not $fields['password']) { return '' }
    $uri = [Uri]$cfg['register']
    $body = @{ pid = '0x' + $fields['pid']; password = $fields['password']; pseudo = $Name } | ConvertTo-Json -Compress
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-RestMethod -Method Post -Uri "$($uri.Scheme)://$($uri.Authority)/social/pseudo" -Body $body -ContentType 'application/json' -TimeoutSec 8 | Out-Null
    } catch {
        $status = 0
        if ($_.Exception.Response) { $status = [int]$_.Exception.Response.StatusCode }
        if ($status -eq 409) { return 'This pseudo is already taken by another player. Choose a different one.' }
    }
    return ''
}
# First PLAY on a server with register=: creates this PC's identity (the format of
# runtime/online_identity.h: pid 0x10000000-0x7FFFFFFE, 16 letters or digits) when the game has
# not made one yet, and registers it once over HTTPS. Returns '' or a message for the player.
function Register-Online($cfg) {
    $url = $cfg['register']
    if (-not $url) { return '' }
    $uri = $null
    if (-not [Uri]::TryCreate($url, 'Absolute', [ref]$uri)) { return 'Bad register= address in server file.' }
    if ($uri.Scheme -ne 'https' -and -not $uri.IsLoopback) { return 'The server sign-up must use https.' }
    $identity = Join-Path $Game 'online_identity.ini'
    $marker = Join-Path $Game 'online_registered.txt'
    if ((Test-Path -LiteralPath $marker) -and (Test-Path -LiteralPath $identity) -and
        ((Get-Content -LiteralPath $marker -Raw).Trim() -eq $url)) { return '' }
    if (-not (Test-Path -LiteralPath $identity)) {
        $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
        $bytes = New-Object byte[] 20; $rng.GetBytes($bytes)
        $principal = [uint32](0x10000000 + [uint64]([BitConverter]::ToUInt32($bytes, 0)) % 0x6FFFFFFF)
        $alphabet = '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'
        $password = -join (4..19 | ForEach-Object { $alphabet[$bytes[$_] % 62] })
        $text = "# Splatoon PC online identity. Keep this file: it identifies you to friends and rooms.`npid=$('{0:x8}' -f $principal)`npassword=$password`n"
        # Written aside then moved: two launches at once cannot leave two different identities.
        $temporary = "$identity.$PID.tmp"
        [System.IO.File]::WriteAllText($temporary, $text, (New-Object System.Text.UTF8Encoding($false)))
        try { [System.IO.File]::Move($temporary, $identity) } catch { Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue }
    }
    $fields = @{}
    foreach ($line in Get-Content -LiteralPath $identity) { if ($line -match '^\s*(pid|password)\s*=\s*(\S+)\s*$') { $fields[$Matches[1]] = $Matches[2] } }
    if (-not $fields['pid'] -or -not $fields['password']) { return 'Game\online_identity.ini is damaged.' }
    $body = @{ pid = '0x' + $fields['pid']; password = $fields['password'] } | ConvertTo-Json -Compress
    try {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-RestMethod -Method Post -Uri $uri -Body $body -ContentType 'application/json' -TimeoutSec 10 | Out-Null
    } catch {
        $status = 0
        if ($_.Exception.Response) { $status = [int]$_.Exception.Response.StatusCode }
        switch ($status) {
            409 { return 'This account exists with another password. Keep your old online_identity.ini.' }
            429 { return 'Too many sign-ups from your connection. Try again later.' }
            403 { return 'This account is banned from the server.' }
            0   { return 'The server cannot be reached. Check your Internet connection.' }
            default { return "Sign-up refused by the server ($status)." }
        }
    }
    Set-Content -LiteralPath $marker -Value $url -Encoding ASCII
    return ''
}
function Confirm-Pseudo {
    $name = $box.Text.Trim()
    if (-not (Test-Pseudo $name)) {
        $problem.Text = T '2 to 10 characters: letters, digits, space, _ - .'
        $box.Focus() | Out-Null
        return $false
    }
    Save-Pseudo $name
    if ($CurrentLanguage) { Save-Setting 'language' "$($CurrentLanguage.Code)" }   # the game reads it at start-up
    return $true
}
$play.Add_Click({
    if (-not (Confirm-Pseudo)) { return }
    $form.UseWaitCursor = $true
    $cfg = Read-ServerFile
    if ($cfg['_bad'].Count) {
        $form.UseWaitCursor = $false
        $problem.Text = (T 'The server file has an invalid value ({0}). Fix or delete scripts\server.txt.') -f ($cfg['_bad'] -join ', ')
        return
    }
    if (-not (Confirm-ForeignRegister $cfg)) { $form.UseWaitCursor = $false; return }
    $message = Test-ServerVersion $cfg
    if (-not $message) { $message = Register-Online $cfg }
    if (-not $message) { $message = Claim-Pseudo $cfg $box.Text.Trim() }
    $form.UseWaitCursor = $false
    if ($message) { $problem.Text = T $message; return }
    $script:choice = 'online'; $form.Close()
})
$offline.Add_LinkClicked({ if (Confirm-Pseudo) { $script:choice = 'offline'; $form.Close() } })
$box.Add_TextChanged({ $problem.Text = '' })

if ($Shot) {
    $form.Show(); $form.Refresh()
    $bitmap = New-Object System.Drawing.Bitmap($form.ClientSize.Width, $form.ClientSize.Height)
    $form.DrawToBitmap($bitmap, (New-Object System.Drawing.Rectangle(0, 0, $form.ClientSize.Width, $form.ClientSize.Height)))
    $bitmap.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png)
    $form.Close(); exit 0
}

[void]$form.ShowDialog()
if (-not $script:choice) { exit 0 }
if (-not (Test-Path -LiteralPath $Executable)) { [System.Windows.Forms.MessageBox]::Show('Game\Splatoon-v288.exe has not been built yet.', 'Splatoon'); exit 1 }
$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
if ($script:choice -eq 'online') {
    $cfg = Read-ServerFile
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'Launch-Online.ps1'))
    if ($cfg['register'] -match '^(https://[A-Za-z0-9.\-]+(:\d{1,5})?)/') { $arguments += @('-SocialUrl', $Matches[1]) }
    if ($cfg['server']) { $arguments += @('-NexServer', $cfg['server'], '-Force'); if ($cfg['nncs1']) { $arguments += @('-Nncs1', $cfg['nncs1']) }; if ($cfg['nncs2']) { $arguments += @('-Nncs2', $cfg['nncs2']) } }
    Start-Process -FilePath $powershell -ArgumentList $arguments -WorkingDirectory $Game -WindowStyle Hidden
} elseif ($script:choice -eq 'offline') {
    $env:SDL_DIRECTINPUT_ENABLED = '0'
    Start-Process -FilePath $Executable -WorkingDirectory $Game
}
