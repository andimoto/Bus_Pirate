/*
 * This file is part of the Bus Pirate project
 * (http://code.google.com/p/the-bus-pirate/).
 *
 * Written and maintained by the Bus Pirate project.
 *
 * To the extent possible under law, the project has waived all copyright and
 * related or neighboring rights to Bus Pirate.  This work is published from
 * United States.
 *
 * For details see: http://creativecommons.org/publicdomain/zero/1.0/
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 */

/**
 * @file aux_pin.c
 *
 * @brief AUX pins handler implementation file.
 */
#include <stdint.h>

#include "aux_pin.h"
#include "base.h"
#include "proc_menu.h"

// TRISDbits.TRISD5
#define AUXPIN_DIR BP_AUX0_DIR

// 20
#define AUXPIN_RPIN BP_AUX_RPIN

// RPOR10bits.RP20R
#define AUXPIN_RPOUT BP_AUX_RPOUT

extern mode_configuration_t mode_configuration;
extern bool command_error;

/**
 * @brief Possible modes for the AUX pins.
 */
typedef enum {
  /** The AUX pin is set in I/O mode. */
  AUX_MODE_IO = 0,
  /** The AUX pin is set in Frequency Counting mode. */
  AUX_MODE_FREQUENCY,
  /** The AUX pin is set in PWM Signal Generation mode. */
  AUX_MODE_PWM
} __attribute__((packed)) aux_mode_t;

/**
 * @brief Reads the AUX signal for one second, returning the detected frequency.
 *
 * @return the detected frequency on the AUX pin in the second spent sampling,
 * in Hz.
 */
static uint32_t poll_frequency_counter_value(void);

/**
 * @brief Gets the average frequency for the given samples count.
 *
 * @param count[in] the number of samples to obtain.
 *
 * @return the average frequency value, in Hz.
 */
static uint32_t average_sample_frequency(const uint16_t count);

/**
 * @brief AUX pins manager internal state variables container.
 */
typedef struct {
  /** The PWM frequency in use. */
  uint16_t pwm_frequency;
  /** The PWM duty cycle in use. */
  uint16_t pwm_duty_cycle;
  /** The AUX pin mode. */
  aux_mode_t mode;
} __attribute__((packed)) aux_state_t;

/**
 * @brief AUX pins manager state.
 */
static aux_state_t state = {0};

/**
 * @brief Sets up input clock prescaler and returns an appropriate divisor.
 *
 * Sets up timer #1's input clock prescaler for the given frequency and returns
 * an appropriate divisor for it.
 *
 * @param[in] frequency the given frequency to set things up for.
 *
 * @return the appropriate PWM frequency divisor.
 */
static uint16_t setup_prescaler_divisor(const uint16_t frequency);

/**
 * @brief PWM frequency divisor for 1:256 prescaler.
 */
#define PWM_DIVISOR_PRESCALER_1_256 62

/**
 * @brief PWM frequency divisor for 1:64 prescaler.
 */
#define PWM_DIVISOR_PRESCALER_1_64 250

/**
 * @brief PWM frequency divisor for 1:8 prescaler.
 */
#define PWM_DIVISOR_PRESCALER_1_8 2000

/**
 * @brief PWM frequency divisor for 1:1 prescaler.
 */
#define PWM_DIVISOR_PRESCALER_1_1 16000

uint16_t setup_prescaler_divisor(const uint16_t frequency) {

  /* Use 1:256 prescaler. */

  if (frequency < 4) {
    T2CONbits.TCKPS1 = ON;
    T2CONbits.TCKPS0 = ON;

    return PWM_DIVISOR_PRESCALER_1_256;
  }

  /* Use 1:64 prescaler. */

  if (frequency < 31) {
    T2CONbits.TCKPS1 = ON;
    T2CONbits.TCKPS0 = OFF;

    return PWM_DIVISOR_PRESCALER_1_64;
  }

  /* Use 1:8 prescaler. */

  if (frequency < 245) {
    T2CONbits.TCKPS1 = OFF;
    T2CONbits.TCKPS0 = ON;

    return PWM_DIVISOR_PRESCALER_1_8;
  }

  /* Use 1:1 prescaler. */

  T2CONbits.TCKPS1 = OFF;
  T2CONbits.TCKPS0 = OFF;

  return PWM_DIVISOR_PRESCALER_1_1;
}

