#include "rtos.h"
#include "usart.h"
#include "gpio.h"
#include "string.h"
#include "iwdg.h"  
#include <stdio.h>
/************************ 全局变量声明区域 / Global Variable Declaration Area ************************/


// 空闲任务入口函数声明 / Idle task entry function declaration
/* IdleTask (priority 0): Feeds SW watchdog, reclaims deleted task memory. */
static void IdleTask_Entry(void* arg);

/************************ 系统全局运行变量 / System Global Runtime Variables ************************/
unsigned int OsRunningTime_ms = 0;
uint8_t OS_Running = 0;               // 系统运行标志位，0未启动1已启动 / System running flag, 0:not running 1:running
volatile uint32_t sw_wdg_counter = 0; // 软件看门狗计时计数器 / Software watchdog tick counter
TaskList *idle_task_ptr = NULL; // IdleTask 指针，用于 CPU 空闲率精确判定
// 多优先级就绪任务链表数组 / Ready task linked list array for multi-priority
TaskList *readyList[Max_PRIORITY];
// 待抢占调度任务指针 / Preempt pending task pointer
TaskList *next_task_ptr = NULL;
// 当前正在运行的任务节点 / Currently running task node
TaskList *runninglist;
// 延时阻塞任务链表头 / Blocked delay task list head
TaskList *blockedlist;
// 手动挂起任务链表头 / Suspended task list head
TaskList *suspendlist;
// 等待资源回收的已删除任务链表头 / Deleted task list waiting for memory recycle
TaskList *tasksWaitingTermination = NULL;
uint16_t os_ready_bitmap = 0; // 就绪优先级位图，用于O(1)查询最高优先级 / Ready priority bitmap for O(1) highest priority lookup
volatile uint32_t idle_tick_count = 0; // SysTick 中 IdleTask 运行次数计数（≈空闲 ms）

/* ======================== 内存管理模块(heap4风格:最佳适配+双向链表+最小内存块约束) / Memory Manager Module (heap4 style: best-fit + doubly linked list + min block constraint) ======================== */
static unsigned char my_rtos_heap[RTOS_HEAP_SIZE];

#define HEAP_MIN_BLOCK_SIZE  ((uint32_t)(sizeof(struct MemBlock) + 8))
// 堆内存最小分配块大小 / Minimum alloc heap block size

typedef struct MemBlock
{
    struct MemBlock *next;  // 下一块内存节点指针 / Next memory block pointer
    struct MemBlock *prev;  // 上一块内存节点指针 / Previous memory block pointer
    uint32_t size;          // 当前内存块总大小 / Total size of current memory block
} MemBlock_t;

static MemBlock_t *freeListHead = NULL; // 空闲内存链表头指针 / Free memory list head pointer

// RTOS堆内存初始化 / RTOS heap memory initialization
void my_os_heap_init(void)
{
    freeListHead = (MemBlock_t *)my_rtos_heap;
    freeListHead->next = NULL;
    freeListHead->prev = NULL;
    freeListHead->size = RTOS_HEAP_SIZE;
}

// RTOS内存分配函数，最佳适配算法 / RTOS memory allocate function, best-fit algorithm
void *my_os_malloc(uint32_t size)
{
    if (size == 0)
        return NULL;

    uint32_t total_size = (size + 7) & ~7; // 向上8字节对齐 / Align size up to 8 bytes
    total_size += sizeof(MemBlock_t);      // 追加内存块头部占用空间 / Add memory block header size

    if (total_size < HEAP_MIN_BLOCK_SIZE)
        total_size = HEAP_MIN_BLOCK_SIZE; // 不小于最小分配块 / Limit size no less than min block

    __disable_irq(); // 关闭全局中断，防止堆操作被打断 / Disable global irq to protect heap operation

    MemBlock_t *best = NULL;
    MemBlock_t *curr = freeListHead;
    uint32_t best_size = 0xFFFFFFFF;

    // 遍历空闲链表查找最优适配块 / Traverse free list to find best fit block
    while (curr != NULL)
    {
        if (curr->size >= total_size && curr->size < best_size)
        {
            best = curr;
            best_size = curr->size;
        }
        curr = curr->next;
    }

    if (best == NULL)
    {
        __enable_irq();
        return NULL; // 无足够空闲内存分配失败 / No enough free memory, malloc failed
    }

    // 分割空闲块，剩余空间满足最小块要求则拆分 / Split block if remaining space meets min size
    if (best->size - total_size >= HEAP_MIN_BLOCK_SIZE)
    {
        MemBlock_t *new_free = (MemBlock_t *)((uint8_t *)best + total_size);
        new_free->size = best->size - total_size;
        new_free->next = best->next;
        new_free->prev = best->prev;
        if (new_free->next) new_free->next->prev = new_free;
        if (new_free->prev) new_free->prev->next = new_free;
        else freeListHead = new_free;
        best->size = total_size;
    }
        else
    {
        // 剩余空间不足，整块分配，从空闲链表移除 / Remain space too small, allocate whole block, remove from free list
        if (best->prev) best->prev->next = best->next;
        else freeListHead = best->next;
        if (best->next) best->next->prev = best->prev;
}

    best->next = NULL;
    best->prev = NULL;
    __enable_irq(); // 恢复全局中断 / Re-enable global irq
    return (void *)((uint8_t *)best + sizeof(MemBlock_t)); // 返回可用内存起始地址 / Return usable data address
}

// RTOS内存释放函数，释放后自动合并相邻空闲块 / RTOS memory free function, auto merge adjacent free blocks
void my_os_free(void *ptr)
{
    if (ptr == NULL) return;
    MemBlock_t *block = (MemBlock_t *)((uint8_t *)ptr - sizeof(MemBlock_t)); // 反向获取内存块头部 / Get block header from data pointer
    __disable_irq(); // 关中断保护堆操作 / Disable irq for heap safety

    // 按地址升序插入空闲链表 / Insert block into free list by address ascending
    MemBlock_t *prev = NULL;
    MemBlock_t *curr = freeListHead;
    while (curr != NULL && curr < block)
    {
        prev = curr;
        curr = curr->next;
    }

    block->next = curr;
    block->prev = prev;
    if (prev) prev->next = block;
    else freeListHead = block;
    if (curr) curr->prev = block;

    // 合并后向相邻空闲块 / Merge next adjacent free block
    if (curr && (uint8_t *)block + block->size == (uint8_t *)curr)
    {
        block->size += curr->size;
        block->next = curr->next;
        if (curr->next) curr->next->prev = block;
    }

    // 合并前向相邻空闲块 / Merge previous adjacent free block
    if (prev && (uint8_t *)prev + prev->size == (uint8_t *)block)
    {
        prev->size += block->size;
        prev->next = block->next;
        if (block->next) block->next->prev = prev;
}

    __enable_irq(); // 恢复中断 / Re-enable irq
}

