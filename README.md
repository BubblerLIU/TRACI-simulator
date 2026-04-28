# TRACI 网络结构模拟

论文 [TRACI: Network Acceleration of Input-Dynamic Communication for Large-Scale Deep Learning Recommendation Model](https://dl.acm.org/doi/10.1145/3695053.3731105) 的网络结构模拟。

## 背景概述

大规模深度学习模型（DLRMs）有一个部分称为嵌入层（Embedding Layer），涉及到查找嵌入表（Embedding Table）的操作。然而，嵌入表体积巨大，单个 GPU 的显存放不下，需要多 GPU 系统训练。

嵌入层的一个操作称为聚合（Aggregation），它将若干向量累加为一个结果向量。在多 GPU 系统中，这些向量可能分布在不同的 GPU 上，需要通过网络传输。这里存在两个可以减少网络流量的机会：

1. 输入复用：某个 GPU 上的某个向量被多次请求，我们可以把它缓存在交换机上以避免重复传输；
2. 输出复用：若干个来自不同 GPU 的向量被同一个 GPU 请求聚合，我们可以当这些向量在交换机上相遇时就完成聚合，只传输聚合结果。

TRACI 网络结构通过在交换机内引入 Reduction Table（RTB）和 In-Switch Cache，并在语义层面上引入 `GetReduce` 原语，实现了上述两个复用。

## 交换机结构