inline void bp_update_duty_cycle(const uint16_t duty_cycle) {
  bp_update_pwm(state.pwm_frequency, duty_cycle);
}

void bp_update_pwm(const uint16_t frequency, const uint16_t duty_cycle) {
  uint16_t period;
  uint16_t cycle;
  uint16_t divisor;

  state.pwm_frequency = frequency;
  state.pwm_duty_cycle = duty_cycle;

  /* Shut timers down. */
  T2CON = 0;
  T4CON = 0;
  OC5CON = 0;

  /* Detach the AUX pin from the PWM generator if no PWM signal is needed. */
  if (frequency == 0) {
    AUXPIN_RPOUT = 0;
    state.mode = AUX_MODE_IO;
    return;
  }

  divisor = setup_prescaler_divisor(frequency);
  period = (divisor / frequency) - 1;
  PR2 = period;
  cycle = (period * duty_cycle) / 100;

  /* Attach the AUX pin to the PWM generator. */
  AUXPIN_RPOUT = OC5_IO;

  /* Setup the PWM generator. */
  OC5R = cycle;
  OC5RS = cycle;
  OC5CON = 0x06;
  T2CONbits.TON = ON;
  state.mode = AUX_MODE_PWM;
}

// setup the PWM/frequency generator
void bp_pwm_setup(void) {
  unsigned int PWM_period, PWM_dutycycle, PWM_freq, PWM_div;
  int done;
  float PWM_pd;

  // cleanup timers
  T2CON = 0; // clear settings
  T4CON = 0;
  OC5CON = 0;

  if (state.mode == AUX_MODE_PWM) { // PWM is on, stop it
    AUXPIN_RPOUT = 0;               // remove output from AUX pin
    // bpWline(OUMSG_AUX_PWM_OFF);
    BPMSG1028;
    state.mode = AUX_MODE_IO;

    if (cmdbuf[((cmdstart + 1) & CMDLENMSK)] == 0x00) {
      // return if no arguments to function
      return;
    }
  }

  done = 0;

  cmdstart = (cmdstart + 1) & CMDLENMSK;

  // get any compound commandline variables
  consumewhitechars();
  PWM_freq = getint();
  consumewhitechars();
  PWM_pd = getint();

  // sanity check values
  if ((PWM_freq > 0) && (PWM_freq < 4000))
    done++;
  if ((PWM_pd > 0) && (PWM_pd < 100))
    done++;

  // calculate frequency:
  // no command line variables, prompt for PWM frequency
  if (done != 2) {
    command_error = false;
    BPMSG1029;
    BPMSG1030;
    PWM_freq = getnumber(50, 1, 4000, 0);
  }

  // choose proper multiplier for whole range
  PWM_div = setup_prescaler_divisor(PWM_freq);
  PWM_period = (PWM_div / PWM_freq) - 1;

  // if no commandline vairable, prompt for duty cycle
  if (done != 2) {
    BPMSG1033;
    PWM_pd = getnumber(50, 0, 99, 0);
  }

  PWM_pd /= 100;
  PWM_dutycycle = PWM_period * PWM_pd;
  // bpWdec(PWM_dutycycle);

  // assign pin with PPS
  AUXPIN_RPOUT = OC5_IO;
  // should be fine on bpv4

  OC5R = PWM_dutycycle;
  OC5RS = PWM_dutycycle;
  OC5CON = 0x6;
  PR2 = PWM_period;
  T2CONbits.TON = ON;

  BPMSG1034;
  state.mode = AUX_MODE_PWM;
}

