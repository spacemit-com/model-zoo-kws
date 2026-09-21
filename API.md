# KWS API

关键词唤醒引擎的 C++ 与 Python 接口。C++ 入口是 `include/kws_service.h`，
Python 入口是 `spacemit_kws`。音频约定：**16 kHz、float、[-1, 1]**，
多通道按 `num_channels` 交织，长度参数一律是**单通道**采样点数。

## 功能特性

- 内置 cFSMN char-CTC 检测器，纯 C++，无推理引擎依赖；K3 上约 2.7% 单核
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
    std::string model_dir;                 // 空 → $KWS_MODEL_DIR → ~/.cache/models/kws/xiaojin
    std::vector<KwsKeyword> keywords;      // 空 → 用模型目录里的全部关键词

    int sample_rate = 16000;
    int num_channels = 1;
    int frame_size = 160;                  // 10 ms

    bool use_beamforming = false;
    int beam_first_channel = 1;

    float threshold = 0.3f;
    int holdoff_ms = 2500;
    int decode_context = 86;               // 模型帧，30 ms/帧
    int score_interval = 1;
    int num_threads = 1;

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
    float GetScore() const;            // 关键词得分，未命中为 0
    bool IsWakeWord() const;           // 越过阈值且通过静默期
    std::string GetKeyword() const;
    int GetKeywordIndex() const;       // 未命中为 -1

    int64_t GetTimestampMs() const;    // 关键词结束处的音频时间，已扣除前瞻
    int GetProcessingTimeMs() const;

    bool IsSuccess() const;
    std::string GetCode() const;
    std::string GetMessage() const;
};
```

### KwsEngineCallback

流式回调顺序：`OnOpen` → `OnEvent`（命中时另有 `OnWakeWord`）→ `OnComplete` → `OnClose`；
出错时 `OnError` → `OnClose`。每个打分点都会有一次 `OnEvent`（未命中时 `GetScore()` 是当前最高分），
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

回调在调用 `SendAudioFrame` 的那个线程上同步触发，里面不要做耗时的事。

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
    void SendAudioFrame(const std::vector<float>& data);
    void SendAudioFrame(const float* data, size_t num_samples);   // num_samples 为单通道点数
    void Stop();

    void Reset();                       // 清空音频时间、FSMN 记忆与静默期
    bool IsInitialized() const;
    bool IsStreaming() const;
    std::string GetLastError() const;   // 初始化失败的原因，成功时为空串

    void SetThreshold(float threshold);
    KwsConfig GetConfig() const;
    std::vector<std::string> GetKeywords() const;

    std::string GetEngineName() const;
    KwsBackendType GetBackendType() const;
    float GetLastScore() const;
    int GetLookaheadMs() const;         // cFSMN：240 ms
};
```

送入的帧长任意（内部按 10 ms 一跳处理），但**必须是整通道的交织数据**。

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
    engine.SendAudioFrame(frame.data(), 160);        // 单通道点数
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
for i in range(0, len(audio) - 640 + 1, 640):      # 160 帧 × 4 通道
    engine.send_audio_frame(audio[i:i + 640])
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
流式路径上的错误走 `OnError` 回调。
