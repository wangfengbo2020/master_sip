# 基于 SIP/RTP 的 4G 集群对讲系统阶段报告

**报告日期：** 2026-10-06  
**项目阶段：** PC/WSL 原型、ARM ECS 服务端和自动部署验证完成  
**目标设备：** 移远 4G 模组（后续移植）

## 1. 项目背景与目标

本项目面向电池供电的 4G 对讲设备，设备以“组”为单位通信。一个组内同一时间只允许一个设备讲话，其他设备处于监听状态。讲话设备按下讲话键后，先向服务器申请讲话权；服务器批准后，讲话设备才开始发送音频数据。所有音频都经过服务器转发，客户端之间不直接传输媒体。

本阶段的目标是：

1. 使用纯 C 实现一个不依赖第三方 SIP/RTP 库的可运行原型。
2. 在 WSL 上模拟三个客户端，在 ARM64 ECS 上运行服务器。
3. 验证 SIP 注册、入组、抢麦、释放讲话权和组内一对多 RTP 转发。
4. 模拟每秒 40 个 Opus RTP 包，并统计发送到接收的毫秒级延时。
5. 解决客户端位于 WSL/NAT 后时，服务器无法正确转发 RTP 的问题。
6. 将 ECS 服务端改为 systemd 自启动，并支持自动编译、自动重启。

## 2. 需求分析

### 2.1 业务需求

- 三个对讲机加入同一个群组，例如 `group_01`。
- 任意一个客户端输入 `talk`，向服务器申请讲话权。
- 同一组只有一个讲话者，其他客户端只能监听。
- 讲话者输入 `release` 后释放讲话权。
- 讲话者异常退出或超时后，服务器自动释放讲话权。
- 所有 RTP 音频必须经过服务器转发。
- 客户端拒绝来自非服务器地址的 RTP 数据。

### 2.2 性能和功耗需求

- 音频目标格式为 Opus。
- 当前测试按每秒 40 包，即每包 25 ms 音频进行模拟。
- 目标码率为 16 kbit/s。
- 电池供电，功耗是首要约束。
- 原型阶段不引入第三方 SIP/RTP 库，便于后续移植到移远模组或外部 MCU。
- 空闲状态下尽量减少网络保活频率，当前 RTP NAT 保活间隔为 10 秒。

## 3. 总体技术方案

### 3.1 协议分工

SIP 负责控制面：

- `REGISTER`：客户端向服务器注册，并上报 RTP 接收端口。
- `INVITE`：加入指定群组。
- `MESSAGE + PTT-REQUEST`：申请讲话权。
- `MESSAGE + PTT-RELEASE`：释放讲话权。
- `BYE`：退出群组。
- `PTT-LISTEN`：服务器通知其他成员进入监听状态。
- `PTT-IDLE`：服务器通知其他成员讲话结束。

RTP/UDP 负责媒体面：

- 讲话客户端只向服务器 RTP 端口发送数据。
- 服务器验证发送者是否持有讲话权。
- 服务器将同一 RTP 包复制给同组其他客户端。
- 监听客户端只接受来自服务器的 RTP 包。

### 3.2 数据流

```text
client_001 -- SIP REGISTER/INVITE/MESSAGE --> ECS SIP :5060
client_002 -- SIP REGISTER/INVITE/MESSAGE --> ECS SIP :5060
client_003 -- SIP REGISTER/INVITE/MESSAGE --> ECS SIP :5060

讲话客户端 -- RTP/UDP --> ECS RTP :40000
                         ECS 转发 --> client_002 RTP
                         ECS 转发 --> client_003 RTP
```

客户端之间不建立 RTP 直连，所有媒体统一走服务器：

```text
客户端 -> ECS 服务端 -> 同组其他客户端
```

### 3.3 抢麦状态机

```text
FREE
  | client_001 发送 PTT-REQUEST
  v
OWNED_BY_client_001
  | client_002/client_003 申请
  |-----------------------> 返回 BUSY
  |
  | client_001 RELEASE、BYE 或超时
  v
FREE
```

