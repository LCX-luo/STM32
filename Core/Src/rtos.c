#include "rtos.h"
#include "usart.h"
#include "gpio.h"
#include "string.h"
#include "iwdg.h"  
#include <stdio.h>
/************************ 궨ڲ ************************/


// ǰϵͳڲĿ
/* IdleTask (priority 0): Feeds SW watchdog, reclaims deleted task memory. */
static void IdleTask_Entry(void* arg);

/************************ ȫֱ ************************/
unsigned int OsRunningTime_ms = 0;
uint8_t OS_Running = 0;               // 0ϵͳδ1
volatile uint32_t sw_wdg_counter = 0; // Ź
// 飬ÿԪһ˫ͷ
TaskList *readyList[Max_PRIORITY];
// һҪִе
TaskList *next_task_ptr = NULL;
// 
TaskList *runninglist;
// ̬
TaskList *blockedlist;
// ̬
TaskList *suspendlist;
// ȴٵ
TaskList *tasksWaitingTermination = NULL;
uint16_t os_ready_bitmap = 0;

/* ======================== 内存管理 (heap4 风格: best-fit + 双向链表 + 最小碎片约束) ======================== */
static unsigned char my_rtos_heap[RTOS_HEAP_SIZE];

#define HEAP_MIN_BLOCK_SIZE  ((uint32_t)(sizeof(struct MemBlock) + 8))

typedef struct MemBlock
{
    struct MemBlock *next;
    struct MemBlock *prev;
    uint32_t size;
} MemBlock_t;

static MemBlock_t *freeListHead = NULL;

void my_os_heap_init(void)
{
    freeListHead = (MemBlock_t *)my_rtos_heap;
    freeListHead->next = NULL;
    freeListHead->prev = NULL;
    freeListHead->size = RTOS_HEAP_SIZE;
}

void *my_os_malloc(uint32_t size)
{
    if (size == 0)
        return NULL;

    uint32_t total_size = (size + 7) & ~7;
    total_size += sizeof(MemBlock_t);

    if (total_size < HEAP_MIN_BLOCK_SIZE)
        total_size = HEAP_MIN_BLOCK_SIZE;

    __disable_irq();

    MemBlock_t *best = NULL;
    MemBlock_t *curr = freeListHead;
    uint32_t best_size = 0xFFFFFFFF;

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
        return NULL;
    }

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
        if (best->prev) best->prev->next = best->next;
        else freeListHead = best->next;
        if (best->next) best->next->prev = best->prev;
}

    best->next = NULL;
    best->prev = NULL;
    __enable_irq();
    return (void *)((uint8_t *)best + sizeof(MemBlock_t));
}

void my_os_free(void *ptr)
{
    if (ptr == NULL) return;
    MemBlock_t *block = (MemBlock_t *)((uint8_t *)ptr - sizeof(MemBlock_t));
    __disable_irq();

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

    if (curr && (uint8_t *)block + block->size == (uint8_t *)curr)
    {
        block->size += curr->size;
        block->next = curr->next;
        if (curr->next) curr->next->prev = block;
    }

    if (prev && (uint8_t *)prev + prev->size == (uint8_t *)block)
    {
        prev->size += block->size;
        prev->next = block->next;
        if (block->next) block->next->prev = prev;
}

    __enable_irq();
}

/************************  ************************/