/* 获取当前空闲堆总大小 | Get free heap size */
uint32_t my_os_get_free_heap(void)
{
    uint32_t total = 0;
    __disable_irq();
    MemBlock_t *curr = freeListHead;
    while (curr != NULL)
    {
        total += curr->size;
        curr = curr->next;
    }
    __enable_irq();
    return total;
}

/************************ 任务链表操作函数 / Task Linked List Operation Functions ************************/

/* taskMoveInReady: 将任务移入就绪链表，更新优先级位图，高优先级抢占快速通路 / Insert task into readyList, update bitmap. VIP path for preemption. */
void taskMoveInReady(TaskList *newTask)
{   
    if (newTask == NULL) {
        return;
    }

    // 设置任务状态为就绪 / Set task state to READY
    newTask->taskTCB.task_state = READY;

    // 抢占快速VIP通路判断 / Preemption fast VIP path judge
    if (runninglist != NULL)
    {
        // 新任务优先级高于当前运行任务，触发抢占 / New task priority higher than running task, trigger preemption
        if (newTask->taskTCB.priority > runninglist->taskTCB.priority)
        {
            // 更新待调度高优先级任务指针 / Update pending highest priority task pointer
            if (next_task_ptr == NULL || newTask->taskTCB.priority > next_task_ptr->taskTCB.priority)
            {
                // 原有待抢占任务放回就绪链表 / Put old pending preempt task back to ready list
                if (next_task_ptr != NULL) {
                    taskMoveInReady(next_task_ptr);
                }
                
                // 标记当前高优先级任务为待调度任务 / Mark new high prio task as pending switch task
                next_task_ptr = newTask;
                
                // 触发PendSV异常，执行上下文切换 / Trigger PendSV exception for context switch
                SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
                
                // VIP抢占通路直接返回，不进入普通就绪链表插入逻辑 / VIP path return directly, skip normal ready list insert
                return; 
    }
        }
    }

    // ============================================
    // 普通流程：任务无抢占，插入对应优先级就绪环形链表 / Normal path: no preemption, insert into priority ready circular list
    // ============================================
    unsigned int priority = newTask->taskTCB.priority;

    // 当前优先级就绪链表为空，初始化环形链表 / Ready list of this priority empty, init circular list
    if (readyList[priority] == NULL)
    {
        readyList[priority] = newTask;
        newTask->prev = newTask;
        newTask->next = newTask;
        os_ready_bitmap |= (1 << priority); // 置位优先级位图标记 / Set bit in ready priority bitmap
    }
    else
    {
        // 插入环形链表尾部 / Insert to tail of circular ready list
        TaskList *head = readyList[priority];
        TaskList *tail = head->prev;
        tail->next = newTask;
        newTask->prev = tail;
        newTask->next = head;
        head->prev = newTask;
    }
}

// 将任务从当前所属链表中移除，就绪链表清空时更新位图 / Remove task from its current list, clear bitmap bit if ready list empty
/* taskMoveOutList: Remove task from its current list. Update bitmap if READY list goes empty. */
void taskMoveOutList(TaskList *task)
{
    if (task == NULL)
        return;

    TaskStateTypeDef state = task->taskTCB.task_state;

    // 运行态/已删除任务不执行移除操作 / Skip remove for RUNNING / DELETE state task
    if (state == RUNNING || state == DELETE)
        return;

    if (state == READY)
        {
        // 就绪链表移除逻辑 / Remove from ready circular list
        if (task->next == task)
            {
            // 当前优先级仅这一个任务，清空链表、清除位图位 / Only one task in this prio, clear list and bitmap bit
            readyList[task->taskTCB.priority] = NULL;
            os_ready_bitmap &= ~(1 << task->taskTCB.priority);
        }
        else
        {
            // 多任务环形链表移除节点 / Remove node from multi-task circular list
            if (readyList[task->taskTCB.priority] == task)
            {
                readyList[task->taskTCB.priority] = task->next;
    }
            task->prev->next = task->next;
            task->next->prev = task->prev;
        }
        }
    else if (state == BLOCKED || state == SUSPEND)
        {
        // 阻塞/挂起单向链表移除逻辑 / Remove from blocked/suspend single linked list
        if (task->prev != NULL)
        {
            task->prev->next = task->next;
        }
        else
        {
            // 当前节点是链表头，更新链表头指针 / Current node is list head, refresh list head
            if (state == BLOCKED)
                blockedlist = task->next;
            else if (state == SUSPEND)
                suspendlist = task->next;
        }

        if (task->next != NULL)
        {
            task->next->prev = task->prev;
        }
}

    // 清空任务前后指针，脱离原链表 / Clear task prev/next pointer to detach from list
    task->prev = NULL;
    task->next = NULL;
}

/************************ 空闲任务实现 / Idle Task Implementation ************************/

// 空闲任务入口函数（优先级0）：喂软件看门狗、回收已删除任务内存 / Idle task entry (priority 0): feed sw watchdog, recycle deleted task memory
/* IdleTask (priority 0): Feeds SW watchdog, reclaims deleted task memory. */
static void IdleTask_Entry(void* arg)
{
    while (1)
    {
        TaskList *toDelete = NULL;

        // 1. 重置软件看门狗计数器，标记CPU正常运行 / Reset sw watchdog counter, mark CPU alive
        sw_wdg_counter = 0;
        // 2. 取出待回收的删除任务节点 / Fetch deleted task node waiting for recycle
        __disable_irq();
            if (tasksWaitingTermination != NULL)
            {
            toDelete = tasksWaitingTermination;
            tasksWaitingTermination = toDelete->next;
            if (tasksWaitingTermination != NULL)
        {
                tasksWaitingTermination->prev = NULL;
            }
        }
        __enable_irq();

        // 3. 释放任务栈与TCB内存 / Free task stack and TCB memory
        if (toDelete != NULL)
        {
            my_os_free(toDelete->taskTCB.stack_base);
            my_os_free(toDelete);
    }

        // 4. 空闲时休眠CPU（可选，注释关闭）/ Sleep CPU when idle (optional, commented out)
        //LOGI("sleep");
         __WFI();
    }
}

