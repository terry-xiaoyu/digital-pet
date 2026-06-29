# SpacemitAudio API

音频采集和播放库，支持 C++ 和 Python。

---

## C++ API

```cpp
namespace SpacemitAudio {

// =============================================================================
// 全局配置
// =============================================================================

/**
 * 全局音频配置
 */
struct AudioConfig {
    int sample_rate = 16000;      // 采样率
    int channels = 1;             // 声道数
    int chunk_size = 3200;        // 每次回调字节数
    int capture_device = -1;      // 录音设备 (-1 = 默认)
    int player_device = -1;       // 播放设备 (-1 = 默认)
};

/**
 * 初始化全局配置 (结构体方式)
 */
void Init(const AudioConfig& config);

/**
 * 初始化全局配置 (参数方式，-1 表示保持当前值)
 */
void Init(int sample_rate = -1,
          int channels = -1,
          int chunk_size = -1,
          int capture_device = -1,
          int player_device = -1);

/**
 * 获取当前全局配置
 */
AudioConfig GetConfig();

// =============================================================================
// AudioCapture - 音频采集器 (回调模式)
// =============================================================================

class AudioCapture {
public:
    using Callback = std::function<void(const uint8_t* data, size_t size)>;

    /**
     * 构造函数
     * @param device_index 设备索引，-1 使用全局配置
     */
    explicit AudioCapture(int device_index = -1);
    ~AudioCapture();

    /**
     * [核心] 设置音频回调
     * @param cb 回调函数，接收 PCM16 little-endian 字节流
     */
    void SetCallback(Callback cb);

    /**
     * 启动采集
     * @param sample_rate 采样率，-1 使用全局配置
     * @param channels 声道数，-1 使用全局配置
     * @param chunk_size 每次回调字节数，-1 使用全局配置
     * @return 是否成功
     */
    bool Start(int sample_rate = -1, int channels = -1, int chunk_size = -1);

    void Stop();                  // 停止采集
    void Close();                 // 关闭设备
    bool IsRunning() const;       // 是否正在采集

    /**
     * 列出可用输入设备
     * @return vector of (index, name)
     */
    static std::vector<std::pair<int, std::string>> ListDevices();
};

// =============================================================================
// AudioPlayer - 音频播放器
// =============================================================================

class AudioPlayer {
public:
    /**
     * 构造函数
     * @param device_index 设备索引，-1 使用全局配置
     */
    explicit AudioPlayer(int device_index = -1);
    ~AudioPlayer();

    /**
     * 启动播放流
     * @param sample_rate 采样率，-1 使用全局配置
     * @param channels 声道数，-1 使用全局配置
     * @return 是否成功
     */
    bool Start(int sample_rate = -1, int channels = -1);

    /**
     * 启动播放流，并指定 PortAudio 输出缓冲帧数
     * @param sample_rate 采样率，-1 使用全局配置
     * @param channels 声道数，-1 使用全局配置
     * @param frames_per_buffer 输出缓冲帧数，<=0 使用默认 256
     * @return 是否成功
     */
    bool Start(int sample_rate, int channels, int frames_per_buffer);

    /**
     * [核心] 写入音频数据播放
     * @param data PCM16 little-endian 字节流
     * @param size 字节数
     * @return 是否成功
     */
    bool Write(const uint8_t* data, size_t size);
    bool Write(const std::vector<uint8_t>& data);

    /**
     * 播放 WAV 文件 (阻塞直到完成)
     * @param file_path WAV 文件路径
     * @return 是否成功
     */
    bool PlayFile(const std::string& file_path);

    void Stop();                  // 停止播放
    void Close();                 // 关闭设备
    bool IsRunning() const;       // 是否正在播放

    /**
     * 列出可用输出设备
     * @return vector of (index, name)
     */
    static std::vector<std::pair<int, std::string>> ListDevices();
};

}  // namespace SpacemitAudio
```

### C++ 示例

```cpp
#include "audio_base.hpp"
using namespace SpacemitAudio;

int main() {
    // 初始化全局配置
    Init(16000, 1, 3200, -1, -1);

    // 录音 (回调模式)
    AudioCapture capture;
    capture.SetCallback([](const uint8_t* data, size_t size) {
        // 处理 PCM16 数据
    });
    capture.Start();  // 使用全局配置
    // ...
    capture.Stop();
    capture.Close();

    // 播放
    AudioPlayer player;
    player.Start();  // 使用全局配置
    player.Write(pcm_data, size);
    player.PlayFile("audio.wav");
    player.Stop();
    player.Close();

    return 0;
}
```

---

## Python API