/* taskMoveInReady: Insert task into readyList, update bitmap. VIP path for preemption. */
void taskMoveInReady(TaskList *newTask)
{   
    if (newTask == NULL) {
        return;
    }

    //  2һȽ״̬Ϊ̬
    newTask->taskTCB.task_state = READY;

    // ֻϵͳѾʼˣŽ VIP ռж
    if (runninglist != NULL)
    {
        // ȼе
        if (newTask->taskTCB.priority > runninglist->taskTCB.priority)
        {
            //  VIP ϯλȱȼȵǰ VIP Ҫ
            if (next_task_ptr == NULL || newTask->taskTCB.priority > next_task_ptr->taskTCB.priority)
            {
                //  1໥ìܵ if Ƕ
                if (next_task_ptr != NULL) {
                    // ԭ VIP λΪͨһ鱾
                    taskMoveInReady(next_task_ptr);
                }
                
                // »ʵǻ
                next_task_ptr = newTask;
                
                //  PendSV 
                SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
                
                // VIP רָоλҪ˫ֱӷ
                return; 
    }
        }
    }

    // ============================================
    // ·Ϊͨ񣨻̭ľ VIP߼
    // ============================================
    unsigned int priority = newTask->taskTCB.priority;

    if (readyList[priority] == NULL)
    {
        readyList[priority] = newTask;
        newTask->prev = newTask;
        newTask->next = newTask;
        os_ready_bitmap |= (1 << priority);
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

// ͳһժٽʹã
/* taskMoveOutList: Remove task from its current list. Update bitmap if READY list goes empty. */
void taskMoveOutList(TaskList *task)
{
    if (task == NULL)
        return;

    TaskStateTypeDef state = task->taskTCB.task_state;

    // ̬ɾ̬κοɵ
    if (state == RUNNING || state == DELETE)
        return;

    if (state == READY)
        {
        // ѭ˫
        if (task->next == task)
            {
            readyList[task->taskTCB.priority] = NULL;
            os_ready_bitmap &= ~(1 << task->taskTCB.priority);
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
        // ˫
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

/************************ ڲϵͳ ************************/

// ϵͳר̨ (ȼ)
/* IdleTask (priority 0): Feeds SW watchdog, reclaims deleted task memory. */
static void IdleTask_Entry(void* arg)
{
    while (1)
    {
        TaskList *toDelete = NULL;

        // 1. ιֻҪУ˵ûиȼ CPU
        sw_wdg_counter = 0;

        // 2. ǷҪʬ
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

        // 3. ִڴͷ (˳ٽٲֹ)
        if (toDelete != NULL)
        {
            my_os_free(toDelete->taskTCB.stack_base);
            my_os_free(toDelete);
    }

        // 4. ѡƬ͹ģʽ
        // __WFI();
    }
}

/************************  API ************************/

// ޸ rtos.h е
/*
 * TaskCreate — Initialize fake stack frame for a new task
 *
 * Stack layout after init (high to low):
 *     +------------------+  <- stack_ptr initially here
 *     |      xPSR        |  0x01000000 (Thumb bit)
 *     +------------------+
 *     |      PC          |  taskFunction address
 *     +------------------+
 *     |      LR          |  0xFFFFFFFD
 *     +------------------+
 *     |      R12         |  0
 *     +------------------+
 *     |      R3 - R0     |  R0 = arg (task parameter)
 *     +------------------+
 *     |      R11 - R4    |  all 0
 *     +------------------+  <- PSP when PendSV first runs
 *
 * On first PendSV, PSP==0 -> skip save, call TaskSwitch directly.
 * TaskSwitch picks this task -> LDMIA restores R4-R11 ->
 * BX LR -> hardware unstack -> PC = taskFunction -> task starts.
 */

/*
 * TaskCreate — Initialize fake stack frame for a new task
 *
 * Stack layout after init (high to low):
 *     +------------------+  <- stack_ptr initially here
 *     |      xPSR        |  0x01000000 (Thumb bit)
 *     +------------------+
 *     |      PC          |  taskFunction address
 *     +------------------+
 *     |      LR          |  0xFFFFFFFD
 *     +------------------+
 *     |      R12         |  0
 *     +------------------+
 *     |      R3 - R0     |  R0 = arg (task parameter)
 *     +------------------+
 *     |      R11 - R4    |  all 0
 *     +------------------+  <- PSP when PendSV first runs
 *
 * On first PendSV, PSP==0 -> skip save, call TaskSwitch directly.
 * TaskSwitch picks this task -> LDMIA restores R4-R11 ->
 * BX LR -> hardware unstack -> PC = taskFunction -> task starts.
 */

/* 创建任务: 分配 TCB+栈, 初始化异常帧 (见 PendSV 栈图) | Create new task */
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

/* TaskSwitch: Select next task to run. Uses O(1) bitmap + CLZ for priority lookup. */
void TaskSwitch(void)
{   
    __disable_irq();
    if (next_task_ptr != NULL)
    {
        if (runninglist != NULL)
    {
            // ֻбռŷŻؾó״̬RUNNING
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
    if (os_ready_bitmap != 0) {
        highest_ready_prio = 31 - __clz((uint32_t)os_ready_bitmap);
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
    taskMoveOutList(runninglist); // ͳһƳ߼

    runninglist->taskTCB.task_state = RUNNING;
    __enable_irq();
}


/* TaskSwitch: Select next task to run. Uses O(1) bitmap + CLZ for priority lookup. */
void TaskSwitch(void);
extern TaskList *runninglist;

#ifndef __INTELLISENSE__
/*
 * Cortex-M3 PendSV_Handler — Context Switch Assembly
 *
 * Hardware auto-stack on exception entry (descending stack):
 *     High addr
 *     +------------------+
 *     |      xPSR        |  <- bit24=1 (Thumb), set by TaskCreate
 *     +------------------+
 *     |      PC          |  <- task entry address
 *     +------------------+
 *     |      LR          |  <- EXC_RETURN=0xFFFFFFFD
 *     +------------------+
 *     |      R12         |
 *     +------------------+
 *     |      R3 - R0     |  <- R0 = task function arg
 *     +------------------+
 *     |      R11 - R4    |  <- saved by STMDB R0!, {R4-R11}
 *     +------------------+  <- PSP points here after save
 *     Low addr
 *
 * Flow:
 *   1. Save: PSP->R0, STMDB R0!,{R4-R11}, save R0 to TCB->stack_ptr
 *   2. C call: TaskSwitch() picks next task
 *   3. Restore: load TCB->stack_ptr->R0, LDMIA R0!,{R4-R11}
 *   4. BX LR -> hardware unstack R0-R3,R12,LR,PC,xPSR
 */

/*
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

/* StartScheduler: Create IdleTask, set PendSV/SysTick prio, start SysTick, never returns. */
void StartScheduler(void)
{
    // 1. ţڵ׼ڼ䣬κжϣ SysTick
    __disable_irq();

    NVIC_SetPriority(PendSV_IRQn, 15);
    NVIC_SetPriority(SysTick_IRQn, 15);
    __set_PSP(0);

    OsRunningTime_ms = 0;
    sw_wdg_counter = 0;

    // 2. ڲ񡣴ʱΪжϹرգĴڴӡ԰ȫ
    TaskCreate(IdleTask_Entry, NULL,0, (unsigned char *)"OS_Idle");
// 2. ڲϸķֵУ飡
    TaskList* idle_task = TaskCreate(IdleTask_Entry, NULL,0, (unsigned char *)"OS_Idle");
    
    // ڶڴ治㵼¿񴴽ʧܣֱڴ˴ϵͳ崻ֹɿص
    if (idle_task == NULL) {
        __disable_irq();
        while(1) {
            // ʵʹҵƷУԵһƣ򴮿ֱһ "OOM Error"
        }
        }
    // ѡ˻зն˲ʾ
    char *msg = "time=0\r\n";
    HAL_UART_Transmit(&huart2, (uint8_t *)msg, strlen(msg), HAL_MAX_DELAY);

    // 3. еĵײӡ׼ˣϵͳб־
    OS_Running = 1;

    // 4. ʱ SysTick ʱ
    SysTick_Config(SystemCoreClock / 1000);

    // 5. ֶ PendSVҪһ
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;

    // 6. ţӭ PendSV ռʽ̬
    __enable_irq();

    while (1)
    {
        // Զߵ
    }
    }

/* SysTick_Handler: 1ms tick. System time, SW watchdog, IWDG refresh, wake blocked tasks. */
void SysTick_Handler(void)
{
    HAL_IncTick();
    // ==========================================
    // ޸ RTOS ûе̹ȥ
    // Բִκص߼
    // ==========================================
    if (OS_Running == 0)
    {
        return; 
    }
    OsRunningTime_ms++;
    sw_wdg_counter++; // Ź
    HAL_IWDG_Refresh(&hiwdg); // жϿŹˢ
    // ŹǷʱ
    if (sw_wdg_counter > WDG_TIMEOUT_MS)
{
        char err_msg[64];
        char *task_name = (runninglist != NULL) ? (char *)runninglist->taskName : "NULL";
        sprintf(err_msg, "\r\nSW_WDT_TIMEOUT! running task: %s\r\n", task_name);

        // =================================================================
        // ޸ġ HAL_UART_TransmitֱѯĴ
        // Գ HAL  BUSY ״̬ Tick 
        // =================================================================
        for (int i = 0; err_msg[i] != '\0'; i++)
        {
            // ȴ TXE (Transmit Data Register Empty) λʾԷһֽ
            while (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_TXE) == RESET)
                ;
            // ֱӽַǿӲݼĴ (DR)
            huart2.Instance->DR = err_msg[i];
        }

        // ȴ TC (Transmission Complete) λȷһַȫɳ
        while (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_TC) == RESET)
            ;

        // Էϣ·
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

/************************ ״̬ API ************************/

/* taskdelay: Block current task for ms milliseconds. */
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

/* suspendTask: Suspend a task (remove from scheduler). task==NULL -> self. */
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

/* resumeTask: Resume a suspended task. */
void resumeTask(TaskList *task)
{
    if (task == NULL)
        return;

    __disable_irq();

    if (task->taskTCB.task_state == SUSPEND)
    {
        taskMoveOutList(task);
        task->taskTCB.task_state = READY; // ȸ״̬
        taskMoveInReady(task);
    }
    __enable_irq();
}

/* TaskDelete: Delete a task. Memory reclaimed by IdleTask. */
void TaskDelete(TaskList *task)
{
    __disable_irq();

    if (task == NULL)
    {
        __enable_irq();
        return;
    }

    // ժ״̬
    taskMoveOutList(task);
    task->taskTCB.task_state = DELETE;

    // ͷ巨ȴ
    task->next = tasksWaitingTermination;
    task->prev = NULL;
    if (tasksWaitingTermination != NULL)
        tasksWaitingTermination->prev = task;
    tasksWaitingTermination = task;

    // ɾԼ̴ȷ CPU
    if (readyList[task->taskTCB.priority] == NULL) {
        os_ready_bitmap &= ~(1 << task->taskTCB.priority);
    }

    if (task == runninglist)
    {
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
    }
    __enable_irq();
}
/* SemaphoreCreate: Create a binary semaphore. count=0 or 1. */
Semaphore_t *SemaphoreCreate(unsigned char initialCount)
{
    // дĶڴзռ
    Semaphore_t *newSem = (Semaphore_t *)my_os_malloc(sizeof(Semaphore_t));
    if (newSem == NULL)
        return NULL;

    // ʼԴ (ֵźֻ 0  1)
    newSem->count = (initialCount > 0) ? 1 : 0;
    newSem->waitList = NULL; // ʼ״̬ûȴ

    return newSem;
}
/* SemaphoreTake: Wait/block on semaphore. */
void SemaphoreTake(Semaphore_t *sem)
{
    if (sem == NULL)
        return;

    __disable_irq(); // жϣϵͳ״̬

    if (sem->count == 1)
    {
        // 1. Դãֱߣκε
        sem->count = 0;
        __enable_irq();
        return;
    }
    else
    {
        // 2. ԴãǰԼ
        TaskList *waitTask = runninglist;

        // ı״̬
        waitTask->taskTCB.task_state = BLOCKED;

        // --- Լ뵽ź waitList  (ȼ򣬸ǰ) ---
        TaskList *curr = sem->waitList;
        TaskList *prev_node = NULL;

        // ѰҲλãֱҵһȼԼ͵Ľڵ
        while (curr != NULL && curr->taskTCB.priority >= waitTask->taskTCB.priority)
        {
            prev_node = curr;
            curr = curr->next;
    }

        if (prev_node == NULL)
        {
            // 1Ϊգߵǰȼߣ뵽ͷ
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
            // 2뵽 prev_node ֮curr ֮ǰ
            waitTask->next = curr;
            waitTask->prev = prev_node;
            prev_node->next = waitTask;
            if (curr != NULL)
            {
                curr->prev = waitTask;
            }
        }

        // 3.  PendSV̵ȣ
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;

        __enable_irq(); // ţ PendSV ռ CPU

        // ==========================================================
        // ؼ߼㡿ǰͣס
        // δĳһ죬 SemaphoreGive ʱ
        // ĴִУ
        // ==========================================================

        // Ȼˣ˵õźˣΪϽٴȷ count Ϊ 0
        // (עڶֵźԣͨǱ Give ֱתȨټ)
    }
}
/* SemaphoreGive: Release semaphore, wake highest-prio waiter. */
void SemaphoreGive(Semaphore_t *sem)
        {
    if (sem == NULL)
        return;

    __disable_irq();

    // Ƿڵź
    if (sem->waitList != NULL)
    {
        // 1. ˵ȣѶͷժ
        TaskList *wakeTask = sem->waitList;

        //  wakeTask  waitList 
        sem->waitList = wakeTask->next;
        if (sem->waitList != NULL)
        {
            sem->waitList->prev = NULL;
        }
        wakeTask->next = NULL;
        wakeTask->prev = NULL;
        wakeTask->taskTCB.task_state = READY;

        

        // 2. Żϵͳľ
        taskMoveInReady(wakeTask);
    }
    else
        {
        // û˵ȣǾͰԿ׷ϣɿ״̬
        sem->count = 1;
}

    __enable_irq();
        }
/* MutexCreate: Create mutex with Priority Inheritance support. */
Mutex_t *MutexCreate(void)
{
    Mutex_t *newMutex = (Mutex_t *)my_os_malloc(sizeof(Mutex_t));
    if (newMutex == NULL)
        return NULL;

    newMutex->count = 1;          // ʼ״̬
    newMutex->waitList = NULL;    // û˵ȴ
    newMutex->owner = NULL;       // û
    newMutex->owner_priority = 0; // Ĭ0

    return newMutex;
    }
/* MutexTake: Take mutex. Implements PIP to prevent priority inversion. */
void MutexTake(Mutex_t *mutex)
{
    if (mutex == NULL)
        return;

    __disable_irq(); // жϱٽ

    if (mutex->count == 1)
    {
        // 1. ãǰֱ
        mutex->count = 0;
        mutex->owner = runninglist;                            // Ȩ
        mutex->owner_priority = runninglist->taskTCB.priority; // ұȼ
        __enable_irq();
        return;
    }
    else
        {
        // 2. ãȼ̳ж (PIP 㷨)
        if (runninglist->taskTCB.priority > mutex->owner->taskTCB.priority)
            {
            //  owner ״̬Ǿ̬Ҫ readyList еλý
            if (mutex->owner->taskTCB.task_state == READY)
            {
                taskMoveOutList(mutex->owner);                                  // ȴԭĵž[cite: 1, 4]
                mutex->owner->taskTCB.priority = runninglist->taskTCB.priority; // θȼ
                taskMoveInReady(mutex->owner);                                  // °ȼ[cite: 1, 4]
            }
            else
            {
                // ״̬ʵڵϵ£ownerͨREADYֱӸֵ
                mutex->owner->taskTCB.priority = runninglist->taskTCB.priority;
            }
        }

        // 3. ǰԹȥŶ (߼źһ)
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

        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk; //  PendSV  CPU
        __enable_irq();

        // --- 𣬵ʱ˵Ѿƽ ---
    }
}
/* MutexGive: Release mutex. Restore owner original priority if PIP was active. */
void MutexGive(Mutex_t *mutex)
{
    if (mutex == NULL)
        return;

    __disable_irq();

    // 1. ȫ飺ֻгˣʸ
    if (mutex->owner != runninglist)
    {
        __enable_irq();
        return; // ߿һ
    }

    // 2. ȼָ֮ǰΪȼ̳бθˣڱԭ
    if (runninglist->taskTCB.priority != mutex->owner_priority)
    {
        runninglist->taskTCB.priority = mutex->owner_priority;
        // ΪȼˣܾͲ readyList ĳЩˣԱһε
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
    }

    // 3. Ŷӵ
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
        // ûŶӣͷ
        mutex->count = 1;
        mutex->owner = NULL; // Ϊ״̬
}

    __enable_irq();
    }
/* QueueCreate: Create message queue with ring buffer. */
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
/* QueueSend: Send to queue. Block if full. */
uint8_t QueueSend(Queue_t *queue, void *item)
{
    if (queue == NULL || item == NULL)
        return 0;

    __disable_irq(); // 1. жϣϵͳԴͻλ

    // 2. ķˣǰ
    // Ϊʲô while  if RTOS Ļƽ
    // 񱻻Ѻ󣬿ڸռռֱռˣ¼Ƿпλ
    while (queue->count >= queue->maxItems)
    {
        TaskList *waitTask = runninglist;
        waitTask->taskTCB.task_state = BLOCKED;

        // --- Լȼߵĺ (txWaitList) ---
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

        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk; // 
        __enable_irq();                      // ŽCPU˯...

        // --- ﱻ߻ѣ˵пλˣ ---
        __disable_irq(); // һ£¹ţȥִ while 
    }

    // 3. ݿʱ϶пλûݿˮص tailβλ
    unsigned char *writePtr = queue->buffer + (queue->tail * queue->itemSize);
    memcpy(writePtr, item, queue->itemSize);

    // 4. ָݣβǰһߵͷȡģ(%)ƻͷ
    queue->tail = (queue->tail + 1) % queue->maxItems;
    queue->count++; // ˮˮ +1

    // 5. ߣڵݣȼߵǸ
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

    __enable_irq(); // ϣ
    return 1;
}
/* QueueReceive: Receive from queue. Block if empty. */
uint8_t QueueReceive(Queue_t *queue, void *buffer)
{
    if (queue == NULL || buffer == NULL)
        return 0;

    __disable_irq();

    // 1. ˮؿˣˮŶ
    while (queue->count == 0)
    {
        TaskList *waitTask = runninglist;
        waitTask->taskTCB.task_state = BLOCKED;

        // --- Լȼߵĺ (rxWaitList) ---
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

        // --- ˯... ֱ߷ݻ ---
        __disable_irq();
    }

    // 2. ݿʱ϶ݣˮص headͷݿû buffer 
    unsigned char *readPtr = queue->buffer + (queue->head * queue->itemSize);
    memcpy(buffer, readPtr, queue->itemSize);

    // 3. ָݣָǰһˮ -1
    queue->head = (queue->head + 1) % queue->maxItems;
    queue->count--;

    // 4. 
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
