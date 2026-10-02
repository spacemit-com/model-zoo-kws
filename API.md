# KWS API

关键词唤醒引擎的 C++ 与 Python 接口。C++ 入口是 `include/kws_service.h`，
Python 入口是 `spacemit_kws`。音频约定：**16 kHz、float、[-1, 1]**，
多通道按 `num_channels` 交织，长度参数一律是**单通道**采样点数。

## 功能特性

- 内置 cFSMN char-CTC 检测器，纯 C++，无推理引擎依赖
- 整段检测（`Detect`）与流式检测（`Start` / `SendAudioFrame` / `Stop`）
- 可选 3 麦固定 MVDR 波束前处理，直接吃 SPV 复合设备的 4 通道裸流
- 多关键词并行打分，每个关键词可单独设阈值
- 打分粒度 30 ms，静默期（holdoff）内不重复上报
- 模型固有前瞻 240 ms，可通过 `GetLookaheadMs()` 查询

## C++ API

### KwsBackendType

```cpp
enum class KwsBackendType {
    CFSMN,    // 内置 cFSMN char-CTC 检测器
    CUSTOM,   // 自定义后端
};
```

### KwsKeyword

```cpp
struct KwsKeyword {
    std::string text;              // "小进小进"
    std::vector<int> token_ids;    // 空则从模型目录的 keywords.txt 解析
    float threshold = 0.0f;        // 0 表示沿用 KwsConfig::threshold
};
```

### KwsConfig

```cpp
struct KwsConfig {
    KwsBackendType backend = KwsBackendType::CFSMN;
    std::string model_dir;                 // 空 → $KWS_MODEL_DIR → ~/.cache/models/kws/xiaojin-v1
    std::vector<KwsKeyword> keywords;      // 空 → 用模型目录里的全部关键词

    int sample_rate = 16000;
    int num_channels = 1;
    int frame_size = 160;                  // 调用方建议分块大小，内部 hop 固定 160

    bool use_beamforming = false;
    int beam_first_channel = 1;

    float threshold = 0.3f;
    int holdoff_ms = 2500;
    int decode_context = 86;               // 模型帧，30 ms/帧
    int score_interval = 1;
    int num_threads = 1;                   // 当前仅支持 1

    static KwsConfig Preset(const std::string& name);     // "xiaojin" / "xiaojin-4mic"
    static std::vector<std::string> AvailablePresets();

    // 以下方法返回副本，不修改原对象
    KwsConfig withModelDir(const std::string& dir) const;
    KwsConfig withKeyword(const std::string& text) const;
    KwsConfig withKeywords(const std::vector<KwsKeyword>& keywords) const;
    KwsConfig withThreshold(float threshold) const;
    KwsConfig withHoldoff(int ms) const;
    KwsConfig withChannels(int channels) const;
    KwsConfig withBeamforming(bool enable) const;
    KwsConfig withScoreInterval(int frames) const;
    KwsConfig withDecodeContext(int frames) const;
    KwsConfig withSampleRate(int rate) const;
    KwsConfig withNumThreads(int threads) const;
};
```

预设：

| 名称 | 通道 | 波束 | 用途 |
| --- | --- | --- | --- |
| `xiaojin` | 1 | 关 | 单通道输入，例如 AEC 之后的信号 |
| `xiaojin-4mic` | 4 | 开（ch1~ch3） | SPV 复合设备裸流，ch0 是板端处理结果 |

### KwsResult

```cpp
class KwsResult {
public:
    float GetScore() const;            // 当前关键词最高分，未越阈值也可能非零
    bool IsWakeWord() const;           // 越过阈值且通过静默期
    std::string GetKeyword() const;
    int GetKeywordIndex() const;       // 未命中为 -1

    int64_t GetTimestampMs() const;    // 命中：末 token 对齐帧结束时间；非命中：打分帧时间
    int GetProcessingTimeMs() const;

    bool IsSuccess() const;
    std::string GetCode() const;
    std::string GetMessage() const;
};
```

### KwsEngineCallback