/************************ 系统对外API接口 / System Public API Functions ************************/

// 任务创建函数声明定义，栈帧初始化（Cortex-M异常栈布局）/ TaskCreate definition, init Cortex-M exception stack frame
/*
 * TaskCreate 初始化新任务伪栈帧 / Initialize fake stack frame for a new task
 *
 * 栈布局（高地址至低地址）/ Stack layout after init (high to low):
 *     +------------------+  <- stack_ptr 初始指向此处 / stack_ptr initially here
 *     |      xPSR        |  0x01000000 (Thumb模式位必须置1 / Thumb bit set)
 *     +------------------+
 *     |      PC          |  任务函数入口地址 / taskFunction address
 *     +------------------+
 *     |      LR          |  0xFFFFFFFD (返回线程模式PSP / EXC_RETURN to PSP)
 *     +------------------+
 *     |      R12         |  初始0 / initialized to 0
 *     +------------------+
 *     |      R3 - R0     |  R0 = 任务入参 / R0 = task entry argument
 *     +------------------+
 *     |      R11 - R4    |  全部初始0 / all initialized to 0
 *     +------------------+  <- 首次PendSV触发时PSP指向该位置 / PSP when PendSV first runs
 *
 * 首次PendSV流程：PSP为0跳过现场保存，直接调用TaskSwitch调度新任务 / On first PendSV, PSP==0 -> skip save, call TaskSwitch directly.
 * TaskSwitch选中任务后LDMIA恢复R4-R11，BX LR触发硬件自动出栈寄存器，跳转任务函数执行 / TaskSwitch picks this task -> LDMIA restores R4-R11 ->
 * BX LR -> hardware unstack -> PC = taskFunction -> task starts.
 */



/* 创建任务：分配TCB内存+任务栈，初始化Cortex-M异常栈帧 / Create new task: allocate TCB & stack, init Cortex-M exception stack frame */
TaskList *TaskCreateEX(void (*taskFunction)(void *), void *arg, unsigned int priority, unsigned int stack_words, unsigned char *TaskName)
{
    if (taskFunction == NULL || priority >= Max_PRIORITY)
        return NULL;
    TaskList *newTask = (TaskList *)my_os_malloc(sizeof(TaskList));
    if (newTask == NULL)
        return NULL;

    unsigned int *taskStack = (unsigned int *)my_os_malloc(stack_words * sizeof(unsigned int));
    if (taskStack == NULL)
    {
        my_os_free(newTask);
        return NULL;
    }

    newTask->taskTCB.stack_ptr = taskStack + stack_words; // 栈指针初始指向栈顶 / Stack ptr point to stack top
    newTask->taskTCB.stack_base = taskStack;                          // 栈内存起始地址 / Stack base address
    newTask->taskTCB.delay_ms = 0;                                    // 阻塞延时清零 / Block delay tick init 0
    newTask->taskTCB.priority = priority;                             // 设置任务优先级 / Set task priority
    newTask->taskTCB.task_state = READY;                              // 任务初始状态就绪 / Init task state READY
    newTask->taskTCB.held_mutex_count = 0;  // Init count
    // 填充异常栈帧16个寄存器位置 / Fill 16 register stack frame
    for (int i = 1; i <= 16; i++)
    {
        newTask->taskTCB.stack_ptr--;
        if (i == 1)
            *newTask->taskTCB.stack_ptr = 0x01000000; // xPSR Thumb位 / xPSR with Thumb bit
        else if (i == 2)
            *newTask->taskTCB.stack_ptr = (unsigned int)taskFunction; // PC任务入口 / PC task entry
        else if (i == 3)
            *newTask->taskTCB.stack_ptr = 0xFFFFFFFD; // LR异常返回值 / EXC_RETURN LR
        else if(i==8)
            *newTask->taskTCB.stack_ptr =   arg==NULL? 0x00000000:(unsigned int)arg; // R0任务入参 / R0 task arg
        else
            *newTask->taskTCB.stack_ptr = 0x00000000; // 其余寄存器清零 / Other regs zero
    }
/*
 * TaskCreate 初始化新任务伪栈帧 / Initialize fake stack frame for a new task
 *
 * 栈布局（高地址至低地址）/ Stack layout after init (high to low):
 *     +------------------+  <- stack_ptr 初始指向此处 / stack_ptr initially here
 *     |      xPSR        |  0x01000000 (Thumb模式位必须置1 / Thumb bit set)
 *     +------------------+
 *     |      PC          |  任务函数入口地址 / taskFunction address
 *     +------------------+
 *     |      LR          |  0xFFFFFFFD (返回线程模式PSP / EXC_RETURN to PSP)
 *     +------------------+
 *     |      R12         |  初始0 / initialized to 0
 *     +------------------+
 *     |      R3 - R0     |  R0 = 任务入参 / R0 = task entry argument
 *     +------------------+
 *     |      R11 - R4    |  全部初始0 / all initialized to 0
 *     +------------------+  <- 首次PendSV触发时PSP指向该位置 / PSP when PendSV first runs
 *
 * 首次PendSV流程：PSP为0跳过现场保存，直接调用TaskSwitch调度新任务 / On first PendSV, PSP==0 -> skip save, call TaskSwitch directly.
 * TaskSwitch选中任务后LDMIA恢复R4-R11，BX LR触发硬件自动出栈寄存器，跳转任务函数执行 / TaskSwitch picks this task -> LDMIA restores R4-R11 ->
 * BX LR -> hardware unstack -> PC = taskFunction -> task starts.
 */
    // 拷贝任务名称至TCB / Copy task name to TCB buffer
    for (int i = 0; i < TASK_NAME_MAX_LENGTH - 1; i++)
    {
        newTask->taskName[i] = TaskName[i];
        if (TaskName[i] == '\0')
            break;
    }
    newTask->taskName[TASK_NAME_MAX_LENGTH - 1] = '\0'; // 字符串结束符 / String terminator

    newTask->next = NULL;
    newTask->prev = NULL;

    taskMoveInReady(newTask); // 将新建任务加入就绪链表 / Add new task to ready list

    char *msg = "createtask\r\n";
    HAL_UART_Transmit(&huart2, (uint8_t *)msg, strlen(msg), HAL_MAX_DELAY);
    return newTask;
    }

