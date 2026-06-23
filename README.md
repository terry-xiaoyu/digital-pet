# 电子宠物 (Digital Pet)

一个基于 Device Agent C SDK 的电子宠物固件：通过 MQTT 接收平台下发的命令，驱动
**头部、身体、尾巴三个步进电机**和**一个小风扇**，并实时上报状态（属性）、遥测与事件。

设备模型定义见 [`device-spec.json`](device-spec.json)，设备主程序见 [`src/main.c`](src/main.c)，
电机控制封装见 [`src/pet_motors.c`](src/pet_motors.c)。

---

## 硬件结构

| 部位 | 驱动 | 电机 / 引脚 | 控制方式 |
|------|------|-------------|----------|
| 头部 | 步进电机 (`pwm_RoHS`) | motor 1：step=46 dir=43 enable=42 stop=83 | 位置模式，绕 90° 中心转动 |
| 身体 | 步进电机 (`pwm_RoHS`) | motor 2：step=34 dir=35 enable=36 stop=82 | 位置模式，绕 90° 中心转动 |
| 尾巴 | 步进电机 (`pwm_RoHS`) | motor 3：step=37 dir=38 enable=39 stop=61 | 位置模式，绕 90° 中心转动 |
| 风扇 | 直流电机 (`pwm_gpio`) | 72 号引脚 | 速度模式，占空比 0–100 |

电机驱动位于 [`motor_pwm/`](motor_pwm/)，依赖 **libgpiod**（仅 Linux 目标板可用）。
用法参考 [`motor_pwm/tests/test_motor_pwm.c`](motor_pwm/tests/test_motor_pwm.c)。

---

## 数据模型

### 属性 (properties) — 设备状态

| 属性 | 类型 | 说明 |
|------|------|------|
| `head_tilting` / `body_turning` / `tail_swinging` | bool | 头/身/尾是否处于偏转姿态 |
| `head_tilt_direction` / `body_turn_direction` / `tail_swing_direction` | enum | 方向：`left` / `right` |
| `head_tilt_amplitude` / `body_turn_amplitude` / `tail_swing_amplitude` | enum | 幅度：`large` / `medium` / `small` |
| `fan_speed` | enum | 风扇档位：`fast` / `medium` / `slow` / `off` |

**每次状态改变后**，设备都会同时上报 `status`、`telemetry` 状态快照，并发出对应的 `*_state_changed` 事件。

### 命令 (commands)

| 命令 | 参数 | 动作 |
|------|------|------|
| `head_tilt` | `direction`, `amplitude`, `speed` | 头部歪向一侧并保持 |
| `head_shake` | `amplitude`, `speed`, `count`(0–3) | 头部摇动若干次后回正 |
| `head_reset` | — | 头部回正 |
| `body_turn` | `direction`, `amplitude`, `speed` | 身体转向一侧并保持 |
| `body_reset` | — | 身体回正 |
| `tail_swing` | `direction`, `amplitude`, `speed` | 尾巴摆向一侧并保持 |
| `tail_wag` | `amplitude`, `speed`, `count`(0–3) | 尾巴摇动若干次后回正 |
| `tail_reset` | — | 尾巴回正 |
| `set_fan_speed` | `speed`(含 `off`) | 设置风扇档位，`off` 关闭 |

参数缺失或非法时返回 `code:400 invalid params`；未知命令返回 `code:404 unknown command`；
成功返回 `code:0 ok`，并在 `data` 中回传最新状态快照。

### 事件 (events)

`head_state_changed`、`body_state_changed`、`tail_state_changed`、`fan_state_changed`，
均为 `info` 类型，载荷反映变化后的状态。

---

## 命令到电机的映射

定义在 [`src/pet_motors.c`](src/pet_motors.c)，可按实际机械结构调整：

- **角度**：中心 90°；幅度 `small` / `medium` / `large` → 偏移 ±10° / ±20° / ±30°
  （落在步进电机 `constant_range=60` 的 [60°, 120°] 行程内）。`left` = 角度减小，`right` = 角度增大。
