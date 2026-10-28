#pragma once

#include <string>

namespace aic {

// The way out for the CLIs' own traffic (Claude's API, its login and updates, Grok's): an HTTP CONNECT proxy on
// the loopback that picks the way for every new connection, so a chat outlives the VPN going off under it
// (a CLI keeps the proxy it was started with, so the choice cannot be made once, at the start):
//   1. direct, when api.anthropic.com's edge does not see a blocked country (Sovereign's tunnel is up, or abroad);
//   2. sing-box's mixed inbound on 127.0.0.1:2080, when something listens there;
//   3. an SSH tunnel (ssh.exe -D) to a server whose password the user stored as `ssh user@host`: the key first,
//      then that password through SSH_ASKPASS, as a tool's `on` logs in. Only a host key ssh already knows.
// A way that fails to open falls through to the next. Listens on the first call; its URL for HTTPS_PROXY,
// empty when it could not.
std::wstring BridgeUrl();
// For the probe: the bridge as it is with the VPN off, without the direct way (sing-box's port and the tunnel).
void BridgeWithoutDirect();

}  // namespace aic
