/*========================================================================*\
 *
 *  MultiLoop  --  four loop() functions on one Arduino
 *
 *  Board:   Arduino Leonardo (tested), Uno and others work as well
 *  Monitor: 115200 baud
 *  Nothing to wire up: the built-in LED is all this needs.
 *
 *  What this shows
 *  ---------------
 *  Four activities run at the same time, and each one is written straight
 *  ahead with delay(), the way you learn it on day one:
 *
 *      loop1  blink pattern: SOS, then a pause
 *      loop2  measurement: read A0, average, print every 2 s
 *      loop3  console: commands from the serial port
 *      loop4  clock: counts seconds, prints every minute
 *
 *  Without a kernel these four would have to be rewritten as interlocking
 *  state machines driven by millis() -- the SOS pattern alone would need a
 *  table and a step counter. Here it is just a sequence of delay() calls.
 *
 *  How it works
 *  ------------
 *  IRKernel_loops.h gives every loop its own task with its own stack,
 *  redirects delay() to irk_delay() and yields after each pass. None of
 *  that shows up in the sketch: no irk_init(), no irk_task_create(), no
 *  irk_yield().
 *
 *  Console commands (send a line):
 *      on / off       blinking on or off
 *      fast / slow    speed of the blink pattern
 *      status         uptime, last reading and stack headroom
 *
 *  The three LEDs of the Leonardo
 *  ------------------------------
 *  Only the LED marked "L" (pin 13) belongs to this sketch. The "RX" and
 *  "TX" LEDs next to the USB socket are driven by the Arduino core itself
 *  on every byte that crosses USB -- they keep flashing after "off",
 *  because loop2 and the clock keep printing. Their pins are not brought
 *  out as normal digital pins.
 *
 *  Memory
 *  ------
 *  Every loop gets its own stack, reserved whether used or not: on AVR
 *  4 x 256 bytes (IRK_LOOP_MAX, IRK_LOOP_STACK in irk_config.h).
 *  Measured with Arduino AVR core 1.8.8:
 *      Leonardo  9588 B flash, 1827 B RAM (71 %) -- 733 B left over
 *      Uno       7304 B flash, 1862 B RAM (90 %) -- only 186 B left,
 *                                                   the IDE warns already
 *  On an Uno, lower IRK_LOOP_MAX to the number of loops you really use and
 *  make IRK_LOOP_STACK tighter. The "status" command shows what is left.
 *
\*========================================================================*/

#include <IRKernel_loops.h>

#ifndef LED_BUILTIN
#define LED_BUILTIN 13
#endif

static volatile bool     blinking = true;
static volatile uint16_t tick_ms  = 200;     /* length of one "short"     */
static volatile uint16_t reading;            /* last average of A0        */
static volatile uint32_t seconds;


/*------------------------------------------------------------------------*\
 *  loop1  --  SOS blink pattern, written straight ahead
\*------------------------------------------------------------------------*/

static void activate_LED(uint16_t duration){
    if (blinking) digitalWrite(LED_BUILTIN, HIGH);
    delay(duration);
    digitalWrite(LED_BUILTIN, LOW);
    delay(tick_ms);
}

void setup1(){
    pinMode(LED_BUILTIN, OUTPUT);
}

void loop1(){
    for (uint8_t i = 0; i < 3; i++) activate_LED(tick_ms);       /* S: short */
    for (uint8_t i = 0; i < 3; i++) activate_LED(tick_ms * 3);   /* O: long  */
    for (uint8_t i = 0; i < 3; i++) activate_LED(tick_ms);       /* S: short */
    delay(tick_ms * 7);                                          /* pause    */
}


/*------------------------------------------------------------------------*\
 *  loop2  --  measurement: average 16 samples, print every 2 s
\*------------------------------------------------------------------------*/

void loop2(){
    uint32_t sum = 0;

    for (uint8_t i = 0; i < 16; i++) {
        sum += analogRead(A0);
        delay(10);                       /* holds nobody up */
    }
    reading = (uint16_t)(sum / 16);

    Serial.print(F("[A0] "));
    Serial.println(reading);
    delay(2000);
}


/*------------------------------------------------------------------------*\
 *  loop3  --  console: read a line and answer it
\*------------------------------------------------------------------------*/

void setup3(){
    Serial.begin(115200);
    while (!Serial && millis() < 3000) delay(10);
    Serial.println(F("MultiLoop -- four loops on one core"));
    Serial.println(F("Commands: on, off, fast, slow, status"));
}

void loop3(){
    static char line[24];
    static uint8_t n = 0;

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (n == 0) continue;
            line[n] = '\0';
            n = 0;

            if      (!strcmp(line, "on"))   { blinking = true;  Serial.println(F("> blinking on")); }
            else if (!strcmp(line, "off"))  { blinking = false; Serial.println(F("> blinking off")); }
            else if (!strcmp(line, "fast")) { tick_ms  = 80;    Serial.println(F("> fast")); }
            else if (!strcmp(line, "slow")) { tick_ms  = 300;   Serial.println(F("> slow")); }
            else if (!strcmp(line, "status")) {
                Serial.print(F("> uptime "));  Serial.print(seconds);
                Serial.print(F(" s, A0 "));    Serial.print(reading);
#if IRK_ENABLE_STACKCHECK
                Serial.print(F(", stack free per loop: "));
                for (irk_task_t t = 2; t <= irk_task_count(); t++) {
                    Serial.print((unsigned)irk_stack_free(t));
                    Serial.print(' ');
                }
#endif
                Serial.println();
            }
            else { Serial.print(F("> unknown: ")); Serial.println(line); }
        } else if (n < sizeof(line) - 1) {
            line[n++] = c;
        }
    }
    delay(20);                           /* a keyboard is in no hurry */
}


/*------------------------------------------------------------------------*\
 *  loop4  --  clock: count seconds, print every minute
\*------------------------------------------------------------------------*/

void loop4(){
    delay(1000);
    seconds++;

    if (seconds % 60 == 0) {
        Serial.print(F("[clock] "));
        Serial.print(seconds / 60);
        Serial.println(F(" min"));
    }
}
