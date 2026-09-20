/*========================================================================*\
 *
 *  SelfTest  --  IRKernel-Selbsttest auf echter Hardware
 *
 *  Prueft der Reihe nach alles, was der Kernel kann, und meldet das
 *  Ergebnis ueber die serielle Schnittstelle (115200 Baud).
 *
 *  Die ersten drei Pruefungen sind die eigentlich wichtigen. Sie lassen
 *  sich auf dem PC nicht ersetzen, weil dort Fibers bzw. ucontext den
 *  Kontextwechsel uebernehmen -- auf dem Mikrocontroller tut das
 *  handgeschriebener Assembler, und genau der muss hier beweisen, dass
 *  er die callee-saved Register korrekt rettet.
 *
 *  Jede Pruefung meldet sich VOR ihrer Ausfuehrung. Stuerzt das Board ab
 *  oder haengt es, benennt die letzte ausgegebene Zeile die schuldige
 *  Pruefung. Alle Warteschleifen sind begrenzt: ein kaputter Scheduler
 *  fuehrt zu einem Fehlschlag, nicht zu einem Haenger.
 *
 *  Erwartete Ausgabe am Ende:   "ERGEBNIS: n Pruefungen, 0 Fehler"
 *
\*========================================================================*/

#include <IRKernel.h>


/*------------------------------------------------------------------------*\
 *  Dimensionierung
\*------------------------------------------------------------------------*/

#if defined(__AVR__)
  #define STK   220            /* knapp, aber ausreichend: Tasks geben
                                  selbst nichts ueber Serial aus */
#else
  #define STK   512
#endif

static uint8_t stack_a[STK];
static uint8_t stack_b[STK];
static uint8_t stack_c[STK];


/*------------------------------------------------------------------------*\
 *  Testgeruest
\*------------------------------------------------------------------------*/

static uint16_t geprueft = 0;
static uint16_t fehler   = 0;

static void beginne(const __FlashStringHelper *name)
{
    Serial.print(F("  ["));
    Serial.print(geprueft + 1);
    Serial.print(F("] "));
    Serial.print(name);
    Serial.print(F(" ... "));
    Serial.flush();               /* damit die Zeile auch bei einem
                                     Absturz noch draussen ist */
}

static void ergebnis(bool ok, const __FlashStringHelper *detail = nullptr)
{
    geprueft++;
    if (ok) {
        Serial.print(F("ok"));
    } else {
        fehler++;
        Serial.print(F("FEHLGESCHLAGEN"));
    }
    if (detail) { Serial.print(F("   ")); Serial.print(detail); }
    Serial.println();
    Serial.flush();
}

static void ergebnis_zahl(bool ok, const __FlashStringHelper *label, long wert)
{
    geprueft++;
    Serial.print(ok ? F("ok") : F("FEHLGESCHLAGEN"));
    Serial.print(F("   ")); Serial.print(label);
    Serial.print(F("=")); Serial.println(wert);
    if (!ok) fehler++;
    Serial.flush();
}

/* Wartet, bis die Bedingung erfuellt ist -- aber hoechstens so lange.
   Gibt dabei die CPU ab, damit die anderen Tasks laufen koennen.
   Rueckgabe: true, wenn die Bedingung eintrat.                        */
#define WARTE_BIS(bedingung, max_ms)                       \
    ({  uint32_t __start = millis();  bool __ok = false;   \
        while ((millis() - __start) < (uint32_t)(max_ms)) { \
            if (bedingung) { __ok = true; break; }          \
            irk_yield();                                    \
        }  __ok;  })

/* Alle Testtasks entfernen und mit sauberem Zustand weitermachen. */
static void aufraeumen(void)
{
    /* Handles bis IRK_MAX_CORES gehoeren den Haupttasks der Kerne */
    for (uint8_t t = IRK_MAX_CORES + 1; t <= irk_task_count(); t++) {
        if (!(irk_task_status(t) & IRK_KILLED)) irk_task_kill(t);
    }
    irk_delay(5);
}


/*========================================================================*\
 *  1  Kontextwechsel findet ueberhaupt statt
\*========================================================================*/

static volatile uint8_t  t1_gelaufen = 0;

static void t1_task(void)
{
    for (;;) { t1_gelaufen = 1; irk_delay(2); }
}