/* TaskCreate wrapper */
TaskList *TaskCreate(void (*taskFunction)(void *), void *arg, unsigned int priority, unsigned char *TaskName)
{
    return TaskCreateEX(taskFunction, arg, priority, TASK_DEFAULT_STACK_SIZE, TaskName);
}

/* 任务调度核心函数：选择下一个待运行任务，位图+CLZ实现O(1)最高优先级查询 / TaskSwitch: Select next task to run. Uses O(1) bitmap + CLZ for priority lookup. */
void TaskSwitch(void)
{   
    __disable_irq();
    // 存在抢占待调度任务，优先处理抢占 / Preempt pending task exists, handle preemption first
    if (next_task_ptr != NULL)
    {
        if (runninglist != NULL)
    {
            // 当前运行任务标记为就绪放回就绪链表 / Mark running task READY and put back to ready list
            if (runninglist->taskTCB.task_state == RUNNING)
            {
                runninglist->taskTCB.task_state = READY;
                taskMoveInReady(runninglist);
            }
        }
        runninglist = next_task_ptr; // 切换至高优先级抢占任务 / Switch to preempt high prio task
        runninglist->taskTCB.task_state = RUNNING;
        next_task_ptr = NULL; // 清空抢占标记 / Clear preempt pending flag
        __enable_irq();
        return;
    }

    // 从就绪位图获取最高就绪优先级 / Get highest ready priority via bitmap CLZ
    int highest_ready_prio = -1;
    if (os_ready_bitmap != 0) {
        highest_ready_prio = 31 - __clz((uint32_t)os_ready_bitmap);
        }

    if (highest_ready_prio == -1){
        __enable_irq();
         return; // 无就绪任务，直接返回 / No ready task, return
    }
       
       
    // 当前运行任务优先级更高，不切换 / Running task priority higher than highest ready, skip switch
    if (runninglist != NULL && runninglist->taskTCB.task_state == RUNNING)
    {
        if (runninglist->taskTCB.priority > highest_ready_prio){
            __enable_irq();
            return ;
        }
            
            
        runninglist->taskTCB.task_state = READY;
        taskMoveInReady(runninglist); // 当前任务放回就绪链表 / Put running task back to ready list
    }

    runninglist = readyList[highest_ready_prio]; // 取出最高优先级就绪任务 / Fetch highest prio ready task
    taskMoveOutList(runninglist); // 将任务从就绪链表移除 / Remove task from ready list

    runninglist->taskTCB.task_state = RUNNING; // 更新为运行态 / Mark task RUNNING
    __enable_irq();
}


/* 任务调度函数声明 / Task switch function declaration */
void TaskSwitch(void);
extern TaskList *runninglist;

#ifndef __INTELLISENSE__
/*
 * Cortex-M3 PendSV中断服务函数：上下文切换汇编实现 / Cortex-M3 PendSV_Handler Context Switch Assembly
 *
 * 异常进入硬件自动压栈顺序（栈递减）/ Hardware auto-stack on exception entry (descending stack):
 *     高地址 / High addr
 *     +------------------+
 *     |      xPSR        |  bit24=1 Thumb位，任务创建时预设 / bit24=1 (Thumb), set by TaskCreate
 *     +------------------+
 *     |      PC          |  任务入口函数地址 / task entry address
 *     +------------------+
 *     |      LR          |  EXC_RETURN返回值0xFFFFFFFD / EXC_RETURN=0xFFFFFFFD
 *     +------------------+
 *     |      R12         |
 *     +------------------+
 *     |      R3 - R0     |  R0存储任务入参 / R0 = task function arg
 *     +------------------+
 *     |      R11 - R4    |  STMDB手动保存寄存器 / saved by STMDB R0!, {R4-R11}
 *     +------------------+  PSP保存完现场后指向此处 / PSP points here after save
 *     低地址 / Low addr
 *
 * 执行流程 / Flow:
 *   1. 保存现场：读取PSP，压入R4-R11，保存栈指针至TCB / Save: PSP->R0, STMDB R0!,{R4-R11}, save R0 to TCB->stack_ptr
 *   2. C语言调度：调用TaskSwitch选出下一个运行任务 / C call: TaskSwitch() picks next task
 *   3. 恢复现场：读取TCB栈指针，LDMIA弹出R4-R11 / Restore: load TCB->stack_ptr->R0, LDMIA R0!,{R4-R11}
 *   4. BX LR触发硬件自动出栈R0-R3,R12,LR,PC,xPSR / BX LR -> hardware unstack R0-R3,R12,LR,PC,xPSR
 */


__asm void PendSV_Handler(void)
{
    PRESERVE8
    IMPORT runninglist
    IMPORT TaskSwitch 

    MRS R0, PSP
    CBZ R0, PendSV_First_Run 

    STMDB R0!, {R4 - R11}
    LDR R1, = runninglist
    LDR R2, [R1] 
    STR R0, [R2]

    PUSH { LR } 
    BL TaskSwitch 
    POP { LR }

    B PendSV_Restore 

PendSV_First_Run 
    BL TaskSwitch 
    LDR LR, = 0xFFFFFFFD 

PendSV_Restore
    LDR R1, = runninglist
    LDR R2, [R1] 
    LDR R0, [R2]

    LDMIA R0!, {R4 - R11} 
    MSR PSP, R0

    BX LR 
    ALIGN
}
#else
void PendSV_Handler(void);
#endif

