#pragma once

// Narrow, non-GUI provider diagnostics used by clean-package validation.
//
// Neither entry point ever prints a key or an authenticated URL; they report
// only booleans and bounded request outcomes. They exist so an installed
// official package can prove — with every provider-key environment variable
// unset — that the embedded release keys are present and actually work.
namespace gif {

// Print "GIPHY configured: yes|no" and "KLIPY configured: yes|no" using the
// standard resolution (runtime override -> compiled key -> missing). No
// network. Returns 0 always (status report, not a gate).
int printProviderStatus();

// As above, plus a real bounded HTTPS trending request per configured provider
// to confirm the resolved key is accepted and returns structured results.
// Prints "<PROVIDER> request: ok (N results)" or a safe failure line. Returns
// 0 only when every provider is configured and its request succeeds; non-zero
// otherwise. Requires a running Qt event loop.
int runProviderSelfTest();

} // namespace gif