- **速度**：`slow` / `medium` / `fast` → 步进速度 1 / 2 / 3（驱动内部限幅 1–10）。
- **风扇**：`off` / `slow` / `medium` / `fast` → 占空比 0 / 33 / 66 / 100，`off` 用 `IDLE` 停转。
- **摇动 (shake/wag)**：以幅度为半径在中心两侧往返 `count` 次，结束时回正。

---

## 目录结构

```
.
├── CMakeLists.txt            根构建文件
├── device-spec.json          设备模型：属性 / 命令 / 事件
├── .env.example              环境变量参考
├── include/device_agent/     SDK 头文件 (client / voice / vision)
├── src/
│   ├── main.c                设备主程序：命令分发、状态/事件上报
│   ├── pet_motors.c / .h      电机抽象层（封装 motor_pwm）
│   ├── client.c              MQTT 实现 (libmosquitto)
│   ├── voice_client.c        语音实现 (libwebsockets)
│   └── vision_client.c       视觉实现 (libcurl)
├── motor_pwm/                电机驱动库（步进 + PWM 风扇，依赖 libgpiod）
├── examples/                 SDK 示例 (basic_device / voice_chat)
└── cmake/toolchains/         交叉编译工具链
```

---

## 依赖

### 目标板 (Linux，含真实电机控制)

```bash
sudo apt update
sudo apt install cmake build-essential pkg-config \
     libmosquitto-dev libcjson-dev libwebsockets-dev libcurl4-openssl-dev \
     libgpiod-dev
```

`libgpiod` 是驱动真实电机所必需的。

### 开发主机 (macOS — 无电机硬件)

```bash
brew install cmake mosquitto cjson libwebsockets curl
```

macOS 上没有 `libgpiod`，电机控制会自动退化为**日志桩**（打印将要执行的动作而不操作硬件），
便于在本机调试 MQTT 命令分发与状态上报逻辑。

> 仅需 MQTT 时可加 `-DDA_BUILD_VOICE=OFF`（免 libwebsockets）和 `-DDA_BUILD_VISION=OFF`（免 libcurl）。

---

## 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)
```

或 `make`。产物：

| 目标 | 路径 |
|------|------|
| 电子宠物固件 | `build/device` |
| SDK 静态库 | `build/libdevice_agent_sdk.a` |

### 电机控制开关

`DA_PET_MOTORS`（默认 `ON`）：检测到 `libgpiod` 时编入 [`motor_pwm/`](motor_pwm/) 库并定义
`PET_HAVE_MOTORS` 驱动真实电机；否则打印警告并使用日志桩。可用 `-DDA_PET_MOTORS=OFF` 强制关闭。

> 步进电机首次运行会通过限位开关自动归中标定，并把每个电机的角度/最大步数写入
> `/root/.motor_<n>_cur_angle`、`/root/.motor_<n>_max_steps`。

### 交叉编译 (Linux → arm64)

```bash
cmake -B build-arm64 -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-arm64.cmake
cmake --build build-arm64
```

详见 [`cmake/toolchains/linux-arm64.cmake`](cmake/toolchains/linux-arm64.cmake) 注释。

---

## 配置

通过环境变量配置（默认值已内置）。复制 `.env.example` 为 `.env` 后按需修改：

```bash
export MQTT_BROKER_URL=mqtt://127.0.0.1:1883
export NAMESPACE=default
export PRODUCT_ID=product-f60mo4
export DEVICE_ID=product-f60mo4-device-da15ec6584ef
```

完整变量（TLS、topic 模板等）见 [`.env.example`](.env.example)。可一次性加载：

```bash
set -a && source .env && set +a
```

### MQTT TLS / mTLS

- `MQTT_BROKER_URL` 设为 `mqtts://host:8883` 启用 TLS；生产环境保持 `MQTT_TLS_INSECURE=0`。
- 自定义 CA：设置 `MQTT_TLS_CA_FILE`；双向认证：同时设置 `MQTT_TLS_CERT_FILE` 与 `MQTT_TLS_KEY_FILE`。