/* 启动RTOS调度器：创建空闲任务、配置中断优先级、初始化SysTick、永不返回 / StartScheduler: Create IdleTask, set PendSV/SysTick prio, start SysTick, never returns. */
void StartScheduler(void)
{
    // 1. 关闭全局中断，配置SysTick与PendSV优先级 / Disable global irq, config SysTick & PendSV priority
    __disable_irq();

    NVIC_SetPriority(PendSV_IRQn, 15); // PendSV最低中断优先级，保证不会抢占中断服务 / PendSV lowest priority, no interrupt preemption
    NVIC_SetPriority(SysTick_IRQn, 15);
    __set_PSP(0); // 初始化进程栈指针为0 / Init PSP to 0

    OsRunningTime_ms = 0;
    sw_wdg_counter = 0;

    // 2. 创建空闲任务（优先级0）/ Create idle task (priority 0)
    TaskCreateEX(IdleTask_Entry, NULL,0, 64, (unsigned char *)"OS_Idle");
// 重复创建空闲任务变量接收句柄 / Receive idle task handle
    TaskList* idle_task = TaskCreateEX(IdleTask_Entry, NULL,0, 64, (unsigned char *)"OS_Idle");
    idle_task_ptr = idle_task; // 保存指针供 CPU 占用率计算

    // 空闲任务创建失败，内存不足卡死 / Idle task create failed, OOM lockup
    if (idle_task == NULL) {
        __disable_irq();
        while(1) {
            // 内存分配失败死循环 / OOM Error infinite loop
        }
        }
    // 打印系统启动日志 / Print system startup log
    char *msg = "time=0\r\n";
    HAL_UART_Transmit(&huart2, (uint8_t *)msg, strlen(msg), HAL_MAX_DELAY);

    // 3. 标记系统已启动运行 / Mark OS running flag
    OS_Running = 1;

    // 4. 配置1ms SysTick系统节拍 / Config SysTick for 1ms tick
    SysTick_Config(SystemCoreClock / 1000);

    // 5. 触发首次PendSV切换至空闲任务 / Trigger first PendSV switch to idle task
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;

    // 6. 开启全局中断 / Enable global irq
    __enable_irq();

    while (1)
    {
        // 调度器后台空循环 / Scheduler background empty loop
    }
    }

/* SysTick中断服务函数：1ms系统节拍，更新系统计时、软件看门狗、硬件独立看门狗、唤醒延时阻塞任务 / SysTick_Handler: 1ms tick. System time, SW watchdog, IWDG refresh, wake blocked tasks. */
void SysTick_Handler(void)
{
    HAL_IncTick();
    // ==========================================
    // RTOS系统节拍处理逻辑 / RTOS tick process logic
    // 系统未启动时跳过RTOS处理 / Skip RTOS logic if OS not running
    // ==========================================
    if (OS_Running == 0)
    {
        return; 
    }
    OsRunningTime_ms++; // 系统运行毫秒计数自增 / System runtime ms counter increase
    sw_wdg_counter++; // 软件看门狗计时自增 / SW watchdog tick increase
    if (runninglist == idle_task_ptr)
        idle_tick_count++; // 当前是 IdleTask 运行 → 计为 1ms 空闲
    HAL_IWDG_Refresh(&hiwdg); // 刷新硬件独立看门狗 / Refresh hardware IWDG
    // 软件看门狗超时检测 / Software watchdog timeout check
    if (sw_wdg_counter > WDG_TIMEOUT_MS)
{
        char err_msg[64];
        char *task_name = (runninglist != NULL) ? (char *)runninglist->taskName : "NULL";
        sprintf(err_msg, "\r\nSW_WDT_TIMEOUT! running task: %s\r\n", task_name);

        // =================================================================
        // 阻塞式串口打印错误信息，避免HAL异步锁死 / Blocking UART print error, avoid HAL BUSY lock
        // 直接操作寄存器发送，不依赖HAL Tick / Direct register transmit, no HAL tick rely
        // =================================================================
        for (int i = 0; err_msg[i] != '\0'; i++)
        {
            // 等待发送数据寄存器为空 / Wait TX data register empty flag
            while (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_TXE) == RESET)
                ;
            // 写入发送寄存器 / Write char to data register
            huart2.Instance->DR = err_msg[i];
        }

        // 等待整帧发送完成 / Wait transmission complete flag
        while (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_TC) == RESET)
            ;

        // 屏蔽所有中断，系统软复位 / Mask all irq, trigger system soft reset
        __set_FAULTMASK(1);
        NVIC_SystemReset();
    }

    // 无延时阻塞任务，直接触发调度 / No blocked delay task, trigger switch directly
    if (blockedlist == NULL)
    {
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
        return;
    }

    __disable_irq();

    // 遍历阻塞链表，唤醒延时到期任务 / Traverse blocked list, wake tasks whose delay expired
    while (blockedlist != NULL && blockedlist->taskTCB.delay_ms <= OsRunningTime_ms)
    {
        TaskList *wakeTask = blockedlist;
        blockedlist = wakeTask->next;

        if (blockedlist != NULL)
            blockedlist->prev = NULL;
        wakeTask->next = NULL;
        wakeTask->prev = NULL;

        wakeTask->taskTCB.task_state = READY; // 任务切换为就绪态 / Mark task READY

        taskMoveInReady(wakeTask); // 加入就绪链表 / Add to ready list
}
    __enable_irq();
    SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk; // 触发上下文切换 / Trigger context switch
}

/************************ 延时/任务控制API / Delay & Task Control API ************************/

static void check_no_mutex_held(void)
{
    if (runninglist && runninglist->taskTCB.held_mutex_count > 0)
    {
        char err[] = "[RTOS FATAL] Blocked while holding mutex!\r\n";
        for (int i = 0; err[i]; i++)
        {
            while (!(USART2->SR & USART_SR_TXE));
            USART2->DR = err[i];
        }
        while (!(USART2->SR & USART_SR_TC));
        __disable_irq();
        while(1){}
    }
}

