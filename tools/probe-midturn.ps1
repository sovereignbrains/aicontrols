# Probe: a user message sent while a turn is still running (here: while a tool call waits for us).
# Prints the order of events: how many `result`s come and when the second message is answered.
$ErrorActionPreference = 'Stop'
$log = Join-Path $env:TEMP 'aic-midturn.log'
Set-Content $log '' -Encoding utf8
function Log($s) { Add-Content $log $s -Encoding utf8 }

$mcp = '{\"mcpServers\":{\"probe\":{\"type\":\"sdk\",\"name\":\"probe\"}}}'
$psi = New-Object Diagnostics.ProcessStartInfo "$env:USERPROFILE\.local\bin\claude.exe"
$psi.Arguments = '--output-format stream-json --verbose --input-format stream-json --permission-prompt-tool stdio ' +
                 "--mcp-config `"$mcp`" --tools `"`" --model claude-haiku-4-5 --setting-sources `"`""
$psi.RedirectStandardInput = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
$psi.StandardOutputEncoding = New-Object Text.UTF8Encoding $false
$psi.UseShellExecute = $false
$psi.WorkingDirectory = $env:TEMP
$psi.Environment['HTTPS_PROXY'] = 'http://127.0.0.1:2092'
$psi.Environment['NO_PROXY'] = '127.0.0.1,localhost'
$psi.Environment['CLAUDE_CODE_SDK_READS_SESSION_STATE'] = '1'
$p = [Diagnostics.Process]::Start($psi)
$null = $p.StandardError.ReadToEndAsync()
$in = New-Object IO.StreamWriter $p.StandardInput.BaseStream, (New-Object Text.UTF8Encoding $false)
function Send($obj) { $j = $obj | ConvertTo-Json -Compress -Depth 30; $in.WriteLine($j); $in.Flush() }
function User($text) { Send @{ type = 'user'; session_id = 'default'; parent_tool_use_id = $null; message = @{ role = 'user'; content = $text } } }

Send @{ type = 'control_request'; request_id = 'req_init'; request = @{ subtype = 'initialize'; hooks = $null } }
$results = 0; $sentSecond = $false; $seenSecond = $false; $t0 = Get-Date
while ($null -ne ($line = $p.StandardOutput.ReadLine())) {
    $m = $line | ConvertFrom-Json
    $at = [int]((Get-Date) - $t0).TotalMilliseconds
    if ($m.type -eq 'control_response' -and $m.response.request_id -eq 'req_init') {
        User 'Call the tool mcp__probe__echo with text "one". When it returns, reply with exactly: FIRST-DONE'
        Log "$at ms: sent message 1"
    }
    elseif ($m.type -eq 'control_request') {
        $r = $m.request; $resp = @{}
        if ($r.subtype -eq 'mcp_message') {
            $msg = $r.message
            $res = switch ($msg.method) {
                'initialize' { @{ protocolVersion = $msg.params.protocolVersion; capabilities = @{ tools = @{} }; serverInfo = @{ name = 'probe'; version = '0.0.1' } } }
                'tools/list' { @{ tools = @(@{ name = 'echo'; description = 'Echo the text back.'; inputSchema = @{ type = 'object'; properties = @{ text = @{ type = 'string' } }; required = @('text') } }) } }
                'tools/call' { @{ content = @(@{ type = 'text'; text = "echo says: $($msg.params.arguments.text)" }) } }
                default { @{} }
            }
            $resp = @{ mcp_response = @{ jsonrpc = '2.0'; id = $msg.id; result = $res } }
            if ($msg.method -eq 'tools/call') {
                if (-not $sentSecond) {
                    $sentSecond = $true
                    User 'Also, after that, reply with exactly: SECOND-SEEN'
                    Log "$at ms: sent message 2 (during the tool call)"
                }
            }
        }
        elseif ($r.subtype -eq 'can_use_tool') { $resp = @{ behavior = 'allow'; updatedInput = $r.input } }
        Send @{ type = 'control_response'; response = @{ subtype = 'success'; request_id = $m.request_id; response = $resp } }
    }
    elseif ($m.type -eq 'assistant') {
        foreach ($c in $m.message.content) { if ($c.type -eq 'text') { Log "$at ms: assistant text: $($c.text)"; if ($c.text -like '*SECOND-SEEN*') { $seenSecond = $true } } }
    }
    elseif ($m.type -eq 'user' -and $m.message.content -is [string]) { Log "$at ms: user echo: $($m.message.content)" }
    elseif ($m.type -eq 'result') {
        $results++
        Log "$at ms: RESULT #$results (stop_reason=$($m.stop_reason), turns=$($m.num_turns))"
        if ($results -ge 2 -or $seenSecond) { $in.Close() }
    }
    elseif ($m.type -eq 'system' -and $m.subtype -in 'session_state_changed', 'queued_command', 'status') { Log "$at ms: system/$($m.subtype) state=$($m.state) $($line.Substring(0, [Math]::Min(120, $line.Length)))" }
}
Log "ended: results=$results"
