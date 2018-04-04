/* Standard includes. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ISR Includes. */
#include "system.h"
#include "altera_avalon_pio_regs.h"
#include "alt_types.h"
#include "sys/alt_irq.h"
#include "altera_up_avalon_ps2.h"

/* Scheduler includes. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

/* Definitions. */
#define mainREG_DECIDE_PARAMETER    ( ( void * ) 0x12345678 )
#define mainREG_LED_OUT_PARAMETER   ( ( void * ) 0x87654321 )
#define mainREG_VGA_OUT_PARAMETER 	( ( void * ) 0x12348765 )
#define mainREG_TEST_PRIORITY       ( tskIDLE_PRIORITY + 1)
#define SAMPLE_FREQ 				16000

#define PS2_1 0x69
#define PS2_2 0x72
#define PS2_3 0x7A
#define PS2_4 0x6B
#define PS2_5 0x73
#define PS2_6 0x74
#define PS2_7 0x6C
#define PS2_8 0x75
#define PS2_9 0x7D
#define PS2_0 0x70
#define PS2_dp 0x71
#define PS2_ENTER 0x5A
#define PS2_DP 0x71
#define PS2_KEYRELEASE 0xF0

/* Function Declarations. */
static void prvDecideTask(void *pvParameters);
static void prvLEDOutTask(void *pvParameters);
static void prvVGAOutTask(void *pvParameters);
void translate_ps2(unsigned char byte, double *value);

/* Global Variables. */
int first_load_shed = 0;
int shed_flag = 0;
int reconnect_load_timeout = 0;
int drop_load_timeout = 0;
int maintenance = 0;

double desired_max_roc_freq = 8;
double desired_min_freq = 48.5;
int desired_flag = 0;

double signal_freq = 0;
double roc_freq = 0;
int loads[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
int switches[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
double input_number = 0.0, input_decimal = 0.0, input_decimal_equiv = 0.0, input_final_number = 0.0;
int input_number_counter = 0, input_decimal_flag = 0, input_duplicate_flag = 0;

/* Handles. */
TimerHandle_t drop_timer;
TimerHandle_t recon_timer;

/* ISRs. */
void button_interrupts_function(void* context, alt_u32 id) {
	int* temp = (int*) context;
	(*temp) = IORD_ALTERA_AVALON_PIO_EDGE_CAP(PUSH_BUTTON_BASE); // Store which button was pressed

	if (maintenance == 1) {
		maintenance = 0;
		printf("Maintenance Mode Disabled\n");

		alt_up_ps2_dev *ps2_device = alt_up_ps2_open_dev(PS2_NAME);
		alt_up_ps2_disable_read_interrupt(ps2_device);
	} 
	else 
	{
		maintenance = 1;
		printf("Maintenance Mode Enabled\n");

		alt_up_ps2_dev *ps2_device = alt_up_ps2_open_dev(PS2_NAME);
		alt_up_ps2_clear_fifo(ps2_device);
		alt_up_ps2_enable_read_interrupt(ps2_device);
	}
	IOWR_ALTERA_AVALON_PIO_EDGE_CAP(PUSH_BUTTON_BASE, 0x7); // Clear edge capture register
}

void freq_relay() {
	unsigned int temp = IORD(FREQUENCY_ANALYSER_BASE, 0); // Get the sample count between the two most recent peaks
	
	//Important: do not swap the order of the two operations otherwise the roc will be 0 all the time
	if (temp > 0) {
		roc_freq = ((SAMPLE_FREQ / (double) temp) - signal_freq) * (SAMPLE_FREQ / (double) temp);
		signal_freq = SAMPLE_FREQ / (double) temp;
	}

	return;
}

void ps2_isr(void* ps2_device, alt_u32 id){
	unsigned char byte;
	alt_up_ps2_read_data_byte_timeout(ps2_device, &byte);

	if (byte == PS2_ENTER) {
		if(input_duplicate_flag == 1) {
			input_duplicate_flag = 0;
		} else {
			if(input_decimal_flag == 1) {
				input_decimal *= 10;
			} else {
				input_number /= 10;
			}
			input_final_number = input_number + input_decimal;
			
			if(desired_flag == 0) {
				desired_min_freq = input_final_number;
				printf("The preferred minimum frequency was set to: %f\n", desired_min_freq);
				desired_flag = 1;
			} else {
				desired_max_roc_freq = input_final_number;
				printf("The preferred maximum rate of change of frequency was set to: %f\n", desired_max_roc_freq);
				desired_flag = 0;
			}

			//Clear numbers
			input_number = 0.0;
			input_decimal = 0.0;
			input_final_number = 0.0;
			input_decimal_equiv = 0.0;

			input_decimal_flag = 0;
			input_number_counter = 0;
		}
	} else if(byte == PS2_KEYRELEASE) {
		input_duplicate_flag = 1;
	} else {
		if(input_duplicate_flag == 1) {
			input_duplicate_flag = 0;
		} else {
			//Take care of decimal point
			if (byte == PS2_DP) {
				input_decimal_flag = 1;
			}

			if (input_decimal_flag == 0) {
				//Take care of upper part of number
				input_number *= 10;
				
				//Translate and add to upper part of number
				translate_ps2(byte, &input_number);
			} else {
				//Take care of lower part of number
				input_number_counter += 1;
				input_decimal_equiv *= 10;

				//Translate and add to lower part of number
				translate_ps2(byte, &input_decimal_equiv);

				//Convert to 'normal' decimal
				int i = 0;
				input_decimal = input_decimal_equiv;
				for(i = 0; i < input_number_counter; i++) {
					input_decimal /= 10.0;
				}
			}
		}
	}
}

/* Function Macros. */
#define drop_load() { \
	if (loads[0] == 1) loads[0] = 0; \
	else if (loads[1] == 1) loads[1] = 0; \
	else if (loads[2] == 1) loads[2] = 0; \
	else if (loads[3] == 1) loads[3] = 0; \
	else if (loads[4] == 1) loads[4] = 0; \
	else if (loads[5] == 1) loads[5] = 0; \
	else if (loads[6] == 1) loads[6] = 0; \
	else loads[7] = 0; \
}

#define reconnect_load() { \
	if ((loads[7] == 0) && (switches[7] == 1)) loads[7] = 1; \
	else if ((loads[6] == 0) && (switches[6] == 1)) loads[6] = 1; \
	else if ((loads[5] == 0) && (switches[5] == 1)) loads[5] = 1; \
	else if ((loads[4] == 0) && (switches[4] == 1)) loads[4] = 1; \
	else if ((loads[3] == 0) && (switches[3] == 1)) loads[3] = 1; \
	else if ((loads[2] == 0) && (switches[2] == 1)) loads[2] = 1; \
	else if ((loads[1] == 0) && (switches[1] == 1)) loads[1] = 1; \
	else if (switches[0] == 1) loads[0] = 1; \
}