static void pruefe_kontextwechsel(void)
{
    beginne(F("Kontextwechsel findet statt"));

    t1_gelaufen = 0;
    irk_task_t t = irk_task_create(t1_task, 1, stack_a, STK, "t1");
    if (t == IRK_NO_TASK) { ergebnis(false, F("Task nicht anlegbar")); return; }

    bool ok = WARTE_BIS(t1_gelaufen == 1, 500);
    ergebnis(ok, ok ? nullptr : F("Task lief nie an"));
    aufraeumen();
}


/*========================================================================*\
 *  2  Callee-saved Register ueberleben den Wechsel
 *
 *  DIE zentrale Pruefung fuer den Assembler-Kontextwechsel.
 *
 *  Die acht Werte sind ueber den irk_yield()-Aufruf hinweg lebendig.
 *  Der Compiler legt sie deshalb in callee-saved Register -- auf ARM
 *  r4..r11, auf AVR r2..r17 -- also genau in die Register, die
 *  irk_ctx_switch() sichern muss. Wird auch nur eines davon nicht
 *  gerettet, kommt hier ein falscher Wert heraus.
 *
 *  Der Startwert kommt aus millis() und ist volatile, damit der
 *  Compiler nichts vorausberechnen und die Pruefung wegoptimieren kann.
\*========================================================================*/

static volatile uint8_t  t2_ergebnis = 0;   /* 0=laeuft, 1=ok, 2=Fehler */
static volatile uint32_t t2_falsch_bei = 0;

static void t2_task(void)
{
    volatile uint32_t saat = (uint32_t)millis() | 1u;

    uint32_t a = saat * 3u;
    uint32_t b = saat * 5u;
    uint32_t c = saat * 7u;
    uint32_t d = saat * 11u;
    uint32_t e = saat * 13u;
    uint32_t f = saat * 17u;
    uint32_t g = saat * 19u;
    uint32_t h = saat * 23u;

    /* Mehrfach umschalten lassen, damit auch wiederholtes Sichern und
       Wiederherstellen abgedeckt ist. */
    for (uint8_t i = 0; i < 8; i++) irk_delay(1);

    uint32_t s = saat;
    if      (a != s *  3u) { t2_falsch_bei = 1; t2_ergebnis = 2; }
    else if (b != s *  5u) { t2_falsch_bei = 2; t2_ergebnis = 2; }
    else if (c != s *  7u) { t2_falsch_bei = 3; t2_ergebnis = 2; }
    else if (d != s * 11u) { t2_falsch_bei = 4; t2_ergebnis = 2; }
    else if (e != s * 13u) { t2_falsch_bei = 5; t2_ergebnis = 2; }
    else if (f != s * 17u) { t2_falsch_bei = 6; t2_ergebnis = 2; }
    else if (g != s * 19u) { t2_falsch_bei = 7; t2_ergebnis = 2; }
    else if (h != s * 23u) { t2_falsch_bei = 8; t2_ergebnis = 2; }
    else                   { t2_ergebnis = 1; }

    for (;;) irk_delay(10);
}

static void pruefe_register(void)
{
    beginne(F("callee-saved Register ueberleben den Wechsel"));

    t2_ergebnis = 0; t2_falsch_bei = 0;
    irk_task_create(t2_task, 1, stack_a, STK, "t2");

    bool fertig = WARTE_BIS(t2_ergebnis != 0, 1000);

    if (!fertig)                 ergebnis(false, F("Task wurde nicht fertig"));
    else if (t2_ergebnis == 1)   ergebnis(true);
    else ergebnis_zahl(false, F("verfaelschter Wert Nr"), (long)t2_falsch_bei);

    aufraeumen();
}


/*========================================================================*\
 *  3  Tiefe Aufrufkette ueberlebt den Wechsel
 *
 *  Prueft, dass nicht nur die Register, sondern der gesamte Stack
 *  erhalten bleibt: die Rekursion gibt mitten in der tiefsten Ebene die
 *  CPU ab und rechnet beim Zurueckwandern eine Pruefsumme aus.
\*========================================================================*/

static volatile uint8_t  t3_ergebnis = 0;
static volatile uint32_t t3_summe    = 0;

static uint32_t rekursion(uint8_t tiefe)
{
    uint32_t eigen = (uint32_t)tiefe * 100u + 7u;

    if (tiefe == 0) {
        irk_delay(3);            /* ganz unten die CPU abgeben */
        return eigen;
    }
    uint32_t tiefer = rekursion((uint8_t)(tiefe - 1));

    /* eigen muss den Wechsel weiter unten unbeschadet ueberstanden
       haben -- es lebt ueber den rekursiven Aufruf hinweg. */
    return eigen + tiefer;
}