服务器为每个 `group_id` 保存独立讲话权。只有收到 `GRANTED` 后，客户端才允许发送 RTP；非讲话者发送的 RTP 会被丢弃。

## 4. 音频和带宽估算

当前模拟参数：

- Opus 目标码率：16 kbit/s。
- 包率：40 包/秒。
- 每包音频时长：25 ms。
- 模拟 Opus payload：约 50 字节/包。
- RTP 时间戳：每包增加 1200，基于 48 kHz 时钟。
- 每个 RTP 包携带 64 位 Unix 毫秒发送时间戳。

单路媒体方向估算：

```text
Opus payload：50 字节 × 40 = 2000 字节/秒 = 16.00 kbit/s
RTP 头及时间戳扩展：约 24 字节/包
UDP 头：8 字节/包
IPv4 头：20 字节/包

总计：102 字节/包 × 40 = 4080 字节/秒
带宽：约 32.64 kbit/s
流量：约 14.01 MiB/小时/单向媒体流
```

三人组内一人讲话、两人监听时：

- 讲话端上行：约 32.64 kbit/s。
- 每个监听端下行：约 32.64 kbit/s。
- ECS 入口：约 32.64 kbit/s。
- ECS 出口：约 65.28 kbit/s。

当前 PC 原型只模拟 Opus 编码后的二进制包长，并未执行 PCM 到 Opus 的实际编码。后续接入真实音频时，只需替换模拟 payload 生成函数和接收端解码播放部分，SIP、RTP、抢麦和服务器转发逻辑可以继续复用。

## 5. 延时统计设计

讲话客户端在 RTP 扩展头中写入发送时刻的 Unix 毫秒时间戳。服务器转发时不修改该字段，监听端收到后计算：

```text
单向端到端延时 = 接收时间戳 - 发送时间戳
```

监听端每秒统计一次：

- 收到的包数。
- Opus payload 总字节数。
- 平均延时。
- 最小延时。
- 最大延时。
- 缺少时间戳的包数。

在不同真实设备上使用该指标前，需要通过 NTP 或运营商网络授时同步设备时钟，否则结果会混入发送端和接收端之间的时钟偏差。当前 WSL 客户端共用同一台 PC 的系统时钟，适合原型测试。

## 6. NAT/RTP 转发问题及解决方案

### 6.1 原始问题

WSL 客户端在 SIP/SDP 中声明的地址类似：

```text
198.18.0.1:20002
```

该地址并不是 ECS 可以直接访问的真实 UDP 地址。服务器虽然执行 `sendto()` 成功并打印 `recipients=2`，但数据实际发送到了错误的地址，因此 client_002 和 client_003 收不到 RTP。

### 6.2 解决方式

客户端加入群组后发送 RTP Keepalive：

- 使用同一个 RTP socket 发送，便于服务器看到真实 NAT 映射。
- payload type 使用 `126`，与 Opus payload type 区分。
- 加入群组后立即发送一次。
- 空闲监听状态每 10 秒发送一次。
- 讲话状态由正常 RTP 数据维持 NAT 映射，不额外发送 Keepalive。

服务器处理 RTP 时：

1. 根据 RTP 源地址或 SSRC 查找客户端。
2. 如果发现 SSRC 对应的真实源地址发生变化，则更新客户端 RTP endpoint。
3. payload type `126` 只用于学习地址，不转发。
4. payload type `111` 的 Opus 数据才进入讲话权检查和组内转发。

该方案在不引入 ICE/STUN/TURN 的情况下，解决了本次 WSL/NAT 测试中的 RTP 回传问题，同时将空闲保活控制在较低频率。

## 7. 软件实现

### 7.1 代码结构