/* Functions. */
void translate_ps2(unsigned char byte, double *value) {
	switch(byte) {
		case PS2_0:
			*value += 0.0;
			break;
		case PS2_1:
			*value += 1.0;
			break;
		case PS2_2:
			*value += 2.0;
			break;
		case PS2_3:
			*value += 3.0;
			break;
		case PS2_4:
			*value += 4.0;
			break;
		case PS2_5:
			*value += 5.0;
			break;
		case PS2_6:
			*value += 6.0;
			break;
		case PS2_7:
			*value += 7.0;
			break;
		case PS2_8:
			*value += 8.0;
			break;
		case PS2_9:
			*value += 9.0;
			break;
		default:
			break;
	}
}

/* Callbacks. */
void vTimerDropCallback(xTimerHandle t_timer) {
	drop_load_timeout = 1;
}

void vTimerReconnectCallback(xTimerHandle t_timer) {
	reconnect_load_timeout = 1;
}

/* Main function. */
int main(void) {
	// Set up Interrupts
	int button_value = 0;
	IOWR_ALTERA_AVALON_PIO_EDGE_CAP(PUSH_BUTTON_BASE, 0x7);
	IOWR_ALTERA_AVALON_PIO_IRQ_MASK(PUSH_BUTTON_BASE, 0x4);
	alt_irq_register(PUSH_BUTTON_IRQ,(void*)&button_value, button_interrupts_function);

	alt_irq_register(FREQUENCY_ANALYSER_IRQ, 0, freq_relay);

	alt_up_ps2_dev *ps2_device = alt_up_ps2_open_dev(PS2_NAME);

	//Set up keyboard
	if(ps2_device == NULL){
		printf("Couldn't find a PS/2 device\n");
		return 1;
	}

	alt_up_ps2_disable_read_interrupt(ps2_device);
	alt_irq_register(PS2_IRQ, ps2_device, ps2_isr);

	drop_timer = xTimerCreate("Shedding Timer", 500, pdFALSE, NULL, vTimerDropCallback);
	recon_timer = xTimerCreate("Reconnect Timer", 500, pdFALSE, NULL, vTimerReconnectCallback);

	// Set up Tasks
	xTaskCreate( prvDecideTask, "Rreg1", configMINIMAL_STACK_SIZE, mainREG_DECIDE_PARAMETER, mainREG_TEST_PRIORITY, NULL);
	xTaskCreate( prvLEDOutTask, "Rreg2", configMINIMAL_STACK_SIZE, mainREG_LED_OUT_PARAMETER, mainREG_TEST_PRIORITY, NULL);
	xTaskCreate( prvVGAOutTask, "Rreg3", configMINIMAL_STACK_SIZE, mainREG_VGA_OUT_PARAMETER, mainREG_TEST_PRIORITY, NULL);
	
	//Start task scheduler
	vTaskStartScheduler();

	// Only reaches here if not enough heap space to start tasks
	for(;;);
}