static void t3_task(void)
{
    /* Sollwert:  sum(i=0..7) (i*100 + 7)  =  2800 + 56 = 2856 */
    t3_summe    = rekursion(7);
    t3_ergebnis = (t3_summe == 2856u) ? 1 : 2;
    for (;;) irk_delay(10);
}

static void pruefe_aufrufkette(void)
{
    beginne(F("tiefe Aufrufkette ueberlebt den Wechsel"));

    t3_ergebnis = 0; t3_summe = 0;
    irk_task_create(t3_task, 1, stack_a, STK, "t3");

    bool fertig = WARTE_BIS(t3_ergebnis != 0, 1000);

    if (!fertig)               ergebnis(false, F("Task wurde nicht fertig"));
    else if (t3_ergebnis == 1) ergebnis(true);
    else ergebnis_zahl(false, F("Pruefsumme (soll 2856)"), (long)t3_summe);

    aufraeumen();
}


/*========================================================================*\
 *  4  Tasks haben wirklich getrennte Stacks
 *
 *  Zwei Tasks legen je ein lokales Feld an, tragen ihre eigene Kennung
 *  ein, geben die CPU ab und pruefen danach, ob noch ihre eigene
 *  Kennung darin steht. Ueberlappten die Stacks, wuerde sich hier eine
 *  Task die Daten der anderen einhandeln.
\*========================================================================*/

static volatile uint8_t t4_ok[2]     = { 0, 0 };
static volatile uint8_t t4_fertig[2] = { 0, 0 };

static void t4_body(uint8_t nr)
{
    uint8_t feld[16];
    for (uint8_t i = 0; i < 16; i++) feld[i] = (uint8_t)(nr * 16u + i);

    for (uint8_t runde = 0; runde < 6; runde++) irk_delay(1);

    uint8_t gut = 1;
    for (uint8_t i = 0; i < 16; i++) {
        if (feld[i] != (uint8_t)(nr * 16u + i)) { gut = 0; break; }
    }
    t4_ok[nr]     = gut;
    t4_fertig[nr] = 1;
    for (;;) irk_delay(10);
}

static void t4_task_a(void) { t4_body(0); }
static void t4_task_b(void) { t4_body(1); }

static void pruefe_getrennte_stacks(void)
{
    beginne(F("Tasks haben getrennte Stacks"));

    t4_ok[0] = t4_ok[1] = 0;
    t4_fertig[0] = t4_fertig[1] = 0;

    irk_task_create(t4_task_a, 1, stack_a, STK, "t4a");
    irk_task_create(t4_task_b, 1, stack_b, STK, "t4b");

    bool fertig = WARTE_BIS(t4_fertig[0] && t4_fertig[1], 1000);
    bool ok = fertig && t4_ok[0] && t4_ok[1];

    ergebnis(ok, ok ? nullptr : F("lokale Daten wurden ueberschrieben"));
    aufraeumen();
}


/*========================================================================*\
 *  5  Fair Share  --  Prioritaet bestimmt den Rechenzeitanteil
\*========================================================================*/

static volatile uint32_t fs_z[3] = { 0, 0, 0 };
static volatile uint8_t  fs_lauf = 0;

static void fs_a(void) { while (fs_lauf) { fs_z[0]++; irk_yield(); } for(;;) irk_delay(10); }
static void fs_b(void) { while (fs_lauf) { fs_z[1]++; irk_yield(); } for(;;) irk_delay(10); }
static void fs_c(void) { while (fs_lauf) { fs_z[2]++; irk_yield(); } for(;;) irk_delay(10); }

