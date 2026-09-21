# KWS 组件

## 1. 项目简介

关键词唤醒（wake word）组件，内置 **cFSMN char-CTC** 检测器：3 麦固定波束 → kaldi fbank →
流式 cFSMN → 受限 CTC 前缀搜索。全部为手写 C++，**不依赖 ONNX Runtime、BLAS 或任何推理引擎**，
在 SpacemiT K3（riscv64）上约占单核 2.7%，打分粒度 30 ms。

默认关键词为 **"小进小进"**，模型由 `iic/speech_charctc_kws_phone-xiaoyun`（Apache-2.0，
756k 参数）在自录数据上微调得到：

| 测试集 | 未微调 | 微调后 |
| --- | --- | --- |
| 跨说话人近场 | 65.5% | 93.1% |
| 1.5 / 3 m 远场（未参与训练） | 10.6% | 96.5% |

六段留出录音上的流式表现（阈值 0.3，4 通道 + 波束）：quiet-near 10/10、quiet-3m 10/10、
play-near 13/13、play-3m 13/13，5 分钟聊天与外放负样本 0 误报。

设备上的板载 KWS 没有 AEC，外放音量大时会失聪；本组件的波束前处理就是为这个场景准备的，
可直接吃 SPV 复合设备的 4 通道裸流。

## 2. 验证模型

### 2.1. 安装依赖

C++ 侧只需要编译器与 CMake ≥ 3.16；Python 侧需要 pybind11 与 numpy。

```bash
# Linux 示例
sudo apt install build-essential cmake
pip install pybind11 numpy
```

### 2.2. 下载模型

构建时由 `cmake/FetchKwsModel.cmake` 自动下载到 `~/.cache/models/kws/xiaojin/`，
约 3 MB，含三个文件：

| 文件 | 说明 |
| --- | --- |
| `cfsmn.bin` | 微调后的 cFSMN 权重（fp32） |
| `beam_w.bin` | 3 麦固定 MVDR 波束系数（257 bins × 3 通道） |
| `keywords.txt` | 关键词文本 → CTC token id，可自行追加 |

也可手动下载或指定别处：

```bash
mkdir -p ~/.cache/models/kws && cd ~/.cache/models/kws
wget https://archive.spacemit.com/spacemit-ai/model_zoo/kws/xiaojin.tar.gz
tar xzf xiaojin.tar.gz

# 或者用环境变量/配置项指向任意目录
export KWS_MODEL_DIR=/data/models/kws-xiaojin
```

离线构建用 `cmake .. -DKWS_MODEL_FETCH_OFF=ON`，构建不会因为下载失败而中断——
没有模型只是跑不了 demo。

### 2.3. 测试

- **在 SDK 中验证**（2.3.1）：在 SpacemiT Robot SDK 工程内用 `mm` 编译，产物部署到 `output/staging`。
- **独立构建下验证**（2.3.2）：在本目录下用 CMake 编译，不依赖完整 SDK。

#### 2.3.1. 在 SDK 中验证

```bash
source build/envsetup.sh
venv 3.13.12
cd components/model_zoo/kws
mm
```

构建完成后，除安装到 `output/staging` 外，wheel 包也会输出到 `output/dist/`，例如
`output/dist/spacemit_kws-1.0.0-xxx.whl`。

**运行**（先在 SDK 根目录 `source build/envsetup.sh`）：

```bash
kws_file_demo --wav your_16k.wav             # 回放一个 wav
arecord -q -D hw:1,0 -f S16_LE -c 4 -r 16000 -t raw | \
    kws_stream_demo --channels 4 --ack "aplay -q ~/.cache/models/assets/audio/006_im_here.wav"
```

#### 2.3.2. 独立构建下验证

```bash
cd /path/to/kws
mkdir -p build && cd build
cmake ..
make -j$(nproc)

./bin/kws_file_demo --wav /path/to/16k_4ch.wav
./bin/kws_file_demo --wav /path/to/16k_mono.wav --no-beam
```

**Python 示例：**

```bash
make -C build kws-install-python        # 或 pip install 构建出的 wheel
python python/examples/kws_file_demo.py --wav /path/to/16k_4ch.wav
```

**契约测试**（不需要模型文件）：

```bash
bash tests/test_config_and_backend_contract.sh
bash tests/test_invalid_input_error_path.sh
```

## 3. 应用开发

### 3.1. 构建与集成产物

| 产物 | 说明 |
| --- | --- |
| `include/kws_service.h` | **C++ API 头文件**，应用侧只需包含此头文件并链接下方库 |
| `build/lib/libkws.a` | C++ 核心库（含 cFSMN 后端），链接时使用 |
| `output/dist/spacemit_kws-1.0.0-xxx.whl` | Python wheel 包，`pip install` 后 `import spacemit_kws` |
| `build/python/spacemit_kws/` | Python 包，`make kws-install-python` 后可直接导入 |

示例可执行文件（非集成必需）：`build/bin/kws_file_demo`、`build/bin/kws_stream_demo`。

### 3.2. API 使用

