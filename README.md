# Custom STM32 RTOS + FOTA 🚀

从零手写的轻量级抢占式实时操作系统，配合自定义 Bootloader，实现了一套完整的**固件在线升级 (FOTA)** 方案。

**硬件平台**：STM32F103C8T6 (ARM Cortex-M3, 64KB Flash, 20KB SRAM)  
**开发环境**：Keil MDK-ARM V5 (ARMCC V5.06) + STM32CubeMX HAL  
**上位机**：Python 3 + tkinter + PySerial

---

## ✨ 核心特性

### 1. 自定义 RTOS 内核

| 特性 | 实现 |
|------|------|
| **抢占式调度** | 16 级优先级，PendSV 汇编级上下文切换 |
| **任务状态机** | READY/RUNNING/BLOCKED/SUSPEND/DELETE 五态流转 |
| **IPC** | 二值信号量、互斥锁(优先级继承 PIP)、消息队列(环形缓冲) |
| **内存管理** | Best-fit 动态分配器，12KB 独立堆，支持合并 |
| **看门狗** | 软件 WDT (9s) + 硬件 IWDG (300ms) 双保险 |
| **异步日志** | `LOGI()` → 消息队列 → PrintTask → DMA UART 发送，业务任务零阻塞 |

### 2. FOTA 在线升级

```
Bootloader (7KB)    App (28KB)    Staging Area (28KB)    Flag (1KB)
0x08000000          0x08002000    0x08009000              0x08001C00
```

- App 和 Staging 等大(28KB)，确保任意版本固件都能完整暂存
- 更新标志位独占一个 Flash 页，擦写时零风险触碰 Bootloader
- Bootloader 从 16KB 压缩到 7KB，释放空间给业务代码

### 3. PC 上位机

- Python + tkinter 原生 GUI，毫秒级时间戳终端
- CRC16 帧校验 + CRC32 文件校验 + 3 次超时重传
- 升级数据全部走 MCU 端 PrintQueue 通道，与日志输出无冲突

---

## 🔧 项目结构

```
├── Core/                    # RTOS 内核 + 业务代码
│   ├── Inc/
│   │   ├── main.h
│   │   └── flash_update.h   # FOTA 协议定义 + Ring Buffer API
│   └── Src/
│       ├── main.c           # 业务任务 + LOGI + PrintTask
│       ├── rtos.c           # 调度器 / IPC / 内存管理 / 汇编切换
│       ├── rtos.h           # RTOS 核心数据结构和 API
│       └── flash_update.c   # FOTA 任务：帧解析 / CRC / Flash 驱动
├── My_bootloader/           # Bootloader 项目
│   └── Core/Src/main.c      # 更新标志检测 + Flash 搬运
├── PC_Tool/
│   ├── fw_updater.py        # 上位机主程序
│   └── protocol.py          # 协议编解码
├── 项目报告.md              # 完整的开发记录（含 12 个 Bug 排查实录）
├── 版本更新.md              # 逐版本变更日志与设计决策
└── README.md
```

---

## 🧠 本项目记录的 12 个 Bug 排查（详见`项目报告.md`）

| # | Bug | 根因 |
|--|-----|------|
| 1 | IROM 设置"没改成功" | 器件级描述和链接器配置是不同 XML 字段 |
| 2 | Bootloader 跳转后 HardFault | SysTick 残留 + HAL_RCC 意外开中断 |
| 3 | 标志位擦坏 Bootloader | 标志位没独占 Flash 页 |
| 4 | 擦除期间看门狗复位 | 每页 30ms 锁 Flash 总线，IWDG 300ms 饿死 |
| 5 | 按键后 HardFault | FOTA 任务 2KB 缓冲区在 1KB 栈上溢出 |
| 6 | PKT_READY 发不出 | DMA 和 PrintTask 抢 UART，HAL 返回 BUSY |
| 7 | PKT_ACK 收不到 | `HAL_UART_Receive_IT` 从未被首次调用 |
| 8 | 升级后 HardFault | Bootloader 把 `0xFFFFFFFF` 当"数据结束" |
| 9 | RTOS 适配差异 | SemaphoreGive ISR 不安全、无软定时器等 |
| 10 | 传输期间按键无响应 | `write_flash_buffer` 连续 512 次 `__disable_irq` |
| 11 | 内存分配器升级 | first-fit -> heap4 best-fit + 双向链表 + 最小碎片约束 |
| 12 | 调度器 O(n)→O(1) | for 循环扫描 → 位图 + CLZ 单周期查找最高优先级 |

---

## 🚀 快速开始

### 1. 构建

```bash
# Bootloader
打开 My_bootloader/MDK-ARM/My_bootloader.uvprojx → Rebuild All → Flash Download

# App
打开 MDK-ARM/cubetest.uvprojx → Rebuild All → Flash Download
# 编译后自动生成 MDK-ARM/cubetest/cubetest.bin
```

### 2. 串口接线

```
STM32 PA2 (TX) ←→ USB-TTL RX
STM32 PA3 (RX) ←→ USB-TTL TX
GND ←→ GND
115200-8N1
```

### 3. FOTA 升级

```bash
pip install pyserial
python PC_Tool/fw_updater.py
# 打开串口 → 选择 .bin → 开始升级
```

---

## 📌 已知约束

- `SemaphoreGive` 不能在 ISR 中调用（会意外 `__enable_irq`）
- `TaskDelete(NULL)` 不删除自身
- 无软件定时器（协议超时使用 `taskdelay` 累加计数）
- 消息队列仅 10 槽（FOTA 进度每 4KB 输出一次，避免刷屏）
