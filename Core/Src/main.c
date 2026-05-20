/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "iwdg.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include <stdarg.h> // 为了使用 va_list
#include "rtos.h"   // 引入自定义 RTOS 系统
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
// 规范：将自定义类型定义统一放置于此
typedef struct
{
  char text[128]; // 确保有足够容量容纳时间戳和可变参数
} LogMsg_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
// 规范：将全局变量和系统内核句柄统一放置于此
Queue_t *PrintQueue = NULL;
Semaphore_t *DmaTxSem = NULL;
Semaphore_t *ButtonSem = NULL;
volatile uint8_t breathing_mode = 1;     // 1: 呼吸模式, 0: 常亮模式
volatile uint8_t global_button_flag = 0; // 通用按键事件标志，可供任意任务轮询读取
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
// 规范：任务入口与私有函数的声明
void PrintTask_Entry(void *arg);
void Task1_Entry(void *arg);
void Task2_Entry(void *arg);
void Task3_Entry(void *arg);
void badtask(void *arg);
uint8_t my_itoa(unsigned int num, char *str);
void LOGI(const char *format, ...);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
// 规范：用户自定义函数的实现均放置在 0 区

/**
 * @brief 超轻量级无符号整数转字符串函数
 */
uint8_t my_itoa(unsigned int num, char *str)
{
  int i = 0;
  if (num == 0)
  {
    str[i++] = '0';
    str[i] = '\0';
    return 1;
  }

  char temp[16];
  int j = 0;
  while (num > 0)
  {
    temp[j++] = (num % 10) + '0';
    num /= 10;
  }

  while (j > 0)
  {
    str[i++] = temp[--j];
  }
  str[i] = '\0';
  return i;
}

/**
 * @brief 应用层异步日志打印函数 (类似 printf)
 * @note 绝不能在中断(ISR)或操作系统启动前调用此函数！
 */
void LOGI(const char *format, ...)
{
  LogMsg_t txMsg;
  int timestamp_len;
  int remain_len;

  timestamp_len = snprintf(txMsg.text, sizeof(txMsg.text), "%ums ", OsRunningTime_ms);
  if (timestamp_len < 0 || timestamp_len >= sizeof(txMsg.text))
  {
    return;
  }

  remain_len = sizeof(txMsg.text) - timestamp_len;

  va_list args;
  va_start(args, format);
  vsnprintf(&txMsg.text[timestamp_len], remain_len, format, args);
  va_end(args);

  QueueSend(PrintQueue, &txMsg);
}

/**
 * @brief DMA 驱动的后台打印任务 (消费者)
 */
void PrintTask_Entry(void *arg)
{
  LogMsg_t rxMsg;
  while (1)
  {
    SemaphoreTake(DmaTxSem);
    QueueReceive(PrintQueue, &rxMsg);
    HAL_UART_Transmit_DMA(&huart2, (uint8_t *)rxMsg.text, strlen(rxMsg.text));
  }
}

/**
 * @brief 业务任务 1：演示任务控制与日志输出
 */
void Task1_Entry(void *arg)
{
  int loop_count = 0;
  TaskList *target_task = (TaskList *)arg;

  while (1)
  {
    loop_count++;
    LOGI("Task1 is running, loop count: %d\r\n", loop_count);

    if (loop_count == 2)
    {
      LOGI("delete task3\r\n");
      TaskDelete(target_task);
      target_task = NULL;
    }
    if (loop_count == 3)
    {
      TaskCreate(Task3_Entry, NULL, 3, (unsigned char *)"Task3_VIP");
      LOGI("create task3\r\n");
    }
    taskdelay(8000);
  }
}

/**
 * @brief 业务任务 2：高频 LED 闪烁
 */
void Task2_Entry(void *arg)
{
  while (1)
  {
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
    LOGI(" 2\r\n");
    taskdelay(500);
  }
}

/**
 * @brief 业务任务 3：VIP 数据处理任务 (含静态变量测试)
 */
void Task3_Entry(void *arg)
{
  static int x;
  while (1)
  {
    x++;
    LOGI(" 3_VIP,%d\r\n", x);
    taskdelay(2900);
  }
}

/**
 * @brief 故意引发饥饿死机的异常任务
 */
void badtask(void *arg)
{
  while (1)
  {
    // 霸占 CPU，不调用任何阻塞函数，引发软看门狗复位
  }
}
/**
 * @brief 工业级按键扫描任务 (含消抖与边沿触发逻辑)
 */