```python
"""
SpacemitAudio Python API
"""
import spacemit_audio
from spacemit_audio import AudioCapture, AudioPlayer

# =============================================================================
# 全局配置
# =============================================================================

def init(sample_rate: int = -1,
         channels: int = -1,
         chunk_size: int = -1,
         capture_device: int = -1,
         player_device: int = -1):
    """
    初始化全局配置 (-1 表示保持默认值)

    Args:
        sample_rate: 采样率 (默认 16000)
        channels: 声道数 (默认 1)
        chunk_size: 每次回调字节数 (默认 3200)
        capture_device: 录音设备索引 (-1 = 默认设备)
        player_device: 播放设备索引 (-1 = 默认设备)
    """
    pass

def get_config() -> dict:
    """
    获取当前全局配置

    Returns:
        {'sample_rate': 16000, 'channels': 1, 'chunk_size': 3200,
         'capture_device': -1, 'player_device': -1}
    """
    pass


# =============================================================================
# AudioCapture - 音频采集器 (回调模式)
# =============================================================================

class AudioCapture:
    """
    音频采集器 (Microphone Interface)
    使用回调模式接收音频数据
    """
    def __init__(self, device_index: int = -1):
        """
        Args:
            device_index: 设备索引，-1 使用全局配置
        """
        pass

    def set_callback(self, callback: callable):
        """
        [核心] 设置音频回调函数

        Args:
            callback: 回调函数，签名 callback(data: bytes)
                      data 为 PCM16 little-endian 字节流

        Example:
            def on_audio(data: bytes):
                asr.send(data)
            capture.set_callback(on_audio)
        """
        pass

    def start(self, sample_rate: int = -1, channels: int = -1, chunk_size: int = -1) -> bool:
        """
        启动录音流

        Args:
            sample_rate: 采样率，-1 使用全局配置
            channels: 声道数，-1 使用全局配置
            chunk_size: 每次回调字节数，-1 使用全局配置

        Returns:
            bool: 是否成功启动
        """
        pass

    def stop(self):
        """停止录音"""
        pass

    def close(self):
        """关闭设备并释放资源"""
        pass

    def is_running(self) -> bool:
        """是否正在录音"""
        pass

    @staticmethod
    def list_devices() -> list:
        """
        列出可用输入设备

        Returns:
            list of (index, name) tuples
        """
        pass

    # Context Manager
    def __enter__(self): return self
    def __exit__(self, exc_type, exc_val, exc_tb): self.close()


# =============================================================================
# AudioPlayer - 音频播放器
# =============================================================================

class AudioPlayer:
    """
    音频播放器 (Speaker Interface)
    """
    def __init__(self, device_index: int = -1):
        """
        Args:
            device_index: 设备索引，-1 使用全局配置
        """
        pass

    def start(self, sample_rate: int = -1, channels: int = -1,
              frames_per_buffer: int = -1) -> bool:
        """
        启动播放流

        Args:
            sample_rate: 采样率，-1 使用全局配置
            channels: 声道数，-1 使用全局配置
            frames_per_buffer: 输出缓冲帧数，<=0 使用默认 256

        Returns:
            bool: 是否成功启动
        """
        pass

    def write(self, data: bytes) -> bool:
        """
        [核心] 写入音频数据播放

        Args:
            data: PCM16 little-endian 字节流

        Returns:
            bool: 是否成功写入

        Example:
            for chunk in tts.stream():
                player.write(chunk)
        """
        pass

    def play_file(self, file_path: str) -> bool:
        """
        播放 WAV 文件 (阻塞直到播放完毕)

        Args:
            file_path: WAV 文件路径

        Returns:
            bool: 是否成功播放
        """
        pass

    def stop(self):
        """停止播放"""
        pass

    def close(self):
        """关闭设备并释放资源"""
        pass

    def is_running(self) -> bool:
        """是否正在播放"""
        pass

    @staticmethod
    def list_devices() -> list:
        """
        列出可用输出设备

        Returns:
            list of (index, name) tuples
        """
        pass

    # Context Manager
    def __enter__(self): return self
    def __exit__(self, exc_type, exc_val, exc_tb): self.close()
```

### Python 示例

```python
import time
import spacemit_audio
from spacemit_audio import AudioCapture, AudioPlayer

# 初始化配置
spacemit_audio.init(sample_rate=16000, channels=1, chunk_size=3200)

# 录音示例 (回调模式)
chunks = []
def on_audio(data: bytes):
    chunks.append(data)
    print(f"\rReceived {len(data)} bytes", end="")

with AudioCapture() as cap:
    cap.set_callback(on_audio)
    cap.start()
    time.sleep(3)

# 播放示例
with AudioPlayer() as player:
    player.start()
    for chunk in chunks:
        player.write(chunk)
```

---

## 数据格式

- **格式**: PCM16 little-endian
- **字节序**: 小端 (Intel)
- **每样本**: 2 bytes (int16)

```
chunk_size = sample_rate * channels * 2 * duration
3200 bytes = 16000 * 1 * 2 * 0.1s (100ms)
```