- `include/ptt_common.h`：公共协议常量、SIP/RTP 结构和接口。
- `src/ptt_common.c`：SIP 解析、RTP 封装和 RTP 解析。
- `src/ptt_server.c`：注册表、群组、讲话权、RTP 一对多转发和 endpoint 学习。
- `src/ptt_client.c`：SIP 客户端状态机、命令行控制、抢麦、RTP 发送和接收统计。
- `scripts/build_server_arm.sh`：ECS ARM64 本机 GCC 编译脚本。
- `scripts/auto_deploy_server.sh`：自动检测、编译和重启脚本。
- `deploy/ptt-server-autodeploy.service`：systemd 自启动单元。

### 7.2 构建方式

WSL 本地客户端：

```bash
cmake -S . -B build
cmake --build build -j
```

ECS ARM64 服务端：

```bash
cd /root/master
./scripts/build_server_arm.sh
```

服务端编译使用 ECS 本机 GCC，不依赖第三方 SIP、RTP 或音频库。

## 8. ECS 自动部署和自启动

当前 ECS 使用：

```text
systemd: ptt-server-autodeploy.service
项目目录：/root/master
SIP：UDP 5060
RTP：UDP 40000
检查周期：30 秒
```

服务启动后执行以下流程：

1. 计算 `include/`、`src/` 和构建脚本的摘要。
2. 首次启动时进行一次 ARM 本机编译。
3. 后续每 30 秒检测代码是否变化。
4. 新代码连续两次检测一致后开始编译。
5. 编译成功后停止旧服务端并启动新服务端。
6. 编译失败时保留原有服务端，不影响当前运行版本。
7. 服务端异常退出时自动尝试重新启动。

当前已验证：

```text
systemctl is-enabled ptt-server-autodeploy.service -> enabled
systemctl is-active ptt-server-autodeploy.service  -> active
UDP 5060 -> listening
UDP 40000 -> listening
```

常用监控命令：

```bash
systemctl status ptt-server-autodeploy.service
journalctl -u ptt-server-autodeploy.service -f -o cat
tail -F /root/master/logs/server.log
tail -F /root/master/logs/auto_deploy.log
```

## 9. 测试过程和结果

### 9.1 本地回归测试

使用 `scripts/run_demo.sh` 启动本地服务端和三个客户端，验证内容包括：

- 三个客户端注册成功。
- 三个客户端加入 `group_01`。
- client_001 获得讲话权。
- client_002 申请讲话时收到 `BUSY`。
- 服务端将 RTP 转发给两个监听端。
- 两个监听端每秒收到约 40 个 RTP 包。
- 讲话释放后，群组回到 `IDLE`。

### 9.2 ECS 公网测试

客户端使用：

```bash
./build/ptt_client \
  --server 123.57.79.87 \
  --id client_001 \
  --group group_01 \
  --sip-port 15071 \
  --rtp-port 20001
```

client_002 和 client_003 使用不同 SIP/RTP 本地端口启动。

服务器可以观察到：

```text
[rtp] endpoint learned id=client_002 addr=公网地址:端口
[rtp] endpoint learned id=client_003 addr=公网地址:端口
[rtp] route=client->server->group ... recipients=2
```

### 9.3 实际接收结果

client_002 已观察到如下结果：

```text
[client_002] [rx/1s] via=server packets=41 opus_payload=2050 bytes delay_ms avg=6.8 min=6 max=7 missing_timestamp=0
[client_002] [rx/1s] via=server packets=40 opus_payload=2000 bytes delay_ms avg=6.7 min=6 max=7 missing_timestamp=0
[client_002] [rx/1s] via=server packets=41 opus_payload=2050 bytes delay_ms avg=6.9 min=6 max=11 missing_timestamp=0
[client_002] [rx/1s] via=server packets=41 opus_payload=2050 bytes delay_ms avg=6.9 min=6 max=8 missing_timestamp=0
[client_002] [rx/1s] via=server packets=40 opus_payload=2000 bytes delay_ms avg=6.8 min=6 max=8 missing_timestamp=0
[client_002] <- PTT-IDLE previous_speaker=client_001, state=IDLE
```

结果说明：