static void pruefe_fairshare(void)
{
    beginne(F("Fair Share 1:2:3"));

    fs_z[0] = fs_z[1] = fs_z[2] = 0;
    fs_lauf = 1;

    irk_task_create(fs_a, 1, stack_a, STK, "fs1");
    irk_task_create(fs_b, 2, stack_b, STK, "fs2");
    irk_task_create(fs_c, 3, stack_c, STK, "fs3");

    irk_delay(2000);              /* messen */
    fs_lauf = 0;
    irk_delay(20);

    uint32_t z1 = fs_z[0], z2 = fs_z[1], z3 = fs_z[2];

    Serial.println();
    Serial.print(F("        Zaehler: ")); Serial.print(z1);
    Serial.print(F(" / ")); Serial.print(z2);
    Serial.print(F(" / ")); Serial.println(z3);

    bool ok = false;
    if (z1 > 20) {
        /* Verhaeltnisse in Promille, um Fliesskomma zu sparen */
        uint32_t v2 = (z2 * 1000u) / z1;
        uint32_t v3 = (z3 * 1000u) / z1;
        Serial.print(F("        Verhaeltnis 1.000 : "));
        Serial.print(v2 / 1000.0, 3); Serial.print(F(" : "));
        Serial.println(v3 / 1000.0, 3);
        ok = (v2 > 1700 && v2 < 2300) && (v3 > 2600 && v3 < 3400);
    }
    Serial.print(F("        "));
    ergebnis(ok, ok ? nullptr : F("Verhaeltnis ausserhalb der Toleranz"));
    aufraeumen();
}


/*========================================================================*\
 *  6  irk_delay() haelt seine Frist
\*========================================================================*/

static void pruefe_delay(void)
{
    beginne(F("irk_delay() haelt die Frist"));

    uint32_t vorher = millis();
    irk_delay(200);
    uint32_t gebraucht = millis() - vorher;

    bool ok = (gebraucht >= 195 && gebraucht <= 260);
    ergebnis_zahl(ok, F("ms (soll 200)"), (long)gebraucht);
}


/*========================================================================*\
 *  7 + 8  Semaphor:  gegenseitiger Ausschluss und FIFO-Reihenfolge
\*========================================================================*/

#define SEM_TEST 0

static volatile uint8_t  sem_drin      = 0;
static volatile uint8_t  sem_max_drin  = 0;
static volatile uint8_t  sem_folge[6];
static volatile uint8_t  sem_folge_n   = 0;
static volatile uint8_t  sem_lauf      = 0;

static void sem_body(void)
{
    uint8_t ich = irk_task_self();
    while (sem_lauf) {
        irk_sema_wait(SEM_TEST);

        sem_drin++;
        if (sem_drin > sem_max_drin) sem_max_drin = sem_drin;
        if (sem_folge_n < 6) sem_folge[sem_folge_n++] = ich;

        irk_delay(3);              /* im kritischen Abschnitt abgeben */

        sem_drin--;
        irk_sema_signal(SEM_TEST);

        irk_delay(2);
    }
    for (;;) irk_delay(10);
}

static void pruefe_semaphor(void)
{
    beginne(F("Semaphor: gegenseitiger Ausschluss"));

    sem_drin = sem_max_drin = sem_folge_n = 0;
    sem_lauf = 1;
    irk_sema_init(SEM_TEST, 1);

    /* Handles merken statt 2/3/4 anzunehmen -- auf Mehrkern-Boards sind
       die ersten Handles fuer die Haupttasks der Kerne reserviert. */
    irk_task_t h1 = irk_task_create(sem_body, 1, stack_a, STK, "s1");
    irk_task_t h2 = irk_task_create(sem_body, 1, stack_b, STK, "s2");
    irk_task_t h3 = irk_task_create(sem_body, 1, stack_c, STK, "s3");

    irk_delay(600);
    sem_lauf = 0;
    irk_delay(30);

    ergebnis_zahl(sem_max_drin == 1, F("max. gleichzeitig drin"),
                  (long)sem_max_drin);

    beginne(F("Semaphor: alle Tasks kommen dran"));
    /* Bei FIFO muessen in den ersten sechs Eintritten alle drei
       Tasks vorkommen -- keine darf uebergangen werden. */
    bool a = false, b = false, c = false;
    for (uint8_t i = 0; i < sem_folge_n; i++) {
        if (sem_folge[i] == h1) a = true;
        if (sem_folge[i] == h2) b = true;
        if (sem_folge[i] == h3) c = true;
    }
    ergebnis(a && b && c && sem_folge_n >= 3,
             (a && b && c) ? nullptr : F("eine Task kam nie dran"));

    aufraeumen();
}


/*========================================================================*\
 *  9 + 10  Queue:  FIFO, Blockieren, und ohne Blockieren
\*========================================================================*/

static irk_queue_t  q;
static uint16_t     q_speicher[4];

static volatile uint16_t q_gesendet  = 0;
static volatile uint16_t q_empfangen = 0;
static volatile uint8_t  q_folge_ok  = 1;
static volatile uint8_t  q_war_voll  = 0;
static volatile uint8_t  q_lauf      = 0;

