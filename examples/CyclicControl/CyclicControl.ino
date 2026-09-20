/*========================================================================*\
 *
 *  CyclicControl  --  Ein Regeltakt, der haelt
 *
 *  Zeigt den Baustein, fuer den dieser Kernel urspruenglich gebaut wurde:
 *  eine zyklische Task, die alle 10 ms genau einmal laeuft und dabei
 *  Vorrang vor allem anderen hat.
 *
 *  Daneben laeuft eine bewusst gefraessige Hintergrundtask, die so viel
 *  Rechenzeit frisst, wie sie bekommen kann. Der Regeltakt bleibt
 *  trotzdem stabil -- das ist der Unterschied zwischen "zyklisch" und
 *  "alle 10 ms mal nachschauen".
 *
 *  Am Ende jeder Sekunde wird ausgegeben, wie viele Aktivierungen es
 *  tatsaechlich gab (Sollwert: 100) und wie stark der Takt geschwankt
 *  hat.
 *
 *  Serielle Ausgabe: 115200 Baud
 *
\*========================================================================*/

#include <IRKernel.h>

#if defined(__AVR__)
  #define STACK_SIZE 200
#else
  #define STACK_SIZE 512
#endif

static uint8_t stack_regler[STACK_SIZE];
static uint8_t stack_last[STACK_SIZE];

#define PERIODE_MS  10

static volatile uint32_t aktivierungen = 0;
static volatile uint32_t last_arbeit   = 0;

/* Jitter-Messung */
static volatile uint32_t letzter_takt  = 0;
static volatile uint16_t jitter_max    = 0;


/*------------------------------------------------------------------------*\
 *  Die zyklische Task
 *
 *  Sie laeuft je Faelligkeit genau einmal bis zum irk_yield() durch.
 *  Danach schlaeft sie, bis die Periode erneut abgelaufen ist -- ohne
 *  dabei Rechenzeit zu belegen.
\*------------------------------------------------------------------------*/
static void regler(void)
{
    for (;;) {
        uint32_t jetzt = millis();

        if (letzter_takt != 0) {
            uint32_t abstand = jetzt - letzter_takt;
            uint32_t abw = (abstand > PERIODE_MS)
                         ? (abstand - PERIODE_MS)
                         : (PERIODE_MS - abstand);
            if (abw > jitter_max) jitter_max = (uint16_t)abw;
        }
        letzter_takt = jetzt;
        aktivierungen++;

        /* --- Hier stuende der eigentliche Regelschritt --- */

        irk_yield();     /* bis zur naechsten Faelligkeit schlafen */
    }
}


/*------------------------------------------------------------------------*\
 *  Die gefraessige Hintergrundtask -- Stoerquelle fuer den Test
\*------------------------------------------------------------------------*/
static void hintergrund(void)
{
    for (;;) {
        volatile uint16_t i;
        for (i = 0; i < 200; i++) { }   /* Beschaeftigungstherapie */
        last_arbeit++;
        irk_yield();
    }
}


void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { }

    Serial.println(F("IRKernel  --  zyklischer Regeltakt unter Last"));
    Serial.print  (F("Periode: ")); Serial.print(PERIODE_MS);
    Serial.println(F(" ms, Sollwert 100 Aktivierungen je Sekunde"));
    Serial.println();

    if (irk_init(1) != 0) {
        Serial.println(F("Kernel liess sich nicht starten."));
        for (;;) { }
    }

    irk_task_t t_regler = irk_task_create(regler, 1,
                                          stack_regler, sizeof(stack_regler),
                                          "regler");

    irk_task_create(hintergrund, 3,          /* hohe Prioritaet, absichtlich */
                    stack_last, sizeof(stack_last), "last");

    /* Und das ist der entscheidende Aufruf: ab jetzt laeuft die Task
       nicht mehr nach Anteilen, sondern nach der Uhr -- und wenn sie
       faellig ist, kommt sie vor allen anderen dran. */
    irk_task_set_cyclic(t_regler, PERIODE_MS);
}


void loop()
{
    aktivierungen = 0;
    jitter_max    = 0;
    last_arbeit   = 0;

    irk_delay(1000);

    Serial.print(F("Aktivierungen: "));   Serial.print(aktivierungen);
    Serial.print(F("   max. Abweichung: ")); Serial.print(jitter_max);
    Serial.print(F(" ms   Hintergrund: ")); Serial.println(last_arbeit);
}