/* Tasks. */
static void prvDecideTask(void *pvParameters) {
	while (1) {
		// Switch Load Management
		int switch_value = IORD_ALTERA_AVALON_PIO_DATA(SLIDE_SWITCH_BASE);
		int masked_switch_value = switch_value & 0x000ff;

		int i, k, no_loads_shed = 1;
		for (i = 7; i >= 0; i--) { // Iterate through switches array and set if the switch is on or off
			k = masked_switch_value >> i;
			if (k & 1) { // If the switch at this position is on
				switches[7-i] = 1;
				if (loads[7-i] == 0) {
					no_loads_shed = 0;
				}
			} else { // If the switch at this position is off
				switches[7-i] = 0;
				loads[7-i] = 0;
			}
		}

		if (no_loads_shed == 1) { // If all available loads are connected, we are not managing loads.
			first_load_shed = 0;
		}

		// Frequency Load Management
		if (maintenance == 0) {
			if (fabs(roc_freq) > desired_max_roc_freq || desired_min_freq > signal_freq) { // If the current system is unstable
				if (first_load_shed == 0) { // Drop a load, if we have no dropped loads. First load drop.
					first_load_shed = 1;
					drop_load();
				} else {
					reconnect_load_timeout = 0; // No longer a continuous run of stable data

					if(shed_flag == 0) { // Stop the timer if we are timing a 500ms for a load reconnection
						xTimerStop(recon_timer, 0);
					}

					if (drop_load_timeout == 0) { // If we haven't had a continuous run of unstable data
						if (xTimerIsTimerActive(drop_timer) == pdFALSE) {
							xTimerStart(drop_timer, 0);
						}
					} else {
						drop_load();
					}
					shed_flag = 1;
				}
			} else {
				drop_load_timeout = 0; // No longer a continuous run of unstable data
				
				if (shed_flag == 1) { // Stop the timer if we are timing a 500ms for a load drop
					xTimerStop(drop_timer, 0);
				}

				if (reconnect_load_timeout == 0) { // If we haven't had a continuous run of stable data
					if (xTimerIsTimerActive(recon_timer) == pdFALSE) {
						xTimerStart(recon_timer, 0);
					}
				} else {
					reconnect_load();
				}
				shed_flag = 0;
			}
		}
		vTaskDelay(20);
	}
}

static void prvLEDOutTask(void *pvParameters)
{
	while (1)
	{
		int loads_num = 0;
		int loads_num_rev = 0;
		int i;
		
		// Inverse array for Red LEDS
		int rev_loads[8];
		for (i = 0; i < 8; i++) {
			if (loads[i] == 0) {
				rev_loads[i] = 1;
			} else {
				rev_loads[i] = 0;
			}
		}

		//Translate to binary
		for (i = 0; i < 8; i++) {
			loads_num = loads_num << 1;
			loads_num = loads_num + loads[i];
			loads_num_rev = loads_num_rev << 1;
			loads_num_rev = loads_num_rev + rev_loads[i];
		}

		// Write to LEDs base
		if (maintenance == 0) {
			IOWR_ALTERA_AVALON_PIO_DATA(GREEN_LEDS_BASE, loads_num_rev);
		} else {
			IOWR_ALTERA_AVALON_PIO_DATA(GREEN_LEDS_BASE, 0);
		}
		IOWR_ALTERA_AVALON_PIO_DATA(RED_LEDS_BASE, loads_num);

		vTaskDelay(10);
	}
}

static void prvVGAOutTask(void *pvParameters)
{
	while (1)
	{
		if (maintenance == 0) {
			printf("Signal frequency: %f Hz\n", signal_freq);
			printf("Rate of change: %f\n", roc_freq);
		}
		vTaskDelay(100);
	}
}
