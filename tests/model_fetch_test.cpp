/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

// ensureModel against a local fixture package served over file://, so the PR test needs no
// network. Usage: model_fetch_test <fixture.tar.gz> <its sha256> <empty work dir>

#include <cstdlib>
#include <filesystem>  // NOLINT(build/c++17)
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "model_fetch.hpp"

namespace {

namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "ASSERTION FAILED: " << message << std::endl;
        std::exit(1);
    }
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Only the requested model directories may remain: every temporary directory is cleaned up.
void requireOnlyEntries(const fs::path& work, size_t expected) {
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(work)) {
        require(entry.path().filename().string().rfind(".fixture-v1.download-", 0) != 0,
                "temporary directory left behind: " + entry.path().string());
        ++count;
    }
    require(count == expected, "unexpected entries in the work directory");
}

kws::ModelRelease fixtureRelease(const std::string& archive, const std::string& sha256) {
    kws::ModelRelease release;
    release.name = "fixture-v1";
    release.url = "file://" + archive;
    release.sha256 = sha256;
    release.required_files = {"cfsmn.bin", "beam_w.bin", "keywords.txt"};
    return release;
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 4, "usage: model_fetch_test <fixture.tar.gz> <sha256> <work dir>");
    const std::string archive = fs::absolute(argv[1]).string();
    const std::string sha256 = argv[2];
    const fs::path work = fs::absolute(argv[3]);
    std::string error;

    // A missing model is downloaded, verified and installed, extra package files included.
    const fs::path installed = work / "installed";
    require(kws::ensureModel(fixtureRelease(archive, sha256), installed.string(), &error),
            "fixture install must succeed: " + error);
    require(readFile(installed / "cfsmn.bin") == "weights\n", "cfsmn.bin content");
    require(readFile(installed / "keywords.txt") == "keywords\n", "keywords.txt content");
    require(fs::is_regular_file(installed / "NOTICE"), "non-required package files are installed too");
    requireOnlyEntries(work, 1);

    // A complete directory is left alone: an unreachable URL is never contacted.
    auto unreachable = fixtureRelease("/nonexistent/fixture.tar.gz", sha256);
    error.clear();
    require(kws::ensureModel(unreachable, installed.string(), &error), "complete model must not download");
    require(error.empty(), "no error for a complete model");

    // A partial directory gets the whole package again.
    fs::remove(installed / "cfsmn.bin");
    require(kws::ensureModel(fixtureRelease(archive, sha256), installed.string(), &error),
            "partial model must be repaired: " + error);
    require(fs::is_regular_file(installed / "cfsmn.bin"), "cfsmn.bin restored");

    // A tampered or wrong archive is rejected before anything is unpacked.
    std::string wrong = sha256;
    wrong[0] = wrong[0] == '0' ? '1' : '0';
    const fs::path rejected = work / "rejected";
    error.clear();
    require(!kws::ensureModel(fixtureRelease(archive, wrong), rejected.string(), &error),
            "SHA256 mismatch must fail");
    require(error.find("SHA256 mismatch") != std::string::npos, "mismatch is reported: " + error);
    require(!fs::exists(rejected / "cfsmn.bin"), "nothing installed from a rejected archive");

    // Download failures are reported, not thrown.
    error.clear();
    require(!kws::ensureModel(unreachable, (work / "unreachable").string(), &error),
            "a missing archive must fail");
    require(error.find("download failed") != std::string::npos, "download failure is reported: " + error);

    // A package that lacks a required file is rejected as a whole.
    auto incomplete = fixtureRelease(archive, sha256);
    incomplete.required_files.push_back("missing.bin");
    error.clear();
    require(!kws::ensureModel(incomplete, (work / "incomplete").string(), &error),
            "a package without a required file must fail");
    require(error.find("missing.bin") != std::string::npos, "the missing file is named: " + error);
    require(!fs::exists(work / "incomplete" / "cfsmn.bin"), "nothing installed from an incomplete package");

    // Invalid releases are refused outright.
    error.clear();
    require(!kws::ensureModel(fixtureRelease(archive, "abc"), (work / "invalid").string(), &error),
            "a malformed SHA256 must be refused");

    // Failed installs create no model directory at all.
    require(!fs::exists(rejected) && !fs::exists(work / "unreachable") && !fs::exists(work / "incomplete"),
            "a failed install must not create the model directory");
    requireOnlyEntries(work, 1);

    std::cout << "PASS --model-fetch" << std::endl;
    return 0;
}