void bp_frequency_counter_setup(void) {
  // frequency accuracy optimized by selecting measurement method, either
  //   counting frequency or measuring period, to maximize resolution.
  // Note: long long int division routine used by C30 is not open-coded  */
  unsigned long long f, p;

  if (state.mode == AUX_MODE_PWM) {
    // bpWline(OUMSG_AUX_FREQ_PWM);
    BPMSG1037;
    return;
  }

  // bpWstring(OUMSG_AUX_FREQCOUNT);
  BPMSG1038;
  // setup timer
  T4CON = 0; // make sure the counters are off
  T2CON = 0;

  // timer 2 external
  AUXPIN_DIR = 1; // aux input

  RPINR3bits.T2CKR = AUXPIN_RPIN; // assign T2 clock input to aux input
  // should be good on bpv4

  T2CON = 0b111010; //(TCKPS1|TCKPS0|T32|TCS); // prescale to 256

  f = poll_frequency_counter_value(); // all measurements within 26bits (<67MHz)

  // counter only seems to be good til around 6.7MHz,
  // use 4.2MHz (nearest power of 2 without exceeding 6.7MHz) for reliable
  // reading
  if (f > 0x3fff) { // if >4.2MHz prescaler required
    f *= 256;       // adjust for prescaler
  } else {          // get a more accurate reading without prescaler
    // bpWline("Autorange");
    BPMSG1245;
    T2CON = 0b001010; //(TCKPS1|TCKPS0|T32|TCS); prescale to 0
    f = poll_frequency_counter_value();
  }
  // at 4000Hz 1 bit resolution of frequency measurement = 1 bit resolution of
  // period measurement
  if (f >
      3999) { // when < 4 KHz  counting edges is inferior to measuring period(s)
    bp_write_dec_dword_friendly(
        f); // this function uses comma's to seperate thousands.
    MSG_PWM_HZ_MARKER;
  } else if (f > 0) {
    BPMSG1245;
    p = average_sample_frequency(f);
    // don't output fractions of frequency that are less then the frequency
    //   resolution provided by an increment of the period timer count.
    if (p > 400000) { // f <= 40 Hz
      // 4e5 < p <= 1,264,911 (625us tics)
      // 12.61911 < f <= 40 Hz
      // output resolution of 1e-5
      f = 16e11 / p;
      bp_write_dec_dword_friendly(f / 100000);
      UART1TX('.');
      f = f % 100000;
      if (f < 10000)
        UART1TX('0');
      if (f < 1000)
        UART1TX('0');
      if (f < 100)
        UART1TX('0');
      if (f < 10)
        UART1TX('0');
      bp_write_dec_dword(f);
      // at p=126,491.1 frequency resolution is .001
    } else if (p > 126491) { // f <= 126.4911
      // 126,491 < p <= 4e5  (625us tics)
      // 40 < f <= 126.4911 Hz
      // output resolution of .0001
      f = 16e10 / p;
      bp_write_dec_dword_friendly(f / 10000);
      UART1TX('.');
      f = f % 10000;
      if (f < 1000)
        UART1TX('0');
      if (f < 100)
        UART1TX('0');
      if (f < 10)
        UART1TX('0');
      bp_write_dec_word(f);
      // at p=40,000 frequency resolution is .01
    } else if (p > 40000) { // f <= 400 Hz
      // 4e4 < p <= 126,491 (625us tics)
      // 126.4911 < f <= 400 Hz
      // output resolution of .001
      f = 16e9 / p;
      bp_write_dec_dword_friendly(f / 1000);
      UART1TX('.');
      f = f % 1000; // frequency resolution < 1e-2
      if (f < 100)
        UART1TX('0');
      if (f < 10)
        UART1TX('0');
      bp_write_dec_word(f);
      // at p=12,649.11 frequency resolution is .1
    } else if (p > 12649) { // f <= 1264.911
      // 12,649 < p <= 4e4  (625us tics)
      // 400 < f < 1,264.911 Hz
      // output resolution of .01
      f = 16e8 / p;
      bp_write_dec_dword_friendly(f / 100);
      UART1TX('.');
      f = f % 100; // frequency resolution < 1e-1
      if (f < 10)
        UART1TX('0');
      bp_write_dec_byte(f);
      // at p=4,000 frequency resolution is 1
    } else { // 4,000 < p <= 12,649 (625us tics)
      // 1,264.911 < f < 4,000 Hz
      // output resolution of .1
      f = 16e7 / p;
      bp_write_dec_dword_friendly(f / 10);
      UART1TX('.');
      f = f % 10; // frequency resolution < 1
      bp_write_dec_byte(f);
    }
    MSG_PWM_HZ_MARKER;
    // END of IF(f>0)
  } else {
    MSG_PWM_FREQUENCY_TOO_LOW;
  }

  // return clock input to other pin
  RPINR3bits.T2CKR = 0b11111; // assign T2 clock input to nothing
  T4CON = 0;                  // make sure the counters are off
  T2CON = 0;
}

