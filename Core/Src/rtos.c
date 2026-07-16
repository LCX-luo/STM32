#include "rtos.h"
#include "usart.h"
#include "gpio.h"
#include "string.h"
#include "iwdg.h"  
#include <stdio.h>
/************************ 宏定义与内部声明 ************************/


// 前置声明系统内部的空闲任务
static void IdleTask_Entry(void* arg);

/************************ 全局变量定义 ************************/
unsigned int OsRunningTime_ms = 0;
uint8_t OS_Running = 0;               // 0代表系统未启动，1代表已启动
volatile uint32_t sw_wdg_counter = 0; // 软件看门狗计数器
// 就绪数组，每个元素是一条双向链表头
TaskList *readyList[Max_PRIORITY];
// 下一个要执行的任务
TaskList *next_task_ptr = NULL;
// 运行中任务
TaskList *runninglist;
// 阻塞态任务
TaskList *blockedlist;
// 挂起态任务
TaskList *suspendlist;
// 等待彻底销毁的任务链表
TaskList *tasksWaitingTermination = NULL;

/************************ 内存管理 ************************/
static unsigned char my_rtos_heap[RTOS_HEAP_SIZE];

typedef struct MemBlock
{
    struct MemBlock *next;
    uint32_t size;
} MemBlock_t;

static MemBlock_t *freeListHead = NULL;

void my_os_heap_init(void)
{
    freeListHead = (MemBlock_t *)my_rtos_heap;
    freeListHead->next = NULL;
    freeListHead->size = RTOS_HEAP_SIZE;
}

void *my_os_malloc(uint32_t size)
{
    if (size == 0)
        return NULL;

    uint32_t total_size = (size + 7) & ~7;
    total_size += sizeof(MemBlock_t);

    MemBlock_t *prev = NULL;
    MemBlock_t *curr = freeListHead;
    MemBlock_t *best_block = NULL;
    MemBlock_t *best_block_prev = NULL;

    __disable_irq();

    while (curr != NULL)
    {
        if (curr->size >= total_size)
        {
            best_block = curr;
            best_block_prev = prev;
            break;
        }
        prev = curr;
        curr = curr->next;
    }

    if (best_block == NULL)
    {
        __enable_irq();
        return NULL;
    }

    if (best_block->size - total_size >= sizeof(MemBlock_t) + 8)
    {
        MemBlock_t *new_free_block = (MemBlock_t *)((uint8_t *)best_block + total_size);
        new_free_block->size = best_block->size - total_size;
        new_free_block->next = best_block->next;
        best_block->size = total_size;

        if (best_block_prev == NULL)
            freeListHead = new_free_block;
        else
            best_block_prev->next = new_free_block;
    }
    else
    {
        if (best_block_prev == NULL)
            freeListHead = best_block->next;
        else
            best_block_prev->next = best_block->next;
    }

    __enable_irq();
    return (void *)((uint8_t *)best_block + sizeof(MemBlock_t));
}

void my_os_free(void *ptr)
{
    if (ptr == NULL)
        return;

    MemBlock_t *block_to_free = (MemBlock_t *)((uint8_t *)ptr - sizeof(MemBlock_t));
    __disable_irq();

    MemBlock_t *curr = freeListHead;
    MemBlock_t *prev = NULL;

    while (curr != NULL && curr < block_to_free)
    {
        prev = curr;
        curr = curr->next;
    }

    block_to_free->next = curr;
    if (prev == NULL)
        freeListHead = block_to_free;
    else
        prev->next = block_to_free;

    if (block_to_free->next != NULL &&
        (uint8_t *)block_to_free + block_to_free->size == (uint8_t *)block_to_free->next)
    {
        block_to_free->size += block_to_free->next->size;
        block_to_free->next = block_to_free->next->next;
    }

    if (prev != NULL &&
        (uint8_t *)prev + prev->size == (uint8_t *)block_to_free)
    {
        prev->size += block_to_free->size;
        prev->next = block_to_free->next;
    }

    __enable_irq();
}

/************************ 核心链表操作 ************************/

