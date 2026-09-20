/*========================================================================*\
 *
 *  SemaphoreMutex  --  Wann man ueberhaupt ein Semaphor braucht
 *
 *  Kooperatives Multitasking heisst: zwischen zwei irk_yield() kann
 *  niemand dazwischenfunken. Einfache gemeinsame Variablen brauchen
 *  deshalb *keine* Sperre -- das ist der grosse Vorteil gegenueber
 *  praeemptiven Systemen.
 *
 *  Ein Semaphor wird erst noetig, wenn ein Betriebsmittel ueber mehrere
 *  irk_yield() hinweg belegt bleiben muss. Genau das zeigt dieses
 *  Beispiel: drei Tasks wollen dieselbe serielle Schnittstelle
 *  benutzen, und eine Ausgabe soll nicht von einer anderen zerhackt
 *  werden.
 *
 *  Serielle Ausgabe: 115200 Baud
 *
\*========================================================================*/

#include <IRKernel.h>

#if defined(__AVR__)
  #define STACK_SIZE 220
#else
  #define STACK_SIZE 512
#endif

static uint8_t stack_a[STACK_SIZE];
static uint8_t stack_b[STACK_SIZE];
static uint8_t stack_c[STACK_SIZE];

#define SEM_SERIAL   0        /* Semaphor-Nummer fuer die Schnittstelle */


/* Gibt eine mehrteilige Meldung aus und gibt zwischendurch die CPU ab.
   Ohne Semaphor wuerden sich die Meldungen der drei Tasks ineinander
   verschachteln. */
static void melde(const char *wer, uint16_t zaehler)
{
    irk_sema_wait(SEM_SERIAL);          /* Schnittstelle belegen */

    Serial.print(F("["));
    irk_yield();                        /* absichtlich mittendrin abgeben */
    Serial.print(wer);
    irk_yield();
    Serial.print(F(" #"));
    Serial.print(zaehler);
    irk_yield();
    Serial.println(F("]"));

    irk_sema_signal(SEM_SERIAL);        /* Schnittstelle freigeben */
}


static void task_a(void)
{
    uint16_t n = 0;
    for (;;) { melde("A", n++); irk_delay(300); }
}

static void task_b(void)
{
    uint16_t n = 0;
    for (;;) { melde("B", n++); irk_delay(500); }
}

static void task_c(void)
{
    uint16_t n = 0;
    for (;;) { melde("C", n++); irk_delay(700); }
}


void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { }

    Serial.println(F("IRKernel  --  Semaphor schuetzt die Schnittstelle"));
    Serial.println(F("Jede Meldung muss vollstaendig und am Stueck erscheinen."));
    Serial.println();

    if (irk_init(1) != 0) {
        Serial.println(F("Kernel liess sich nicht starten."));
        for (;;) { }
    }

    /* Zaehlerstand 1 = binaeres Semaphor, also ein Mutex. */
    irk_sema_init(SEM_SERIAL, 1);

    irk_task_create(task_a, 1, stack_a, sizeof(stack_a), "A");
    irk_task_create(task_b, 1, stack_b, sizeof(stack_b), "B");
    irk_task_create(task_c, 1, stack_c, sizeof(stack_c), "C");
}


void loop()
{
    irk_delay(5000);

    /* Die Haupttask darf natuerlich auch -- ueber dasselbe Semaphor. */
    irk_sema_wait(SEM_SERIAL);
    Serial.println(F("--- Haupttask meldet sich ---"));
    irk_sema_signal(SEM_SERIAL);
}
