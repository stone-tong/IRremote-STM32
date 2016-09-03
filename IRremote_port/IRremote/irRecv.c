#include "IRremote.h"
#include "IRremoteInt.h"
#include "gpio.h"
#include "stm32f4xx_hal_tim.h"

//+=============================================================================
// Decodes the received IR message
// Returns 0 if no data ready, 1 if data ready.
// Results of decoding are stored in results
//
int IRrecv_decode (ir_decode_results *results)
{
	results->rawbuf   = irparams.rawbuf;
	results->rawlen   = irparams.rawlen;
	results->overflow = irparams.overflow;

	if (irparams.rcvstate != IR_STATE_STOP)  return 0 ;

#if IR_DECODE_SONY
	IR_DBG_PRINTLN("Attempting Sony decode");
	if (IRrecv_decodeSony(results))  return 1 ;
#endif

#if IR_DECODE_RC5
	IR_DBG_PRINTLN("Attempting RC5 decode");
	if (IRrecv_decodeRC5(results))  return 1 ;
#endif

#if IR_DECODE_RC6
	IR_DBG_PRINTLN("Attempting RC6 decode");
	if (IRrecv_decodeRC6(results))  return 1 ;
#endif

	// decodeHash returns a hash on any input.
	// Thus, it needs to be last in the list.
	// If you add any decodes, add them before this.
	if (IRrecv_decodeHash(results))  return 1 ;

	// Throw away and start over
	IRrecv_resume();
	return 0;
}

//+=============================================================================
void IRrecv_IRrecvInit (GPIO_TypeDef* recvpinport, uint16_t recvpin)
{
	irparams.recvpinport = recvpinport;
	irparams.recvpin = recvpin;
	irparams.blinkflag = 0;
}

void IRrecv_IRrecvInitBlink (GPIO_TypeDef* recvpinport, uint16_t recvpin, GPIO_TypeDef* blinkpinport, uint16_t blinkpin)
{
	irparams.recvpinport = recvpinport;
	irparams.recvpin = recvpin;
	irparams.blinkpinport = blinkpinport;
	irparams.blinkpin = blinkpin;

	GPIO_InitTypeDef GPIO_InitStruct;
	GPIO_InitStruct.Pin = blinkpin;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	HAL_GPIO_Init(blinkpinport, &GPIO_InitStruct);

	irparams.blinkflag = 0;
}



//+=============================================================================
// initialization
//
void  IRrecv_enableIRIn()
{
	// Setup pulse clock timer interrupt
	// Prescale /8 (16M/8 = 0.5 microseconds per tick)
	// Therefore, the timer interval can range from 0.5 to 128 microseconds
	// Depending on the reset value (255 to 0)

	TIM_HandleTypeDef htim2;
	htim2.Instance = TIM2;
	htim2.Init.Prescaler = 1000;
	htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
	htim2.Init.Period = 1000;
	htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV2;
	if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
	{
		Error_Handler();
	}
	TIM_ClockConfigTypeDef sClockSourceConfig;
	sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
	if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
	{
		Error_Handler();
	}

	/* TIM4 clock enable */
	__HAL_RCC_TIM9_CLK_ENABLE();

	/* TIM4 interrupt init */
	HAL_NVIC_SetPriority(TIM1_BRK_TIM2_IRQn, 2, 0);
	HAL_NVIC_EnableIRQ(TIM1_BRK_TIM2_IRQn);
	if(HAL_TIM_Base_Start_IT(&htim2) != HAL_OK)
	{
		Error_Handler();
	}

	// Initialize state machine variables
	irparams.rcvstate = IR_STATE_IDLE;
	irparams.rawlen = 0;

	// Set pin modes
	GPIO_InitTypeDef GPIO_InitStruct;
	GPIO_InitStruct.Pin = irparams.recvpin;
	GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
	GPIO_InitStruct.Pull = GPIO_PULLDOWN;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	HAL_GPIO_Init(irparams.recvpinport, &GPIO_InitStruct);
}

//+=============================================================================
// Return if receiving new IR signals
//
uint8_t  IRrecv_isIdle ( )
{
 return (irparams.rcvstate == IR_STATE_IDLE || irparams.rcvstate == IR_STATE_STOP) ? 1 : 0;
}
//+=============================================================================
// Restart the ISR state machine
//
void  IRrecv_resume ( )
{
	irparams.rcvstate = IR_STATE_IDLE;
	irparams.rawlen = 0;
}

//+=============================================================================
// hashdecode - decode an arbitrary IR code.
// Instead of decoding using a standard encoding scheme
// (e.g. Sony, NEC, RC5), the code is hashed to a 32-bit value.
//
// The algorithm: look at the sequence of MARK signals, and see if each one
// is shorter (0), the same length (1), or longer (2) than the previous.
// Do the same with the SPACE signals.  Hash the resulting sequence of 0's,
// 1's, and 2's to a 32-bit value.  This will give a unique value for each
// different code (probably), for most code systems.
//
// http://arcfn.com/2010/01/using-arbitrary-remotes-with-arduino.html
//
// Compare two tick values, returning 0 if newval is shorter,
// 1 if newval is equal, and 2 if newval is longer
// Use a tolerance of 20%
//
int  IRrecv_compare (unsigned int oldval,  unsigned int newval)
{
	if      (newval < oldval * .8)  return 0 ;
	else if (oldval < newval * .8)  return 2 ;
	else                            return 1 ;
}

//+=============================================================================
// Use FNV hash algorithm: http://isthe.com/chongo/tech/comp/fnv/#FNV-param
// Converts the raw code values into a 32-bit hash code.
// Hopefully this code is unique for each button.
// This isn't a "real" decoding, just an arbitrary value.
//
#define IR_FNV_PRIME_32 16777619
#define IR_FNV_BASIS_32 2166136261

long  IRrecv_decodeHash (ir_decode_results *results)
{
	long  hash = IR_FNV_BASIS_32;

	// Require at least 6 samples to prevent triggering on noise
	if (results->rawlen < 6)  return 0 ;

	for (int i = 1;  (i + 2) < results->rawlen;  i++) {
		int value =  IRrecv_compare(results->rawbuf[i], results->rawbuf[i+2]);
		// Add value into the hash
		hash = (hash * IR_FNV_PRIME_32) ^ value;
	}

	results->value       = hash;
	results->bits        = 32;
	results->decode_type = UNKNOWN;

	return 1;
}