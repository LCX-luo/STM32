#ifndef __RTOS_H
#define __RTOS_H

#include <stdint.h>
#include <stddef.h>
#include "main.h" // 引入系统基础定义

/************************ 宏定义 ************************/
#define Max_PRIORITY 16
#define TASK_NAME_MAX_LENGTH 10
#define RTOS_HEAP_SIZE 12288
#define TASK_DEFAULT_STACK_SIZE 256
#define WDG_TIMEOUT_MS 9000 // 软件看门狗超时时间配置为9s
/************************ 任务状态枚举定义 ************************/
typedef enum
{
    READY = 0,   /* 就绪态：等待CPU调度 */
    RUNNING = 1, /* 运行态：正在占用CPU执行 */
    BLOCKED = 2, /* 阻塞态：等待延时/信号量等资源 */
    SUSPEND = 3, /* 挂起态：被主动暂停，无法被调度 */
    DELETE = 4,  /* 删除态：任务已被删除，等待回收 */
} TaskStateTypeDef;  /* READY=0, RUNNING=1, BLOCKED=2, SUSPEND=3, DELETE=4 */



/************************ 任务控制块TCB ************************/
typedef struct TCB
{
    unsigned int *stack_ptr;     /* 任务栈指针 */
    unsigned int *stack_base;    /* 任务栈基址（固定不变，用于内存释放） */
    unsigned int delay_ms;       /* 任务延时计数器 */
    unsigned int priority;       /* 任务优先级 */
    TaskStateTypeDef task_state; /* 任务状态（联合体） */
} myTCB;

typedef struct TaskList TaskList;
struct TaskList
{
    myTCB taskTCB;
    unsigned char taskName[TASK_NAME_MAX_LENGTH];
    TaskList *next;
    TaskList *prev;
};
/*************************二值信号量结构体*************************/
typedef struct
{
    unsigned char count; /* 核心标志：0代表无资源，1代表有资源 */
    TaskList *waitList;  /* 私人候车室：专门等待这个信号量的任务链表 */
} Semaphore_t;
/************************ 暴露给外部的全局变量声明 ************************/
// 允许 main.c 等其他文件读取系统运行时间与运行状态
extern unsigned int OsRunningTime_ms;
extern uint8_t OS_Running;
extern uint16_t os_ready_bitmap;

/************************ RTOS 公开 API 声明 ************************/
void my_os_heap_init(void);
void *my_os_malloc(uint32_t size);
void my_os_free(void *ptr);

// 修改 rtos.h 中的声明
TaskList *TaskCreate(void (*taskFunction)(void *), void *arg, unsigned int priority, unsigned char *TaskName);
void StartScheduler(void);
void taskdelay(unsigned int ms);
void TaskDelete(TaskList *task);
void suspendTask(TaskList *task);
void resumeTask(TaskList *task);

// 信号量 API 声明
Semaphore_t* SemaphoreCreate(unsigned char initialCount);
void SemaphoreTake(Semaphore_t *sem);
void SemaphoreGive(Semaphore_t *sem);




/*************************互斥锁结构体*************************/
typedef struct
{
    unsigned char count;        /* 0代表锁被占用，1代表锁空闲 */
    TaskList *waitList;         /* 阻塞在这个锁上的任务链表，按优先级从高到低排序 */
    TaskList *owner;            /* 【核心新增】记录当前持有锁的任务 (所有者) */
    unsigned int owner_priority; /* 【核心新增】记录所有者原本的优先级，用于归还后恢复 */
} Mutex_t;

// 互斥锁 API 声明
Mutex_t* MutexCreate(void);
void MutexTake(Mutex_t *mutex);
void MutexGive(Mutex_t *mutex);

/*************************消息队列结构体*************************/
typedef struct
{
    unsigned char *buffer;      /* 物理水池：指向动态分配的一大块连续内存（环形缓冲区） */
    
    unsigned int itemSize;      /* 水桶大小：每个消息有多大？（比如一个int是4字节，一个结构体是20字节） */
    unsigned int maxItems;      /* 水池容量：最多能装多少个“水桶”？ */
    
    unsigned int head;          /* 读指针：下一个要被读出的消息的偏移索引 */
    unsigned int tail;          /* 写指针：下一个要被写入的消息的偏移索引 */
    unsigned int count;         /* 当前水量：当前队列里积压了多少个未读消息 */

    TaskList *rxWaitList;       /* 消费者候车室：队列空了，想读数据的任务在这里排队睡觉 */
    TaskList *txWaitList;       /* 生产者候车室：队列满了，想写数据的任务在这里排队睡觉 */
} Queue_t;

// 消息队列 API 声明
Queue_t* QueueCreate(unsigned int maxItems, unsigned int itemSize);
uint8_t QueueSend(Queue_t *queue, void *item);
uint8_t QueueReceive(Queue_t *queue, void *buffer);
#endif /* __RTOS_H */
