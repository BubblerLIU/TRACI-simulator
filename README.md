# TRACI 网络结构模拟

论文 [TRACI: Network Acceleration of Input-Dynamic Communication for Large-Scale Deep Learning Recommendation Model](https://dl.acm.org/doi/10.1145/3695053.3731105) 的网络结构模拟。

## 背景概述

大规模深度学习模型（DLRMs）的一个部分称为嵌入层（Embedding Layer），涉及到查找嵌入表（Embedding Table）的操作。然而，嵌入表体积巨大，单个 GPU 的显存放不下，需要多 GPU 系统训练。

嵌入层的一个操作称为聚合（Aggregation），它将若干向量累加为一个结果向量。在多 GPU 系统中，这些向量可能分布在不同的 GPU 上，需要通过网络传输。这里存在两个可以减少网络流量的机会：

- 输入复用：某个 GPU 上的某个向量被多次请求，我们可以把它缓存在交换机上以避免重复传输；
- 输出复用：若干个来自不同 GPU 的向量被同一个 GPU 请求聚合，我们可以当这些向量在交换机上相遇时就完成聚合，只传输聚合结果。

TRACI 通过在交换机内引入 Reduction Table（RTB）和 In-Switch Cache，并引入 `GetReduce` 原语，实现了上述两个复用。

## GetReduce

`GetReduce` 原语的请求和回复格式分别为：

- `GetReduce.req(IAddr, OAddr)`
- `GetReduce.resp(IAddr, count, data[,OAddr])`

参数含义：

- `IAddr`：被聚合向量地址（远程 GPU 上的向量地址）；
- `OAddr`：聚合地址（本地 GPU 上的向量地址）；
- `count`：当前数据包内已经聚合的向量个数（输出复用计数）；
- `data`：向量数据。

## Reduction Table（RTB）

RTB 是存储在交换机里的一张表，其结构和功能如下。

### 条目结构

![RTB Entry](./assets/RTB_entry.png)

一个 RTB Entry 的结构如上图所示，各字段含义为：

- `Tag`：`GetReduce` 请求的 `OAddr`，唯一确定一个 RTB Entry;
- `Data`：聚合的向量数据；
- `Waiting count`：当前条目正在等待返回的请求数；
- `Arrived count`：当前条目已经返回并聚合的请求数。

### 归约逻辑

当一条 `GetReduce.req` 到达交换机时，交换机检查 RTB：

1. 若 RTB 内有 `Tag` 与当前请求的 `OAddr` 相同的条目，则该条目 `Waiting count += 1`;
2. 若 RTB 内没有 `Tag` 与当前请求的 `OAddr` 相同的条目，则尝试创建一个 `Tag = OAddr` 的新条目;
3. 若 RTB 已满，分为两种情况：
    - 若当前请求来自 GPU，则等待；
    - 若当前请求来自交换机，则绕过归约逻辑，执行 Baseline 逻辑。

当一条 `GetReduce.resp` 到达交换机时，交换机检查 RTB：

1. 若 RTB 内有 `Tag` 与当前回复的 `OAddr` 相同的条目，则将当前回复的数据聚合到 RTB 的 `Data` 中，并使 `Waiting count -= 1` 和 `Arrived count += 1`，然后**丢弃当前回复**;
2. 若 RTB 内没有 `Tag` 与当前回复的 `OAddr` 相同的条目，则执行 Baseline 逻辑。