static void q_sender(void)
{
    uint16_t n = 0;
    while (q_lauf) {
        if (irk_queue_count(&q) >= 4) q_war_voll = 1;
        irk_queue_send(&q, &n);
        q_gesendet++;
        n++;
        irk_delay(1);
    }
    for (;;) irk_delay(10);
}

static void q_empfaenger(void)
{
    uint16_t erwartet = 0, wert;
    while (q_lauf) {
        irk_queue_recv(&q, &wert);
        if (wert != erwartet) q_folge_ok = 0;
        erwartet++;
        q_empfangen++;
        irk_delay(4);              /* absichtlich langsamer */
    }
    for (;;) irk_delay(10);
}

static void pruefe_queue(void)
{
    beginne(F("Queue: FIFO-Reihenfolge und Blockieren"));

    q_gesendet = q_empfangen = 0;
    q_folge_ok = 1; q_war_voll = 0; q_lauf = 1;
    irk_queue_init(&q, q_speicher, sizeof(uint16_t), 4);

    irk_task_create(q_sender,     1, stack_a, STK, "qs");
    irk_task_create(q_empfaenger, 1, stack_b, STK, "qe");

    irk_delay(800);
    q_lauf = 0;
    irk_delay(30);

    uint16_t vorsprung = (uint16_t)(q_gesendet - q_empfangen);
    bool ok = q_folge_ok && q_war_voll && (q_empfangen > 10) &&
              (vorsprung <= 5);

    Serial.println();
    Serial.print(F("        gesendet=")); Serial.print(q_gesendet);
    Serial.print(F(" empfangen="));       Serial.print(q_empfangen);
    Serial.print(F(" Vorsprung="));       Serial.println(vorsprung);
    Serial.print(F("        "));

    if      (!q_folge_ok) ergebnis(false, F("FIFO-Reihenfolge verletzt"));
    else if (!q_war_voll) ergebnis(false, F("Queue lief nie voll"));
    else                  ergebnis(ok, ok ? nullptr : F("Sender blockierte nicht"));

    aufraeumen();

    /* --- ohne Blockieren --- */
    beginne(F("Queue: try_send/try_recv"));

    irk_queue_init(&q, q_speicher, sizeof(uint16_t), 4);
    bool tok = true;
    uint16_t v;

    if (irk_queue_try_recv(&q, &v) != 0) tok = false;   /* leer */
    for (uint16_t i = 0; i < 4; i++) {
        v = (uint16_t)(i * 11u);
        if (irk_queue_try_send(&q, &v) != 1) tok = false;
    }
    v = 999;
    if (irk_queue_try_send(&q, &v) != 0) tok = false;   /* voll */
    for (uint16_t i = 0; i < 4; i++) {
        if (irk_queue_try_recv(&q, &v) != 1 || v != (uint16_t)(i * 11u))
            tok = false;
    }
    ergebnis(tok, tok ? nullptr : F("Fuellen/Ueberlauf/Leeren fehlerhaft"));
}


/*========================================================================*\
 *  11  Zyklische Task haelt ihren Takt
\*========================================================================*/

static volatile uint16_t zyk_takte = 0;
static volatile uint8_t  zyk_lauf  = 0;

static void zyk_task(void)
{
    for (;;) { zyk_takte++; irk_yield(); }
}

static void zyk_last(void)
{
    while (zyk_lauf) { irk_yield(); }
    for (;;) irk_delay(10);
}

static void pruefe_zyklisch(void)
{
    beginne(F("zyklische Task haelt den Takt"));

    zyk_takte = 0;
    zyk_lauf  = 1;

    irk_task_t tz = irk_task_create(zyk_task, 1, stack_a, STK, "zyk");
    irk_task_create(zyk_last, 3, stack_b, STK, "last");   /* Stoerlast */

    irk_task_set_cyclic(tz, 10);      /* alle 10 ms */
    irk_delay(1000);                  /* -> etwa 100 Takte */

    uint16_t n = zyk_takte;
    zyk_lauf = 0;
    irk_delay(20);

    bool ok = (n >= 85 && n <= 115);
    ergebnis_zahl(ok, F("Takte in 1000 ms (soll ~100)"), (long)n);
    aufraeumen();
}


/*========================================================================*\
 *  12  Task loeschen und Slot wiederverwenden
\*========================================================================*/

static volatile uint8_t kill_lief = 0;

static void kill_task(void)
{
    for (;;) { kill_lief = 1; irk_delay(2); }
}