/* taskdelay：阻塞当前任务指定毫秒时长 / taskdelay: Block current task for ms milliseconds. */
void taskdelay(unsigned int ms)
{
    check_no_mutex_held();
    if (ms == 0)
    {
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk; // 0延时主动让出CPU / Zero delay yield CPU
        return;
    }

    TaskList *delayTask = runninglist;
    if (delayTask == NULL)
        return;

    __disable_irq();

    delayTask->taskTCB.task_state = BLOCKED; // 标记任务阻塞 / Mark task BLOCKED
    delayTask->taskTCB.delay_ms = ms + OsRunningTime_ms; // 计算唤醒系统时刻 / Calculate wake system tick

    // 按唤醒时间升序插入阻塞链表 / Insert into blocked list sorted by wake tick ascending
    TaskList *curr = blockedlist;
    TaskList *prev_node = NULL;

    while (curr != NULL && curr->taskTCB.delay_ms < delayTask->taskTCB.delay_ms)
    {
        prev_node = curr;
        curr = curr->next;
    }

    if (blockedlist == NULL)
    {
        blockedlist = delayTask;
        delayTask->prev = NULL;
        delayTask->next = NULL;
    }
    else if (prev_node == NULL)
    {
        delayTask->next = blockedlist;
        delayTask->prev = NULL;
        blockedlist->prev = delayTask;
        blockedlist = delayTask;
    }
    else
    {
        delayTask->prev = prev_node;
        delayTask->next = curr;
        prev_node->next = delayTask;
        if (curr != NULL)
            curr->prev = delayTask;
    }

    __enable_irq();
    SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk; // 触发调度让出CPU / Trigger switch to yield CPU
}

/* suspendTask：挂起指定任务，入参NULL则挂起自身 / suspendTask: Suspend a task (remove from scheduler). task==NULL -> self. */
void suspendTask(TaskList *task)
{
    __disable_irq();

    if (task == NULL)
        task = runninglist; // 空参数挂起当前运行任务 / NULL param suspend self

    taskMoveOutList(task); // 从原链表移除 / Remove from original list
    task->taskTCB.task_state = SUSPEND; // 标记挂起态 / Mark task SUSPEND

    // 插入挂起链表头部 / Insert to head of suspend list
    task->next = suspendlist;
    task->prev = NULL;
    if (suspendlist != NULL)
        suspendlist->prev = task;
    suspendlist = task;

    __enable_irq();

    if (task == runninglist)
    {
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk; // 挂起自身需要切换任务 / Suspend self, trigger switch
}
    }

/* resumeTask：恢复已挂起任务至就绪态 / resumeTask: Resume a suspended task. */
void resumeTask(TaskList *task)
{
    if (task == NULL)
        return;

    __disable_irq();

    if (task->taskTCB.task_state == SUSPEND)
    {
        taskMoveOutList(task); // 从挂起链表移除 / Remove from suspend list
        task->taskTCB.task_state = READY; // 切换就绪态 / Mark task READY
        taskMoveInReady(task); // 加入就绪链表 / Add to ready list
    }
    __enable_irq();
}

/* TaskDelete：删除任务，内存由空闲任务统一回收 / TaskDelete: Delete a task. Memory reclaimed by IdleTask. */
void TaskDelete(TaskList *task)
{
    __disable_irq();

    if (task == NULL)
    {
        __enable_irq();
        return;
    }

    // 将任务从所属链表移除 / Remove task from original list
    taskMoveOutList(task);
    task->taskTCB.task_state = DELETE; // 标记待删除状态 / Mark task DELETE

    // 插入待回收链表，交由空闲任务释放内存 / Add to recycle list, idle task free memory later
    task->next = tasksWaitingTermination;
    task->prev = NULL;
    if (tasksWaitingTermination != NULL)
        tasksWaitingTermination->prev = task;
    tasksWaitingTermination = task;

    // 当前优先级无就绪任务，清除位图标记 / Clear bitmap bit if no task left in this priority
    if (readyList[task->taskTCB.priority] == NULL) {
        os_ready_bitmap &= ~(1 << task->taskTCB.priority);
    }

    // 删除自身需要触发调度切换其他任务 / Delete running task, trigger switch
    if (task == runninglist)
    {
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
    }
    __enable_irq();
}
/* SemaphoreCreate：创建二值信号量，初始计数0/1 / SemaphoreCreate: Create a binary semaphore. count=0 or 1. */
Semaphore_t *SemaphoreCreate(unsigned char initialCount)
{
    // 分配信号量结构体内存 / Allocate semaphore struct memory
    Semaphore_t *newSem = (Semaphore_t *)my_os_malloc(sizeof(Semaphore_t));
    if (newSem == NULL)
        return NULL;

    // 限制计数仅0或1，二值信号量 / Limit count to 0 or 1, binary semaphore
    newSem->count = (initialCount > 0) ? 1 : 0;
    newSem->waitList = NULL; // 信号量阻塞等待任务链表 / Blocked waiting task list for semaphore

    return newSem;
}
/* SemaphoreTake：获取信号量，无资源则阻塞当前任务 / SemaphoreTake: Wait/block on semaphore. */
void SemaphoreTake(Semaphore_t *sem)
{
    check_no_mutex_held();
    if (sem == NULL)
        return;

    __disable_irq(); // 关中断保护信号量操作 / Disable irq for semaphore safety

    if (sem->count == 1)
    {
        // 持有资源，计数清零直接返回 / Resource available, clear count and return
        sem->count = 0;
        __enable_irq();
        return;
    }
    else
    {
        // 无资源，当前任务进入阻塞等待链表 / No resource, block current task and add to wait list
        TaskList *waitTask = runninglist;

        waitTask->taskTCB.task_state = BLOCKED;

        // --- 等待链表按优先级降序插入（高优先级在前） / Insert wait list sorted descending by priority ---
        TaskList *curr = sem->waitList;
        TaskList *prev_node = NULL;

        while (curr != NULL && curr->taskTCB.priority >= waitTask->taskTCB.priority)
        {
            prev_node = curr;
            curr = curr->next;
    }

        if (prev_node == NULL)
        {
            // 最高优先级，插入链表头部 / Highest priority, insert list head
            waitTask->next = sem->waitList;
            waitTask->prev = NULL;
            if (sem->waitList != NULL)
            {
                sem->waitList->prev = waitTask;
            }
            sem->waitList = waitTask;
        }
        else
        {
            // 插入中间节点 / Insert between prev_node and curr
            waitTask->next = curr;
            waitTask->prev = prev_node;
            prev_node->next = waitTask;
            if (curr != NULL)
            {
                curr->prev = waitTask;
            }
        }

        // 触发PendSV切换其他任务 / Trigger PendSV to switch task
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;

        __enable_irq(); // 开中断，等待其他任务释放信号量 / Re-enable irq, wait semaphore give from other task
    }
}
/* SemaphoreGive：释放二值信号量，唤醒等待链表最高优先级任务 / SemaphoreGive: Release semaphore, wake highest-prio waiter. */
void SemaphoreGive(Semaphore_t *sem)
        {
    if (sem == NULL)
        return;

    __disable_irq();

    // 存在等待任务，唤醒最高优先级阻塞任务 / Waiting task exists, wake highest priority blocked task
    if (sem->waitList != NULL)
    {
        TaskList *wakeTask = sem->waitList;

        // 将唤醒任务移出等待链表 / Remove wake task from semaphore wait list
        sem->waitList = wakeTask->next;
        if (sem->waitList != NULL)
        {
            sem->waitList->prev = NULL;
        }
        wakeTask->next = NULL;
        wakeTask->prev = NULL;
        wakeTask->taskTCB.task_state = READY;

        taskMoveInReady(wakeTask); // 唤醒任务加入就绪链表 / Add waked task to ready list
    }
    else
        {
        // 无等待任务，信号量计数置1 / No waiting task, set semaphore count to 1
        sem->count = 1;
}

    __enable_irq();
        }
