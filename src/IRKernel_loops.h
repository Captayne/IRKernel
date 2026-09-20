/*========================================================================*\
 *
 *  IRKernel_loops.h  --  several loop() functions, as if the board had
 *                        more than one core
 *
 *  Include this instead of <IRKernel.h> and simply write more loops:
 *
 *      #include <IRKernel_loops.h>
 *
 *      void setup1() { pinMode(3, OUTPUT); }
 *      void loop1()  { digitalWrite(3, HIGH); delay(500);
 *                      digitalWrite(3, LOW);  delay(500); }
 *
 *      void setup2() { }
 *      void loop2()  { Serial.println(analogRead(A0)); delay(200); }
 *
 *  That is all: no irk_init(), no irk_task_create(), no stacks, no
 *  irk_yield(). Every loop you use gets its own task with its own stack.
 *  Control is handed over at the end of each pass and at every delay().
 *
 *  What this header does for that
 *  ------------------------------
 *  - setup1..setup16 and loop1..loop16 are declared weak here. At run time
 *    the wrapper sees which ones the sketch actually defines and creates a
 *    task only for those.
 *  - delay() is redirected to irk_delay() inside the sketch, so a waiting
 *    loop no longer holds up the others; yield() becomes irk_yield().
 *    delayMicroseconds() is left alone -- short, exact waits for
 *    bit-banged protocols should still busy-wait.
 *    Define IRK_LOOPS_NO_DELAY_MACRO before including to keep both.
 *  - setup() and loop() are supplied by the wrapper if the sketch has
 *    none. Write your own and call irk_loops_begin() once in setup().
 *
 *  How many loops
 *  --------------
 *  IRK_LOOP_MAX in irk_config.h decides how many are possible; 16 is the
 *  ceiling this header declares. Three things limit it in practice:
 *
 *      IRK_LOOP_MAX     how many stacks are reserved (AVR 4, else 8)
 *      IRK_MAX_TASKS    tasks in total (AVR 6, else 24) -- the main task
 *                       counts, so an AVR has room for 5 loops at most
 *      RAM              every loop costs its stack, used or not
 *
 *  Limits
 *  ------
 *  - Cooperative: control is handed over at the END of a pass and at
 *    delay(). A pass that computes for 200 ms straight keeps the others
 *    waiting that long. Call irk_yield() or delay(0) in between if that
 *    matters.
 *  - Count and stack size live in irk_config.h (IRK_LOOP_MAX,
 *    IRK_LOOP_STACK). A #define in the sketch does not reach the library
 *    -- the Arduino IDE compiles it separately.
 *  - On RP2040/RP2350, setup1()/loop1() already mean the REAL second core
 *    of the chip. There the wrapper starts numbering at 2: loop2..loop16
 *    are tasks, loop1 stays the second core.
 *
\*========================================================================*/

#ifndef IRKERNEL_LOOPS_H
#define IRKERNEL_LOOPS_H

#include <Arduino.h>
#include "IRKernel.h"

/* Highest number this header declares. */
#define IRK_LOOP_CEILING   16

#if IRK_LOOP_MAX > IRK_LOOP_CEILING
#  error "IRK_LOOP_MAX is larger than 16 -- add declarations in IRKernel_loops.h/.cpp first"
#endif

/* First number the wrapper hands out. */
#if defined(ARDUINO_ARCH_RP2040)
#  define IRK_LOOP_FIRST   2    /* loop1 is the real second core there */
#else
#  define IRK_LOOP_FIRST   1
#endif

#define IRK_LOOP_LAST      (IRK_LOOP_FIRST + IRK_LOOP_MAX - 1)

/* To be defined by the sketch -- weak, so the wrapper can tell which ones
   exist. Names that are not defined have the address 0. */
extern "C" {
void setup1(void)  __attribute__((weak));
void setup2(void)  __attribute__((weak));
void setup3(void)  __attribute__((weak));
void setup4(void)  __attribute__((weak));
void setup5(void)  __attribute__((weak));
void setup6(void)  __attribute__((weak));
void setup7(void)  __attribute__((weak));
void setup8(void)  __attribute__((weak));
void setup9(void)  __attribute__((weak));
void setup10(void) __attribute__((weak));
void setup11(void) __attribute__((weak));
void setup12(void) __attribute__((weak));
void setup13(void) __attribute__((weak));
void setup14(void) __attribute__((weak));
void setup15(void) __attribute__((weak));
void setup16(void) __attribute__((weak));

void loop1(void)  __attribute__((weak));
void loop2(void)  __attribute__((weak));
void loop3(void)  __attribute__((weak));
void loop4(void)  __attribute__((weak));
void loop5(void)  __attribute__((weak));
void loop6(void)  __attribute__((weak));
void loop7(void)  __attribute__((weak));
void loop8(void)  __attribute__((weak));
void loop9(void)  __attribute__((weak));
void loop10(void) __attribute__((weak));
void loop11(void) __attribute__((weak));
void loop12(void) __attribute__((weak));
void loop13(void) __attribute__((weak));
void loop14(void) __attribute__((weak));
void loop15(void) __attribute__((weak));
void loop16(void) __attribute__((weak));
}

/* Creates the tasks. Called automatically when the sketch has no setup()
   of its own. Calling it more than once is harmless.
   Returns the number of loops created. */
uint8_t irk_loops_begin(void);

/* Wait without holding up the other loops. */
#ifndef IRK_LOOPS_NO_DELAY_MACRO
#  undef  delay
#  define delay(ms)   irk_delay((uint32_t)(ms))
#  undef  yield
#  define yield()     irk_yield()
#endif

#endif /* IRKERNEL_LOOPS_H */