static void pruefe_loeschen(void)
{
    beginne(F("Task loeschen und Slot wiederverwenden"));

    kill_lief = 0;
    irk_task_t a = irk_task_create(kill_task, 1, stack_a, STK, "k1");
    WARTE_BIS(kill_lief == 1, 300);

    irk_task_kill(a);
    irk_delay(10);
    bool tot = (irk_task_status(a) & IRK_KILLED) != 0;

    kill_lief = 0;
    irk_task_t b = irk_task_create(kill_task, 1, stack_a, STK, "k2");
    bool wieder = WARTE_BIS(kill_lief == 1, 300);

    bool ok = tot && wieder && (b == a);
    ergebnis(ok, ok ? nullptr : F("Slot wurde nicht sauber wiederverwendet"));
    aufraeumen();
}


/*========================================================================*\
 *  13  Stackreserve  --  informativ, aber mit Untergrenze
\*========================================================================*/

static void pruefe_stackreserve(void)
{
#if IRK_ENABLE_STACKCHECK
    beginne(F("Stackreserve der Testtasks"));

    kill_lief = 0;
    irk_task_t t = irk_task_create(kill_task, 1, stack_a, STK, "sr");
    WARTE_BIS(kill_lief == 1, 300);
    irk_delay(50);

    size_t frei = irk_stack_free(t);

    Serial.println();
    Serial.print(F("        Stack ")); Serial.print((unsigned)STK);
    Serial.print(F(" Byte, davon nie benutzt: ")); Serial.print((unsigned long)frei);
    Serial.println(F(" Byte"));
    Serial.print(F("        "));

    /* Weniger als 32 Byte Reserve ist bereits gefaehrlich. */
    ergebnis(frei >= 32, frei >= 32 ? nullptr : F("Reserve zu knapp!"));
    aufraeumen();
#endif
}


/*========================================================================*\
 *  Ablauf
\*========================================================================*/

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { }
    delay(200);

    Serial.println();
    Serial.println(F("========================================"));
    Serial.println(F(" IRKernel  --  Selbsttest auf Hardware"));
    Serial.println(F("========================================"));

    Serial.print(F("  Board-RAM je Taskstack: ")); Serial.println((unsigned)STK);
    Serial.print(F("  max. Tasks: "));  Serial.println((unsigned)IRK_MAX_TASKS);
    Serial.print(F("  Semaphore:  "));  Serial.println((unsigned)IRK_MAX_SEMAPHORES);
    Serial.print(F("  Mindest-Zeitscheibe: ")); Serial.print((unsigned long)IRK_MIN_TIMESLICE_US);
    Serial.println(F(" us (0 = irk_yield gibt sofort ab)"));
    Serial.println();
    Serial.flush();

    if (irk_init(1) != 0) {
        Serial.println(F("  KERNEL LIESS SICH NICHT STARTEN -- Abbruch."));
        for (;;) { }
    }

    /* --- Die drei Pruefungen, um die es hier eigentlich geht --- */
    Serial.println(F("  -- Kontextwechsel --"));
    pruefe_kontextwechsel();
    pruefe_register();
    pruefe_aufrufkette();
    pruefe_getrennte_stacks();

    Serial.println();
    Serial.println(F("  -- Scheduling --"));
    pruefe_fairshare();
    pruefe_delay();
    pruefe_zyklisch();

    Serial.println();
    Serial.println(F("  -- Synchronisation --"));
    pruefe_semaphor();

    Serial.println();
    Serial.println(F("  -- Inter-Task-Kommunikation --"));
    pruefe_queue();

    Serial.println();
    Serial.println(F("  -- Taskverwaltung --"));
    pruefe_loeschen();
    pruefe_stackreserve();

    Serial.println();
    Serial.println(F("========================================"));
    Serial.print(F(" ERGEBNIS: "));
    Serial.print(geprueft);
    Serial.print(F(" Pruefungen, "));
    Serial.print(fehler);
    Serial.println(F(" Fehler"));
    Serial.println(F("========================================"));
    Serial.flush();
}


void loop()
{
    /* Ergebnis alle fuenf Sekunden wiederholen, damit man es auch
       findet, wenn der Monitor spaeter geoeffnet wird. */
    delay(5000);
    Serial.print(F("ERGEBNIS: "));
    Serial.print(geprueft); Serial.print(F(" Pruefungen, "));
    Serial.print(fehler);   Serial.println(F(" Fehler"));
}
