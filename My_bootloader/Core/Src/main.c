/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

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

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
#define APP_START_ADDRESS  0x08002000     // App 起始地址（28KB）
#define STAGING_ADDR       0x08009000     // Staging 起始地址（28KB）
#define STAGING_SIZE       0x00007000
#define APP_SIZE           0x00007000
#define FLAG_PAGE_ADDR     0x08001C00     // 标志位页（独立第7页）
#define UPDATE_FLAG_ADDR   0x08001FFC     // 标志位地址
#define COPY_PROGRESS_ADDR 0x08001C00     // 拷贝进度位图（8 字节，每页 1 bit）
#define MAGIC_UPDATE_READY 0xA5A5A5A5

typedef void (*pFunction)(void);

/**
  * @brief 搬运新固件：擦 App → 拷贝 Staging → 清理
  */
/* copy_new_firmware: Copy with progress bitmap (1-bit per page, zero-copy-power-loss safe) */
static void copy_new_firmware(void)
{
    uint32_t src, dst, word, page_error = 0;
    uint32_t progress_lo, progress_hi;
    FLASH_EraseInitTypeDef erase = {0};

    HAL_FLASH_Unlock();

    /* Read progress bitmap (bit N = 1 = page N not yet copied) */
    progress_lo = *(__IO uint32_t*)COPY_PROGRESS_ADDR;
    progress_hi = *(__IO uint32_t*)(COPY_PROGRESS_ADDR + 4);

    /* Copy each unfinished page */
    for (uint32_t page = 0; page < 28; page++) {
        uint32_t mask = 1UL << (page & 31);        /* bit mask within 32-bit word */

        if (page < 32) {
            if (!(progress_lo & mask)) continue;    /* already done */
        } else {
            if (!(progress_hi & mask)) continue;
        }

        /* Erase this page first (ensures 0xFFFF state after power cut) */
        erase.TypeErase = FLASH_TYPEERASE_PAGES;
        erase.PageAddress = APP_START_ADDRESS + page * 1024;
        erase.NbPages = 1;
        if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK) while(1);

        /* Copy one page (1KB = 256 words) */
        for (uint32_t off = 0; off < 1024; off += 4) {
            dst = APP_START_ADDRESS + page * 1024 + off;
            src = STAGING_ADDR + page * 1024 + off;
            word = *(__IO uint32_t*)src;
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, dst, word) != HAL_OK) while(1);
        }

        /* Mark page done: clear the bit (Program 1->0, no erase needed) */
        uint32_t new_val;
        if (page < 32) {
            new_val = progress_lo & ~mask;
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, COPY_PROGRESS_ADDR, new_val);
            progress_lo = new_val;
        } else {
            new_val = progress_hi & ~mask;
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, COPY_PROGRESS_ADDR + 4, new_val);
            progress_hi = new_val;
        }
    }

    /* All done: erase staging + flag page (clears MAGIC + progress together) */
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = STAGING_ADDR;
    erase.NbPages = 28;
    HAL_FLASHEx_Erase(&erase, &page_error);

    erase.PageAddress = FLAG_PAGE_ADDR;
    erase.NbPages = 1;
    HAL_FLASHEx_Erase(&erase, &page_error);

    HAL_FLASH_Lock();
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
/* Bootloader 入口: 检查标志位 -> 搬运 -> 跳转 App | Bootloader entry: check flag -> copy -> jump */
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
  /* USER CODE BEGIN 2 */
    // === check update flag ===
    uint32_t magic = *(__IO uint32_t*)UPDATE_FLAG_ADDR;
    if (magic == MAGIC_UPDATE_READY) {
        copy_new_firmware();
    }

    // === jump to app ===
    uint32_t app_stack_pointer = *(__IO uint32_t*)APP_START_ADDRESS;

    if ((app_stack_pointer & 0x2FFE0000) == 0x20000000)
    {
        __disable_irq();
        uint32_t app_reset_handler = *(__IO uint32_t*)(APP_START_ADDRESS + 4);
        pFunction JumpToApp = (pFunction)app_reset_handler;
        SCB->VTOR = APP_START_ADDRESS;
        __set_MSP(app_stack_pointer);
        JumpToApp();
    }
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

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
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