void taskMoveInReady(TaskList *newTask)
{   
    if (newTask == NULL) {
        return;
    }

    // 【修正 2】：第一步，必须无条件先将状态改为就绪态
    newTask->taskTCB.task_state = READY;

    // 只有系统已经开始调度了，才进行 VIP 抢占判定
    if (runninglist != NULL)
    {
        // 如果新任务优先级大于正在运行的任务
        if (newTask->taskTCB.priority > runninglist->taskTCB.priority)
        {
            // 如果 VIP 席位空缺，或者新任务优先级比当前 VIP 还要高
            if (next_task_ptr == NULL || newTask->taskTCB.priority > next_task_ptr->taskTCB.priority)
            {
                // 【修正 1】：剥离相互矛盾的 if 嵌套
                if (next_task_ptr != NULL) {
                    // 原来的 VIP 退位，作为普通任务重新走一遍本函数，挂入链表
                    taskMoveInReady(next_task_ptr);
                }
                
                // 新皇登基
                next_task_ptr = newTask;
                
                // 悬起 PendSV 请求调度
                SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
                
                // VIP 任务已在专属指针中就位，不需要挂入双向链表，直接返回
                return; 
            }
        }
    }

    // ============================================
    // 下方为普通任务（或被淘汰的旧 VIP）的链表插入逻辑
    // ============================================
    unsigned int priority = newTask->taskTCB.priority;

    if (readyList[priority] == NULL)
    {
        readyList[priority] = newTask;
        newTask->prev = newTask;
        newTask->next = newTask;
    }
    else
    {
        TaskList *head = readyList[priority];
        TaskList *tail = head->prev;
        tail->next = newTask;
        newTask->prev = tail;
        newTask->next = head;
        head->prev = newTask;
    }
}

