/*========================================================================*\
 *
 *  FairShare  --  Was IRKernel anders macht
 *
 *  Drei Tasks mit den Prioritaeten 1, 2 und 3 zaehlen, so schnell sie
 *  koennen. Nach jeder Sekunde gibt die Haupttask aus, wie oft jede
 *  gelaufen ist.
 *
 *  Erwartetes Ergebnis: die Zaehlerstaende verhalten sich wie 1 : 2 : 3.
 *
 *  Das ist der Unterschied zu den beiden ueblichen Ansaetzen:
 *
 *    - Round-Robin (die meisten kooperativen Arduino-Scheduler) wuerde
 *      1 : 1 : 1 liefern -- Prioritaeten kennt es nicht.
 *
 *    - Strikte Prioritaet (FreeRTOS & Co.) wuerde 0 : 0 : n liefern --
 *      die wichtigste Task verdraengt die anderen vollstaendig.
 *
 *  IRKernel verteilt stattdessen *Anteile*. Niemand verhungert, und
 *  wichtigere Arbeit bekommt trotzdem mehr Rechenzeit.
 *
 *  Serielle Ausgabe: 115200 Baud
 *
\*========================================================================*/

#include <IRKernel.h>

/* Auf AVR wird jeder Taskstack als statisches Array angelegt, damit der
   Linker den Verbrauch kennt. Auf groesseren MCUs darf man auch NULL
   uebergeben und die Groesse angeben. */
#if defined(__AVR__)
  #define STACK_SIZE 200
#else
  #define STACK_SIZE 512
#endif

static uint8_t stack1[STACK_SIZE];
static uint8_t stack2[STACK_SIZE];
static uint8_t stack3[STACK_SIZE];

static volatile uint32_t counter1 = 0;
static volatile uint32_t counter2 = 0;
static volatile uint32_t counter3 = 0;


/*  Die Tasks.  Wichtig: irk_yield() gibt die CPU ab.  Weil das
 *  Multitasking kooperativ ist, kann zwischen zwei irk_yield() niemand
 *  dazwischenfunken -- die Zaehler brauchen deshalb keinerlei Sperre.  */

static void task_niedrig(void)
{
    for (;;) { counter1++; irk_yield(); }
}

static void task_mittel(void)
{
    for (;;) { counter2++; irk_yield(); }
}

/* Handles der Tasks -- nicht als 2, 3, 4 annehmen: auf Mehrkern-Boards
   sind die ersten Handles fuer die Haupttasks der Kerne reserviert. */
static irk_task_t t_niedrig, t_mittel, t_hoch;

static void task_hoch(void)
{
    for (;;) { counter3++; irk_yield(); }
}


void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { }

    Serial.println(F("IRKernel  --  Fair-Share-Demonstration"));
    Serial.println(F("Prioritaeten 1 : 2 : 3"));
    Serial.println();

    /* Kernel starten. Die Haupttask (setup/loop) bekommt Prioritaet 1. */
    if (irk_init(1) != 0) {
        Serial.println(F("Kernel liess sich nicht starten."));
        for (;;) { }
    }

    t_niedrig = irk_task_create(task_niedrig, 1, stack1, sizeof(stack1), "niedrig");
    t_mittel  = irk_task_create(task_mittel,  2, stack2, sizeof(stack2), "mittel");
    t_hoch    = irk_task_create(task_hoch,    3, stack3, sizeof(stack3), "hoch");
}


void loop()
{
    /* irk_delay() gibt die CPU ab, waehrend es wartet -- anders als
       delay(), das sie blockieren wuerde. */
    irk_delay(1000);

    uint32_t c1 = counter1, c2 = counter2, c3 = counter3;

    Serial.print(F("Prio 1: ")); Serial.print(c1);
    Serial.print(F("   Prio 2: ")); Serial.print(c2);
    Serial.print(F("   Prio 3: ")); Serial.println(c3);

    if (c1 > 0) {
        Serial.print(F("  Verhaeltnis  1.00 : "));
        Serial.print((float)c2 / (float)c1, 2);
        Serial.print(F(" : "));
        Serial.println((float)c3 / (float)c1, 2);
    }

#if defined(IRK_ENABLE_STACKCHECK) && IRK_ENABLE_STACKCHECK
    Serial.print(F("  freier Stack (Byte): "));
    Serial.print(irk_stack_free(t_niedrig)); Serial.print(' ');
    Serial.print(irk_stack_free(t_mittel));  Serial.print(' ');
    Serial.println(irk_stack_free(t_hoch));
#endif
    Serial.println();
}
