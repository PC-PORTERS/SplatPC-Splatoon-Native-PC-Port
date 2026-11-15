// integrity_proof.h - the proof sent with each sign-in: this install's version and the hash of its code.
// Same result as scripts/integrity.py attestation() for the same folder (tools/tests/test_online_native.py).
#pragma once
#include <filesystem>
#include <string>

namespace rbrew_native {

struct Attestation {
    std::string version; // VERSION, without a leading "v"; empty when unreadable
    std::string code;    // sha256 of the manifest text this PC's files give; empty without a manifest
};

std::string read_version(const std::filesystem::path& root);
Attestation attest(const std::filesystem::path& root);

} // namespace rbrew_native