uint32_t bp_measure_frequency(void) {
  uint32_t frequency;

  /*
   * Setup timer 4
   *
   * MSB
   * 0-0------000-0-
   * | |      ||| |
   * | |      ||| +--- TCS:   External clock from pin.
   * | |      ||+----- T32:   TIMER4 is not bound with TIMER5 for 32 bit mode.
   * | |      ++------ TCKPS: 1:1 Prescaler.
   * | +-------------- TSIDL: Continue module operation in idle mode.
   * +---------------- TON:   Timer OFF.
   */
  T4CON = 0x0000;

  /*
   * Setup timer 2
   *
   * MSB
   * 0-0------000-0-
   * | |      ||| |
   * | |      ||| +--- TCS:   External clock from pin.
   * | |      ||+----- T32:   TIMER2 is not bound with TIMER3 for 32 bit mode.
   * | |      ++------ TCKPS: 1:1 Prescaler.
   * | +-------------- TSIDL: Continue module operation in idle mode.
   * +---------------- TON:   Timer OFF.
   */
  T2CON = 0x0000;

  AUXPIN_DIR = INPUT;

  /* Set timer 2 clock input pin. */
  RPINR3bits.T2CKR = AUXPIN_RPIN;

  /*
   * Finish timer 2 setup
   *
   * MSB
   * 0-0------111-1-
   * | |      ||| |
   * | |      ||| +--- TCS:   Internal clock.
   * | |      ||+----- T32:   TIMER2 is bound with TIMER3 for 32 bit mode.
   * | |      ++------ TCKPS: 1:256 Prescaler.
   * | +-------------- TSIDL: Continue module operation in idle mode.
   * +---------------- TON:   Timer OFF.
   */
  T2CON = (ON << _T2CON_TCS_POSITION) | (ON << _T2CON_T32_POSITION) |
          (ON << _T2CON_TCKPS0_POSITION) | (ON << _T2CON_TCKPS1_POSITION);

  frequency = poll_frequency_counter_value();
  if (frequency > 0xFF) {
    /* Adjust for prescaler. */
    frequency *= 256;
  } else {
    /* Use a less aggressive prescaler, set to 1:1. */
    T2CONbits.TCKPS0 = OFF;
    T2CONbits.TCKPS1 = OFF;
    frequency = poll_frequency_counter_value();
  }

  /* Remove clock input pin assignment. */
  RPINR3bits.T2CKR = 0b011111;

  /*
   * Stop timer 4
   *
   * MSB
   * 0-0------000-0-
   * | |      ||| |
   * | |      ||| +--- TCS:   External clock from pin.
   * | |      ||+----- T32:   TIMER4 is not bound with TIMER5 for 32 bit mode.
   * | |      ++------ TCKPS: 1:1 Prescaler.
   * | +-------------- TSIDL: Continue module operation in idle mode.
   * +---------------- TON:   Timer OFF.
   */
  T4CON = 0x0000;

  /*
   * Stop timer 2
   *
   * MSB
   * 0-0------000-0-
   * | |      ||| |
   * | |      ||| +--- TCS:   External clock from pin.
   * | |      ||+----- T32:   TIMER2 is not bound with TIMER3 for 32 bit mode.
   * | |      ++------ TCKPS: 1:1 Prescaler.
   * | +-------------- TSIDL: Continue module operation in idle mode.
   * +---------------- TON:   Timer OFF.
   */
  T2CON = 0x0000;

  return frequency;
}

