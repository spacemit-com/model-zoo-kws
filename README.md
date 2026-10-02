# KWS 组件

## 1. 项目简介

关键词唤醒（wake word）组件，内置 **cFSMN char-CTC** 检测器：3 麦固定波束 → kaldi fbank →
流式 cFSMN → 受限 CTC 前缀搜索。全部为手写 C++，**不依赖 ONNX Runtime、BLAS 或任何推理引擎**，
打分粒度 30 ms。旧版本在 SpacemiT K3（riscv64）录音回放中约占单核 2.7%，
该历史数字不作为当前版本的性能承诺。

默认关键词为 **"小进小进"**，模型由 `iic/speech_charctc_kws_phone-xiaoyun`（Apache-2.0，
756k 参数）在自录数据上微调得到。已有录音回放结果属于开发集上的探索性评估，
包含调参、波束标定所用录音，不能作为独立测试集的唤醒率或误报率。
发布效果需在冻结模型、前处理和阈值后，使用独立说话人/会话与长时负样本重新验证。

本组件可直接接收 SPV 复合设备的 4 通道裸流并进行三麦波束处理。
播放期间的回声消除需要准确的播放 PCM 参考，可通过下述可选 WebRTC AEC 模式试验。

## 2. 验证模型

### 2.1. 安装依赖

C++ 侧只需要编译器与 CMake ≥ 3.16；Python 侧需要 pybind11 与 numpy。

```bash
# Linux 示例
sudo apt install build-essential cmake
pip install pybind11 numpy
```

### 2.2. 提供模型

模型不进 git，发布包为 `xiaojin-v1`（约 2.3 MB），默认目录 `~/.cache/models/kws/xiaojin-v1`。
默认目录缺文件时，第一次初始化引擎会自动把发布包下载到该目录：先核对预置的 SHA256 再解包，
需要 `curl`、`tar`、`sha256sum`（或 `shasum`）命令和能访问 archive.spacemit.com 的网络。
下载期间初始化会等待：离线时约 1~11 s 后报下载失败，传输卡住时最多约 2 分钟。
设 `KWS_MODEL_DOWNLOAD=0` 可关闭自动下载（离线环境，或要确认运行时不联网时），缺模型时立即报错。

构建默认不下载模型（与 asr 一致）。需要提前放好时，可任选一种方式：

```bash
# 方式一：配置时下载，校验 TLS 与 CMake 中预置的 SHA256
cmake -B build -S . -DKWS_MODEL_FETCH_OFF=OFF

# 方式二：手动下载并核对 md5
mkdir -p ~/.cache/models/kws && cd ~/.cache/models/kws
curl -fLO https://archive.spacemit.com/spacemit-ai/model_zoo/kws/xiaojin-v1.tar.gz
curl -fLO https://archive.spacemit.com/spacemit-ai/model_zoo/kws/xiaojin-v1.tar.gz.md5
md5sum -c xiaojin-v1.tar.gz.md5 && tar xzf xiaojin-v1.tar.gz && rm xiaojin-v1.tar.gz*
```

模型目录包含：

| 文件 | 说明 |
| --- | --- |
| `cfsmn.bin` | 微调后的 cFSMN 权重（fp32） |
| `beam_w.bin` | 3 麦固定 MVDR 波束系数（257 bins × 3 通道） |
| `keywords.txt` | 关键词文本 → CTC token id，可自行追加；`!` 开头的行为易混词否决 |
| `LICENSE`、`NOTICE`、`README.txt` | 许可证、第三方署名与模型说明 |

使用其他目录或候选模型时，运行时显式指定：

```bash
export KWS_MODEL_DIR="$HOME/.cache/models/kws/xiaojin-ft02"
# 也可使用 --model-dir 或 KwsConfig::model_dir
```

只有默认目录会自动下载；其他目录须已包含 `cfsmn.bin`、`beam_w.bin`、`keywords.txt`。
SDK 构建的程序统一安装到 SDK 根目录的 `output/staging/bin/`，
源码仍在 `components/model_zoo/kws/`。训练数据、checkpoint 和实验记录由训练仓库管理。
配置时下载失败不会被报告成模型就绪；没有模型也可以编译库、运行合成权重回归测试。

现有 `cfsmn.bin` 格式保持兼容：`KWSF`、9 个 little-endian int32 维度、
按导出顺序排列的 float32（含 CMVN）。加载器校验维度、精确文件大小与有限值，
前处理要求输入维度为 400。修复流式边界和解码器无需重新训练或修改权重数值。
`beam_w.bin` 是设备/麦克风布局相关的固定波束系数，不能当作通用 AEC 权重。

### 2.3. 测试

