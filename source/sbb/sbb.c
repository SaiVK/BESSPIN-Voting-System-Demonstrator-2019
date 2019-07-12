/**
 * Smart Ballot Box API
 * @refine sbb.lando
 */

// General includes
#include <stdio.h>
#include <string.h>

// FreeRTOS specific includes
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

// Subsystem includes
#include "sbb.h"
#include "sbb_freertos.h"
#include "sbb_logging.h"
#include "crypto.h"

// BESSPIN Voting System devices
#include "gpio.h"
#include "serLcd.h"

// Timeouts
#define BALLOT_DETECT_TIMEOUT_MS 10000
#define CAST_OR_SPOIL_TIMEOUT_MS 30000
#define SPOIL_EJECT_TIMEOUT_MS 6000
#define CAST_INGEST_TIMEOUT_MS 6000

TickType_t ballot_detect_timeout = 0;
TickType_t cast_or_spoil_timeout = 0;

bool barcode_present = false;
char barcode[BARCODE_MAX_LENGTH] = {0};
barcode_length_t barcode_length  = 0;
SemaphoreHandle_t barcode_mutex;


// Needed for checking barcode validity
#define TIMESTAMP_LENGTH_BYTES 4
#define ENCRYPTED_BLOCK_LENGTH_BYTES AES_BLOCK_LENGTH_BYTES
#define DECODED_BARCODE_LENGTH_BYTES                                    \
    ENCRYPTED_BLOCK_LENGTH_BYTES + TIMESTAMP_LENGTH_BYTES + AES_BLOCK_LENGTH_BYTES
// Round up to the next multiple of AES_BLOCK_LENGTH_BYTES (which is a power of 2)
#define CBC_MAC_MESSAGE_BYTES                                           \
    (((TIMESTAMP_LENGTH_BYTES + AES_BLOCK_LENGTH_BYTES) + (AES_BLOCK_LENGTH_BYTES-1)) & ~(AES_BLOCK_LENGTH_BYTES-1))

// Assigns declarations for FreeRTOS functions; these may not be
// accurate but are currently required to avoid crashing wp.

//@ assigns \nothing;
extern void serLcdPrintf(char *str, uint8_t len);
//@ assigns \nothing;
extern void serLcdPrintTwoLines(char* line_1, uint8_t len_1, char* line_2, uint8_t len_2);
//@ assigns \nothing;
extern size_t xStreamBufferReceive(StreamBufferHandle_t xStreamBuffer,
                                   void *pvRxData,
                                   size_t xBufferLengthBytes,
                                   TickType_t xTicksToWait);
//@ assigns \nothing;
extern EventBits_t xEventGroupWaitBits(EventGroupHandle_t xEventGroup,
                                       const EventBits_t uxBitsToWaitFor,
                                       const BaseType_t xClearOnExit,
                                       const BaseType_t xWaitForAllBits,
                                       TickType_t xTicksToWait);

// main code

void initialize(void) {
    gpio_set_as_input(BUTTON_CAST_IN);
    gpio_set_as_input(BUTTON_SPOIL_IN);
    gpio_set_as_input(PAPER_SENSOR_IN);
    gpio_set_as_input(PAPER_SENSOR_OUT);
    gpio_set_as_output(MOTOR_0);
    gpio_set_as_output(MOTOR_1);
    gpio_set_as_output(BUTTON_CAST_LED);
    gpio_set_as_output(BUTTON_SPOIL_LED);

    barcode_mutex = xSemaphoreCreateMutex();
 DevicesInitialized: return;
}

/* global invariant Button_lighting_conditions_power_on:
   \forall cast_button_light cbl, spoil_button_light sbl;
   \at(lights_off(cbl, sbl), DevicesInitialized);
*/

/* global invariant Paper_ejected_on_power_on:
   \forall paper_present p; \at(p == none, DevicesInitialized);
*/

/* global invariant Motor_initial_state:
   \forall motor m; \at(!motor_running(m), DevicesInitialized);
*/

void perform_tabulation(void) { printf("Performing tabulation!\r\n"); }

bool is_barcode_valid(barcode_t the_barcode, barcode_length_t its_length) {
    /**
       the_barcode = base64( encrypted-block # timestamp # cbc-mac       )
         where
           cbc-mac         = last AES block of AES( encrypted-block # timestamp )
    */
    int r;
    size_t olen;
    // 1. Decode
    uint8_t decoded_barcode[DECODED_BARCODE_LENGTH_BYTES] = {0};
    r = mbedtls_base64_decode(&decoded_barcode[0], DECODED_BARCODE_LENGTH_BYTES, // destination
                              &olen,                                       // output length
                              &the_barcode[0],     its_length);            // source
    configASSERT(DECODED_BARCODE_LENGTH_BYTES == olen);

    // 2. Copy to a new buffer for AES_CBC_MAC, pad with 0.
    uint8_t our_digest_input[CBC_MAC_MESSAGE_BYTES] = {0};
    uint8_t our_digest_output[AES_BLOCK_LENGTH_BYTES] = {0};
    memcpy(&our_digest_input[0],
           &decoded_barcode[0],
           ENCRYPTED_BLOCK_LENGTH_BYTES+TIMESTAMP_LENGTH_BYTES);
    aes_cbc_mac(&our_digest_input[0], CBC_MAC_MESSAGE_BYTES, // Input
                &our_digest_output[0]);                        // Output

    // 3. Compare computed digest against cbc-mac in the barcode
    bool b_match = true;
    for (size_t i = 0; b_match && (i < AES_BLOCK_LENGTH_BYTES); ++i) {
        b_match &= (our_digest_output[i] == decoded_barcode[i + ENCRYPTED_BLOCK_LENGTH_BYTES + TIMESTAMP_LENGTH_BYTES]);
    }

    return b_match;
}