uint32_t poll_frequency_counter_value(void) {
  uint32_t counter_low;
  uint32_t counter_high;

  /* Set 32-bits period register for timer #2 (0xFFFFFFFF). */
  PR3 = 0xFFFF;
  PR2 = 0xFFFF;

  /* Clear timer #2 counter. */
  TMR3HLD = 0;
  TMR2 = 0;

  /* Clear timer #4 counter. */
  TMR5HLD = 0;
  TMR4 = 0;

  /* Set timer #4 as 32 bits. */
  T4CONbits.T32 = YES;

  /* Set 32-bits period register for timer #4 (0x00F42400, one second). */
  PR5 = 0x00F4;
  PR4 = 0x2400;

  /* Clear timer #4 interrupt flag (32 bits mode). */
  IFS1bits.T5IF = OFF;

  /* Start timer #4. */
  T4CONbits.TON = ON;

  /* Start timer #2. */
  T2CONbits.TON = ON;

  /* Wait for timer #4 interrupt to occur. */
  while (IFS1bits.T5IF == 0) {
  }

  /* Stop timers. */
  T2CONbits.TON = OFF;
  T4CONbits.TON = OFF;

  /* Timer #2 now contains the frequency value. */
  counter_low = TMR2;
  counter_high = TMR3HLD;

  return (counter_high << 16) + counter_low;
}

#if defined(BUSPIRATEV4)
#define IC1ICBNE IC1CON1bits.ICBNE
#define IC2ICBNE IC2CON1bits.ICBNE
#else
#define IC1ICBNE IC1CONbits.ICBNE
#define IC2ICBNE IC2CONbits.ICBNE
#endif /* BUSPIRATEV4 */