- **在 SDK 中验证**（2.3.1）：在 SpacemiT Robot SDK 工程内用 `mm` 编译，产物部署到 `output/staging`。
- **独立构建下验证**（2.3.2）：在本目录下用 CMake 编译，不依赖完整 SDK。

#### 2.3.1. 在 SDK 中验证

```bash
source build/envsetup.sh
venv 3.13.12
cd components/model_zoo/kws
mm --with-deps
```

构建完成后，除安装到 `output/staging` 外，wheel 包也会输出到 `output/dist/`，例如
`output/dist/spacemit_kws-1.0.0-xxx.whl`。

**运行**（先在 SDK 根目录 `source build/envsetup.sh`）：

```bash
kws_file_demo --wav your_16k.wav             # 回放一个 wav
kws_stream_demo -l                         # audio 组件的输入/输出设备列表
kws_stream_demo -i 0 -c 4 -t 30             # SPV 设备：4 通道采集，自动使用三麦波束
# 默认持续到 Ctrl+C；-t 可限制秒数。设备索引以 -l 的结果为准。
```

`kws_stream_demo` 默认通过 audio 组件的 `AudioCapture` 打开麦克风。
麦克风模式唤醒后，使用 `AudioPlayer` 播放
`~/.cache/models/assets/audio/006_im_here.wav`（“我在”）。播放在独立任务中执行，
不阻塞采集和推理，也不叠加重复应答。`-o N` 选择输出设备，`--ack-wav FILE`
替换应答音（PCM16 WAV），`--no-ack` 关闭应答音。未安装默认应答音时会明确提示并继续监听。
按 Ctrl+C 后停止采集，等待正在播放的应答音结束，再关闭设备退出。

Linux 下 demo 默认只枚举 ALSA 硬件声卡，避免 PortAudio 在启动时探测不可用的
PipeWire 等虚拟 PCM 插件而卡住；即使指定了 `-i/-o`，PortAudio 初始化仍会先枚举设备。
该设置仅作用于 demo 进程，KWS 核心库与系统音频配置不变。
需要桌面虚拟设备时，显式设置 `PA_ALSA_IGNORE_ALL_PLUGINS=0`；列举与运行必须使用
相同设置，设备索引以当前 `-l` 的输出为准：

```bash
PA_ALSA_IGNORE_ALL_PLUGINS=0 kws_stream_demo -l
PA_ALSA_IGNORE_ALL_PLUGINS=0 kws_stream_demo -i N -c C
```

启动时会先输出 `opening ... audio devices...`，出现 `microphone listening` 才表示
采集已启动。Ctrl+Z 只会挂起进程，并不会释放声卡；正常退出请使用 Ctrl+C。

应答音首次安装：

```bash
mkdir -p ~/.cache/models/assets/audio
curl -fL https://archive.spacemit.com/spacemit-ai/model_zoo/assets/audio/006_im_here.wav \
    -o ~/.cache/models/assets/audio/006_im_here.wav
```

SDK 构建默认启用 `BUILD_KWS_MICROPHONE`，需要先构建 audio（可用 `mm --with-deps`）。
KWS 核心库、文件推理和 Python 绑定不依赖音频采集设备。
仍需接收外部 PCM 流时，显式使用 `--stdin`；该模式默认不播放，
可通过 `--ack-wav FILE` 启用。原有 `--ack CMD` 保留，指定后替代内置应答音：

