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
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h> // 引入 stdio 用于 sprintf 格式化字符串
#include "string.h"
#include "rtos.h"
#include "usart.h"
#include "gpio.h"
#include "string.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void Task1_Entry(void);
void Task2_Entry(void);
void Task3_Entry(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

// 【新增】超轻量级：无符号整数转字符串函数
// 将数字 num 转换为字符串存入 str，并返回字符串的长度
uint8_t my_itoa(unsigned int num, char *str)
{
  int i = 0;
  if (num == 0)
  {
    str[i++] = '0';
    str[i] = '\0';
    return 1;
  }

  char temp[16]; // 临时倒序存放
  int j = 0;
  while (num > 0)
  {
    temp[j++] = (num % 10) + '0';
    num /= 10;
  }

  // 正序输出到目标字符串
  while (j > 0)
  {
    str[i++] = temp[--j];
  }
  str[i] = '\0';
  return i;
}

// ... 下面保留你之前的 LogMsg_t 定义和队列指针 ...
typedef struct
{
  char text[32];
} LogMsg_t;

Queue_t *PrintQueue;
// DMA发送完成的同步信号量
Semaphore_t *DmaTxSem;

void PrintTask_Entry(void)
{
  LogMsg_t rxMsg;
  while (1)
  {
    // 2. 等待 DMA 叉车处于空闲状态
    // (如果上一帧还没发完，任务在这里挂起，不占 CPU)
    SemaphoreTake(DmaTxSem);
    // 1. 等待业务任务把日志扔进仓库（水池）
    QueueReceive(PrintQueue, &rxMsg);

    // 3. 叉车空闲了，启动 DMA 硬件发送！
    // 注意：这里调用的是 _DMA 后缀的非阻塞函数，调用后瞬间返回！
    HAL_UART_Transmit_DMA(&huart2, (uint8_t *)rxMsg.text, strlen(rxMsg.text));

    // 循环回去接着等队列，如果队列还有数据，马上拿出来，但会在 SemaphoreTake 处等前一帧发完。
  }
}

// ==========================================================
// 改造后的 Task1
// ==========================================================
void Task1_Entry(void)
{
  LogMsg_t txMsg;
  uint8_t len;
  while (1)
  {
    // 1. 先把时间戳数字变成字符串写入 txMsg.text
    len = my_itoa(OsRunningTime_ms, txMsg.text);

    // 2. 把后缀拼接到时间戳后面 (替代原来的 "%ums 1\n")
    strcpy(&txMsg.text[len], "ms 1\r\n");

    QueueSend(PrintQueue, &txMsg);
    taskdelay(8000);
  }
}

// ==========================================================
// 改造后的 Task2
// ==========================================================
void Task2_Entry(void)
{
  LogMsg_t txMsg;
  uint8_t len;
  while (1)
  {
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);

    len = my_itoa(OsRunningTime_ms, txMsg.text);
    strcpy(&txMsg.text[len], "ms 2\r\n");

    QueueSend(PrintQueue, &txMsg);
    taskdelay(500);
  }
}

// ==========================================================
// 改造后的 Task3 (VIP)
// ==========================================================
void Task3_Entry(void)
{
  LogMsg_t txMsg;
  uint8_t len;
  while (1)
  {
    len = my_itoa(OsRunningTime_ms, txMsg.text);
    strcpy(&txMsg.text[len], "ms 3_VIP\r\n");

    QueueSend(PrintQueue, &txMsg);
    taskdelay(2900);
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
  /* USER CODE BEGIN 2 */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);

  char *msg = "\r\n--- RTOS System Starting ---\r\n";
  HAL_UART_Transmit(&huart2, (uint8_t *)msg, strlen(msg), HAL_MAX_DELAY);
  HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13); // 点亮 LED
  // 1. 初始化 RTOS 堆内存
  my_os_heap_init();
  // 创建一个容量为 10，每个坑位大小为一个 LogMsg_t 的队列
  PrintQueue = QueueCreate(10, sizeof(LogMsg_t));
  // 【新增】创建一个二值信号量，初始值为 1 (代表资源可用)
  DmaTxSem = SemaphoreCreate(1);
  // 2. 创建三个任务，分配不同的优先级
  TaskCreate(PrintTask_Entry, 2, "PrintTask");
  TaskCreate(Task1_Entry, 1, (unsigned char *)"Task1");
  TaskCreate(Task2_Entry, 1, (unsigned char *)"Task2");
  TaskCreate(Task3_Entry, 2, (unsigned char *)"Task3_VIP"); // 高优先级

  // 3. 启动 RTOS 调度器，点火！
  StartScheduler();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    // 永远不会走到这里
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
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
   */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
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
/* USER CODE BEGIN 4 */

// 【新增】HAL库串口/DMA发送彻底完成后的中断回调函数
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART2)
  {
    // 核心：叉车搬完了，在中断里给出一发二值信号量！
    // 这会瞬间唤醒正在 SemaphoreTake 处死等的 PrintTask，让它安排下一车。
    SemaphoreGive(DmaTxSem);
  }
}

/* USER CODE END 4 */
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