uint32_t average_sample_frequency(const uint16_t count) {
  uint32_t current_low, counter_low, current_high, counter_high, total_samples;
  uint16_t index;

  /* Clear input capture interrupts. */
  IFS0bits.IC2IF = OFF;
  IFS0bits.IC1IF = OFF;

  /* Assign input capture pin. */
  RPINR7bits.IC2R = AUXPIN_RPIN;
  RPINR7bits.IC1R = AUXPIN_RPIN;

#if defined(BUSPIRATEV4)

  /* Setup timer 4 for interval measurement. */
  TMR5HLD = 0x0000;
  TMR4 = 0x0000;

  /*
   * Start timer 4
   *
   * MSB
   * 1-0------001-0-
   * | |      ||| |
   * | |      ||| +--- TCS:   External clock from pin.
   * | |      ||+----- T32:   TIMER4 is bound with TIMER5 for 32 bit mode.
   * | |      ++------ TCKPS: 1:1 Prescaler.
   * | +-------------- TSIDL: Continue module operation in idle mode.
   * +---------------- TON:   Timer ON.
   */
  T4CON = (ON << _T4CON_TON_POSITION) | (ON << _T4CON_T32_POSITION);

  /* Setup input capture 2. */

  /*
   * IC2CON1
   *
   * MSB
   * --0011---00--011
   *   ||||   ||  |||
   *   ||||   ||  +++-- ICM:    Simple capture mode, on every rising edge.
   *   ||||   ++------- ICI:    Interrupt on every capture event.
   *   |+++------------ ICTSEL: Use input capture timer 5.
   *   +--------------- ICSIDL: Input capture continues on CPU idle mode.
   */
  IC2CON1 = (0b011 << _IC2CON1_ICM_POSITION) | (0b00 << _IC2CON1_ICI_POSITION) |
            (0b011 << _IC2CON1_ICTSEL_POSITION) |
            (OFF << _IC2CON1_ICSIDL_POSITION);

  /*
   * IC2CON2
   *
   * MSB
   * -------000-10100
   *        ||| |||||
   *        ||| +++++-- SYNCSEL:  Use Input Capture 2 as trigger.
   *        ||+-------- TRIGSTAT: Timer source has not been triggered.
   *        |+--------- ICTRIG:   Synchronize input capture with SYNCSEL source.
   *        +---------- IC32:     Do not cascade input capture units.
   */
  IC2CON2 = (0b10100 << _IC2CON2_SYNCSEL_POSITION) |
            (OFF << _IC2CON2_TRIGSTAT_POSITION) |
            (OFF << _IC2CON2_ICTRIG_POSITION) | (OFF << _IC2CON2_IC32_POSITION);

  /* Setup input capture 1. */

  /*
   * IC1CON1
   *
   * --0010---00--011
   *   ||||   ||  |||
   *   ||||   ||  +++-- ICM:    Simple capture mode, on every rising edge.
   *   ||||   ++------- ICI:    Interrupt on every capture event.
   *   |+++------------ ICTSEL: Use input capture timer 4.
   *   +--------------- ICSIDL: Input capture continues on CPU idle mode.
   */
  IC1CON1 = (0b011 << _IC1CON1_ICM_POSITION) | (0b00 << _IC1CON1_ICI_POSITION) |
            (0b010 << _IC1CON1_ICTSEL_POSITION) |
            (OFF << _IC1CON1_ICSIDL_POSITION);

  /*
   * IC1CON2
   *
   * MSB
   * -------000-10100
   *        ||| |||||
   *        ||| +++++-- SYNCSEL:  Use Input Capture 2 as trigger.
   *        ||+-------- TRIGSTAT: Timer source has not been triggered.
   *        |+--------- ICTRIG:   Synchronize input capture with SYNCSEL source.
   *        +---------- IC32:     Do not cascade input capture units.
   */
  IC1CON2 = (0b10100 << _IC1CON2_SYNCSEL_POSITION) |
            (OFF << _IC1CON2_TRIGSTAT_POSITION) |
            (OFF << _IC1CON2_ICTRIG_POSITION) | (OFF << _IC1CON2_IC32_POSITION);

#else

  /* Setup timer 2 for interval measurement. */
  TMR3HLD = 0x0000;
  TMR2 = 0x0000;

  /* Start timer 2. */

  /*
   * T2CON
   *
   * MSB
   * 1-0------001-0-
   * | |      ||| |
   * | |      ||| +--- TCS:   External clock from pin.
   * | |      ||+----- T32:   TIMER2 is bound with TIMER3 for 32 bit mode.
   * | |      ++------ TCKPS: 1:1 Prescaler.
   * | +-------------- TSIDL: Continue module operation in idle mode.
   * +---------------- TON:   Timer ON.
   */
  T2CON = (ON << _T2CON_TON_POSITION) | (ON << _T2CON_T32_POSITION);

  /* Setup Input Capture 2. */

  /*
   * IC2CON
   *
   * MSB
   * --0-----000--011
   *   |     |||  |||
   *   |     |||  +++-- ICM:    Capture every rising edge.
   *   |     |++------- ICI:    Interrupt on every capture event.
   *   |     +--------- ICTMR:  TMR3 contents are captured on event.
   *   +--------------- ICSIDL: Input capture continues on CPU idle.
   */
  IC2CON = (0b011 << _IC2CON_ICM_POSITION) | (0b00 << _IC2CON_ICI_POSITION) |
           (OFF << _IC2CON_ICTMR_POSITION) | (OFF << _IC2CON_ICSIDL_POSITION);

  /* Setup Input Capture 1. */

  /*
   * IC1CON
   *
   * MSB
   * --0-----100--011
   *   |     |||  |||
   *   |     |||  +++-- ICM:    Capture every rising edge.
   *   |     |++------- ICI:    Interrupt on every capture event.
   *   |     +--------- ICTMR:  TMR2 contents are captured on event.
   *   +--------------- ICSIDL: Input capture continues on CPU idle.
   */
  IC1CON = (0b011 << _IC2CON_ICM_POSITION) | (0b00 << _IC2CON_ICI_POSITION) |
           (ON << _IC2CON_ICTMR_POSITION) | (OFF << _IC2CON_ICSIDL_POSITION);

#endif /* BUSPIRATEV4 */

  /* Flush IC1. */
  while (IC1ICBNE == ON) {
    current_low = IC1BUF;
  }

  /* Flush IC2. */
  while (IC2ICBNE == ON) {
    counter_low = IC2BUF;
  }

  while (IC1ICBNE == OFF) {
  }

  counter_low = IC1BUF;
  counter_high = IC2BUF;
  total_samples = 0;

  for (index = 0; index < count; index++) {
    /* Wait for signal. */
    while (IC1ICBNE == OFF) {
    }

    current_low = IC1BUF;
    current_high = IC2BUF;
    total_samples +=
        ((current_high - counter_high) << 16) + (current_low - counter_low);
    counter_high = current_high;
    counter_low = current_low;
  }

#if defined(BUSPIRATEV4)

  /* Stop input capture units. */

  /*
   * IC1CON1
   *
   * --0000---00--000
   *   ||||   ||  |||
   *   ||||   ||  +++-- ICM:    Input capture module turned off.
   *   ||||   ++------- ICI:    Interrupt on every capture event.
   *   |+++------------ ICTSEL: Use input capture timer 3.
   *   +--------------- ICSIDL: Input capture continues on CPU idle mode.
   */
  IC1CON1 = 0x0000;

  /*
   * IC2CON1
   *
   * --0000---00--000
   *   ||||   ||  |||
   *   ||||   ||  +++-- ICM:    Input capture module turned off.
   *   ||||   ++------- ICI:    Interrupt on every capture event.
   *   |+++------------ ICTSEL: Use input capture timer 3.
   *   +--------------- ICSIDL: Input capture continues on CPU idle mode.
   */
  IC2CON1 = 0x0000;

  /* Stop timer 4. */
  T4CONbits.TON = OFF;

#else

  /* Stop Input Capture 1. */

  /*
   * IC1CON
   *
   * MSB
   * --0-----000--000
   *   |     |||  |||
   *   |     |||  +++-- ICM:    Capture module turned off.
   *   |     |++------- ICI:    Interrupt on every capture event.
   *   |     +--------- ICTMR:  TMR3 contents are captured on event.
   *   +--------------- ICSIDL: Input capture continues on CPU idle.
   */
  IC1CON = 0x0000;

  /* Stop Input Capture 2. */

  /*
   * IC2CON
   *
   * MSB
   * --0-----000--000
   *   |     |||  |||
   *   |     |||  +++-- ICM:    Capture module turned off.
   *   |     |++------- ICI:    Interrupt on every capture event.
   *   |     +--------- ICTMR:  TMR3 contents are captured on event.
   *   +--------------- ICSIDL: Input capture continues on CPU idle.
   */
  IC2CON = 0x0000;

  /* Stop timer 2. */

  T2CONbits.TON = OFF;

#endif /* BUSPIRATEV4 */

  return total_samples / count;
}

