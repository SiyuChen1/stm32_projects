/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h5xx_hal.h"

#include "stm32h5xx_nucleo.h"
#include <stdio.h>

#include "stm32h5xx_ll_ucpd.h"
#include "stm32h5xx_ll_bus.h"
#include "stm32h5xx_ll_cortex.h"
#include "stm32h5xx_ll_rcc.h"
#include "stm32h5xx_ll_system.h"
#include "stm32h5xx_ll_utils.h"
#include "stm32h5xx_ll_pwr.h"
#include "stm32h5xx_ll_gpio.h"
#include "stm32h5xx_ll_dma.h"

#include "stm32h5xx_ll_exti.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define TRACE_CK_Pin GPIO_PIN_2
#define TRACE_CK_GPIO_Port GPIOE
#define TRACE_D0_Pin GPIO_PIN_3
#define TRACE_D0_GPIO_Port GPIOE
#define TRACE_D1_Pin GPIO_PIN_4
#define TRACE_D1_GPIO_Port GPIOE
#define TRACE_D2_Pin GPIO_PIN_5
#define TRACE_D2_GPIO_Port GPIOE
#define TRACE_D3_Pin GPIO_PIN_6
#define TRACE_D3_GPIO_Port GPIOE
#define USER_BUTTON_Pin GPIO_PIN_13
#define USER_BUTTON_GPIO_Port GPIOC
#define LED2_YELLOW_Pin GPIO_PIN_4
#define LED2_YELLOW_GPIO_Port GPIOF
#define RMII_MDC_Pin GPIO_PIN_1
#define RMII_MDC_GPIO_Port GPIOC
#define RMII_REF_CLK_Pin GPIO_PIN_1
#define RMII_REF_CLK_GPIO_Port GPIOA
#define RMII_MDIO_Pin GPIO_PIN_2
#define RMII_MDIO_GPIO_Port GPIOA
#define DEBUG_GPIO_1_Pin GPIO_PIN_3
#define DEBUG_GPIO_1_GPIO_Port GPIOA
#define VBUS_SENSE_Pin GPIO_PIN_4
#define VBUS_SENSE_GPIO_Port GPIOA
#define RMII_CRS_DV_Pin GPIO_PIN_7
#define RMII_CRS_DV_GPIO_Port GPIOA
#define RMII_RXD0_Pin GPIO_PIN_4
#define RMII_RXD0_GPIO_Port GPIOC
#define RMII_RXD1_Pin GPIO_PIN_5
#define RMII_RXD1_GPIO_Port GPIOC
#define LED1_GREEN_Pin GPIO_PIN_0
#define LED1_GREEN_GPIO_Port GPIOB
#define SYNC_IN_Pin GPIO_PIN_1
#define SYNC_IN_GPIO_Port GPIOB
#define UCPD_CC1_Pin GPIO_PIN_13
#define UCPD_CC1_GPIO_Port GPIOB
#define UCPD_CC2_Pin GPIO_PIN_14
#define UCPD_CC2_GPIO_Port GPIOB
#define RMII_TXD1_Pin GPIO_PIN_15
#define RMII_TXD1_GPIO_Port GPIOB
#define LED3_RED_Pin GPIO_PIN_4
#define LED3_RED_GPIO_Port GPIOG
#define UCPD_DBn_Pin GPIO_PIN_9
#define UCPD_DBn_GPIO_Port GPIOA
#define USB_FS_N_Pin GPIO_PIN_11
#define USB_FS_N_GPIO_Port GPIOA
#define USB_FS_P_Pin GPIO_PIN_12
#define USB_FS_P_GPIO_Port GPIOA
#define SWDIO_Pin GPIO_PIN_13
#define SWDIO_GPIO_Port GPIOA
#define SWCLK_Pin GPIO_PIN_14
#define SWCLK_GPIO_Port GPIOA
#define T_JTDI_Pin GPIO_PIN_15
#define T_JTDI_GPIO_Port GPIOA
#define RMII_TXT_EN_Pin GPIO_PIN_11
#define RMII_TXT_EN_GPIO_Port GPIOG
#define RMI_TXD0_Pin GPIO_PIN_13
#define RMI_TXD0_GPIO_Port GPIOG
#define SWO_Pin GPIO_PIN_3
#define SWO_GPIO_Port GPIOB
#define XSHUT_Pin GPIO_PIN_6
#define XSHUT_GPIO_Port GPIOB
#define INTR_Pin GPIO_PIN_7
#define INTR_GPIO_Port GPIOB
#define INTR_EXTI_IRQn EXTI7_IRQn
#define DEBUG_GPIO_2_Pin GPIO_PIN_0
#define DEBUG_GPIO_2_GPIO_Port GPIOE

/* USER CODE BEGIN Private defines */
/**
 * Hand-added (not CubeMX-regenerated) for the STM32 -> XIAO SPI bridge - see
 * MX_SPI4_Init()/HAL_SPI_MspInit() and send_vis_frame_spi() in vl53l9_app.c, and
 * stm32_utility/spi/spi.ino in the separate Arduino repo for the receiving side.
 *
 * Verified free of the X-NUCLEO-53L9A1 shield's pins (I3C1 on PB8/PB9, XSHUT on
 * PB6, INTR on PB7, SYNC_IN on PB1, TIM3_CH2 on PB5, the 3 status LEDs) and of the
 * "uart" sibling sketch's link (USART6 on PG14/PG9), cross-checked against this
 * project's own .ioc and the Nucleo-144 Zio connector map. SPI4_SCK (PE12),
 * SPI4_MISO (PE13) and SPI4_MOSI (PE14) are real SPI4/AF5 pins per ST's own
 * CubeMX pin database (db/mcu/STM32H563ZITx.xml) - not a guess. SPI4_NSS (PE11)
 * is driven as a plain output GPIO (software-managed NSS), not the SPI
 * peripheral's hardware NSS. Arduino D-numbers, where the pin has one: PE13=D3,
 * PE14=D4, PE11=D5, PE9=D6 (PE12 is Zio-only, no base D0-D15 label).
 */
#define SPI4_NSS_Pin GPIO_PIN_11
#define SPI4_NSS_GPIO_Port GPIOE
#define XIAO_READY_Pin GPIO_PIN_9
#define XIAO_READY_GPIO_Port GPIOE
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
