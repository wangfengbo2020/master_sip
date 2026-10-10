# SIP PTT 三客户端模拟程序

这是一个用于第一阶段验证的纯 C 示例：三个 PC 客户端通过 SIP/UDP 注册并加入同一个群组，某个客户端取得讲话权后，通过 RTP/UDP 向服务端发送 Opus 音频包，服务端再把同一个 RTP 包复制给同组的另外两个客户端。

当前传输参数：

- RTP payload type：动态类型 `111`，SDP声明为 `opus/48000/1`。
- 包率：每秒40包，即每包25ms音频，RTP时间戳每包增加1200（48000 × 25ms）。
- 默认目标码率：16 kbit/s，可通过 `--opus-bitrate` 修改。
- 默认模拟payload：约50字节/包，内容为二进制测试数据。
- 每个RTP包带64位Unix毫秒发送时间戳，监听端每秒统计一次端到端延时。

> 为继续满足“不使用第三方库”，PC原型当前只模拟真实Opus编码后的包长和二进制内容，不执行PCM到Opus的编解码。接入真实音频时，用移远/MCU上的Opus编码器输出替换 `build_simulated_opus_payload()`，RTP、SIP、抢麦和服务端转发逻辑无需修改。严格实现25ms Opus包时，可将5个5ms Opus帧重打包到一个RTP包；如果编码器只提供常见的20ms帧，则建议改成每秒50包。

> 这是可运行的协议原型，只实现本场景需要的 SIP 子集，尚未包含完整 SIP 事务重传、Digest 鉴权、TLS、SRTP、NAT 穿透和生产级安全防护。

## 架构

```text
client_001 -- SIP REGISTER/INVITE/MESSAGE --+
client_002 -- SIP REGISTER/INVITE/MESSAGE --+--> ptt_server SIP :5060
client_003 -- SIP REGISTER/INVITE/MESSAGE --+

client_001 -- Opus/RTP uplink -------------> ptt_server RTP :40000
                                             |-- copy --> client_002 RTP :20002
                                             +-- copy --> client_003 RTP :20003
```

SIP用于：

- `REGISTER`：设备注册并上报自己的RTP接收端口。
- `INVITE`：加入指定群组。
- `MESSAGE` + `PTT-REQUEST`：申请讲话权。
- `MESSAGE` + `PTT-RELEASE`：释放讲话权。
- `BYE`：离开群组。

RTP用于承载实际数据。客户端之间不直接发送媒体：讲话端只把RTP发给服务端；服务端验证讲话权后，按 `group_id` 复制并转发给同组的其他成员。客户端也会拒绝任何非服务端地址发来的RTP包。

## 默认数据量估算

以单声道Opus、16 kbit/s、40包/秒、IPv4为例：

```text
Opus payload             50 bytes/包 × 40 = 2000 bytes/s = 16.00 kbit/s
RTP头+毫秒时间戳扩展      24 bytes/包
UDP头                     8 bytes/包
IPv4头                   20 bytes/包
合计                    102 bytes/包 × 40 = 4080 bytes/s = 32.64 kbit/s
每个持续媒体方向                                      约14.01 MiB/小时
```

三人组内一人讲话、两人监听时：

- 讲话设备上行约32.64 kbit/s。
- 每个监听设备下行约32.64 kbit/s。
- 服务端入口1路约32.64 kbit/s，出口2路合计约65.28 kbit/s。
- 三条媒体方向连续讲话一小时，合计IP数据约42.03 MiB。

以上不含LTE无线层、VPN、隧道和运营商侧额外开销；Opus使用VBR时，实际payload会随语音内容变化。

## 延时统计

讲话端在RTP扩展头中写入 `CLOCK_REALTIME` 的Unix毫秒时间，服务端不修改该字段，监听端收到后计算：

```text
端到端网络延时 = 接收Unix毫秒时间 - 发送Unix毫秒时间
```

监听端每秒输出包数、payload字节数、平均/最小/最大延时。例如：

```text
[client_002] [rx/1s] via=server packets=40 opus_payload=2000 bytes delay_ms avg=0.5 min=0 max=1 missing_timestamp=0
```