- `packets=40/41`：符合每秒 40 包，41 包是统计窗口边界和调度造成的正常现象。
- `opus_payload=2000/2050`：每包约 50 字节，符合模拟参数。
- 平均延时约 `6.7~6.9 ms`。
- 最大延时大部分为 7~8 ms，偶发 11 ms。
- `missing_timestamp=0`：时间戳完整。
- `PTT-IDLE`：讲话结束和状态广播正常。

截至当前阶段，PC/WSL 客户端、ECS ARM64 服务端、SIP 控制、抢麦、RTP 转发、NAT endpoint 学习、延时统计和 ECS 自启动链路已打通。

## 10. 当前限制和风险

当前版本是验证原型，不应直接作为生产版本发布，主要限制如下：

1. 当前没有真实 PCM 到 Opus 的编码，音频 payload 为模拟二进制数据。
2. SIP 只实现本场景需要的子集，尚未实现完整事务重传和严格状态机。
3. 尚未加入 SIP Digest 鉴权。
4. 尚未启用 TLS、SRTP 或其他加密机制。
5. 尚未实现正式的用户、设备和群组权限管理。
6. RTP 仍基于 UDP，暂未实现丢包重传、抖动缓冲和音频PLC策略。
7. 公网 ECS 必须正确配置安全组和主机防火墙，至少开放 UDP 5060 和 UDP 40000。
8. 真实 4G 设备之间进行单向延时测量前，需要统一时钟。
9. 当前 Keepalive 为 10 秒，实际功耗还需要在目标移远模组上测量和调优。

## 11. 下一阶段计划

### 11.1 接入真实音频

- 接入麦克风采集。
- 确定采样率、单声道/双声道和帧长。
- 集成移远模组或 MCU 侧 Opus 编码器。
- 将真实 Opus 帧替换当前模拟 payload。
- 接收端增加抖动缓冲、Opus 解码和播放。

### 11.2 移植到移远模组

需要先确认具体移远型号和运行方式：

- 模组 OpenCPU 直接运行业务程序；或
- 外部 MCU 通过 UART/USB 控制模组进行 Socket 通信。

需要替换的适配层包括：

- `socket/bind/sendto/recvfrom/select`：替换为模组 Socket API 或 AT Socket。
- `CLOCK_MONOTONIC`：替换为模组毫秒定时器。
- `CLOCK_REALTIME`：替换为经过网络授时的 Unix 时间。
- `stdin` 命令：替换为 GPIO 按键事件。
- 模拟 Opus payload：替换为音频采集和编码输出。
- RTP 接收：接入解码和播放缓冲区。

### 11.3 功耗和可靠性测试

- 对比空闲、监听、讲话三个状态的 4G 电流。
- 测量 10 秒、20 秒和 30 秒 Keepalive 对功耗的影响。
- 测试弱信号、网络切换、UDP 丢包和服务器重启恢复。
- 增加注册刷新、网络断线重连和服务器不可达重试策略。
- 评估是否需要 MQTT/HTTPS 作为设备管理通道，SIP/RTP 只承担实时对讲。

### 11.4 安全加固

- SIP Digest 鉴权。
- 设备身份和群组权限校验。
- TLS/SRTP 或端到端加密。
- 防止伪造 PTT-REQUEST 和 RTP 注入。
- 限制单设备注册数量和异常流量。

## 12. 阶段结论

本阶段已经完成一个纯 C 的 SIP/RTP 组呼原型，并在 WSL 三客户端和 ARM64 ECS 环境完成公网链路验证。当前系统能够实现：

```text
客户端注册
  -> 加入群组
  -> 申请讲话权
  -> 服务器仲裁
  -> 讲话端发送 RTP
  -> ECS 一对多转发
  -> 监听端接收并统计延时
  -> 释放讲话权
```

当前实测监听端平均延时约 7 ms，服务器 UDP 5060/40000 正常监听，ECS 自动部署服务已设置为 `enabled` 和 `active`。下一步工作的核心不是继续修改 SIP/RTP 基础流程，而是接入真实 Opus 音频、完成移远模组适配、进行功耗测量和生产级安全加固。
