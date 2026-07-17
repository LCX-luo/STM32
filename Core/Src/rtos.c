#include "rtos.h"
#include "usart.h"
#include "gpio.h"
#include "string.h"
#include "iwdg.h"  
#include <stdio.h>
/************************ ºê¶¨ÒåÓëÄÚ²¿ÉùÃ÷ ************************/


// Ç°ÖÃÉùÃ÷ÏµÍ³ÄÚ²¿µÄ¿ÕÏÐÈÎÎñ
static void IdleTask_Entry(void* arg);

/************************ È«¾Ö±äÁ¿¶¨Òå ************************/
unsigned int OsRunningTime_ms = 0;
uint8_t OS_Running = 0;               // 0´ú±íÏµÍ³Î´Æô¶¯£¬1´ú±íÒÑÆô¶¯
volatile uint32_t sw_wdg_counter = 0; // Èí¼þ¿´ÃÅ¹·¼ÆÊýÆ÷
// ¾ÍÐ÷Êý×é£¬Ã¿¸öÔªËØÊÇÒ»ÌõË«ÏòÁ´±íÍ·
TaskList *readyList[Max_PRIORITY];
// ÏÂÒ»¸öÒªÖ´ÐÐµÄÈÎÎñ
TaskList *next_task_ptr = NULL;
// ÔËÐÐÖÐÈÎÎñ
TaskList *runninglist;
// ×èÈûÌ¬ÈÎÎñ
TaskList *blockedlist;
// ¹ÒÆðÌ¬ÈÎÎñ
TaskList *suspendlist;
// µÈ´ý³¹µ×Ïú»ÙµÄÈÎÎñÁ´±í
TaskList *tasksWaitingTermination = NULL;

/* ======================== å†…å­˜ç®¡ç† (heap4 é£Žæ ¼: best-fit + åŒå‘é“¾è¡¨ + æœ€å°ç¢Žç‰‡çº¦æŸ) ======================== */
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

/************************ ºËÐÄÁ´±í²Ù×÷ ************************/