流式回调顺序：`OnOpen` → `OnEvent`（命中时另有 `OnWakeWord`）→ `OnComplete` → `OnClose`；
内部故障时 `OnError` → `OnClose`；输入拒绝、输入或事件队列溢出只报告 `OnError`，监听继续。正常消费时每个打分点都会有一次 `OnEvent`（未命中时 `GetScore()` 是当前最高分），
命中时**先** `OnEvent` **再** `OnWakeWord`。

```cpp
class KwsEngineCallback {
public:
    virtual void OnOpen() {}
    virtual void OnEvent(std::shared_ptr<KwsResult> result) {}
    virtual void OnWakeWord(const std::string& keyword, float score, int64_t timestamp_ms) {}
    virtual void OnComplete() {}
    virtual void OnError(const std::string& message) {}
    virtual void OnClose() {}
};
```

流式输入由引擎自己的推理线程处理，回调由独立事件线程按序执行；业务回调不会占用音频回调或推理线程。
回调可以调用 `Stop`、`Reset`、`SetThreshold`、`SetCallback`，不能销毁引擎。
外部线程的 `Stop` 等待已接收音频、尾部结果和关闭回调；回调内的 `Stop` 只等待推理排空，随后分发尾部与关闭事件。
`Start` 返回本次启动操作是否成功，回调随后停止监听不会改变其返回值。上一轮关闭事件尚未处理完时不能重启。
`Reset` 丢弃待处理输入和未分发的旧打分，并重置时间与统计；已进入的回调不会被强行中断。
`SetCallback` 影响之后入队的事件，已入队事件保留原接收者；注册过的回调对象保留到引擎销毁，避免在推理线程执行用户析构函数。
引擎析构前停止外部送帧；析构会等待内部线程及正在执行的回调，业务回调必须能返回。Python 的 Stop/析构等待期间释放 GIL。
事件队列最多 128 项。慢消费者导致满队列时先淘汰未命中的打分，否则淘汰较旧的检测/错误事件；保留生命周期事件。
所有淘汰累计到 `GetStreamStats().event_overruns`，事件线程恢复后报告 `OnError`。

### KwsEngine

```cpp
class KwsEngine {
public:
    explicit KwsEngine(KwsBackendType backend = KwsBackendType::CFSMN,
                        const std::string& model_dir = "");
    explicit KwsEngine(const KwsConfig& config);

    // 整段检测：返回其中最高的一次打分（命中优先）
    std::shared_ptr<KwsResult> Detect(const std::vector<float>& audio, int sample_rate = 16000);
    std::shared_ptr<KwsResult> Detect(const float* data, size_t num_samples, int sample_rate = 16000);

    // 流式检测
    void SetCallback(std::shared_ptr<KwsEngineCallback> callback);
    bool Start();
    KwsAudioStatus SendAudioFrame(const std::vector<float>& data);
    KwsAudioStatus SendAudioFrame(const float* data, size_t num_samples);   // num_samples 为单通道点数
    void Stop();

    void Reset();                       // 丢弃排队输入，清空时间、FSMN 记忆、静默期与统计
    KwsStreamStats GetStreamStats() const;
    bool IsInitialized() const;
    bool IsStreaming() const;
    std::string GetLastError() const;   // 最近一次操作的错误，成功时为空串

    void SetThreshold(float threshold);
    KwsConfig GetConfig() const;
    std::vector<std::string> GetKeywords() const;

    std::string GetEngineName() const;
    KwsBackendType GetBackendType() const;
    float GetLastScore() const;
    int GetLookaheadMs() const;         // cFSMN：240 ms
};
```

`SendAudioFrame` 是非阻塞入口：只检查形状并复制到预分配队列，不等待锁、不做推理、不分配内存、不调用业务回调。
空输入返回 `EMPTY`，不改变会话。每次最多 20480 样本/通道，且**必须是整通道的交织数据**；音频按最多 160 样本/通道分块。
支持一个生产者，并发送帧立即返回 `BUSY`；控制方法不是实时入口，不能从音频回调调用。

