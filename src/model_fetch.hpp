/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * 运行时模型下载
 *
 * 默认模型目录缺文件时，从 archive.spacemit.com 下载发布包，先核对 SHA256 再解包。
 * 只用 curl / tar / sha256sum（或 shasum）命令，不给静态库引入链接依赖。
 */

#ifndef MODEL_FETCH_HPP
#define MODEL_FETCH_HPP

#include <string>
#include <vector>

namespace kws {

// 一个发布包：url 指向 tar.gz，包内顶层目录为 <name>/，其中包含 required_files。
struct ModelRelease {
    std::string name;
    std::string url;
    std::string sha256;                       // 发布包的 SHA256，小写十六进制
    std::vector<std::string> required_files;  // 第一个文件最后落盘，它存在即代表安装完整
};

// 默认发布包；名称和 SHA256 与 cmake/FetchKwsModel.cmake 一致（tests/test_model_fetch.sh 校验）。
ModelRelease defaultModelRelease();

// 确保 model_dir 里有 release.required_files。缺任一文件时重新安装整个发布包：
// 下载到 model_dir 旁的临时目录，SHA256 不符则不解包，解包后逐个 rename 进 model_dir。
// 失败时返回 false 并写入 *error（不抛异常），model_dir 中已有的文件不受影响。
// 首次下载会阻塞调用方：离线时约 1~11 s 后失败，传输卡住时最多约 2 分钟。
bool ensureModel(const ModelRelease& release, const std::string& model_dir, std::string* error);

// KWS_MODEL_DOWNLOAD 设为 0 / off / false / no 时关闭自动下载。
bool modelDownloadEnabled();

}  // namespace kws

#endif  // MODEL_FETCH_HPP