```bash
arecord -q -D hw:CARD=Device,DEV=0 -f S16_LE -c 4 -r 16000 -t raw | \
    kws_stream_demo --stdin --channels 4 --beam
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

独立构建默认只提供 `kws_stream_demo --stdin`。如已安装 SDK audio 组件，可使用
`-DBUILD_KWS_MICROPHONE=ON -DCMAKE_PREFIX_PATH=/path/to/sdk/output/staging` 启用麦克风采集与应答音播放。

#### 2.3.3. 播放期间使用 WebRTC AEC

`BUILD_KWS_AEC=ON` 为 stream demo 增加可选 AEC3 模式。依赖 SDK audio 和
`webrtc-audio-processing-2` 开发文件；可复用 `application/native/omni_agent`
构建过的 `~/.cache/thirdparty/webrtc-audio-processing`。CMake 不会自动下载或更新该缓存。
系统安装可通过 pkg-config 查找，也可指定 `KWS_WEBRTC_ROOT` 和 `KWS_WEBRTC_LIBRARY`。

```bash
mm -j4 -DBUILD_KWS_MICROPHONE=ON -DBUILD_KWS_AEC=ON
kws_stream_demo -i 0 -c 4 -o 0 --aec
```

AEC 模式使用 `AudioDuplex` 在同一 10 ms 回调中采播，4 路采集、默认 2 路播放。
送给喇叭的同一段单声道 PCM 作为 WebRTC 播放参考；对 KWS 消费的每路麦克风
分别做 AEC 后，再进入原有三麦波束与检测器。降噪和 AGC 关闭，AEC3 保留其默认高通处理。
音频回调只搬运预分配缓冲，通过无锁 SPSC 队列传递对齐的采集/参考帧；不等待消费者、不做推理或文件 I/O。
AEC 在采集消费者线程执行，KWS 在引擎推理线程执行，唤醒事件在独立事件线程执行。
AEC 输入队列满时整对丢弃采集/参考帧并累计计数，消费者检测到时间缺口后重置 AEC/KWS 连续状态，监听继续。
不使用、也不假定 SPV 第 0 通道是回声参考或已经消除回声的语音。

`--aec-bypass` 保留相同全双工链路并旁路 AEC，可做 A/B；`--aec-delay-ms N`
设置设备延迟提示（默认 50 ms，0～500 ms，参考对齐由 WebRTC 管理）。
`--playback-channels N` 改输出通道数。应答音须为 16 kHz、单声道 PCM16 WAV；
`--ack CMD` 和 `--stdin` 不支持 AEC 模式，因为该链路拿不到外部播放的准确 PCM 参考。
Ctrl+C 会关闭全双工设备，停止当前播放。

诊断时可添加 `--aec-playback FILE` 在采集开始时播放一段测试音频，
添加 `--aec-record PREFIX -t SECONDS` 保存同步的 `PREFIX.raw.wav`、
`PREFIX.aec.wav` 和 `PREFIX.ref.wav`（已有文件不会被覆盖）。
记录中的 raw/aec 保持输入通道布局，ref 为实际提交给输出设备的单声道播放参考。

全双工 demo 的唤醒冷却期默认为 500 ms，便于测试连续唤醒；`--aec-bypass`
使用相同设置，`--holdoff-ms N` 可显式覆盖。其他 demo 模式及 `KwsConfig`
默认仍为 2500 ms。每次命中后消费该关键词的已解码历史，持续的末尾 token
不会重复触发；特征、模型和 AEC 状态继续运行。分数表示尚未消费的新关键词候选。
新的唤醒可以中断并重新播放本程序的应答音，不会因为上一次应答尚未结束而丢弃；
`--aec-playback` 的诊断刺激保持固定播放时间线。

AEC 不改变模型阈值，但会改变输入波形，不能保证所有双讲条件下保留唤醒分数；
该模式仍需现场双讲和远场测试，默认不开启。若原生 `aplay` 也播放成噪声，
应先恢复声卡基础播放，再评估 AEC；此时正常文件的 PCM 无法代表异常的实际声学输出。

**Python 示例：**

```bash
make -C build kws-install-python        # 或 pip install 构建出的 wheel
python python/examples/kws_file_demo.py --wav /path/to/16k_4ch.wav
```

**契约测试**（不需要模型文件）：

```bash
bash tests/test_config_and_backend_contract.sh
bash tests/test_invalid_input_error_path.sh
bash tests/test_inference_regression.sh
```

## 3. 应用开发

### 3.1. 构建与集成产物

| 产物 | 说明 |
| --- | --- |
| `include/kws_service.h` | **C++ API 头文件**，应用侧只需包含此头文件并链接下方库 |
| `build/lib/libkws.a` | C++ 核心库（含 cFSMN 后端），链接时使用 |
| `output/dist/spacemit_kws-1.0.0-xxx.whl` | Python wheel 包，`pip install` 后 `import spacemit_kws` |
| `build/python/_spacemit_kws*.so` | Python 扩展；安装 wheel 后通过 `spacemit_kws` 导入 |

示例可执行文件（非集成必需）：`build/bin/kws_file_demo`、`build/bin/kws_stream_demo`。

### 3.2. API 使用

**C++**：头文件 `include/kws_service.h` 为唯一 API 入口，实现为 PIMPL。
音频为 `[-1, 1]` 的 float，16 kHz；多通道时按 `num_channels` 交织，
`SendAudioFrame` 的长度参数是**单通道**采样点数；流式调用异步执行，返回 `KwsAudioStatus`，最多 20480 点/通道/次。
音频会在返回前复制到固定 128 块队列（每块最多 160 点）。满队列立即返回 `QUEUE_FULL`，不阻塞采集；后续处理在缺口处重置连续状态。
空帧无操作，错误形状/非有限或越界样本报告错误并保留会话。`GetStreamStats()` 可查看输入与事件溢出。
业务事件独立分发且队列有界；缓慢回调可能导致旧事件被淘汰并报告溢出。控制接口和 `Stop`/析构允许等待，不应放入音频 callback。
外部 `Stop` 返回后尾部和关闭回调已完成；回调内调用 `Stop` 不等待自身。详见 [API.md](API.md)。
`Detect` 和 `Stop` 都会排空前瞻缓存，不需自行补静音；最后不足 160 点的音频也应送入。
特征边界遵循训练配置：fbank 只保留完整窗口，拼帧复制左边界并丢弃右侧不完整上下文。
`GetLookaheadMs()` 只返回模型前瞻，不含 fbank、拼帧和可选波束的延迟。

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
    auto status = engine.SendAudioFrame(frame, 160); // 检查 ACCEPTED / QUEUE_FULL 等状态
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
status = engine.send_audio_frame(frame)  # 返回 KwsAudioStatus，结果由事件线程回调
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
| `model_dir` | `string` | `~/.cache/models/kws/xiaojin-v1` | 模型目录，空则读环境变量 `KWS_MODEL_DIR` |
| `keywords` | `vector<KwsKeyword>` | 模型自带 | 关键词；只给文本时从 `keywords.txt` 解析 token id |
| `num_channels` | `int` | `1` | 1～64；SPV 复合设备为 4 |
| `use_beamforming` | `bool` | `false` | 3 麦固定波束，要求 `beam_first_channel + 3 ≤ num_channels` |
| `beam_first_channel` | `int` | `1` | 波束取的第一路；关闭波束时表示取哪一路做单声道 |
| `threshold` | `float` | `0.3` | 关键词得分阈值 |
| `holdoff_ms` | `int` | `2500` | 两次上报之间的静默期 |
| `decode_context` | `int` | `86` | 1～4096 个模型帧（86 × 30 ms ≈ 2.6 s） |
| `score_interval` | `int` | `1` | 每 N 个模型帧打一次分，调 CPU 用 |
| `frame_size` | `int` | `160` | 调用方建议分块大小，内部 hop 固定 160 |
| `num_threads` | `int` | `1` | 当前只支持 1，其他值初始化失败 |

详见 [API.md](API.md)。

## 4. 常见问题

| 现象 | 可能原因 | 处理 |
| --- | --- | --- |
| `Model weights not found` | 非默认目录里没有模型、`model_dir` 不对，或 `KWS_MODEL_DOWNLOAD=0` | 见 [2.2](#22-提供模型)，或设 `KWS_MODEL_DIR` |
| `Model download failed` | 无网络、缺 `curl`/`tar`/`sha256sum`，或下载内容 SHA256 不符 | 看错误详情；离线环境按 [2.2](#22-提供模型) 手动放模型 |
| 一直不唤醒 | 采样率不是 16 kHz；通道数/交织方式不符；音频没归一化到 `[-1, 1]` | 先用 `kws_file_demo` 回放同一段录音确认链路 |
| 远场唤醒率低 | 单通道输入没有走波束 | 4 通道时开 `use_beamforming`；或在前级接 AEC/降噪 |
| 外放时失聪 | 回声淹没人声 | 试用 `--aec`，确保实际播放经过该全双工链路，并检查 raw/aec/ref 录音 |
| 误唤醒偏多 | 阈值偏低 | 提高 `threshold`；`keywords.txt` 里也可给单个关键词单独设阈值 |
| 唤醒延迟约 0.3 s | 模型前瞻 240 ms，另有特征及波束延迟 | 事件时间戳已按 token 对齐映射到输入音频，不要再次扣除前瞻 |
| CPU 偏高 | 打分粒度太细 | 调大 `score_interval`（2 = 每 60 ms 打一次分） |

## 5. 版本与发布

| 版本 | 说明 |
| --- | --- |
| 1.0.0 | 提供 C++ / Python 接口，内置 cFSMN char-CTC 后端、3 麦固定波束、整段与流式检测、易混词否决；模型发布包 `xiaojin-v1`。 |
| 1.1.0 | 默认模型目录缺文件时运行时自动下载 `xiaojin-v1` 并校验 SHA256；`KWS_MODEL_DOWNLOAD=0` 关闭。 |

## 6. 贡献方式

欢迎参与贡献：提交 Issue 反馈问题，或通过 Pull Request 提交代码。

- **编码规范**：C++ 代码遵循 [Google C++ 风格指南](https://google.github.io/styleguide/cppguide.html)。
- **提交前检查**：`tests/` 下契约及推理回归测试不依赖外部模型，提交前请先跑通。
  推理回归可额外使用 `KWS_TEST_MODEL_DIR`、`KWS_TEST_AUDIO_F32`（mono float32 原始音频）、
  `KWS_TEST_REFERENCE`（导出工具生成的 features/logits 对）验证真实模型与 PyTorch 参考。

## 7. License

本组件源码文件头声明为 Apache-2.0，最终以本目录 `LICENSE` 文件为准；
第三方来源见 `NOTICE`。