| 返回状态 | 含义 |
| --- | --- |
| `ACCEPTED` | 整块音频已复制，源缓冲可立即复用；采样值检查在推理线程完成 |
| `EMPTY` | 空输入，无操作 |
| `NOT_STARTED` | 没有接收中的流或正在停止/Reset |
| `INVALID_PARAMETER` | 空指针、错误交织长度或超过单次长度上限；会话保留 |
| `QUEUE_FULL` | 整块未接收；累计丢失样本并记录时间缺口，监听继续 |
| `BUSY` | 另一生产者正在送帧；调用方须串行送帧 |

输入队列为 128 块，容量上限是 1.28 秒（不足 160 点的块也占一格），数据到达即可处理，不等待队列填满。
发生输入溢出后，下一个接受块带时间缺口，推理线程重置连续状态并保留流的时间偏移，不拼接不连续音频。
`GetStreamStats()` 返回 `accepted_samples`、`dropped_samples`、`input_overruns`、`event_overruns` 和近似 `queued_blocks`。
离线快速喂流时在调用线程根据可用块数节流；实时采集不得等待队列空位，也不要反复重送已经判为丢失的块。
所有采样值必须有限且在 `[-1, 1]` 内。Python 二维数组形状必须为 `(N, num_channels)`。
Python 形状错误直接抛 `ValueError`，不改变引擎状态；采样值错误进入引擎错误处理路径。
`Detect` 和 `Stop` 排空所有模型前瞻，最后不足一跳的输入不会丢弃；无需调用方补静音。
fbank 丢弃不完整窗口，拼帧复制左边界并丢弃缺右上下文的帧，保持训练侧边界约定。
`Stop` 可重复调用，`Reset` 丢弃缓存但不改变开流状态；开流期间 `Detect` 返回错误。

cFSMN 命中后消费该关键词的已解码历史，并等待持续的末尾 token 结束后重新检测，
避免滑窗位置变化导致同一词语重复上报。声学模型和前处理状态不会因唤醒而重置。
`GetLastScore` 是最新未消费候选的分数，命中后的旧高分不会一直保留。
API 的 `holdoff_ms` 默认仍为 2500；stream demo 的全双工 AEC/旁路模式默认使用
500 ms，可通过 `--holdoff-ms` 覆盖。
非法阈值更新保留旧值并设置 `GetLastError()`；非法音频异步报告 `OnError` 并丢弃对应块，会话仍可接收后续音频。
`GetLookaheadMs()` 不包含特征和波束延迟。命中时间戳使用末 token 对齐帧，30 ms 粒度，
已映射到输入时间并扣除波束延迟，不应再次扣除模型前瞻。

### C++ 示例

```cpp
#include "kws_service.h"
using namespace SpacemiT;

auto config = KwsConfig::Preset("xiaojin").withThreshold(0.35f);
KwsEngine engine(config);
if (!engine.IsInitialized()) {
    std::cerr << "init failed: " << engine.GetLastError() << std::endl;
    return 1;
}

std::vector<float> audio = LoadWav16k("hello.wav");
auto result = engine.Detect(audio);
if (result->IsWakeWord()) {
    printf("%s score %.3f at %.1fs\n", result->GetKeyword().c_str(),
            result->GetScore(), result->GetTimestampMs() / 1000.0);
}
```

### 流式回调示例

```cpp
#include "kws_service.h"
using namespace SpacemiT;

class WakeHandler : public KwsEngineCallback {
public:
    void OnWakeWord(const std::string& keyword, float score, int64_t ts_ms) override {
        printf("[WAKE] %s %.3f @%.1fs\n", keyword.c_str(), score, ts_ms / 1000.0);
        // 打断正在播放的 TTS，开始录音
    }
    void OnError(const std::string& message) override {
        fprintf(stderr, "kws: %s\n", message.c_str());
    }
};

auto config = KwsConfig::Preset("xiaojin-4mic");     // 4 通道 + 波束
KwsEngine engine(config);
engine.SetCallback(std::make_shared<WakeHandler>());
engine.Start();

std::vector<float> frame(160 * 4);
while (Capture(frame.data(), 160)) {
    auto status = engine.SendAudioFrame(frame.data(), 160); // 单通道点数；检查拒绝/溢出状态
}
engine.Stop();
```