同一台PC上的WSL进程共用系统时钟，适合本地验证。部署到不同4G设备后，发送端和接收端必须先通过NTP或网络授时同步；否则该数值会混入设备时钟偏差，不能当作精确的单向网络延时。

## 组内唯一讲话权规则

服务端为每个 `group_id` 保存一个独立的讲话权状态：

```text
FREE
  | 收到设备A的 PTT-REQUEST
  v
OWNED_BY_A ---- 设备B/C申请 ----> 返回 BUSY，继续保持 OWNED_BY_A
  | 收到设备A的 PTT-RELEASE、BYE或超时
  v
FREE
```

- 默认所有设备都处于监听状态。
- 设备按下讲话键后先发通知，不能立即发业务数据。
- 只有收到服务端 `X-PTT-Result: GRANTED` 后才能发送RTP。
- 非讲话权持有者发送的RTP会被服务端直接丢弃。
- 持有者释放讲话权后，其他成员才可以成功申请。

## WSL依赖

以 Ubuntu WSL 为例：

```bash
sudo apt update
sudo apt install -y build-essential cmake
```

CMake只是PC端构建工具，不会链接进程序，也不是运行时依赖。如果希望直接用GCC编译：

```bash
gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Iinclude \
  src/ptt_common.c src/ptt_server.c -o ptt_server

gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Iinclude \
  src/ptt_common.c src/ptt_client.c -o ptt_client
```

## 一键运行三个客户端

在项目目录中执行：

```bash
bash scripts/run_demo.sh
```

程序会自动完成以下过程：

1. 编译服务端和客户端。
2. 启动一个服务端。
3. 启动 `client_001`、`client_002`、`client_003`。
4. 三个客户端注册并加入 `group_01`。
5. `client_001` 自动申请讲话权并发送2秒模拟Opus RTP流，每25ms一包。
6. `client_002` 在此期间申请讲话权，服务端应返回 `BUSY`，证明组内讲话权互斥。
7. `client_002` 和 `client_003` 每秒显示一次40包、2000字节以及毫秒延时统计。

日志保存在 `logs/` 目录。

正确结果应包含：

```text
[floor] group=group_01 owner=client_001 GRANTED
[floor] group=group_01 requester=client_002 BUSY owner=client_001
[rtp] route=client->server->group ... payload=50 bytes recipients=2 ... total=40
```

两个接收客户端应包含：

```text
[client_002] [rx/1s] via=server packets=40 opus_payload=2000 bytes delay_ms ...
[client_003] [rx/1s] via=server packets=40 opus_payload=2000 bytes delay_ms ...
```

## 手动运行

先编译：

```bash
cmake -S . -B build
cmake --build build -j
```

终端1启动服务端：

```bash
./build/ptt_server
```

终端2启动客户端1：

```bash
./build/ptt_client --server 127.0.0.1 --id client_001 --group group_01 --sip-port 15071 --rtp-port 20001
```

终端3启动客户端2：

```bash
./build/ptt_client --server 127.0.0.1 --id client_002 --group group_01 --sip-port 15072 --rtp-port 20002
```

终端4启动客户端3：

```bash
./build/ptt_client --server 127.0.0.1 --id client_003 --group group_01 --sip-port 15073 --rtp-port 20003
```

三个客户端都显示 `state=IDLE` 后，在任意一个客户端终端输入：

```text
talk + Enter       通知服务器本机要开始讲话
release + Enter    停止发送并释放讲话权
status + Enter     查看当前状态和讲话者
quit + Enter       退出
```

例如在 `client_001` 输入 `talk`：

```text
client_001: floor GRANTED, state=TALKING
client_002: PTT-LISTEN speaker=client_001, state=LISTENING
client_003: PTT-LISTEN speaker=client_001, state=LISTENING
```

随后 `client_001` 每25ms向服务端发送一个RTP包，服务端转发给另外两个客户端；监听端每秒打印一次接收包数和延时统计。

在 `client_001` 输入 `release` 后：

```text
client_002: PTT-IDLE previous_speaker=client_001, state=IDLE
client_003: PTT-IDLE previous_speaker=client_001, state=IDLE
```

之后可以在 `client_002` 或 `client_003` 输入 `talk`，由它取得下一次讲话权。


