# Custom STM32 RTOS 🚀

本项目是一个从零手写、专为 ARM Cortex-M3 (STM32F1系列) 打造的轻量级抢占式实时操作系统 (RTOS)。结合了 STM32CubeMX 生成的 HAL 库底层驱动，实现了一套包含任务调度、内存管理、进程间通信(IPC)以及高可靠性异常保护的嵌入式软件架构。

## ✨ 核心特性 (Core Features)

### 1. 抢占式任务调度 (Preemptive Scheduler)

- **多优先级支持**：支持最多 16 个任务优先级 (`Max_PRIORITY`)，高优先级任务可瞬间抢占低优先级任务。
- **状态机流转**：完整实现了任务的就绪 (READY)、运行 (RUNNING)、阻塞 (BLOCKED)、挂起 (SUSPEND) 和删除回收 (DELETE) 状态流转。
- **底层上下文切换**：基于 ARM Cortex-M3 的 `PendSV` 异常与汇编级堆栈操控，实现极低延迟的任务切换。

### 2. 丰富的进程间通信 (IPC & Synchronization)

- **互斥锁 (Mutex)**：内置 **优先级继承协议 (PIP, Priority Inheritance Protocol)**，完美解决经典 RTOS 中的优先级翻转 (Priority Inversion) 问题。
- **二值信号量 (Binary Semaphore)**：支持中断唤醒与任务间同步。
- **消息队列 (Message Queue)**：基于环形缓冲区 (Ring Buffer) 设计，支持跨任务的数据安全搬运。

### 3. 高级应用框架 (Advanced Application Framework)

- **异步日志系统 (`LOGI`)**：类似 Linux `syslog`。业务任务只需调用 `LOGI("var: %d", x)`，字符串解析与组装在任务局部栈完成，并通过消息队列发往专门的 `PrintTask`，由 **DMA 硬件后台完成 UART 发送**，实现业务任务 0 阻塞。
- **工业级按键状态机**：内置抗机械抖动、边沿触发（屏蔽长按重复触发）的非阻塞按键扫描逻辑，完全释放 CPU 算力。

### 4. 健壮的系统保护机制 (System Robustness)

- **独立堆内存分配器**：实现了一套无碎片的动态内存分配与回收算法 (`my_os_malloc` / `my_os_free`)。
- **软件看门狗 (SW WDT)**：`IdleTask` 负责喂狗。一旦检测到高优任务死锁或 CPU 饥饿，看门狗在 `SysTick` 中断内触发。
- **硬核遗言打印**：看门狗超时后，**绕过 HAL 库状态锁**，直接轮询 MCU 硬件寄存器 (UART `TXE` / `TC`) 发送导致卡死的任务名称，随后触发 `NVIC_SystemReset()` 硬件复位。

------

## 📂 目录结构 (Directory Structure)

项目基于标准的 STM32CubeMX 目录规范与 Keil MDK-ARM 构建：

Plaintext

```
├── Core/
│   ├── Inc/
│   │   ├── main.h
│   │   └── rtos.h         # RTOS 核心数据结构与 API 声明
│   └── Src/
│       ├── main.c         # 业务逻辑、任务入口与外设初始化
│       └── rtos.c         # RTOS 内核实现 (调度器、链表操作、汇编切换)
├── Drivers/
│   ├── CMSIS/             # ARM Cortex-M 核心文件
│   └── STM32F1xx_HAL_Driver/ # ST 官方 HAL 库
├── MDK-ARM/
│   └── cubetest.uvprojx   # Keil MDK 工程文件
└── README.md
```

------

## 🛠️ 环境依赖 (Dependencies)

- **硬件目标**: STM32F103C8T6 (或任意 STM32F1 系列单片机)
- **开发环境**: Keil uVision 5 (MDK-ARM)
- **底层库**: STM32 HAL Driver (通过 STM32CubeMX 生成)
- **编译器**: ARMCC (V5.06)

------

## 🚀 快速上手 (Quick Start)

### 1. 任务创建与系统点火

在 `main.c` 中，初始化硬件后，分配内存并创建任务，最后启动调度器：

C

```
// 初始化 RTOS 堆内存与 IPC
my_os_heap_init();
PrintQueue = QueueCreate(10, sizeof(LogMsg_t));

// 创建任务：函数指针, 传入参数, 优先级(数字越大优先级越高), 任务名
TaskCreate(Task1_Entry, NULL, 2, (unsigned char *)"Task1");
TaskCreate(PrintTask_Entry, NULL, 3, (unsigned char *)"PrintTask");

// 启动调度器 (将接管 CPU，不再返回)
StartScheduler();
```

### 2. 使用异步日志系统

在任意业务任务中，就像使用 `printf` 一样使用 `LOGI`，系统会自动附带时间戳并交由后台 DMA 发送，绝对不会卡顿当前任务：

C

```
void Task1_Entry(void *arg)
{
    int loop_count = 0;
    while (1)
    {
        loop_count++;
        // 自动输出例如: "[1000] Task1 is running, count: 1"
        LOGI("Task1 is running, count: %d\r\n", loop_count);
        
        taskdelay(1000); // RTOS 非阻塞延时
    }
}
```

------

## 🧠 设计架构图解 (Architecture Notes)

- **双向链表管理**: 内核维护了 `readyList[]` (就绪数组)、`blockedlist` (阻塞链表)、`suspendlist` (挂起链表) 等，通过统一的 `taskMoveInReady` 和 `taskMoveOutList` 实现 O(1) 或 O(N) 的任务状态拔插。
- **空闲任务回收**: 处于 `DELETE` 状态的任务不会立刻释放内存，而是移交给系统最低优先级的 `OS_Idle` 任务进行内存安全回收，防止在临界区内进行耗时的内存释放操作。