// 统一的任务摘除函数：在临界区内使用，不可阻塞！
void taskMoveOutList(TaskList *task)
{
    if (task == NULL)
        return;

    TaskStateTypeDef state = task->taskTCB.task_state;

    // 运行态或已删除态不在任何可调度链表中
    if (state == RUNNING || state == DELETE)
        return;

    if (state == READY)
    {
        // 循环双向链表剥离
        if (task->next == task)
        {
            readyList[task->taskTCB.priority] = NULL;
        }
        else
        {
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
        // 线性双向链表剥离
        if (task->prev != NULL)
        {
            task->prev->next = task->next;
        }
        else
        {
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

    task->prev = NULL;
    task->next = NULL;
}

/************************ 内部系统任务 ************************/

// 操作系统专属后台空闲任务 (最低优先级)
static void IdleTask_Entry(void* arg)
{
    while (1)
    {
        TaskList *toDelete = NULL;

        // 1. 喂狗：只要空闲任务能运行，说明没有高优先级任务死锁卡死 CPU
        sw_wdg_counter = 0;

        // 2. 检查是否有需要收尸的任务
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

        // 3. 执行真正的内存释放 (退出临界区后再操作，防止阻塞调度)
        if (toDelete != NULL)
        {
            my_os_free(toDelete->taskTCB.stack_base);
            my_os_free(toDelete);
        }

        // 4. 可以选填：单片机进入低功耗模式
        // __WFI();
    }
}

/************************ 任务与调度 API ************************/

// 修改 rtos.h 中的声明
TaskList *TaskCreate(void (*taskFunction)(void *), void *arg, unsigned int priority, unsigned char *TaskName)
{
    if (taskFunction == NULL || priority >= Max_PRIORITY)
        return NULL;
    TaskList *newTask = (TaskList *)my_os_malloc(sizeof(TaskList));
    if (newTask == NULL)
        return NULL;

    unsigned int *taskStack = (unsigned int *)my_os_malloc(TASK_DEFAULT_STACK_SIZE * sizeof(unsigned int));
    if (taskStack == NULL)
    {
        my_os_free(newTask);
        return NULL;
    }

    newTask->taskTCB.stack_ptr = taskStack + TASK_DEFAULT_STACK_SIZE;
    newTask->taskTCB.stack_base = taskStack;
    newTask->taskTCB.delay_ms = 0;
    newTask->taskTCB.priority = priority;
    newTask->taskTCB.task_state = READY;

    for (int i = 1; i <= 16; i++)
    {
        newTask->taskTCB.stack_ptr--;
        if (i == 1)
            *newTask->taskTCB.stack_ptr = 0x01000000;
        else if (i == 2)
            *newTask->taskTCB.stack_ptr = (unsigned int)taskFunction;
        else if (i == 3)
            *newTask->taskTCB.stack_ptr = 0xFFFFFFFD;
        else if(i==8)
            *newTask->taskTCB.stack_ptr =   arg==NULL? 0x00000000:(unsigned int)arg;
        else
            *newTask->taskTCB.stack_ptr = 0x00000000;
    }

    for (int i = 0; i < TASK_NAME_MAX_LENGTH - 1; i++)
    {
        newTask->taskName[i] = TaskName[i];
        if (TaskName[i] == '\0')
            break;
    }
    newTask->taskName[TASK_NAME_MAX_LENGTH - 1] = '\0';

    newTask->next = NULL;
    newTask->prev = NULL;

    taskMoveInReady(newTask);

    char *msg = "createtask\r\n";
    HAL_UART_Transmit(&huart2, (uint8_t *)msg, strlen(msg), HAL_MAX_DELAY);
    return newTask;
}

void TaskSwitch(void)
{   
    __disable_irq();
    if (next_task_ptr != NULL)
    {
        if (runninglist != NULL)
        {
            // 只有被抢占的任务才放回就绪表，主动让出的任务状态不是RUNNING
            if (runninglist->taskTCB.task_state == RUNNING)
            {
                runninglist->taskTCB.task_state = READY;
                taskMoveInReady(runninglist);
            }
        }
        runninglist = next_task_ptr;
        runninglist->taskTCB.task_state = RUNNING;
        next_task_ptr = NULL;
        __enable_irq();
        return;
    }

    int highest_ready_prio = -1;
    for (int i = Max_PRIORITY - 1; i >= 0; i--)
    {
        if (readyList[i] != NULL)
        {
            highest_ready_prio = i;
            break;
        }
    }

    if (highest_ready_prio == -1){
        __enable_irq();
         return;
    }
       

    if (runninglist != NULL && runninglist->taskTCB.task_state == RUNNING)
    {
        if (runninglist->taskTCB.priority > highest_ready_prio){
            __enable_irq();
            return ;
        }
            

        runninglist->taskTCB.task_state = READY;
        taskMoveInReady(runninglist);
    }

    runninglist = readyList[highest_ready_prio];
    taskMoveOutList(runninglist); // 统一的移出逻辑

    runninglist->taskTCB.task_state = RUNNING;
    __enable_irq();
}

extern void TaskSwitch(void);
extern TaskList *runninglist;

#ifndef __INTELLISENSE__
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

void StartScheduler(void)
{
    // 1. 关门！在点火准备期间，绝不允许任何中断（包括 SysTick）来捣乱
    __disable_irq();

    NVIC_SetPriority(PendSV_IRQn, 15);
    NVIC_SetPriority(SysTick_IRQn, 15);
    __set_PSP(0);

    OsRunningTime_ms = 0;
    sw_wdg_counter = 0;

    // 2. 创建内部任务。此时因为中断关闭，里面的串口打印绝对安全
    TaskCreate(IdleTask_Entry, NULL,0, (unsigned char *)"OS_Idle");
// 2. 创建内部任务。增加严格的返回值校验！
    TaskList* idle_task = TaskCreate(IdleTask_Entry, NULL,0, (unsigned char *)"OS_Idle");
    
    // 如果由于堆内存不足导致空闲任务创建失败，直接在此处将系统宕机锁死，防止引发不可控的连环崩溃
    if (idle_task == NULL) {
        __disable_irq();
        while(1) {
            // 在实际工业产品中，这里可以点亮一个红灯，或者向串口直喷一个 "OOM Error"
        }
    }
    // （可选：加了换行符，终端才能立刻显示）
    char *msg = "time=0\r\n";
    HAL_UART_Transmit(&huart2, (uint8_t *)msg, strlen(msg), HAL_MAX_DELAY);

    // 3. 所有的底层打印和准备都完成了，设置系统运行标志
    OS_Running = 1;

    // 4. 这时候再启动 SysTick 定时器
    SysTick_Config(SystemCoreClock / 1000);

    // 5. 手动挂起 PendSV，索要第一次任务调度
    SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;

    // 6. 开门！迎接 PendSV 抢占，正式切入任务态！
    __enable_irq();

    while (1)
    {
        // 永远不会走到这里
    }
}

void SysTick_Handler(void)
{
    HAL_IncTick();
    // ==========================================
    // 【核心修复】：如果 RTOS 还没有点火启动，立刻滚回去！
    // 绝对不允许往下执行任何与任务调度相关的逻辑
    // ==========================================
    if (OS_Running == 0)
    {
        return; 
    }
    OsRunningTime_ms++;
    sw_wdg_counter++; // 软件看门狗计数增加
    HAL_IWDG_Refresh(&hiwdg); // 中断看门狗刷新
    // 检查软件看门狗是否超时
    if (sw_wdg_counter > WDG_TIMEOUT_MS)
    {
        char err_msg[64];
        char *task_name = (runninglist != NULL) ? (char *)runninglist->taskName : "NULL";
        sprintf(err_msg, "\r\nSW_WDT_TIMEOUT! running task: %s\r\n", task_name);

        // =================================================================
        // 【核心修改】：抛弃 HAL_UART_Transmit，直接轮询寄存器暴力发送
        // 这样可以彻底无视 HAL 库的 BUSY 锁状态和 Tick 冻结问题
        // =================================================================
        for (int i = 0; err_msg[i] != '\0'; i++)
        {
            // 等待 TXE (Transmit Data Register Empty) 置位，表示可以发送下一个字节
            while (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_TXE) == RESET)
                ;
            // 直接将字符强行塞入硬件发送数据寄存器 (DR)
            huart2.Instance->DR = err_msg[i];
        }

        // 等待 TC (Transmission Complete) 置位，确保最后一个字符安全飞出引脚
        while (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_TC) == RESET)
            ;

        // 遗言发送完毕，安心上路
        __set_FAULTMASK(1);
        NVIC_SystemReset();
    }

    if (blockedlist == NULL)
    {
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
        return;
    }

    __disable_irq();

    while (blockedlist != NULL && blockedlist->taskTCB.delay_ms <= OsRunningTime_ms)
    {
        TaskList *wakeTask = blockedlist;
        blockedlist = wakeTask->next;

        if (blockedlist != NULL)
            blockedlist->prev = NULL;
        wakeTask->next = NULL;
        wakeTask->prev = NULL;

        wakeTask->taskTCB.task_state = READY;

        
        taskMoveInReady(wakeTask);
    }
    __enable_irq();
    SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
}

/************************ 状态控制 API ************************/

void taskdelay(unsigned int ms)
{
    if (ms == 0)
    {
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
        return;
    }

    TaskList *delayTask = runninglist;
    if (delayTask == NULL)
        return;

    __disable_irq();

    delayTask->taskTCB.task_state = BLOCKED;
    delayTask->taskTCB.delay_ms = ms + OsRunningTime_ms;

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
    SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
}

void suspendTask(TaskList *task)
{
    __disable_irq();

    if (task == NULL)
        task = runninglist;

    taskMoveOutList(task);
    task->taskTCB.task_state = SUSPEND;

    task->next = suspendlist;
    task->prev = NULL;
    if (suspendlist != NULL)
        suspendlist->prev = task;
    suspendlist = task;

    __enable_irq();

    if (task == runninglist)
    {
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
    }
}

void resumeTask(TaskList *task)
{
    if (task == NULL)
        return;

    __disable_irq();

    if (task->taskTCB.task_state == SUSPEND)
    {
        taskMoveOutList(task);
        task->taskTCB.task_state = READY; // 必须先改状态！
        taskMoveInReady(task);
    }
    __enable_irq();
}

void TaskDelete(TaskList *task)
{
    __disable_irq();

    if (task == NULL)
    {
        __enable_irq();
        return;
    }

    // 物理摘除并改状态
    taskMoveOutList(task);
    task->taskTCB.task_state = DELETE;

    // 头插法放入等待回收链表
    task->next = tasksWaitingTermination;
    task->prev = NULL;
    if (tasksWaitingTermination != NULL)
        tasksWaitingTermination->prev = task;
    tasksWaitingTermination = task;

    // 如果是删除自己，必须立刻触发调度放弃 CPU
    if (task == runninglist)
    {
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
    }
    __enable_irq();
}
Semaphore_t *SemaphoreCreate(unsigned char initialCount)
{
    // 从你手写的堆内存中分配空间
    Semaphore_t *newSem = (Semaphore_t *)my_os_malloc(sizeof(Semaphore_t));
    if (newSem == NULL)
        return NULL;

    // 初始化资源数量 (二值信号量只能是 0 或 1)
    newSem->count = (initialCount > 0) ? 1 : 0;
    newSem->waitList = NULL; // 初始状态下没有任务等待

    return newSem;
}
void SemaphoreTake(Semaphore_t *sem)
{
    if (sem == NULL)
        return;

    __disable_irq(); // 必须关中断，保护系统链表和状态的完整性

    if (sem->count == 1)
    {
        // 1. 资源可用，直接拿走，不触发任何调度
        sem->count = 0;
        __enable_irq();
        return;
    }
    else
    {
        // 2. 资源不可用，当前任务必须阻塞自己
        TaskList *waitTask = runninglist;

        // 改变任务状态
        waitTask->taskTCB.task_state = BLOCKED;

        // --- 链表操作：将自己加入到信号量的 waitList 中 (按优先级排序，高在前) ---
        TaskList *curr = sem->waitList;
        TaskList *prev_node = NULL;

        // 寻找插入位置：遍历链表直到找到一个优先级比自己低的节点
        while (curr != NULL && curr->taskTCB.priority >= waitTask->taskTCB.priority)
        {
            prev_node = curr;
            curr = curr->next;
        }

        if (prev_node == NULL)
        {
            // 情况1：链表为空，或者当前任务优先级最高，插入到表头
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
            // 情况2：插入到 prev_node 之后，curr 之前
            waitTask->next = curr;
            waitTask->prev = prev_node;
            prev_node->next = waitTask;
            if (curr != NULL)
            {
                curr->prev = waitTask;
            }
        }

        // 3. 悬起 PendSV，请求立刻调度！
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;

        __enable_irq(); // 开门，让 PendSV 进来抢占 CPU

        // ==========================================================
        // 【关键逻辑点】：当前任务会在这里“停住”。
        // 当未来某一天，其他任务调用 SemaphoreGive 唤醒它时，
        // 它的代码会从这里继续往下执行！
        // ==========================================================

        // 既然醒来了，说明拿到信号量了，为了严谨，再次确保 count 为 0
        // (注：由于二值信号量的特性，通常是被 Give 直接转移了所有权，不用再减)
    }
}
void SemaphoreGive(Semaphore_t *sem)
{
    if (sem == NULL)
        return;

    __disable_irq();

    // 检查是否有人在等这个信号量
    if (sem->waitList != NULL)
    {
        // 1. 有人等！把队头的任务摘下来
        TaskList *wakeTask = sem->waitList;

        // 将 wakeTask 从 waitList 中脱离
        sem->waitList = wakeTask->next;
        if (sem->waitList != NULL)
        {
            sem->waitList->prev = NULL;
        }
        wakeTask->next = NULL;
        wakeTask->prev = NULL;
        wakeTask->taskTCB.task_state = READY;

        

        // 2. 将其放回系统的就绪链表
        taskMoveInReady(wakeTask);
    }
    else
    {
        // 没人等，那就把钥匙放在桌上（变成可用状态）
        sem->count = 1;
    }

    __enable_irq();
}
Mutex_t *MutexCreate(void)
{
    Mutex_t *newMutex = (Mutex_t *)my_os_malloc(sizeof(Mutex_t));
    if (newMutex == NULL)
        return NULL;

    newMutex->count = 1;          // 初始状态可用
    newMutex->waitList = NULL;    // 没人等待
    newMutex->owner = NULL;       // 还没人拿锁
    newMutex->owner_priority = 0; // 默认0

    return newMutex;
}
void MutexTake(Mutex_t *mutex)
{
    if (mutex == NULL)
        return;

    __disable_irq(); // 关中断保护临界区

    if (mutex->count == 1)
    {
        // 1. 锁可用，当前任务直接拿走
        mutex->count = 0;
        mutex->owner = runninglist;                            // 宣誓主权！
        mutex->owner_priority = runninglist->taskTCB.priority; // 记下我本来的优先级
        __enable_irq();
        return;
    }
    else
    {
        // 2. 锁不可用，触发优先级继承判定 (PIP 算法核心)
        if (runninglist->taskTCB.priority > mutex->owner->taskTCB.priority)
        {
            // 如果持有锁的 owner 状态是就绪态，我们需要把它在 readyList 中的位置进行升级
            if (mutex->owner->taskTCB.task_state == READY)
            {
                taskMoveOutList(mutex->owner);                                  // 先从原来的低优就绪链表剥离[cite: 1, 4]
                mutex->owner->taskTCB.priority = runninglist->taskTCB.priority; // 拔高优先级
                taskMoveInReady(mutex->owner);                                  // 重新按高优先级插入就绪链表[cite: 1, 4]
            }
            else
            {
                // 如果它处于其他状态（其实在单核体系下，owner通常都在READY里），直接改数值即可
                mutex->owner->taskTCB.priority = runninglist->taskTCB.priority;
            }
        }

        // 3. 当前任务乖乖去排队 (逻辑与信号量几乎一致)
        TaskList *waitTask = runninglist;
        waitTask->taskTCB.task_state = BLOCKED;

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

        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk; // 悬起 PendSV 交出 CPU
        __enable_irq();

        // --- 任务在这里挂起，当被唤醒时，说明锁已经移交到它手上了 ---
    }
}
void MutexGive(Mutex_t *mutex)
{
    if (mutex == NULL)
        return;

    __disable_irq();

    // 1. 安全检查：只有持有锁的人，才有资格还锁！
    if (mutex->owner != runninglist)
    {
        __enable_irq();
        return; // 或者可以在这里加一个错误断言
    }

    // 2. 优先级恢复：如果之前因为优先级继承被拔高了，现在必须打回原形
    if (runninglist->taskTCB.priority != mutex->owner_priority)
    {
        runninglist->taskTCB.priority = mutex->owner_priority;
        // 因为优先级降下来了，可能就不如 readyList 里的某些任务了，所以必须请求一次调度
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
    }

    // 3. 处理排队的任务
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

        wakeTask->taskTCB.task_state = READY;
        taskMoveInReady(wakeTask);
        
    }
    else
    {
        // 没人排队，彻底释放
        mutex->count = 1;
        mutex->owner = NULL; // 锁变为无主状态
    }

    __enable_irq();
}
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

    newQueue->maxItems = maxItems;
    newQueue->itemSize = itemSize;
    newQueue->head = 0;
    newQueue->tail = 0;
    newQueue->count = 0;
    newQueue->txWaitList = NULL;
    newQueue->rxWaitList = NULL;
    return newQueue;
}
uint8_t QueueSend(Queue_t *queue, void *item)
{
    if (queue == NULL || item == NULL)
        return 0;

    __disable_irq(); // 1. 关中断，保护系统资源和环形缓冲区

    // 2. 核心防御：如果队列满了，当前任务必须阻塞！
    // 为什么用 while 而不是 if？这是 RTOS 的黄金法则：
    // 任务被唤醒后，可能由于高优抢占，空间又被别人占了，所以醒来后必须重新检查是否真的有空位。
    while (queue->count >= queue->maxItems)
    {
        TaskList *waitTask = runninglist;
        waitTask->taskTCB.task_state = BLOCKED;

        // --- 将自己按优先级插入生产者的候车室 (txWaitList) ---
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

        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk; // 悬起调度
        __enable_irq();                      // 开门交出CPU，任务在这里沉睡...

        // --- 任务在这里被消费者唤醒，说明有空位了！ ---
        __disable_irq(); // 醒来后第一件事，重新关门，回去执行 while 检查
    }

    // 3. 数据拷贝：此时肯定有空位，把用户数据拷贝进水池的 tail（尾部）位置
    unsigned char *writePtr = queue->buffer + (queue->tail * queue->itemSize);
    memcpy(writePtr, item, queue->itemSize);

    // 4. 环形指针推演：尾巴向前走一步，如果走到尽头，用取模(%)绕回头部
    queue->tail = (queue->tail + 1) % queue->maxItems;
    queue->count++; // 水池水量 +1

    // 5. 唤醒消费者：如果有任务在等数据，叫醒优先级最高的那个
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

    __enable_irq(); // 发送完毕，开门
    return 1;
}
uint8_t QueueReceive(Queue_t *queue, void *buffer)
{
    if (queue == NULL || buffer == NULL)
        return 0;

    __disable_irq();

    // 1. 如果水池空了，想喝水的任务必须阻塞排队
    while (queue->count == 0)
    {
        TaskList *waitTask = runninglist;
        waitTask->taskTCB.task_state = BLOCKED;

        // --- 将自己按优先级插入消费者的候车室 (rxWaitList) ---
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

        // --- 沉睡... 直到有生产者发了数据唤醒它 ---
        __disable_irq();
    }

    // 2. 数据拷贝：此时肯定有数据，从水池的 head（头部）把数据拷贝到用户的 buffer 中
    unsigned char *readPtr = queue->buffer + (queue->head * queue->itemSize);
    memcpy(buffer, readPtr, queue->itemSize);

    // 3. 环形指针推演：读指针向前走一步，水量 -1
    queue->head = (queue->head + 1) % queue->maxItems;
    queue->count--;

    // 4. 唤醒生产者
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