## Android Demo

Android工程位于 `android/`，PC客户端与Android通过JNI共用 `native/` 下的C代码。
使用Android Studio打开该目录，首次同步需要安装：

- Android SDK Platform 35
- Android SDK Build-Tools
- NDK (Side by side)
- CMake 3.22.1

页面只要求输入组名，设备ID由Android系统ID生成，本地SIP/RTP端口由操作系统动态分配。
连接服务端 `123.57.79.87`（SIP/UDP 5060、RTP/UDP 40000）后，可完成加入组、
抢麦、占用提示、讲话波浪动画、释放讲话权、RTP包数和延时显示。

当前Android版发送16 kbit/s、40包/秒、约50字节/包的模拟Opus负载，尚未接入麦克风采集、
真实Opus编码、抖动缓冲、解码和扬声器播放。

## 部署到 Linux 服务端

在服务端编译后运行：

```bash
./build/ptt_server --bind 0.0.0.0 --sip-port 5060 --rtp-port 40000
```

服务器安全组/防火墙需开放：

```text
UDP 5060      SIP信令
UDP 40000     RTP上行和服务端转发
```

PC客户端示例：

```bash
./build/ptt_client \
  --server 123.57.79.87 \
  --id client_001 --group group_01 \
  --sip-port 15071 --rtp-port 20001
```

服务端会根据客户端的RTP源地址和SSRC更新转发地址，可支持常见的UDP NAT映射；但正式公网版本仍应增加心跳、NAT保活、鉴权和加密。

### ECS 自动编译和重启

ARM ECS 上已经提供 `scripts/auto_deploy_server.sh` 和
`deploy/ptt-server-autodeploy.service`。systemd 服务启动后会：

1. 每 30 秒计算 `native/include/`、`native/src/`、`apps/server/` 和 `scripts/build_server_arm.sh` 的哈希。
2. 检测到新代码后，等待连续两次轮询结果一致，再使用 ECS 本机 GCC 编译。
3. 编译成功后停止旧的 `ptt_server` 并启动新版本；编译失败时保留正在运行的旧版本。
4. 如果服务进程异常退出，下一次轮询会自动重新启动。

部署新代码后不需要手动登录 ECS 执行编译。状态和日志位于：

```text
/root/master/logs/auto_deploy.log
/root/master/logs/server.log
/root/master/run/deployed.sha256
```

查看自动部署服务状态：

```bash
systemctl status ptt-server-autodeploy.service
```

## 代码入口

- `apps/server/ptt_server.c`：注册表、群组、讲话权以及RTP一对多转发。
- `apps/pc/ptt_client_main.c`：PC命令行外壳。
- `native/src/ptt_client.c`：可复用的SIP客户端状态机和RTP收发。
- `native/src/ptt_common.c`：SIP头解析和RTP封装/解析。
- `native/include/ptt_client.h`、`native/include/ptt_common.h`：公共接口。
- `android/`：调用相同C核心的Android Studio工程。

## 移植到移远4G模组

本项目没有使用任何第三方 SIP、RTP 或网络库。WSL版本依赖的只是Linux系统提供的C/POSIX接口。

设备端移植时保留：

- SIP报文生成与解析。
- `REGISTER`、`INVITE`、`MESSAGE`、`BYE`状态机。
- 组内抢麦状态。
- RTP头封装和解析。
- 序号、48kHz RTP时间戳、毫秒发送时间戳和SSRC管理。

根据具体移远型号替换：

- `socket/bind/sendto/recvfrom/select`：替换为模组OpenCPU Socket API，或者MCU侧的AT Socket适配层。
- `CLOCK_MONOTONIC`：替换为模组本地毫秒定时器。
- `CLOCK_REALTIME`：替换为经NTP/网络授时校准的Unix毫秒时钟。
- `stdin`按键：替换为GPIO按键事件。
- `build_simulated_opus_payload()`：替换为音频采集和Opus编码后的二进制帧。
- `receive_rtp()`：把收到的payload送入解码和播放缓冲区。

正式移植前需要确定移远模组的准确型号，以及采用模组OpenCPU运行程序，还是外部MCU通过UART/USB控制模组。