void ledtask(void *arg)
{
  while (1)
  {
    // 1. 等待信号量。没有按键时，任务处于 BLOCKED 状态，不消耗算力
    SemaphoreTake(ButtonSem);

    // 2. 既然走到了这里，说明 EXTI 触发了。
    // 为了和原本功能一致，进行软件消抖：
    taskdelay(20);

    // 3. 再次确认电平（过滤电磁干扰）
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_10) == GPIO_PIN_SET)
    {
      // 执行原本的逻辑
      HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_5);
      LOGI("Button Pressed by EXTI! LED Toggled.\r\n");

      // 4. 等待松开逻辑保持不变
      while (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_10) == GPIO_PIN_SET)
      {
        taskdelay(10);
      }
      taskdelay(20); // 松手消抖
    }
  }
}

// =========================================================================
// 2. 全新通用按键事件分发任务 (替代原本的 ledtask)
// 原理：利用 EXTI 中断释放的 Semaphore，唤醒本任务进行消抖并改变全局状态
// =========================================================================
void GenericButtonTask_Entry(void *arg)
{
  while (1)
  {
    // 1. 阻塞等待底层 EXTI 硬件中断触发 (释放 CPU 算力)
    SemaphoreTake(ButtonSem);

    // 2. 软件消抖
    taskdelay(20);

    // 3. 确认有效按下 (基于你原本的 PA10 高电平有效逻辑)
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_10) == GPIO_PIN_SET)
    {
      // --- 核心业务逻辑触发区 ---
      
      // 功能 A：翻转呼吸灯状态
      breathing_mode = !breathing_mode; 
      
      // 功能 B：置位通用事件标志，供其他随意编写的业务任务读取
      global_button_flag = 1;           
      
      LOGI("Button Pressed! Breathing Mode: %d\r\n", breathing_mode);

      // 4. 死区滞回：等待按键彻底松开，防止长按导致的连续误触发
      while (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_10) == GPIO_PIN_SET)
      {
        taskdelay(10);
      }
      taskdelay(20); // 松手消抖
    }
  }
}

// =========================================================================
// 3. 改造后的 PWM 任务 (支持状态打断)
// 原理：在每次调整占空比前读取全局状态，如果模式改变，立即 break 跳出循环
// =========================================================================
void pwmtask(void *arg)
{
  while (1)
  {
    if (breathing_mode == 1)
    {
      // --- 呼吸模式 ---
      for (int i = 0; i < 100; i++)
      {
        // 关键逻辑：如果在渐变中途按下按键，立刻跳出循环，防止响应迟钝
        if (breathing_mode == 0) break; 
        
        __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, i);
        taskdelay(10);
      }
      for (int i = 0; i < 100; i++)
      {
        if (breathing_mode == 0) break;
        
        __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 100 - i);
        taskdelay(10);
      }
    }
    else
    {
      // --- 常亮模式 ---
      // 将 CCR 设置为 100 (100% 占空比)
      __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 100);
      
      // 挂起自身让出 CPU，避免 while(1) 疯狂空转导致其他任务饥饿
      taskdelay(50); 
    }
  }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART2_UART_Init();
  MX_IWDG_Init();
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);

  char *msg = "\r\n--- RTOS System Starting ---\r\n";
  HAL_UART_Transmit(&huart2, (uint8_t *)msg, strlen(msg), HAL_MAX_DELAY);
  HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
  // 1. 初始化系统核心与内存
  my_os_heap_init();

  // 2. 初始化核心 IPC（进程间通信）机制
  PrintQueue = QueueCreate(10, sizeof(LogMsg_t));
  DmaTxSem = SemaphoreCreate(1);
  ButtonSem = SemaphoreCreate(0);
  // 3. 创建所有业务任务
  TaskList *task = TaskCreate(Task3_Entry, NULL, 2, (unsigned char *)"Task3_VIP");
  TaskCreate(PrintTask_Entry, NULL, 3, (unsigned char *)"PrintTask");
  TaskCreate(Task1_Entry, task, 2, (unsigned char *)"Task1");
  TaskCreate(Task2_Entry, NULL, 2, (unsigned char *)"Task2");
  TaskCreate(ledtask, NULL, 2, (unsigned char *)"LedTask");
  TaskCreate(pwmtask, NULL, 3, (unsigned char *)"PwmTask");
  //TaskCreate(GenericButtonTask_Entry, NULL, 2, (unsigned char *)"GenericButtonTask");
  // 4. 启动调度器，系统接管 CPU 控制权
  StartScheduler();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
/**
 * @brief 串口 DMA 发送完成中断回调函数
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART2)
  {
    SemaphoreGive(DmaTxSem); // 唤醒 PrintTask 进行下一帧发送
  }
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == GPIO_PIN_10)
  {
    // 路径终点：一旦产生硬件中断，立刻给信号量，唤醒任务
    SemaphoreGive(ButtonSem);
  }
}

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM1 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM1)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
