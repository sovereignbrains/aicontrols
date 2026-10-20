# Probe: claude.exe over stream-json with an in-process ("sdk") MCP server and stdio permission prompts.
$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot 'probe.log'
Set-Content $log '' -Encoding utf8
function Log($s) { Add-Content $log $s -Encoding utf8 }

$mcp = '{\"mcpServers\":{\"probe\":{\"type\":\"sdk\",\"name\":\"probe\"}}}'
$psi = New-Object Diagnostics.ProcessStartInfo "$env:USERPROFILE\.local\bin\claude.exe"
$psi.Arguments = '--output-format stream-json --verbose --input-format stream-json --permission-prompt-tool stdio ' +
                 "--mcp-config `"$mcp`" --tools `"`" --model claude-haiku-4-5 --setting-sources `"`""
$psi.RedirectStandardInput = $true; $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
$psi.StandardOutputEncoding = New-Object Text.UTF8Encoding $false
$psi.UseShellExecute = $false
$psi.WorkingDirectory = $PSScriptRoot
$psi.Environment['HTTPS_PROXY'] = 'http://127.0.0.1:2092'
$psi.Environment['NO_PROXY'] = '127.0.0.1,localhost'
$psi.Environment['CLAUDE_CODE_ENTRYPOINT'] = 'sdk-py'
$p = [Diagnostics.Process]::Start($psi)
$errTask = $p.StandardError.ReadToEndAsync()
$in = New-Object IO.StreamWriter $p.StandardInput.BaseStream, (New-Object Text.UTF8Encoding $false)
function Send($obj) { $j = $obj | ConvertTo-Json -Compress -Depth 30; Log ">> $j"; $in.WriteLine($j); $in.Flush() }

Send @{ type = 'control_request'; request_id = 'req_init'; request = @{ subtype = 'initialize'; hooks = $null } }

$calls = 0
while ($null -ne ($line = $p.StandardOutput.ReadLine())) {
    $m = $line | ConvertFrom-Json
    $short = if ($line.Length -gt 600) { $line.Substring(0, 600) + '…' } else { $line }
    Log "<< $short"
    if ($m.type -eq 'control_response' -and $m.response.request_id -eq 'req_init') {
        Send @{ type = 'user'; session_id = 'default'; parent_tool_use_id = $null
                message = @{ role = 'user'; content = 'Call the tool mcp__probe__echo with text "one", then call it again with text "two". Then reply with one line: what each call returned.' } }
    }
    elseif ($m.type -eq 'control_request') {
        $r = $m.request; $resp = $null
        if ($r.subtype -eq 'mcp_message') {
            $msg = $r.message
            $res = switch ($msg.method) {
                'initialize' { @{ protocolVersion = $msg.params.protocolVersion; capabilities = @{ tools = @{} }; serverInfo = @{ name = 'probe'; version = '0.0.1' } } }
                'tools/list' { @{ tools = @(@{ name = 'echo'; description = 'Echo the text back.'
                                               inputSchema = @{ type = 'object'; properties = @{ text = @{ type = 'string' } }; required = @('text') } }) } }
                'tools/call' { @{ content = @(@{ type = 'text'; text = "echo says: $($msg.params.arguments.text)" }) } }
                default { @{} }
            }
            $resp = @{ mcp_response = @{ jsonrpc = '2.0'; id = $msg.id; result = $res } }
        }
        elseif ($r.subtype -eq 'can_use_tool') {
            $calls++
            $resp = if ($calls -eq 1) { @{ behavior = 'allow'; updatedInput = $r.input } }
                    else { @{ behavior = 'deny'; message = 'The user declined this call.' } }
        }
        else { $resp = @{} }
        Send @{ type = 'control_response'; response = @{ subtype = 'success'; request_id = $m.request_id; response = $resp } }
    }
    elseif ($m.type -eq 'result') { $in.Close() }
}
$p.WaitForExit()
Log "exit $($p.ExitCode)"
Log "stderr: $($errTask.Result)"