bool is_cast_button_pressed(void) {
    return the_state.B == CAST_BUTTON_DOWN;
}

bool is_spoil_button_pressed(void) {
    return the_state.B == SPOIL_BUTTON_DOWN;
}

void just_received_barcode(void) {
    if (xSemaphoreTake(barcode_mutex, portMAX_DELAY) == pdTRUE) {
        barcode_present = true;
        xSemaphoreGive(barcode_mutex);
    }
}

void set_received_barcode(barcode_t the_barcode, barcode_length_t its_length) {
    configASSERT(its_length <= BARCODE_MAX_LENGTH);
    if (xSemaphoreTake(barcode_mutex, portMAX_DELAY) == pdTRUE) {
        memcpy(barcode, the_barcode, its_length);
        barcode_length = its_length;
        xSemaphoreGive(barcode_mutex);
    }
}

bool has_a_barcode(void) {
    return the_state.BS == BARCODE_PRESENT_AND_RECORDED;
}

barcode_length_t what_is_the_barcode(barcode_t the_barcode) {
    configASSERT(barcode_length > 0);
    memcpy(the_barcode, barcode, barcode_length);
    return barcode_length;
}

void spoil_button_light_on(void) { gpio_write(BUTTON_SPOIL_LED); }

void spoil_button_light_off(void) { gpio_clear(BUTTON_SPOIL_LED); }

void cast_button_light_on(void) { gpio_write(BUTTON_CAST_LED); }

void cast_button_light_off(void) { gpio_clear(BUTTON_CAST_LED); }

void move_motor_forward(void) {
    gpio_clear(MOTOR_0);
    gpio_write(MOTOR_1);
    CHANGE_STATE(the_state, M, MOTORS_TURNING_FORWARD);
}

void move_motor_back(void) {
    gpio_write(MOTOR_0);
    gpio_clear(MOTOR_1);
    CHANGE_STATE(the_state, M, MOTORS_TURNING_BACKWARD);
}

void stop_motor(void) {
    gpio_clear(MOTOR_0);
    gpio_clear(MOTOR_1);
    CHANGE_STATE(the_state, M, MOTORS_OFF);
}


void display_this_text(const char *the_text, uint8_t its_length) {
    #ifdef SIMULATION
    debug_printf("DISPLAY: %s\r\n", the_text);
    #else
    serLcdPrintf(the_text, its_length);
    #endif
}

void display_this_2_line_text(const char *line_1, uint8_t length_1,
                              const char *line_2, uint8_t length_2) {
    #ifdef SIMULATION
    debug_printf("DISPLAY: %s\r\nLINETWO: %s\r\n", line_1, line_2);
    #else
    serLcdPrintTwoLines(line_1, length_1, line_2, length_2);
    #endif
}

bool ballot_detected(void) {
    return (the_state.P == PAPER_DETECTED);
}

void eject_ballot(void) {
    move_motor_back();
    // run the motor for a bit to get the paper back over the switch
    TickType_t spoil_timeout =
        xTaskGetTickCount() + pdMS_TO_TICKS(SPOIL_EJECT_TIMEOUT_MS);
    while (xTaskGetTickCount() < spoil_timeout) {
        // wait for the motor to run a while
    }

    stop_motor();
}

void spoil_ballot(void) {
    spoil_button_light_off();
    cast_button_light_off();
    display_this_text(spoiling_ballot_text,
                      strlen(spoiling_ballot_text));
    eject_ballot();
}

void cast_ballot(void) {
    move_motor_forward();

    // run the motor for a bit to get the paper into the box
    TickType_t cast_timeout =
        xTaskGetTickCount() + pdMS_TO_TICKS(CAST_INGEST_TIMEOUT_MS);
    while (xTaskGetTickCount() < cast_timeout) {
        // wait for the motor to run a while
    }

    stop_motor();
}

void go_to_standby(void) {
    stop_motor();
    cast_button_light_off();
    spoil_button_light_off();
    display_this_2_line_text(welcome_text, strlen(welcome_text),
                             insert_ballot_text, strlen(insert_ballot_text));
    CHANGE_STATE(the_state, M, MOTORS_OFF);
    CHANGE_STATE(the_state, D, SHOWING_TEXT);
    CHANGE_STATE(the_state, P, NO_PAPER_DETECTED);
    CHANGE_STATE(the_state, BS, BARCODE_NOT_PRESENT);
    CHANGE_STATE(the_state, S, INNER);
    CHANGE_STATE(the_state, B, ALL_BUTTONS_UP);
}

void ballot_detect_timeout_reset(void) {
    ballot_detect_timeout =
        xTaskGetTickCount() + pdMS_TO_TICKS(BALLOT_DETECT_TIMEOUT_MS);
}

bool ballot_detect_timeout_expired(void) {
    return (xTaskGetTickCount() > ballot_detect_timeout);
}

void cast_or_spoil_timeout_reset(void) {
    cast_or_spoil_timeout =
        xTaskGetTickCount() + pdMS_TO_TICKS(CAST_OR_SPOIL_TIMEOUT_MS);
}

bool cast_or_spoil_timeout_expired(void) {
    return (xTaskGetTickCount() > cast_or_spoil_timeout);
}