void bp_aux_pin_set_high_impedance(void) {
#ifdef BUSPIRATEV3
  if (mode_configuration.alternate_aux == 0) {
    BP_AUX0_DIR = INPUT;
  } else {
    BP_CS_DIR = INPUT;
  }
#else
  switch (mode_configuration.alternate_aux) {
  case 0:
    BP_AUX0_DIR = INPUT;
    break;

  case 1:
    BP_CS_DIR = INPUT;
    break;

  case 2:
    BP_AUX1_DIR = INPUT;
    break;

  case 3:
    BP_AUX2_DIR = INPUT;
    break;

  default:
    break;
  }
#endif /* BUSPIRATEV3 */

  BPMSG1039;
}

void bp_aux_pin_set_high(void) {
#ifdef BUSPIRATEV3
  if (mode_configuration.alternate_aux == 0) {
    BP_AUX0_DIR = OUTPUT;
    BP_AUX0 = HIGH;
  } else {
    BP_CS_DIR = OUTPUT;
    BP_CS = HIGH;
  }
#else
  switch (mode_configuration.alternate_aux) {
  case 0:
    BP_AUX0_DIR = OUTPUT;
    BP_AUX0 = HIGH;
    break;

  case 1:
    BP_CS_DIR = OUTPUT;
    BP_CS = HIGH;
    break;

  case 2:
    BP_AUX1_DIR = OUTPUT;
    BP_AUX1 = HIGH;
    break;

  case 3:
    BP_AUX2_DIR = OUTPUT;
    BP_AUX2 = HIGH;
    break;

  default:
    break;
  }
#endif /* BUSPIRATEV3 */

  BPMSG1040;
}

