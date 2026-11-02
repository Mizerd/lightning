#pragma once

// Narrow, non-GUI provider diagnostics used by clean-package validation.
//
// Neither entry point ever prints a key or an authenticated URL; they report
// only booleans and bounded request outcomes. They exist so an installed
// official package can prove — with every provider-key environment variable
// unset — that the embedded release keys are present and actually work.
namespace gif {

int printProviderStatus();

int runProviderSelfTest();

}