/* MutexCreate：创建互斥锁，支持优先级继承PIP防优先级反转 / MutexCreate: Create mutex with Priority Inheritance support. */
Mutex_t *MutexCreate(void)
{
    Mutex_t *newMutex = (Mutex_t *)my_os_malloc(sizeof(Mutex_t));
    if (newMutex == NULL)
        return NULL;

    newMutex->count = 1;          // 互斥锁初始可用计数1 / Mutex initial available count 1
    newMutex->waitList = NULL;    // 等待互斥锁阻塞任务链表 / Blocked task wait list for mutex
    newMutex->owner = NULL;       // 锁持有任务指针 / Mutex owner task pointer
    newMutex->owner_priority = 0; // 持有者原始优先级（用于PIP恢复）/ Owner original priority for PIP restore

    return newMutex;
    }
/* MutexTake：获取互斥锁，实现优先级继承PIP机制解决优先级反转 / MutexTake: Take mutex. Implements PIP to prevent priority inversion. */
void MutexTake(Mutex_t *mutex)
{
    if (mutex == NULL)
        return;

    __disable_irq(); // 关中断保护互斥锁操作 / Disable irq for mutex safety

    if (mutex->count == 1)
    {
        // 锁空闲，当前任务持有锁 / Mutex free, current task take ownership
        mutex->count = 0;
        mutex->owner = runninglist;
        runninglist->taskTCB.held_mutex_count++;
        mutex->owner_priority = runninglist->taskTCB.priority;
        __enable_irq();
        return;
    }
    else
    {
        // 锁被占用，执行优先级继承PIP / Mutex occupied, run Priority Inheritance
        if (runninglist->taskTCB.priority > mutex->owner->taskTCB.priority)
            {
            // 等待任务优先级高于持有者，提升持有者优先级至等待任务等级 / Waiting task higher prio, boost owner prio to waiter's prio
            if (mutex->owner->taskTCB.task_state == READY)
            {
                taskMoveOutList(mutex->owner);
                mutex->owner->taskTCB.priority = runninglist->taskTCB.priority;
                taskMoveInReady(mutex->owner);
            }
            else
            {
                // 持有者非就绪态，直接修改优先级字段 / Owner not ready, modify prio field directly
                mutex->owner->taskTCB.priority = runninglist->taskTCB.priority;
            }
        }

        // 当前任务进入阻塞等待链表 / Block current task and add to mutex wait list
        TaskList *waitTask = runninglist;
        waitTask->taskTCB.task_state = BLOCKED;

        // 等待链表按优先级降序插入 / Insert wait list sorted descending by priority
        TaskList *curr = mutex->waitList;
        TaskList *prev_node = NULL;
        while (curr != NULL && curr->taskTCB.priority >= waitTask->taskTCB.priority)
        {
            prev_node = curr;
            curr = curr->next;
        }

        if (prev_node == NULL)
        {
            waitTask->next = mutex->waitList;
            waitTask->prev = NULL;
            if (mutex->waitList != NULL)
                mutex->waitList->prev = waitTask;
            mutex->waitList = waitTask;
        }
        else
        {
            waitTask->next = curr;
            waitTask->prev = prev_node;
            prev_node->next = waitTask;
            if (curr != NULL)
                curr->prev = waitTask;
    }

        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk; // 触发调度切换任务 / Trigger PendSV switch task
        __enable_irq();
    }
}
/* MutexGive：释放互斥锁，恢复持有者原始优先级，唤醒最高优先级等待任务 / MutexGive: Release mutex. Restore owner original priority if PIP was active. */
void MutexGive(Mutex_t *mutex)
{
    if (mutex == NULL)
        return;

    __disable_irq();

    // 仅允许锁持有者释放锁 / Only mutex owner can release lock
    if (mutex->owner != runninglist)
    {
        __enable_irq();
        return;
    }

    runninglist->taskTCB.held_mutex_count--;

    // 若优先级被提升，恢复原始优先级 / Restore original priority if PIP boosted prio
    if (runninglist->taskTCB.priority != mutex->owner_priority)
    {
        runninglist->taskTCB.priority = mutex->owner_priority;
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk; // 优先级变更触发抢占检测 / Prio change trigger preemption check
    }

    // 存在等待任务，转交锁所有权 / Waiting task exists, transfer mutex ownership
    if (mutex->waitList != NULL)
    {
        TaskList *wakeTask = mutex->waitList;
        mutex->waitList = wakeTask->next;
        if (mutex->waitList != NULL)
            mutex->waitList->prev = NULL;
        wakeTask->next = NULL;
        wakeTask->prev = NULL;

        mutex->owner = wakeTask;
        mutex->owner_priority = wakeTask->taskTCB.priority;
        wakeTask->taskTCB.held_mutex_count++;
        
        wakeTask->taskTCB.task_state = READY;
        taskMoveInReady(wakeTask);
        
    }
    else
    {
        // 无等待任务，锁恢复空闲状态 / No waiting task, mutex return to free
        mutex->count = 1;
        mutex->owner = NULL;
}

    __enable_irq();
    }