void bp_aux_pin_set_low(void) {
#ifdef BUSPIRATEV3
  if (mode_configuration.alternate_aux == 0) {
    BP_AUX0_DIR = OUTPUT;
    BP_AUX0 = LOW;
  } else {
    BP_CS_DIR = OUTPUT;
    BP_CS = LOW;
  }
#else
  switch (mode_configuration.alternate_aux) {
  case 0:
    BP_AUX0_DIR = OUTPUT;
    BP_AUX0 = LOW;
    break;

  case 1:
    BP_CS_DIR = OUTPUT;
    BP_CS = LOW;
    break;

  case 2:
    BP_AUX1_DIR = OUTPUT;
    BP_AUX1 = LOW;
    break;

  case 3:
    BP_AUX2_DIR = OUTPUT;
    BP_AUX2 = LOW;
    break;

  default:
    break;
  }
#endif /* BUSPIRATEV3 */

  BPMSG1041;
}

bool bp_aux_pin_read(void) {
#ifdef BUSPIRATEV3
  if (mode_configuration.alternate_aux == 0) {
    BP_AUX0_DIR = INPUT;
    Nop();
    Nop();
    return BP_AUX0;
  }

  BP_CS_DIR = INPUT;
  Nop();
  Nop();
  return BP_CS;
#else
  switch (mode_configuration.alternate_aux & 0b00000011) {
  case 0:
    BP_AUX0_DIR = INPUT;
    Nop();
    Nop();
    return BP_AUX0;

  case 1:
    BP_CS_DIR = INPUT;
    Nop();
    Nop();
    return BP_CS;

  case 2:
    BP_AUX1_DIR = INPUT;
    Nop();
    Nop();
    return BP_AUX1;

  case 3:
    BP_AUX2_DIR = INPUT;
    Nop();
    Nop();
    return BP_AUX2;

  default:
    /* Should not happen. */
    return LOW;
  }
#endif /* BUSPIRATEV3 */
}

void bp_servo_setup(void) {
  unsigned int PWM_period, PWM_dutycycle;
  unsigned char entryloop = 0;
  float PWM_pd;

  // Clear timers
  T2CON = 0; // clear settings
  T4CON = 0;
  OC5CON = 0;

  if (state.mode == AUX_MODE_PWM) { // PWM is on, stop it
    if (cmdbuf[((cmdstart + 1) & CMDLENMSK)] ==
        0x00) {         // no extra data, stop servo
      AUXPIN_RPOUT = 0; // remove output from AUX pin
      BPMSG1028;
      state.mode = AUX_MODE_IO;
      return; // return if no arguments to function
    }
  }

  cmdstart = (cmdstart + 1) & CMDLENMSK;

  // Get servo position from command line or prompt for value
  consumewhitechars();
  PWM_pd = getint();
  if (command_error || (PWM_pd > 180)) {
    command_error = false;
    BPMSG1254;
    PWM_pd = getnumber(90, 0, 180, 0);
    entryloop = 1;
  }

// Setup multiplier for 50 Hz
servoset:
  T2CONbits.TCKPS1 = 1;
  T2CONbits.TCKPS0 = 1;
  PWM_period = 1250;
  PWM_pd /= 3500;
  PWM_dutycycle = (PWM_period * PWM_pd) + 62;

  // assign pin with PPS
  AUXPIN_RPOUT = OC5_IO;
  OC5R = PWM_dutycycle;
  OC5RS = PWM_dutycycle;
  OC5CON = 0x6;
  PR2 = PWM_period;
  T2CONbits.TON = ON;
  BPMSG1255;
  state.mode = AUX_MODE_PWM;

  if (entryloop == 1) {
    PWM_pd = getnumber(-1, 0, 180, 1);
    if (PWM_pd < 0) {
      bpBR;
      return;
    }
    goto servoset;
  }
}
