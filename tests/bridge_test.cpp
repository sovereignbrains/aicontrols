// The bridge's refusals: which CONNECT targets lead into a network rather than out. Exit code 0: all passed.
#include <iostream>

#include "core/bridge.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

}  // namespace

int main() {
    using aic::LocalTarget;
    Expect(!LocalTarget("api.anthropic.com") && !LocalTarget("claude.ai") && !LocalTarget("API.Anthropic.COM.") &&
               !LocalTarget("api.x.ai") && !LocalTarget("storage.googleapis.com"),
           "the CLIs' hosts go out");
    Expect(!LocalTarget("1.1.1.1") && !LocalTarget("160.79.104.10") && !LocalTarget("2606:4700::6810:84e5") &&
               !LocalTarget("172.32.0.1") && !LocalTarget("100.128.0.1") && !LocalTarget("198.18.0.1"),
           "public addresses go out (198.18/15 is a fake-ip pool's)");

    Expect(LocalTarget("127.0.0.1") && LocalTarget("127.8.9.10") && LocalTarget("0.0.0.0") && LocalTarget("10.0.0.1") &&
               LocalTarget("172.16.0.1") && LocalTarget("172.31.255.255") && LocalTarget("192.168.31.1") &&
               LocalTarget("169.254.1.1") && LocalTarget("100.64.0.1") && LocalTarget("224.0.0.251") &&
               LocalTarget("255.255.255.255"),
           "IPv4: loopback, private, link-local, shared, multicast");
    Expect(LocalTarget("::1") && LocalTarget("::") && LocalTarget("fe80::1") && LocalTarget("fe80::1%12") &&
               LocalTarget("fd12:3456::1") && LocalTarget("FC00::1") && LocalTarget("fec0::1") && LocalTarget("ff02::fb"),
           "IPv6: loopback, unspecified, link-local (with a zone), unique local, site-local, multicast");
    Expect(LocalTarget("::ffff:127.0.0.1") && LocalTarget("::ffff:192.168.0.1") && LocalTarget("::127.0.0.1") &&
               LocalTarget("64:ff9b::10.0.0.1") && !LocalTarget("::ffff:1.1.1.1") && !LocalTarget("64:ff9b::1.1.1.1"),
           "IPv4 inside IPv6");
    Expect(LocalTarget("127.1") && LocalTarget("0x7f.1") && LocalTarget("0x7f000001") && LocalTarget("2130706433") &&
               LocalTarget("010.0.0.1") && LocalTarget("012.0.0.1") && LocalTarget("1.2.3") && LocalTarget("1.0x2"),
           "loose forms of an address, whatever they would be");
    Expect(LocalTarget("localhost") && LocalTarget("LocalHost.") && LocalTarget("router") && LocalTarget("a.localhost") &&
               LocalTarget("nas.local") && LocalTarget("printer.lan") && LocalTarget("box.home") &&
               LocalTarget("box.home.arpa") && LocalTarget("svc.internal") && LocalTarget("wiki.intranet") &&
               LocalTarget("") && LocalTarget("."),
           "local names, one label, nothing");
    Expect(!LocalTarget("localhost.com") && !LocalTarget("local.example.org") && !LocalTarget("lan.ru"),
           "a local word inside a public name");

    if (failures == 0) {
        std::cout << "ok\n";
    }
    return failures == 0 ? 0 : 1;
}
