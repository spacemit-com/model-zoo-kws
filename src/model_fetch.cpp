/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "model_fetch.hpp"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>  // NOLINT(build/c++17)
#include <string>
#include <system_error>
#include <vector>

extern char** environ;

namespace kws {

namespace {

namespace fs = std::filesystem;

const char kArchiveBase[] = "https://archive.spacemit.com/spacemit-ai/model_zoo/kws/";

// 直接 exec，不经过 shell：路径里有空格或引号也不会被解释。返回退出码，无法启动时返回 -1。
int runCommand(const std::vector<std::string>& args, std::string* out) {
    int fds[2] = {-1, -1};
    if (out && pipe(fds) != 0) return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (out) {
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, fds[0]);
        posix_spawn_file_actions_addclose(&actions, fds[1]);
    }
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (out) close(fds[1]);
    if (rc != 0) {
        if (out) close(fds[0]);
        return -1;
    }
    if (out) {
        char buf[256];
        for (;;) {
            const ssize_t n = read(fds[0], buf, sizeof(buf));
            if (n > 0) {
                out->append(buf, static_cast<size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                break;
            }
        }
        close(fds[0]);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool isSha256(const std::string& hex) {
    if (hex.size() != 64) return false;
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool sha256Of(const std::string& path, std::string* hex) {
    const std::vector<std::vector<std::string>> tools = {
        {"sha256sum", path},
        {"shasum", "-a", "256", path},
    };
    for (const auto& tool : tools) {
        std::string out;
        if (runCommand(tool, &out) != 0) continue;
        std::string digest = out.substr(0, out.find_first_of(" \t\n"));
        for (auto& c : digest) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (isSha256(digest)) {
            *hex = digest;
            return true;
        }
    }
    return false;
}

bool allPresent(const ModelRelease& release, const fs::path& dir) {
    std::error_code ec;
    for (const auto& file : release.required_files) {
        if (!fs::is_regular_file(dir / file, ec)) return false;
    }
    return true;
}

}  // namespace

ModelRelease defaultModelRelease() {
    ModelRelease release;
    release.name = "xiaojin-v1";
    release.url = std::string(kArchiveBase) + release.name + ".tar.gz";
    release.sha256 = "9e3a6f3d2142dcc2f65077539906a840c3e911215febdf631645ecb11e5db5d7";
    release.required_files = {"cfsmn.bin", "beam_w.bin", "keywords.txt"};
    return release;
}

bool modelDownloadEnabled() {
    const char* env = std::getenv("KWS_MODEL_DOWNLOAD");
    if (!env) return true;
    std::string value(env);
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return !(value == "0" || value == "off" || value == "false" || value == "no");
}

namespace {

bool ensureModelImpl(const ModelRelease& release, const std::string& model_dir, std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    const fs::path dir(model_dir);
    if (allPresent(release, dir)) return true;
    if (release.name.empty() || release.required_files.empty() || !isSha256(release.sha256)) {
        return fail("invalid model release");
    }

    std::error_code ec;
    const fs::path parent = dir.has_parent_path() ? dir.parent_path() : fs::path(".");
    fs::create_directories(parent, ec);
    // 临时目录与 model_dir 同在 parent 下，最后的 rename 不会跨文件系统。
    std::string pattern = (parent / ("." + release.name + ".download-XXXXXX")).string();
    std::vector<char> tmp_name(pattern.begin(), pattern.end());
    tmp_name.push_back('\0');
    if (!mkdtemp(tmp_name.data())) {
        return fail("cannot create a temporary directory in " + parent.string() + ": " + std::strerror(errno));
    }
    const fs::path tmp(tmp_name.data());
    struct RemoveOnExit {
        fs::path path;
        ~RemoveOnExit() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } cleanup{tmp};

    std::fprintf(stderr, "[KWS] model %s missing in %s, downloading %s\n",
            release.name.c_str(), model_dir.c_str(), release.url.c_str());
    const fs::path archive = tmp / (release.name + ".tar.gz");
    const std::vector<std::string> curl = {
        "curl", "-fsSL", "--connect-timeout", "5", "--max-time", "60", "--retry", "1",
        "-o", archive.string(), release.url,
    };
    if (runCommand(curl, nullptr) != 0) {
        return fail("download failed: " + release.url + " (needs the curl command and network access)");
    }
    std::string actual;
    if (!sha256Of(archive.string(), &actual)) {
        return fail("cannot compute SHA256 of " + archive.string() + " (needs sha256sum or shasum)");
    }
    if (actual != release.sha256) {
        return fail("SHA256 mismatch for " + release.url + ": expected " + release.sha256 + ", got " + actual);
    }

    const fs::path unpack = tmp / "unpack";
    fs::create_directories(unpack, ec);
    if (ec || runCommand({"tar", "-xzf", archive.string(), "-C", unpack.string()}, nullptr) != 0) {
        return fail("cannot unpack " + release.url);
    }
    const fs::path source = unpack / release.name;
    for (const auto& file : release.required_files) {
        if (!fs::is_regular_file(source / file, ec)) {
            return fail(release.url + " does not contain " + release.name + "/" + file);
        }
    }

    fs::create_directories(dir, ec);
    if (ec) return fail("cannot create " + model_dir + ": " + ec.message());
    const fs::path marker = release.required_files.front();
    fs::directory_iterator it(source, ec);
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path file = it->path();
        std::error_code type_ec;
        if (!it->is_regular_file(type_ec) || file.filename() == marker) continue;
        fs::rename(file, dir / file.filename(), ec);
        if (ec) return fail("cannot install " + file.filename().string() + ": " + ec.message());
    }
    if (ec) return fail("cannot list " + source.string() + ": " + ec.message());
    fs::rename(source / marker, dir / marker, ec);
    if (ec) return fail("cannot install " + marker.string() + ": " + ec.message());

    std::fprintf(stderr, "[KWS] model %s installed in %s\n", release.name.c_str(), model_dir.c_str());
    return true;
}

}  // namespace

bool ensureModel(const ModelRelease& release, const std::string& model_dir, std::string* error) {
    try {
        return ensureModelImpl(release, model_dir, error);
    } catch (const std::exception& e) {
        if (error) *error = std::string("model install failed: ") + e.what();
        return false;
    }
}

}  // namespace kws