void taskMoveInReady(TaskList *newTask)
{   
    if (newTask == NULL) {
        return;
    }

    // ¡¾ÐÞÕý 2¡¿£ºµÚÒ»²½£¬±ØÐëÎÞÌõ¼þÏÈ½«×´Ì¬¸ÄÎª¾ÍÐ÷Ì¬
    newTask->taskTCB.task_state = READY;

    // Ö»ÓÐÏµÍ³ÒÑ¾­¿ªÊ¼µ÷¶ÈÁË£¬²Å½øÐÐ VIP ÇÀÕ¼ÅÐ¶¨
    if (runninglist != NULL)
    {
        // Èç¹ûÐÂÈÎÎñÓÅÏÈ¼¶´óÓÚÕýÔÚÔËÐÐµÄÈÎÎñ
        if (newTask->taskTCB.priority > runninglist->taskTCB.priority)
        {
            // Èç¹û VIP Ï¯Î»¿ÕÈ±£¬»òÕßÐÂÈÎÎñÓÅÏÈ¼¶±Èµ±Ç° VIP »¹Òª¸ß
            if (next_task_ptr == NULL || newTask->taskTCB.priority > next_task_ptr->taskTCB.priority)
            {
                // ¡¾ÐÞÕý 1¡¿£º°þÀëÏà»¥Ã¬¶ÜµÄ if Ç¶Ì×
                if (next_task_ptr != NULL) {
                    // Ô­À´µÄ VIP ÍËÎ»£¬×÷ÎªÆÕÍ¨ÈÎÎñÖØÐÂ×ßÒ»±é±¾º¯Êý£¬¹ÒÈëÁ´±í
                    taskMoveInReady(next_task_ptr);
                }
                
                // ÐÂ»ÊµÇ»ù
                next_task_ptr = newTask;
                
                // ÐüÆð PendSV ÇëÇóµ÷¶È
                SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
                
                // VIP ÈÎÎñÒÑÔÚ×¨ÊôÖ¸ÕëÖÐ¾ÍÎ»£¬²»ÐèÒª¹ÒÈëË«ÏòÁ´±í£¬Ö±½Ó·µ»Ø
                return; 
            }
        }
    }

    // ============================================
    // ÏÂ·½ÎªÆÕÍ¨ÈÎÎñ£¨»ò±»ÌÔÌ­µÄ¾É VIP£©µÄÁ´±í²åÈëÂß¼­
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

// Í³Ò»µÄÈÎÎñÕª³ýº¯Êý£ºÔÚÁÙ½çÇøÄÚÊ¹ÓÃ£¬²»¿É×èÈû£¡
void taskMoveOutList(TaskList *task)
{
    if (task == NULL)
        return;

    TaskStateTypeDef state = task->taskTCB.task_state;

    // ÔËÐÐÌ¬»òÒÑÉ¾³ýÌ¬²»ÔÚÈÎºÎ¿Éµ÷¶ÈÁ´±íÖÐ
    if (state == RUNNING || state == DELETE)
        return;

    if (state == READY)
    {
        // Ñ­»·Ë«ÏòÁ´±í°þÀë
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
        // ÏßÐÔË«ÏòÁ´±í°þÀë
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

/************************ ÄÚ²¿ÏµÍ³ÈÎÎñ ************************/

// ²Ù×÷ÏµÍ³×¨ÊôºóÌ¨¿ÕÏÐÈÎÎñ (×îµÍÓÅÏÈ¼¶)
static void IdleTask_Entry(void* arg)
{
    while (1)
    {
        TaskList *toDelete = NULL;

        // 1. Î¹¹·£ºÖ»Òª¿ÕÏÐÈÎÎñÄÜÔËÐÐ£¬ËµÃ÷Ã»ÓÐ¸ßÓÅÏÈ¼¶ÈÎÎñËÀËø¿¨ËÀ CPU
        sw_wdg_counter = 0;

        // 2. ¼ì²éÊÇ·ñÓÐÐèÒªÊÕÊ¬µÄÈÎÎñ
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

        // 3. Ö´ÐÐÕæÕýµÄÄÚ´æÊÍ·Å (ÍË³öÁÙ½çÇøºóÔÙ²Ù×÷£¬·ÀÖ¹×èÈûµ÷¶È)
        if (toDelete != NULL)
        {
            my_os_free(toDelete->taskTCB.stack_base);
            my_os_free(toDelete);
        }

        // 4. ¿ÉÒÔÑ¡Ìî£ºµ¥Æ¬»ú½øÈëµÍ¹¦ºÄÄ£Ê½
        // __WFI();
    }
}

/************************ ÈÎÎñÓëµ÷¶È API ************************/

// ÐÞ¸Ä rtos.h ÖÐµÄÉùÃ÷
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
            // Ö»ÓÐ±»ÇÀÕ¼µÄÈÎÎñ²Å·Å»Ø¾ÍÐ÷±í£¬Ö÷¶¯ÈÃ³öµÄÈÎÎñ×´Ì¬²»ÊÇRUNNING
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
    taskMoveOutList(runninglist); // Í³Ò»µÄÒÆ³öÂß¼­

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
    // 1. ¹ØÃÅ£¡ÔÚµã»ð×¼±¸ÆÚ¼ä£¬¾ø²»ÔÊÐíÈÎºÎÖÐ¶Ï£¨°üÀ¨ SysTick£©À´µ·ÂÒ
    __disable_irq();

    NVIC_SetPriority(PendSV_IRQn, 15);
    NVIC_SetPriority(SysTick_IRQn, 15);
    __set_PSP(0);

    OsRunningTime_ms = 0;
    sw_wdg_counter = 0;

    // 2. ´´½¨ÄÚ²¿ÈÎÎñ¡£´ËÊ±ÒòÎªÖÐ¶Ï¹Ø±Õ£¬ÀïÃæµÄ´®¿Ú´òÓ¡¾ø¶Ô°²È«
    TaskCreate(IdleTask_Entry, NULL,0, (unsigned char *)"OS_Idle");
// 2. ´´½¨ÄÚ²¿ÈÎÎñ¡£Ôö¼ÓÑÏ¸ñµÄ·µ»ØÖµÐ£Ñé£¡
    TaskList* idle_task = TaskCreate(IdleTask_Entry, NULL,0, (unsigned char *)"OS_Idle");
    
    // Èç¹ûÓÉÓÚ¶ÑÄÚ´æ²»×ãµ¼ÖÂ¿ÕÏÐÈÎÎñ´´½¨Ê§°Ü£¬Ö±½ÓÔÚ´Ë´¦½«ÏµÍ³å´»úËøËÀ£¬·ÀÖ¹Òý·¢²»¿É¿ØµÄÁ¬»·±ÀÀ£
    if (idle_task == NULL) {
        __disable_irq();
        while(1) {
            // ÔÚÊµ¼Ê¹¤Òµ²úÆ·ÖÐ£¬ÕâÀï¿ÉÒÔµãÁÁÒ»¸öºìµÆ£¬»òÕßÏò´®¿ÚÖ±ÅçÒ»¸ö "OOM Error"
        }
    }
    // £¨¿ÉÑ¡£º¼ÓÁË»»ÐÐ·û£¬ÖÕ¶Ë²ÅÄÜÁ¢¿ÌÏÔÊ¾£©
    char *msg = "time=0\r\n";
    HAL_UART_Transmit(&huart2, (uint8_t *)msg, strlen(msg), HAL_MAX_DELAY);

    // 3. ËùÓÐµÄµ×²ã´òÓ¡ºÍ×¼±¸¶¼Íê³ÉÁË£¬ÉèÖÃÏµÍ³ÔËÐÐ±êÖ¾
    OS_Running = 1;

    // 4. ÕâÊ±ºòÔÙÆô¶¯ SysTick ¶¨Ê±Æ÷
    SysTick_Config(SystemCoreClock / 1000);

    // 5. ÊÖ¶¯¹ÒÆð PendSV£¬Ë÷ÒªµÚÒ»´ÎÈÎÎñµ÷¶È
    SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;

    // 6. ¿ªÃÅ£¡Ó­½Ó PendSV ÇÀÕ¼£¬ÕýÊ½ÇÐÈëÈÎÎñÌ¬£¡
    __enable_irq();

    while (1)
    {
        // ÓÀÔ¶²»»á×ßµ½ÕâÀï
    }
}

void SysTick_Handler(void)
{
    HAL_IncTick();
    // ==========================================
    // ¡¾ºËÐÄÐÞ¸´¡¿£ºÈç¹û RTOS »¹Ã»ÓÐµã»ðÆô¶¯£¬Á¢¿Ì¹ö»ØÈ¥£¡
    // ¾ø¶Ô²»ÔÊÐíÍùÏÂÖ´ÐÐÈÎºÎÓëÈÎÎñµ÷¶ÈÏà¹ØµÄÂß¼­
    // ==========================================
    if (OS_Running == 0)
    {
        return; 
    }
    OsRunningTime_ms++;
    sw_wdg_counter++; // Èí¼þ¿´ÃÅ¹·¼ÆÊýÔö¼Ó
    HAL_IWDG_Refresh(&hiwdg); // ÖÐ¶Ï¿´ÃÅ¹·Ë¢ÐÂ
    // ¼ì²éÈí¼þ¿´ÃÅ¹·ÊÇ·ñ³¬Ê±
    if (sw_wdg_counter > WDG_TIMEOUT_MS)
    {
        char err_msg[64];
        char *task_name = (runninglist != NULL) ? (char *)runninglist->taskName : "NULL";
        sprintf(err_msg, "\r\nSW_WDT_TIMEOUT! running task: %s\r\n", task_name);

        // =================================================================
        // ¡¾ºËÐÄÐÞ¸Ä¡¿£ºÅ×Æú HAL_UART_Transmit£¬Ö±½ÓÂÖÑ¯¼Ä´æÆ÷±©Á¦·¢ËÍ
        // ÕâÑù¿ÉÒÔ³¹µ×ÎÞÊÓ HAL ¿âµÄ BUSY Ëø×´Ì¬ºÍ Tick ¶³½áÎÊÌâ
        // =================================================================
        for (int i = 0; err_msg[i] != '\0'; i++)
        {
            // µÈ´ý TXE (Transmit Data Register Empty) ÖÃÎ»£¬±íÊ¾¿ÉÒÔ·¢ËÍÏÂÒ»¸ö×Ö½Ú
            while (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_TXE) == RESET)
                ;
            // Ö±½Ó½«×Ö·ûÇ¿ÐÐÈûÈëÓ²¼þ·¢ËÍÊý¾Ý¼Ä´æÆ÷ (DR)
            huart2.Instance->DR = err_msg[i];
        }

        // µÈ´ý TC (Transmission Complete) ÖÃÎ»£¬È·±£×îºóÒ»¸ö×Ö·û°²È«·É³öÒý½Å
        while (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_TC) == RESET)
            ;

        // ÒÅÑÔ·¢ËÍÍê±Ï£¬°²ÐÄÉÏÂ·
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

/************************ ×´Ì¬¿ØÖÆ API ************************/

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
        task->taskTCB.task_state = READY; // ±ØÐëÏÈ¸Ä×´Ì¬£¡
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

    // ÎïÀíÕª³ý²¢¸Ä×´Ì¬
    taskMoveOutList(task);
    task->taskTCB.task_state = DELETE;

    // Í·²å·¨·ÅÈëµÈ´ý»ØÊÕÁ´±í
    task->next = tasksWaitingTermination;
    task->prev = NULL;
    if (tasksWaitingTermination != NULL)
        tasksWaitingTermination->prev = task;
    tasksWaitingTermination = task;

    // Èç¹ûÊÇÉ¾³ý×Ô¼º£¬±ØÐëÁ¢¿Ì´¥·¢µ÷¶È·ÅÆú CPU
    if (task == runninglist)
    {
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
    }
    __enable_irq();
}
Semaphore_t *SemaphoreCreate(unsigned char initialCount)
{
    // ´ÓÄãÊÖÐ´µÄ¶ÑÄÚ´æÖÐ·ÖÅä¿Õ¼ä
    Semaphore_t *newSem = (Semaphore_t *)my_os_malloc(sizeof(Semaphore_t));
    if (newSem == NULL)
        return NULL;

    // ³õÊ¼»¯×ÊÔ´ÊýÁ¿ (¶þÖµÐÅºÅÁ¿Ö»ÄÜÊÇ 0 »ò 1)
    newSem->count = (initialCount > 0) ? 1 : 0;
    newSem->waitList = NULL; // ³õÊ¼×´Ì¬ÏÂÃ»ÓÐÈÎÎñµÈ´ý

    return newSem;
}
void SemaphoreTake(Semaphore_t *sem)
{
    if (sem == NULL)
        return;

    __disable_irq(); // ±ØÐë¹ØÖÐ¶Ï£¬±£»¤ÏµÍ³Á´±íºÍ×´Ì¬µÄÍêÕûÐÔ

    if (sem->count == 1)
    {
        // 1. ×ÊÔ´¿ÉÓÃ£¬Ö±½ÓÄÃ×ß£¬²»´¥·¢ÈÎºÎµ÷¶È
        sem->count = 0;
        __enable_irq();
        return;
    }
    else
    {
        // 2. ×ÊÔ´²»¿ÉÓÃ£¬µ±Ç°ÈÎÎñ±ØÐë×èÈû×Ô¼º
        TaskList *waitTask = runninglist;

        // ¸Ä±äÈÎÎñ×´Ì¬
        waitTask->taskTCB.task_state = BLOCKED;

        // --- Á´±í²Ù×÷£º½«×Ô¼º¼ÓÈëµ½ÐÅºÅÁ¿µÄ waitList ÖÐ (°´ÓÅÏÈ¼¶ÅÅÐò£¬¸ßÔÚÇ°) ---
        TaskList *curr = sem->waitList;
        TaskList *prev_node = NULL;

        // Ñ°ÕÒ²åÈëÎ»ÖÃ£º±éÀúÁ´±íÖ±µ½ÕÒµ½Ò»¸öÓÅÏÈ¼¶±È×Ô¼ºµÍµÄ½Úµã
        while (curr != NULL && curr->taskTCB.priority >= waitTask->taskTCB.priority)
        {
            prev_node = curr;
            curr = curr->next;
        }

        if (prev_node == NULL)
        {
            // Çé¿ö1£ºÁ´±íÎª¿Õ£¬»òÕßµ±Ç°ÈÎÎñÓÅÏÈ¼¶×î¸ß£¬²åÈëµ½±íÍ·
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
            // Çé¿ö2£º²åÈëµ½ prev_node Ö®ºó£¬curr Ö®Ç°
            waitTask->next = curr;
            waitTask->prev = prev_node;
            prev_node->next = waitTask;
            if (curr != NULL)
            {
                curr->prev = waitTask;
            }
        }

        // 3. ÐüÆð PendSV£¬ÇëÇóÁ¢¿Ìµ÷¶È£¡
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;

        __enable_irq(); // ¿ªÃÅ£¬ÈÃ PendSV ½øÀ´ÇÀÕ¼ CPU

        // ==========================================================
        // ¡¾¹Ø¼üÂß¼­µã¡¿£ºµ±Ç°ÈÎÎñ»áÔÚÕâÀï¡°Í£×¡¡±¡£
        // µ±Î´À´Ä³Ò»Ìì£¬ÆäËûÈÎÎñµ÷ÓÃ SemaphoreGive »½ÐÑËüÊ±£¬
        // ËüµÄ´úÂë»á´ÓÕâÀï¼ÌÐøÍùÏÂÖ´ÐÐ£¡
        // ==========================================================

        // ¼ÈÈ»ÐÑÀ´ÁË£¬ËµÃ÷ÄÃµ½ÐÅºÅÁ¿ÁË£¬ÎªÁËÑÏ½÷£¬ÔÙ´ÎÈ·±£ count Îª 0
        // (×¢£ºÓÉÓÚ¶þÖµÐÅºÅÁ¿µÄÌØÐÔ£¬Í¨³£ÊÇ±» Give Ö±½Ó×ªÒÆÁËËùÓÐÈ¨£¬²»ÓÃÔÙ¼õ)
    }
}
void SemaphoreGive(Semaphore_t *sem)
{
    if (sem == NULL)
        return;

    __disable_irq();

    // ¼ì²éÊÇ·ñÓÐÈËÔÚµÈÕâ¸öÐÅºÅÁ¿
    if (sem->waitList != NULL)
    {
        // 1. ÓÐÈËµÈ£¡°Ñ¶ÓÍ·µÄÈÎÎñÕªÏÂÀ´
        TaskList *wakeTask = sem->waitList;

        // ½« wakeTask ´Ó waitList ÖÐÍÑÀë
        sem->waitList = wakeTask->next;
        if (sem->waitList != NULL)
        {
            sem->waitList->prev = NULL;
        }
        wakeTask->next = NULL;
        wakeTask->prev = NULL;
        wakeTask->taskTCB.task_state = READY;

        

        // 2. ½«Æä·Å»ØÏµÍ³µÄ¾ÍÐ÷Á´±í
        taskMoveInReady(wakeTask);
    }
    else
    {
        // Ã»ÈËµÈ£¬ÄÇ¾Í°ÑÔ¿³×·ÅÔÚ×ÀÉÏ£¨±ä³É¿ÉÓÃ×´Ì¬£©
        sem->count = 1;
    }

    __enable_irq();
}
Mutex_t *MutexCreate(void)
{
    Mutex_t *newMutex = (Mutex_t *)my_os_malloc(sizeof(Mutex_t));
    if (newMutex == NULL)
        return NULL;

    newMutex->count = 1;          // ³õÊ¼×´Ì¬¿ÉÓÃ
    newMutex->waitList = NULL;    // Ã»ÈËµÈ´ý
    newMutex->owner = NULL;       // »¹Ã»ÈËÄÃËø
    newMutex->owner_priority = 0; // Ä¬ÈÏ0

    return newMutex;
}
void MutexTake(Mutex_t *mutex)
{
    if (mutex == NULL)
        return;

    __disable_irq(); // ¹ØÖÐ¶Ï±£»¤ÁÙ½çÇø

    if (mutex->count == 1)
    {
        // 1. Ëø¿ÉÓÃ£¬µ±Ç°ÈÎÎñÖ±½ÓÄÃ×ß
        mutex->count = 0;
        mutex->owner = runninglist;                            // ÐûÊÄÖ÷È¨£¡
        mutex->owner_priority = runninglist->taskTCB.priority; // ¼ÇÏÂÎÒ±¾À´µÄÓÅÏÈ¼¶
        __enable_irq();
        return;
    }
    else
    {
        // 2. Ëø²»¿ÉÓÃ£¬´¥·¢ÓÅÏÈ¼¶¼Ì³ÐÅÐ¶¨ (PIP Ëã·¨ºËÐÄ)
        if (runninglist->taskTCB.priority > mutex->owner->taskTCB.priority)
        {
            // Èç¹û³ÖÓÐËøµÄ owner ×´Ì¬ÊÇ¾ÍÐ÷Ì¬£¬ÎÒÃÇÐèÒª°ÑËüÔÚ readyList ÖÐµÄÎ»ÖÃ½øÐÐÉý¼¶
            if (mutex->owner->taskTCB.task_state == READY)
            {
                taskMoveOutList(mutex->owner);                                  // ÏÈ´ÓÔ­À´µÄµÍÓÅ¾ÍÐ÷Á´±í°þÀë[cite: 1, 4]
                mutex->owner->taskTCB.priority = runninglist->taskTCB.priority; // °Î¸ßÓÅÏÈ¼¶
                taskMoveInReady(mutex->owner);                                  // ÖØÐÂ°´¸ßÓÅÏÈ¼¶²åÈë¾ÍÐ÷Á´±í[cite: 1, 4]
            }
            else
            {
                // Èç¹ûËü´¦ÓÚÆäËû×´Ì¬£¨ÆäÊµÔÚµ¥ºËÌåÏµÏÂ£¬ownerÍ¨³£¶¼ÔÚREADYÀï£©£¬Ö±½Ó¸ÄÊýÖµ¼´¿É
                mutex->owner->taskTCB.priority = runninglist->taskTCB.priority;
            }
        }

        // 3. µ±Ç°ÈÎÎñ¹Ô¹ÔÈ¥ÅÅ¶Ó (Âß¼­ÓëÐÅºÅÁ¿¼¸ºõÒ»ÖÂ)
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

        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk; // ÐüÆð PendSV ½»³ö CPU
        __enable_irq();

        // --- ÈÎÎñÔÚÕâÀï¹ÒÆð£¬µ±±»»½ÐÑÊ±£¬ËµÃ÷ËøÒÑ¾­ÒÆ½»µ½ËüÊÖÉÏÁË ---
    }
}
void MutexGive(Mutex_t *mutex)
{
    if (mutex == NULL)
        return;

    __disable_irq();

    // 1. °²È«¼ì²é£ºÖ»ÓÐ³ÖÓÐËøµÄÈË£¬²ÅÓÐ×Ê¸ñ»¹Ëø£¡
    if (mutex->owner != runninglist)
    {
        __enable_irq();
        return; // »òÕß¿ÉÒÔÔÚÕâÀï¼ÓÒ»¸ö´íÎó¶ÏÑÔ
    }

    // 2. ÓÅÏÈ¼¶»Ö¸´£ºÈç¹ûÖ®Ç°ÒòÎªÓÅÏÈ¼¶¼Ì³Ð±»°Î¸ßÁË£¬ÏÖÔÚ±ØÐë´ò»ØÔ­ÐÎ
    if (runninglist->taskTCB.priority != mutex->owner_priority)
    {
        runninglist->taskTCB.priority = mutex->owner_priority;
        // ÒòÎªÓÅÏÈ¼¶½µÏÂÀ´ÁË£¬¿ÉÄÜ¾Í²»Èç readyList ÀïµÄÄ³Ð©ÈÎÎñÁË£¬ËùÒÔ±ØÐëÇëÇóÒ»´Îµ÷¶È
        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk;
    }

    // 3. ´¦ÀíÅÅ¶ÓµÄÈÎÎñ
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
        // Ã»ÈËÅÅ¶Ó£¬³¹µ×ÊÍ·Å
        mutex->count = 1;
        mutex->owner = NULL; // Ëø±äÎªÎÞÖ÷×´Ì¬
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

    __disable_irq(); // 1. ¹ØÖÐ¶Ï£¬±£»¤ÏµÍ³×ÊÔ´ºÍ»·ÐÎ»º³åÇø

    // 2. ºËÐÄ·ÀÓù£ºÈç¹û¶ÓÁÐÂúÁË£¬µ±Ç°ÈÎÎñ±ØÐë×èÈû£¡
    // ÎªÊ²Ã´ÓÃ while ¶ø²»ÊÇ if£¿ÕâÊÇ RTOS µÄ»Æ½ð·¨Ôò£º
    // ÈÎÎñ±»»½ÐÑºó£¬¿ÉÄÜÓÉÓÚ¸ßÓÅÇÀÕ¼£¬¿Õ¼äÓÖ±»±ðÈËÕ¼ÁË£¬ËùÒÔÐÑÀ´ºó±ØÐëÖØÐÂ¼ì²éÊÇ·ñÕæµÄÓÐ¿ÕÎ»¡£
    while (queue->count >= queue->maxItems)
    {
        TaskList *waitTask = runninglist;
        waitTask->taskTCB.task_state = BLOCKED;

        // --- ½«×Ô¼º°´ÓÅÏÈ¼¶²åÈëÉú²úÕßµÄºò³µÊÒ (txWaitList) ---
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

        SCB->ICSR |= SCB_ICSR_PENDSVSET_Msk; // ÐüÆðµ÷¶È
        __enable_irq();                      // ¿ªÃÅ½»³öCPU£¬ÈÎÎñÔÚÕâÀï³ÁË¯...

        // --- ÈÎÎñÔÚÕâÀï±»Ïû·ÑÕß»½ÐÑ£¬ËµÃ÷ÓÐ¿ÕÎ»ÁË£¡ ---
        __disable_irq(); // ÐÑÀ´ºóµÚÒ»¼þÊÂ£¬ÖØÐÂ¹ØÃÅ£¬»ØÈ¥Ö´ÐÐ while ¼ì²é
    }

    // 3. Êý¾Ý¿½±´£º´ËÊ±¿Ï¶¨ÓÐ¿ÕÎ»£¬°ÑÓÃ»§Êý¾Ý¿½±´½øË®³ØµÄ tail£¨Î²²¿£©Î»ÖÃ
    unsigned char *writePtr = queue->buffer + (queue->tail * queue->itemSize);
    memcpy(writePtr, item, queue->itemSize);

    // 4. »·ÐÎÖ¸ÕëÍÆÑÝ£ºÎ²°ÍÏòÇ°×ßÒ»²½£¬Èç¹û×ßµ½¾¡Í·£¬ÓÃÈ¡Ä£(%)ÈÆ»ØÍ·²¿
    queue->tail = (queue->tail + 1) % queue->maxItems;
    queue->count++; // Ë®³ØË®Á¿ +1

    // 5. »½ÐÑÏû·ÑÕß£ºÈç¹ûÓÐÈÎÎñÔÚµÈÊý¾Ý£¬½ÐÐÑÓÅÏÈ¼¶×î¸ßµÄÄÇ¸ö
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

    __enable_irq(); // ·¢ËÍÍê±Ï£¬¿ªÃÅ
    return 1;
}
uint8_t QueueReceive(Queue_t *queue, void *buffer)
{
    if (queue == NULL || buffer == NULL)
        return 0;

    __disable_irq();

    // 1. Èç¹ûË®³Ø¿ÕÁË£¬ÏëºÈË®µÄÈÎÎñ±ØÐë×èÈûÅÅ¶Ó
    while (queue->count == 0)
    {
        TaskList *waitTask = runninglist;
        waitTask->taskTCB.task_state = BLOCKED;

        // --- ½«×Ô¼º°´ÓÅÏÈ¼¶²åÈëÏû·ÑÕßµÄºò³µÊÒ (rxWaitList) ---
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

        // --- ³ÁË¯... Ö±µ½ÓÐÉú²úÕß·¢ÁËÊý¾Ý»½ÐÑËü ---
        __disable_irq();
    }

    // 2. Êý¾Ý¿½±´£º´ËÊ±¿Ï¶¨ÓÐÊý¾Ý£¬´ÓË®³ØµÄ head£¨Í·²¿£©°ÑÊý¾Ý¿½±´µ½ÓÃ»§µÄ buffer ÖÐ
    unsigned char *readPtr = queue->buffer + (queue->head * queue->itemSize);
    memcpy(buffer, readPtr, queue->itemSize);

    // 3. »·ÐÎÖ¸ÕëÍÆÑÝ£º¶ÁÖ¸ÕëÏòÇ°×ßÒ»²½£¬Ë®Á¿ -1
    queue->head = (queue->head + 1) % queue->maxItems;
    queue->count--;

    // 4. »½ÐÑÉú²úÕß
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