## Python API

```python
# =============================================================================
# 快捷函数
# =============================================================================
spacemit_kws.detect(
    audio,                  # numpy float32，[-1, 1]；(N,) 或 (N, C)
    keyword="",             # 空 = 模型自带关键词
    model_dir="",
    threshold=0.3,
    channels=1,
    beamforming=False,
    sample_rate=16000,
) -> KwsResult

# =============================================================================
# KwsConfig - 配置
# =============================================================================
KwsConfig.preset(name)              # "xiaojin" / "xiaojin-4mic"
KwsConfig.available_presets()
config.model_dir / keywords / num_channels / use_beamforming / beam_first_channel
config.threshold / holdoff_ms / decode_context / score_interval / num_threads
config.with_keyword(text) / with_threshold(v) / with_channels(n) / with_beamforming(b) ...

# =============================================================================
# KwsResult - 检测结果（只读属性）
# =============================================================================
result.score               # float
result.is_wake_word        # bool
result.keyword             # str
result.keyword_index       # int，未命中为 -1
result.timestamp_ms        # int
result.processing_time_ms  # int
result.success / result.code / result.message

# =============================================================================
# KwsCallback - 回调类（继承后覆盖需要的方法）
# =============================================================================
class KwsCallback:
    def on_open(self): ...
    def on_event(self, result): ...
    def on_wake_word(self, keyword, score, timestamp_ms): ...
    def on_complete(self): ...
    def on_error(self, message): ...
    def on_close(self): ...

# =============================================================================
# KwsEngine - 引擎
# =============================================================================
engine = KwsEngine(config)              # 或 KwsEngine("xiaojin-4mic", model_dir="")
engine.detect(audio, sample_rate=16000) -> KwsResult
engine.set_callback(cb)
engine.start() / engine.send_audio_frame(frame) / engine.stop()
engine.get_stream_stats()  # 含 input_overruns / event_overruns；send_audio_frame 返回 KwsAudioStatus
engine.reset() / engine.set_threshold(v)
engine.get_config() / engine.get_keywords()
engine.initialized / engine.streaming / engine.last_error
engine.engine_name / engine.backend_type / engine.last_score / engine.lookahead_ms
```

### Python 示例

```python
import numpy as np
import spacemit_kws


class Handler(spacemit_kws.KwsCallback):
    def on_wake_word(self, keyword, score, timestamp_ms):
        print(f"[WAKE] {keyword} {score:.3f} @{timestamp_ms / 1000:.1f}s")


config = spacemit_kws.KwsConfig.preset("xiaojin-4mic")
engine = spacemit_kws.KwsEngine(config)
assert engine.initialized, engine.last_error

engine.set_callback(Handler())
engine.start()
for i in range(0, len(audio), 640):               # audio 为一维交织数组；160 帧 × 4 通道
    # 离线数组喂流可等待；实际音频 callback 不等待队列。
    import time
    while engine.get_stream_stats().queued_blocks + 4 > 128:
        time.sleep(0.001)
    status = engine.send_audio_frame(audio[i:i + 640])
engine.stop()
```

## 错误处理

初始化失败时 `IsInitialized()` 为 `false`，原因用 `GetLastError()` 取；
Python 侧对应 `engine.initialized` 与 `engine.last_error`。常见错误：

| 错误 | 含义 |
| --- | --- |
| `Model weights not found: <path>/cfsmn.bin` | 模型没下载，或 `model_dir` 指错 |
| `Keyword '<text>' has no token ids` | 关键词不在 `keywords.txt` 里，也没有显式给 `token_ids` |
| `cFSMN backend only supports 16 kHz` | 采样率不是 16000 |
| `beamforming needs 3 channels from beam_first_channel on` | 通道数不够开波束 |

运行期的 `Detect` 失败通过 `KwsResult::IsSuccess()` / `GetCode()` / `GetMessage()` 反映，
流式输入错误保留到 `GetLastError()` 并报告 `OnError`，成功处理后清除；内部推理故障报告 `OnError → OnClose`。送帧的即时结果以 `KwsAudioStatus` 为准。