---

## 运行

```bash
./build/device          # 或 make run
```

启动时会初始化电机（归中），连接 broker 并上报 `online` 状态，然后等待命令；`Ctrl-C` 退出时上报 `offline`。

### 用 mosquitto 发命令测试

```bash
P=product-f60mo4; D=product-f60mo4-device-da15ec6584ef
CMD="device-agent/$P/device/$D/commands"

# 订阅响应 / 遥测 / 事件
mosquitto_sub -h 127.0.0.1 -v \
  -t "device-agent/$P/device/$D/responses" \
  -t "v1/$P/$D/telemetry" -t "v1/$P/$D/event" &

# 头部向右大幅快速歪头
mosquitto_pub -h 127.0.0.1 -t "$CMD" \
  -m '{"requestId":"r1","cmd":"head_tilt","params":{"direction":"right","amplitude":"large","speed":"fast"}}'

# 风扇中速
mosquitto_pub -h 127.0.0.1 -t "$CMD" \
  -m '{"requestId":"r2","cmd":"set_fan_speed","params":{"speed":"medium"}}'

# 尾巴小幅慢速摇 2 次
mosquitto_pub -h 127.0.0.1 -t "$CMD" \
  -m '{"requestId":"r3","cmd":"tail_wag","params":{"amplitude":"small","speed":"slow","count":2}}'

# 头部回正
mosquitto_pub -h 127.0.0.1 -t "$CMD" \
  -m '{"requestId":"r4","cmd":"head_reset","params":{}}'
```

每条命令都会收到一条 response，并伴随状态快照（status + telemetry）和对应的 `*_state_changed` 事件。

---

## MQTT Topic 布局

| Topic | 方向 | 用途 |
|-------|------|------|
| `device-agent/{productId}/device/{deviceId}/commands` | ← 订阅 | 接收命令 |
| `device-agent/{productId}/device/{deviceId}/responses` | → 发布 | 命令响应 |
| `v1/{productId}/{deviceId}/telemetry` | → 发布 | 状态遥测 |
| `v1/{productId}/{deviceId}/event` | → 发布 | 设备事件 |

---

## 实现说明

- 状态保存在 [`src/main.c`](src/main.c) 的 `g_state` 中，`state_to_json()` 按 `device-spec.json`
  的属性生成快照；命令处理后统一调用 `report_state()` 上报。
- 步进动作为**同步阻塞**执行（在 MQTT 回调线程内），`head_shake count=3` 等会持续数秒，仍在
  keepalive 范围内；如需命令立即返回，可改为后台线程执行运动。
- 风扇由驱动内部线程持续输出 PWM，设置后立即返回，不阻塞。

---

## 故障排查

| 现象 | 处理 |
|------|------|
| `libgpiod not found`（CMake 警告） | 在目标板安装 `libgpiod-dev`；主机无此库时电机走日志桩属正常 |
| `libmosquitto not found` | 安装 `libmosquitto-dev` / `mosquitto-devel` / `mosquitto` (brew) |
| `libwebsockets not found` | 安装 `libwebsockets-dev` 或加 `-DDA_BUILD_VOICE=OFF` |
| `libcurl not found` | 安装 `libcurl4-openssl-dev` 或加 `-DDA_BUILD_VISION=OFF` |
| Connection refused | 确认 broker 已启动且 `MQTT_BROKER_URL` 正确 |
| 电机不动 / 角度异常 | 检查 GPIO 接线与限位开关；删除 `/root/.motor_*` 重新标定 |

低层 MQTT 诊断：运行前设置 `MQTT_DEBUG=1`，在 stderr 输出协议与 TLS 日志（含断连原因码）。