/* QueueCreate：创建消息环形队列，分配缓冲区内存 / QueueCreate: Create message queue with ring buffer. */
Queue_t *QueueCreate(unsigned int maxItems, unsigned int itemSize)
{
    if (maxItems == 0 || itemSize == 0)
        return NULL;

    Queue_t *newQueue = (Queue_t *)my_os_malloc(sizeof(Queue_t));
    if (newQueue == NULL)
        return NULL;

    newQueue->buffer = my_os_malloc(maxItems * itemSize);
    if (newQueue->buffer == NULL)
    {
        my_os_free(newQueue);
        return NULL;
    }

    newQueue->maxItems = maxItems;    // 队列最大存储条目数 / Max queue item count
    newQueue->itemSize = itemSize;    // 单条消息字节大小 / Single item byte size
    newQueue->head = 0;               // 环形队列读指针 / Ring buffer read head index
    newQueue->tail = 0;               // 环形队列写指针 / Ring buffer write tail index
    newQueue->count = 0;              // 当前队列有效消息数 / Current valid item count
    newQueue->txWaitList = NULL;      // 队列满时阻塞发送任务链表 / Tx blocked list when queue full
    newQueue->rxWaitList = NULL;      // 队列空时阻塞接收任务链表 / Rx blocked list when queue empty
    return newQueue;
}
/* QueueSend：向队列发送消息，队列满则阻塞发送任务 / QueueSend: Send to queue. Block if full. */
uint8_t QueueSend(Queue_t *queue, void *item)
{
    check_no_mutex_held();
    if (queue == NULL || item == NULL)
        return 0;

    __disable_irq(); // 关中断保护队列操作 / Disable irq for queue safety

    // 队列满，发送任务阻塞至有空闲空间 / Queue full, block sender until space available
    while (queue->count >= queue->maxItems)
    {
        TaskList *waitTask = runninglist;
        waitTask->taskTCB.task_state = BLOCKED;

        // --- 发送阻塞链表按优先级降序插入 / Insert tx wait list sorted descending by priority ---
        TaskList *curr = queue->txWaitList;
        TaskList *prev_node = NULL;
        while (curr != NULL && curr->taskTCB.priority >= waitTask->taskTCB.priority)
        {
            prev_node = curr;
            curr = curr->next;
        }
        if (prev_node == NULL)
        {
            waitTask->next = queue->txWaitList;
            waitTask->prev = NULL;
            if (queue->txWaitList != NULL)
                queue->txWaitList->prev = waitTask;
            queue->txWaitList = waitTask;
        }
        else
        {
            waitTask->next = curr;
            waitTask->prev = prev_node;
            prev_node->next = waitTask;
            if (curr != NULL)
                curr->prev = waitTask;
        }

        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk; // 触发调度让出CPU / Trigger switch yield CPU
        __enable_irq();

        // 重新关中断，循环判断队列状态 / Re-disable irq, recheck queue status
        __disable_irq();
    }

    // 拷贝消息至环形队列尾部 / Copy item to ring buffer tail
    unsigned char *writePtr = queue->buffer + (queue->tail * queue->itemSize);
    memcpy(writePtr, item, queue->itemSize);

    // 更新写指针环形取模，消息计数+1 / Update tail index modulo max, item count +1
    queue->tail = (queue->tail + 1) % queue->maxItems;
    queue->count++;

    // 唤醒等待接收的最高优先级任务 / Wake highest priority waiting receiver
    if (queue->rxWaitList != NULL)
    {
        TaskList *wakeTask = queue->rxWaitList;
        queue->rxWaitList = wakeTask->next;
        if (queue->rxWaitList != NULL)
            queue->rxWaitList->prev = NULL;
        wakeTask->next = NULL;
        wakeTask->prev = NULL;
        
        wakeTask->taskTCB.task_state = READY;
            taskMoveInReady(wakeTask); 
        
}

    __enable_irq();
    return 1; // 发送成功返回1 / Send success return 1
}
/* QueueReceive：从队列读取消息，队列为空则阻塞接收任务 / QueueReceive: Receive from queue. Block if empty. */
uint8_t QueueReceive(Queue_t *queue, void *buffer)
{
    check_no_mutex_held();
    if (queue == NULL || buffer == NULL)
        return 0;

    __disable_irq();

    // 队列为空，接收任务阻塞至有新消息 / Queue empty, block receiver until new item arrives
    while (queue->count == 0)
    {
        TaskList *waitTask = runninglist;
        waitTask->taskTCB.task_state = BLOCKED;

        // --- 接收阻塞任务链表插入逻辑，按优先级降序排列 / Insert logic for rx blocked task list, sorted descending by priority ---
        TaskList *curr = queue->rxWaitList;
        TaskList *prev_node = NULL;
        while (curr != NULL && curr->taskTCB.priority >= waitTask->taskTCB.priority)
        {
            prev_node = curr;
            curr = curr->next;
        }
        if (prev_node == NULL)
        {
            waitTask->next = queue->rxWaitList;
            waitTask->prev = NULL;
            if (queue->rxWaitList != NULL)
                queue->rxWaitList->prev = waitTask;
            queue->rxWaitList = waitTask;
        }
        else
        {
            waitTask->next = curr;
            waitTask->prev = prev_node;
            prev_node->next = waitTask;
            if (curr != NULL)
                curr->prev = waitTask;
        }

        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
        __enable_irq();

        // --- 任务被唤醒后重新关闭中断，再次校验队列状态 / Re-disable interrupt after task wakeup, recheck queue status ---
        __disable_irq();
    }

    // 2. 从环形队列读指针位置拷贝消息到用户缓冲区 / Copy message from ring buffer read head to user buffer
    unsigned char *readPtr = queue->buffer + (queue->head * queue->itemSize);
    memcpy(buffer, readPtr, queue->itemSize);

    // 3. 更新读指针索引，队列有效消息计数减一 / Update read head index, decrease valid message count by 1
    queue->head = (queue->head + 1) % queue->maxItems;
    queue->count--;

    // 4. 队列产生空闲位置，唤醒阻塞等待发送的任务 / Queue has free space, wake tasks blocked on sending
    if (queue->txWaitList != NULL)
    {
        TaskList *wakeTask = queue->txWaitList;
        queue->txWaitList = wakeTask->next;
        if (queue->txWaitList != NULL)
            queue->txWaitList->prev = NULL;
        wakeTask->next = NULL;
        wakeTask->prev = NULL;

        wakeTask->taskTCB.task_state = READY;
        taskMoveInReady(wakeTask);
    }
    __enable_irq();
    return 1;
}