**C++**：头文件 `include/kws_service.h` 为唯一 API 入口，实现为 PIMPL。
音频为 `[-1, 1]` 的 float，16 kHz；多通道时按 `num_channels` 交织，
`SendAudioFrame` 的长度参数是**单通道**采样点数。

```cpp
#include "kws_service.h"
using namespace SpacemiT;

class MyCallback : public KwsEngineCallback {
    void OnWakeWord(const std::string& keyword, float score, int64_t ts_ms) override {
        // 打断 TTS、开始录音 ...
    }
};

// SPV 复合设备：4 通道交织，ch0 为板端处理结果，ch1~ch3 走 3 麦波束
auto config = KwsConfig::Preset("xiaojin-4mic").withThreshold(0.3f);
KwsEngine engine(config);
if (!engine.IsInitialized()) {
    std::cerr << engine.GetLastError() << std::endl;   // 模型缺失、关键词无 token id 等
    return 1;
}

engine.SetCallback(std::make_shared<MyCallback>());
engine.Start();
while (capture(frame))          // frame: 160 × 4 个 float
    engine.SendAudioFrame(frame, 160);
engine.Stop();
```

单通道（例如 AEC 之后的信号）用 `KwsConfig::Preset("xiaojin")`，整段音频则可以直接：

```cpp
auto result = engine.Detect(samples, num_samples);
if (result->IsWakeWord()) std::cout << result->GetKeyword() << " " << result->GetScore();
```

**Python**：

```python
import spacemit_kws

result = spacemit_kws.detect(audio)                 # audio: float32 numpy，[-1, 1]
print(result.keyword, result.score, result.is_wake_word)

engine = spacemit_kws.KwsEngine("xiaojin-4mic")     # 流式
engine.set_callback(my_callback)
engine.start()
engine.send_audio_frame(frame)
engine.stop()
```

**CMake 集成**：

```cmake
add_subdirectory(kws)
target_link_libraries(your_target PRIVATE kws)
target_include_directories(your_target PRIVATE ${KWS_SOURCE_DIR}/include)
```

### 3.3. 配置参数

| 参数 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `backend` | `KwsBackendType` | `CFSMN` | 后端类型 |
| `model_dir` | `string` | `~/.cache/models/kws/xiaojin` | 模型目录，空则读环境变量 `KWS_MODEL_DIR` |
| `keywords` | `vector<KwsKeyword>` | 模型自带 | 关键词；只给文本时从 `keywords.txt` 解析 token id |
| `num_channels` | `int` | `1` | 送入的交织通道数，SPV 复合设备为 4 |
| `use_beamforming` | `bool` | `false` | 3 麦固定波束，要求 `beam_first_channel + 3 ≤ num_channels` |
| `beam_first_channel` | `int` | `1` | 波束取的第一路；关闭波束时表示取哪一路做单声道 |
| `threshold` | `float` | `0.3` | 关键词得分阈值 |
| `holdoff_ms` | `int` | `2500` | 两次上报之间的静默期 |
| `decode_context` | `int` | `86` | 解码上下文帧数（86 × 30 ms ≈ 2.6 s） |
| `score_interval` | `int` | `1` | 每 N 个模型帧打一次分，调 CPU 用 |

详见 [API.md](API.md)。

## 4. 常见问题

| 现象 | 可能原因 | 处理 |
| --- | --- | --- |
| `Model weights not found` | 没下载模型，或 `model_dir` 不对 | 见 [2.2](#22-下载模型)，或设 `KWS_MODEL_DIR` |
| 一直不唤醒 | 采样率不是 16 kHz；通道数/交织方式不符；音频没归一化到 `[-1, 1]` | 先用 `kws_file_demo` 回放同一段录音确认链路 |
| 远场唤醒率低 | 单通道输入没有走波束 | 4 通道时开 `use_beamforming`；或在前级接 AEC/降噪 |
| 外放时失聪 | 回声淹没人声 | 用 4 通道 + 波束，必要时在波束后再接 AEC |
| 误唤醒偏多 | 阈值偏低 | 提高 `threshold`；`keywords.txt` 里也可给单个关键词单独设阈值 |
| 唤醒延迟约 0.3 s | 模型右序 2 帧 × 4 层 = 240 ms 前瞻，改不掉 | 用 `GetLookaheadMs()` 让上层排期时扣除 |
| CPU 偏高 | 打分粒度太细 | 调大 `score_interval`（2 = 每 60 ms 打一次分） |

## 5. 版本与发布

| 版本 | 说明 |
| --- | --- |
| 1.0.0 | 提供 C++ / Python 接口，内置 cFSMN char-CTC 后端、3 麦固定波束、整段与流式检测。 |

## 6. 贡献方式

欢迎参与贡献：提交 Issue 反馈问题，或通过 Pull Request 提交代码。

- **编码规范**：C++ 代码遵循 [Google C++ 风格指南](https://google.github.io/styleguide/cppguide.html)。
- **提交前检查**：`tests/` 下两个契约测试不依赖模型，提交前请先跑通。

## 7. License

本组件源码文件头声明为 Apache-2.0，最终以本目录 `LICENSE` 文件为准；
第三方来源见 `NOTICE`。
