/*========================================================================*\
 *
 *  test_kernel.c  --  Prueft die Scheduling-Politik des IRKernel
 *
 *  Laeuft auf dem Entwicklungsrechner gegen das Host-Backend. Die Uhr
 *  wird dabei simuliert: jede Task "arbeitet" je Durchlauf genau eine
 *  Millisekunde, indem sie die Testuhr weiterstellt. Damit sind die
 *  Ergebnisse exakt reproduzierbar und der ganze Testlauf dauert
 *  Sekundenbruchteile statt Minuten.
 *
 *  Bauen und ausfuehren:  test/run_tests.sh
 *
\*========================================================================*/

#include <stdio.h>
#include <string.h>

#include "../src/IRKernel.h"

/* Steuerbare Uhr des Host-Backends */
extern void irk_test_clock_enable(int on);
extern void irk_test_clock_set(uint32_t ms);
extern void irk_test_clock_add(uint32_t ms);
extern void irk_test_clock_set_us(uint64_t us);
extern void irk_test_clock_add_us(uint64_t us);

/* Startwert der Testuhr in Mikrosekunden. run_tests.sh setzt ihn auf
   2 Sekunden vor den 32-Bit-Ueberlauf -- der Wrap faellt dann mitten in
   jeden einzelnen Test. */
#ifndef IRK_TEST_T0
#define IRK_TEST_T0  0ULL
#endif


/*========================================================================*\
 *  Winziges Testgeruest
\*========================================================================*/

static int tests_run = 0, tests_failed = 0;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        tests_run++;                                            \
        if (!(cond)) {                                          \
            tests_failed++;                                     \
            printf("  FEHLER  " __VA_ARGS__);                   \
            printf("\n          (%s:%d)\n", __FILE__, __LINE__);\
        }                                                       \
    } while (0)

#define SECTION(name) printf("\n== %s\n", name)

/* Prueft a/b gegen soll mit Toleranz in Prozent */
static int ratio_ok(uint32_t a, uint32_t b, double expect, double tol_pct)
{
    double r, dev;
    if (b == 0) return 0;
    r   = (double)a / (double)b;
    dev = (r - expect) / expect * 100.0;
    if (dev < 0) dev = -dev;
    return dev <= tol_pct;
}

/* Eine Millisekunde "Arbeit" und danach die CPU abgeben */
static void work_1ms(void)
{
    irk_test_clock_add(1);
    irk_yield();
}


/*========================================================================*\
 *  1. Fair Share:  Prioritaet == Anteil an der Rechenzeit
\*========================================================================*/

static volatile int   fs_stop;
static volatile long  fs_runs[4];

static void fs_task_1(void) { while (!fs_stop) { fs_runs[1]++; work_1ms(); } for(;;) work_1ms(); }
static void fs_task_2(void) { while (!fs_stop) { fs_runs[2]++; work_1ms(); } for(;;) work_1ms(); }
static void fs_task_3(void) { while (!fs_stop) { fs_runs[3]++; work_1ms(); } for(;;) work_1ms(); }

static void test_fair_share(void)
{
    static char s1[64000], s2[64000], s3[64000];
    irk_task_t t1, t2, t3;
    int i;

    SECTION("Fair Share -- Prioritaet bestimmt den Rechenzeitanteil");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    memset((void *)fs_runs, 0, sizeof(fs_runs));
    fs_stop = 0;

    irk_init(1);

    t1 = irk_task_create(fs_task_1, 1, s1, sizeof(s1), "prio1");
    t2 = irk_task_create(fs_task_2, 2, s2, sizeof(s2), "prio2");
    t3 = irk_task_create(fs_task_3, 3, s3, sizeof(s3), "prio3");

    CHECK(t1 && t2 && t3, "Tasks konnten nicht angelegt werden");

    /* Haupttask gibt 4000-mal ab, damit die Simulation laeuft. */
    for (i = 0; i < 4000; i++) work_1ms();

    fs_stop = 1;
    for (i = 0; i < 50; i++) work_1ms();   /* Tasks auslaufen lassen */

    printf("  Laufzeiten:  prio1=%lu  prio2=%lu  prio3=%lu  (ms)\n",
           (unsigned long)irk_task_runtime(t1),
           (unsigned long)irk_task_runtime(t2),
           (unsigned long)irk_task_runtime(t3));
    printf("  Durchlaeufe: prio1=%ld  prio2=%ld  prio3=%ld\n",
           fs_runs[1], fs_runs[2], fs_runs[3]);

    /* Das Kernversprechen: doppelte Prioritaet -> doppelte Rechenzeit. */
    CHECK(ratio_ok(irk_task_runtime(t2), irk_task_runtime(t1), 2.0, 10.0),
          "prio2/prio1 sollte ~2.0 sein, ist %.2f",
          (double)irk_task_runtime(t2) / (double)irk_task_runtime(t1));

    CHECK(ratio_ok(irk_task_runtime(t3), irk_task_runtime(t1), 3.0, 10.0),
          "prio3/prio1 sollte ~3.0 sein, ist %.2f",
          (double)irk_task_runtime(t3) / (double)irk_task_runtime(t1));

    /* Niemand darf verhungern -- das ist der Unterschied zu strikter
       Prioritaetsverdraengung. */
    CHECK(fs_runs[1] > 100, "Task mit niedrigster Prioritaet ist verhungert");

    irk_deinit();
}


/*========================================================================*\
 *  2. Zyklische Task:  fester Takt mit Vorrang
\*========================================================================*/

static volatile int  cyc_ticks;
static volatile int  cyc_stop;

static void cyclic_task(void)
{
    while (!cyc_stop) {
        cyc_ticks++;
        irk_yield();          /* bis zur naechsten Faelligkeit schlafen */
    }
    for (;;) work_1ms();   /* Leerlauf: Zeit muss vergehen */
}

static void test_cyclic(void)
{
    static char st[64000];
    irk_task_t tc;
    int i;

    SECTION("Zyklische Task -- periodische Aktivierung mit Vorrang");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    cyc_ticks = 0;
    cyc_stop  = 0;

    irk_init(1);

    tc = irk_task_create(cyclic_task, 1, st, sizeof(st), "regler");
    CHECK(tc != 0, "zyklische Task konnte nicht angelegt werden");

    irk_task_set_cyclic(tc, 10);           /* alle 10 ms */

    for (i = 0; i < 1000; i++) work_1ms(); /* 1000 ms simulieren */

    printf("  Aktivierungen in 1000 ms bei 10 ms Periode: %d (erwartet ~100)\n",
           cyc_ticks);

    CHECK(cyc_ticks >= 90 && cyc_ticks <= 110,
          "erwartet ~100 Aktivierungen, gezaehlt %d", cyc_ticks);

    cyc_stop = 1;
    irk_deinit();
}


/*========================================================================*\
 *  3. Rueckkehr aus dem Zyklusbetrieb
 *
 *  Nach irk_task_set_normal() laeuft eine vormals zyklische Task wieder
 *  als gewoehnliche Fair-Share-Task -- der Wartegrund "Zyklus" muss
 *  restlos verschwinden, und kein anderer darf dabei entstehen.
\*========================================================================*/

static volatile int  cyc_runs;
static volatile int  cyc_stop;

static void cyc_task(void)
{
    while (!cyc_stop) { cyc_runs++; work_1ms(); }
    for (;;) work_1ms();   /* Leerlauf: Zeit muss vergehen */
}

static void test_cyclic_to_normal(void)
{
    static char st[64000];
    irk_task_t t;
    int before, i;

    SECTION("Rueckkehr aus dem Zyklusbetrieb in den Normalbetrieb");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    cyc_runs = 0;
    cyc_stop = 0;

    irk_init(1);
    t = irk_task_create(cyc_task, 2, st, sizeof(st), "wechsler");

    irk_task_set_cyclic(t, 50);
    for (i = 0; i < 200; i++) work_1ms();
    before = cyc_runs;

    irk_task_set_normal(t, 2);

    CHECK((irk_task_status(t) & IRK_WAIT_CYCLE) == 0,
          "Zyklus-Bit steht nach set_normal() noch (Status 0x%04X)",
          irk_task_status(t));
    CHECK(irk_task_status(t) == IRK_RUNNING,
          "Task ist nach set_normal() nicht lauffaehig (Status 0x%04X)",
          irk_task_status(t));

    for (i = 0; i < 200; i++) work_1ms();

    printf("  Durchlaeufe zyklisch: %d, danach normal: %d\n",
           before, cyc_runs - before);

    CHECK(cyc_runs - before > before,
          "Task laeuft nach set_normal() nicht haeufiger als im Zyklus");

    cyc_stop = 1;
    irk_deinit();
}


/*========================================================================*\
 *  4. Prioritaet 0
 *
 *  Prioritaet 0 heisst "angelegt, aber nicht eingeplant". Die Task darf
 *  dadurch weder laufen noch den Scheduler stoeren (keine Division durch
 *  ihre Prioritaet).
\*========================================================================*/

static volatile int prio0_runs;
static volatile int prio0_stop;

static void prio0_task(void)
{
    while (!prio0_stop) { prio0_runs++; work_1ms(); }
    for (;;) work_1ms();   /* Leerlauf: Zeit muss vergehen */
}

static void test_prio_zero(void)
{
    static char st[64000], st2[64000];
    irk_task_t t, t2;
    int i, after_disable;

    SECTION("Prioritaet 0 -- kein Absturz, Task wird ausgeplant");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    prio0_runs = 0;
    prio0_stop = 0;

    irk_init(1);
    t  = irk_task_create(prio0_task, 1, st,  sizeof(st),  "aus");
    t2 = irk_task_create(prio0_task, 1, st2, sizeof(st2), "an");
    CHECK(t && t2, "Tasks konnten nicht angelegt werden");

    for (i = 0; i < 100; i++) work_1ms();

    irk_task_set_prio(t, 0);                 /* ausplanen */
    for (i = 0; i < 100; i++) work_1ms();
    after_disable = prio0_runs;
    for (i = 0; i < 100; i++) work_1ms();

    /* Erreicht der Test diese Zeile, hat es keinen Division-Fehler gegeben. */
    CHECK(1, "unerreichbar");
    printf("  Ueberlebt: Prioritaet 0 gesetzt, %d weitere Durchlaeufe\n",
           prio0_runs - after_disable);

    CHECK(irk_task_get_prio(t) == 0, "Prioritaet 0 wurde nicht uebernommen");

    prio0_stop = 1;
    irk_deinit();
}


/*========================================================================*\
 *  5. Semaphore -- gegenseitiger Ausschluss und FIFO-Weckreihenfolge
\*========================================================================*/

#define SEM_MUTEX  0

static volatile int  sem_inside;      /* wie viele gleichzeitig drin sind */
static volatile int  sem_max_inside;
static volatile int  sem_order[8];
static volatile int  sem_order_n;
static volatile int  sem_stop;

static void sema_worker(void)
{
    irk_task_t me = irk_task_self();
    while (!sem_stop) {
        irk_sema_wait(SEM_MUTEX);

        sem_inside++;
        if (sem_inside > sem_max_inside) sem_max_inside = sem_inside;
        if (sem_order_n < 8) sem_order[sem_order_n++] = me;

        work_1ms();          /* im kritischen Abschnitt die CPU abgeben */

        sem_inside--;
        irk_sema_signal(SEM_MUTEX);

        work_1ms();
    }
    for (;;) work_1ms();   /* Leerlauf: Zeit muss vergehen */
}

static void test_semaphore(void)
{
    static char s1[64000], s2[64000], s3[64000];
    irk_task_t t1, t2, t3;
    int i;

    SECTION("Semaphore -- gegenseitiger Ausschluss, ohne jede Allokation");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    sem_inside = sem_max_inside = sem_order_n = 0;
    sem_stop   = 0;

    irk_init(1);
    irk_sema_init(SEM_MUTEX, 1);           /* binaeres Semaphor */

    t1 = irk_task_create(sema_worker, 1, s1, sizeof(s1), "w1");
    t2 = irk_task_create(sema_worker, 1, s2, sizeof(s2), "w2");
    t3 = irk_task_create(sema_worker, 1, s3, sizeof(s3), "w3");
    CHECK(t1 && t2 && t3, "Tasks konnten nicht angelegt werden");

    for (i = 0; i < 600; i++) work_1ms();

    printf("  Hoechste Zahl gleichzeitig im kritischen Abschnitt: %d\n",
           sem_max_inside);
    printf("  Erste Eintritte (Task-Handles): ");
    for (i = 0; i < sem_order_n && i < 6; i++) printf("%d ", sem_order[i]);
    printf("\n");

    CHECK(sem_max_inside == 1,
          "gegenseitiger Ausschluss verletzt: %d Tasks gleichzeitig drin",
          sem_max_inside);

    CHECK(sem_order_n >= 4, "Semaphor wurde kaum benutzt (%d Eintritte)",
          sem_order_n);

    sem_stop = 1;
    for (i = 0; i < 50; i++) work_1ms();
    irk_deinit();
}


/*========================================================================*\
 *  6. irk_delay() -- Frist wird eingehalten
\*========================================================================*/

static volatile uint32_t dly_wake_at;
static volatile int      dly_done;

static void delay_task(void)
{
    irk_delay(100);
    dly_wake_at = irk_task_runtime(IRK_MAIN_TASK);  /* Platzhalter */
    dly_done    = 1;
    for (;;) work_1ms();   /* Leerlauf: Zeit muss vergehen */
}

static uint32_t sim_now;

static void test_delay(void)
{
    static char st[64000];
    irk_task_t t;
    int i;
    uint32_t woke = 0;

    SECTION("irk_delay() -- Task schlaeft und wacht rechtzeitig auf");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    dly_done = 0;
    sim_now  = 0;

    irk_init(1);
    t = irk_task_create(delay_task, 1, st, sizeof(st), "schlaefer");
    CHECK(t != 0, "Task konnte nicht angelegt werden");

    for (i = 0; i < 300 && !dly_done; i++) {
        sim_now++;
        work_1ms();
        if (dly_done && woke == 0) woke = sim_now;
    }

    printf("  Aufgewacht nach %lu ms (angefordert: 100 ms)\n",
           (unsigned long)woke);

    CHECK(dly_done, "Task ist nie aufgewacht");
    CHECK(woke >= 100 && woke <= 115,
          "erwartet 100..115 ms, aufgewacht nach %lu ms", (unsigned long)woke);

    irk_deinit();
}


/*========================================================================*\
 *  7. Task loescht sich selbst und laeuft nicht weiter
 *
 *  irk_task_kill() auf die eigene Task kehrt nie zurueck: in einer
 *  bereits geloeschten Task darf keine Anweisung mehr ausgefuehrt
 *  werden, unabhaengig von der Mindest-Zeitscheibe.
\*========================================================================*/

static volatile int suicide_after_kill;
static volatile int suicide_reached;

static void suicide_task(void)
{
    suicide_reached = 1;
    irk_task_kill(irk_task_self());
    suicide_after_kill = 1;      /* darf niemals erreicht werden */
    for (;;) work_1ms();   /* Leerlauf: Zeit muss vergehen */
}

static void test_self_kill(void)
{
    static char st[64000];
    irk_task_t t;
    int i;

    SECTION("Selbstloeschung kehrt nicht in die tote Task zurueck");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    suicide_after_kill = 0;
    suicide_reached    = 0;

    irk_init(1);
    t = irk_task_create(suicide_task, 1, st, sizeof(st), "kamikaze");
    CHECK(t != 0, "Task konnte nicht angelegt werden");

    for (i = 0; i < 100; i++) work_1ms();

    CHECK(suicide_reached, "Task lief gar nicht erst an");
    CHECK(!suicide_after_kill,
          "Code nach der Selbstloeschung wurde ausgefuehrt");
    CHECK(irk_task_status(t) & IRK_KILLED,
          "Task ist nicht als geloescht eingetragen");

    printf("  Task lief an: %d, Code nach kill erreicht: %d (soll 0)\n",
           suicide_reached, suicide_after_kill);

    irk_deinit();
}


/*========================================================================*\
 *  8. Slot-Wiederverwendung nach dem Loeschen
\*========================================================================*/

static volatile int reuse_runs;
static volatile int reuse_stop;

static void reuse_task(void) {
    while (!reuse_stop) { reuse_runs++; work_1ms(); }
    for (;;) work_1ms();   /* Leerlauf: Zeit muss vergehen */
}

static void test_slot_reuse(void)
{
    static char s1[64000], s2[64000];
    irk_task_t a, b;
    int i;

    SECTION("Taskslots werden nach dem Loeschen wiederverwendet");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    reuse_runs = 0;
    reuse_stop = 0;

    irk_init(1);

    a = irk_task_create(reuse_task, 1, s1, sizeof(s1), "a");
    for (i = 0; i < 20; i++) work_1ms();
    irk_task_kill(a);
    for (i = 0; i < 5; i++) work_1ms();

    b = irk_task_create(reuse_task, 1, s2, sizeof(s2), "b");
    for (i = 0; i < 20; i++) work_1ms();

    printf("  erstes Handle %d, nach Loeschen neu vergeben: %d\n", a, b);

    CHECK(b == a, "Slot wurde nicht wiederverwendet (%d statt %d)", b, a);
    CHECK(irk_task_count() == IRK_MAX_CORES + 1,
          "Taskzahl unerwartet: %d", irk_task_count());

    reuse_stop = 1;
    irk_deinit();
}


/*========================================================================*\
 *  9. Queues -- Reihenfolge, Blockieren bei voll und bei leer
\*========================================================================*/

static irk_queue_t  q_test;
static uint16_t     q_store[4];

static volatile int      q_prod_sent   = 0;
static volatile int      q_cons_got    = 0;
static volatile int      q_order_ok    = 1;
static volatile int      q_prod_blocked = 0;
static volatile int      q_stop        = 0;

/* Erzeuger: schickt aufsteigende Zahlen, so schnell er darf. */
static void q_producer(void)
{
    uint16_t n = 0;
    while (!q_stop) {
        /* Wenn die Queue voll ist, muss irk_queue_send() blockieren --
           das laesst sich daran erkennen, dass der Zaehler dabei
           stehenbleibt, waehrend der Verbraucher weiterlaeuft. */
        if (irk_queue_count(&q_test) >= 4) q_prod_blocked = 1;

        irk_queue_send(&q_test, &n);
        q_prod_sent++;
        n++;
        work_1ms();
    }
    for (;;) work_1ms();
}

/* Verbraucher: liest langsamer, als der Erzeuger schreibt. */
static void q_consumer(void)
{
    uint16_t erwartet = 0, wert;
    while (!q_stop) {
        irk_queue_recv(&q_test, &wert);
        if (wert != erwartet) q_order_ok = 0;   /* FIFO verletzt */
        erwartet++;
        q_cons_got++;
        work_1ms(); work_1ms(); work_1ms();     /* absichtlich traege */
    }
    for (;;) work_1ms();
}

static void test_queue(void)
{
    static char s1[64000], s2[64000];
    irk_task_t tp, tc;
    int i;

    SECTION("Queues -- FIFO-Reihenfolge und Blockieren bei voll/leer");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    q_prod_sent = q_cons_got = 0;
    q_order_ok  = 1;
    q_prod_blocked = 0;
    q_stop = 0;

    irk_init(1);

    CHECK(irk_queue_init(&q_test, q_store, sizeof(uint16_t), 4) == 0,
          "Queue liess sich nicht einrichten");

    tp = irk_task_create(q_producer, 1, s1, sizeof(s1), "prod");
    tc = irk_task_create(q_consumer, 1, s2, sizeof(s2), "cons");
    CHECK(tp && tc, "Tasks konnten nicht angelegt werden");

    for (i = 0; i < 600; i++) work_1ms();

    printf("  gesendet: %d   empfangen: %d   in der Queue: %d\n",
           q_prod_sent, q_cons_got, irk_queue_count(&q_test));

    CHECK(q_cons_got > 20, "es wurde kaum etwas uebertragen (%d)", q_cons_got);
    CHECK(q_order_ok, "FIFO-Reihenfolge verletzt");
    CHECK(q_prod_blocked, "Queue lief nie voll -- Blockieren ungeprueft");

    /* Der Erzeuger darf hoechstens um die Kapazitaet vorauseilen. Waere
       das Blockieren wirkungslos, liefe er beliebig weit davon. */
    CHECK((q_prod_sent - q_cons_got) <= 4 + 1,
          "Erzeuger eilt um %d voraus, Kapazitaet ist nur 4",
          q_prod_sent - q_cons_got);

    q_stop = 1;
    for (i = 0; i < 20; i++) work_1ms();
    irk_deinit();
}


/*========================================================================*\
 *  10. Queues ohne Blockieren  --  try_send / try_recv
\*========================================================================*/

static void test_queue_nonblocking(void)
{
    static irk_queue_t q;
    static uint8_t     store[3];
    uint8_t v;
    int r;

    SECTION("Queues -- try_send/try_recv blockieren nie");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    irk_init(1);

    CHECK(irk_queue_init(&q, store, sizeof(uint8_t), 3) == 0,
          "Queue liess sich nicht einrichten");

    /* Leere Queue: try_recv muss 0 melden, nicht blockieren. */
    r = irk_queue_try_recv(&q, &v);
    CHECK(r == 0, "try_recv auf leerer Queue lieferte %d statt 0", r);

    /* Bis zum Rand fuellen. */
    v = 10; CHECK(irk_queue_try_send(&q, &v) == 1, "1. try_send scheiterte");
    v = 20; CHECK(irk_queue_try_send(&q, &v) == 1, "2. try_send scheiterte");
    v = 30; CHECK(irk_queue_try_send(&q, &v) == 1, "3. try_send scheiterte");

    CHECK(irk_queue_count(&q) == 3, "Fuellstand %d statt 3",
          irk_queue_count(&q));

    /* Volle Queue: try_send muss 0 melden. */
    v = 40;
    r = irk_queue_try_send(&q, &v);
    CHECK(r == 0, "try_send auf voller Queue lieferte %d statt 0", r);

    /* Wieder auslesen, Reihenfolge pruefen. */
    CHECK(irk_queue_try_recv(&q, &v) == 1 && v == 10, "1. Wert falsch: %d", v);
    CHECK(irk_queue_try_recv(&q, &v) == 1 && v == 20, "2. Wert falsch: %d", v);
    CHECK(irk_queue_try_recv(&q, &v) == 1 && v == 30, "3. Wert falsch: %d", v);
    CHECK(irk_queue_count(&q) == 0, "Queue nicht leer");

    /* Ringpuffer-Umlauf: nochmal fuellen und leeren. */
    v = 50; irk_queue_try_send(&q, &v);
    v = 60; irk_queue_try_send(&q, &v);
    CHECK(irk_queue_try_recv(&q, &v) == 1 && v == 50,
          "Umlauf: 1. Wert falsch: %d", v);
    CHECK(irk_queue_try_recv(&q, &v) == 1 && v == 60,
          "Umlauf: 2. Wert falsch: %d", v);

    printf("  Fuellen, Ueberlauf, Leeren und Ringumlauf verhalten sich korrekt\n");

    irk_deinit();
}


/*========================================================================*\
 *  11. Task loeschen, waehrend sie in einer Warteschlange haengt
 *
 *  Haengt eine wartende Task beim Loeschen in einer Semaphor- oder
 *  Queue-Schlange, muss sie dort ausgekettet werden -- sonst wartet die
 *  Schlange ewig auf eine Task, die es nicht mehr gibt.
\*========================================================================*/

static irk_queue_t  q_kill;
static uint8_t      q_kill_store[2];
static volatile int kill_waiter_ran = 0;
static volatile int kill_other_got  = 0;

static void kill_waiter(void)
{
    uint8_t v;
    kill_waiter_ran = 1;
    irk_queue_recv(&q_kill, &v);     /* blockiert: Queue ist leer */
    kill_waiter_ran = 2;             /* darf nicht erreicht werden */
    for (;;) work_1ms();
}

static void kill_other(void)
{
    uint8_t v;
    for (;;) {
        if (irk_queue_try_recv(&q_kill, &v) == 1) kill_other_got++;
        work_1ms();
    }
}

static void test_kill_while_waiting(void)
{
    static char s1[64000], s2[64000];
    irk_task_t tw, to;
    uint8_t v = 99;
    int i;

    SECTION("Loeschen einer Task, die in einer Warteschlange haengt");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    kill_waiter_ran = 0;
    kill_other_got  = 0;

    irk_init(1);
    irk_queue_init(&q_kill, q_kill_store, sizeof(uint8_t), 2);

    tw = irk_task_create(kill_waiter, 1, s1, sizeof(s1), "warter");
    to = irk_task_create(kill_other,  1, s2, sizeof(s2), "anderer");
    CHECK(tw && to, "Tasks konnten nicht angelegt werden");

    for (i = 0; i < 30; i++) work_1ms();
    CHECK(kill_waiter_ran == 1, "Warter hat sich nicht eingereiht");

    /* Jetzt loeschen, waehrend sie wartet. */
    irk_task_kill(tw);
    for (i = 0; i < 20; i++) work_1ms();

    /* Ein Element einstellen. Waere der Warter noch in der Schlange,
       ginge das Element an eine tote Task -- die andere Task bekaeme
       nichts. */
    irk_queue_send(&q_kill, &v);
    for (i = 0; i < 30; i++) work_1ms();

    printf("  Warter geloescht; die andere Task hat %d Element(e) erhalten\n",
           kill_other_got);

    CHECK(kill_waiter_ran == 1, "geloeschte Task lief weiter");
    CHECK(kill_other_got >= 1,
          "Element ging verloren -- geloeschte Task steckt noch in der Schlange");

    irk_deinit();
}


/*========================================================================*\
 *  12. Stack-Fruehwarnung
 *
 *  Auf dem Host benutzen die Fibers ihren eigenen Stack, nicht den
 *  uebergebenen Puffer -- echter Verbrauch laesst sich hier nicht messen.
 *  Geprueft wird deshalb die Logik: der Test "verbraucht" Stack, indem er
 *  die Einfaerbung am Boden des Puffers selbst ueberschreibt. Ob der
 *  Verbrauch auf echter Hardware richtig gemessen wird, zeigt
 *  RP2040_DemoSelftest.
\*========================================================================*/

static volatile int        alarm_anzahl = 0;
static volatile irk_task_t alarm_task   = 0;
static volatile size_t     alarm_frei   = 0;

static void alarm_rueckruf(irk_task_t tsk, size_t frei)
{
    alarm_anzahl++;
    alarm_task = tsk;
    alarm_frei = frei;
}

static void watch_task(void) { for (;;) work_1ms(); }

static void test_stack_watch(void)
{
    static uint8_t st[64000];
    irk_task_t t;
    int i;

    SECTION("Stack-Fruehwarnung -- meldet Unterschreitung genau einmal");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    alarm_anzahl = 0; alarm_task = 0; alarm_frei = 0;

    irk_init(1);
    t = irk_task_create(watch_task, 1, st, sizeof(st), "watch");
    CHECK(t != 0, "Task konnte nicht angelegt werden");

    irk_stack_watch(128, alarm_rueckruf);

    /* Unberuehrter Stack: keine Warnung */
    for (i = 0; i < 100; i++) work_1ms();
    CHECK(alarm_anzahl == 0, "Warnung ohne Anlass (%d)", alarm_anzahl);

    /* Verbrauch simulieren: Byte 100 ueber dem Boden beschreiben ->
       nur noch 100 Byte nie benutzt, Schwelle ist 128 */
    st[100] = 0x00;
    for (i = 0; i < 100; i++) work_1ms();

    printf("  Warnungen: %d, Task %d, gemeldeter Rest %lu Byte (soll 100)\n",
           alarm_anzahl, alarm_task, (unsigned long)alarm_frei);

    CHECK(alarm_anzahl == 1, "erwartet genau 1 Warnung, waren %d", alarm_anzahl);
    CHECK(alarm_task == t, "falsche Task gemeldet: %d statt %d", alarm_task, t);
    CHECK(alarm_frei == 100, "gemeldeter Rest %lu statt 100",
          (unsigned long)alarm_frei);

    /* Weiterlaufen: darf nicht erneut melden */
    for (i = 0; i < 200; i++) work_1ms();
    CHECK(alarm_anzahl == 1, "Warnung wiederholt (%d)", alarm_anzahl);

    /* Neu scharf schalten: meldet wieder */
    irk_stack_watch(128, alarm_rueckruf);
    for (i = 0; i < 100; i++) work_1ms();
    CHECK(alarm_anzahl == 2, "nach erneutem Scharfschalten %d statt 2 Warnungen",
          alarm_anzahl);

    /* Abschalten mit NULL: keine weitere Meldung */
    irk_stack_watch(128, NULL);
    for (i = 0; i < 100; i++) work_1ms();
    CHECK(alarm_anzahl == 2, "trotz Abschaltung gemeldet");

    irk_deinit();
}


/*========================================================================*\
 *  13. Mikrosekunden: irk_delay_us() und ein 500-Hz-Zyklus
 *
 *  Die Wecklatenz ist durch den Takt begrenzt, in dem die anderen Tasks
 *  abgeben (hier 100 us), plus eine gesetzte Mindest-Zeitscheibe.
\*========================================================================*/

static void work_us(uint32_t us)
{
    irk_test_clock_add_us(us);
    irk_yield();
}

static volatile int      dus_done;
static volatile uint64_t dus_woke_after;
static volatile int      cus_ticks, cus_stop;

static void delay_us_task(void)
{
    irk_time_t vorher = irk_now_us();
    irk_delay_us(2500);
    dus_woke_after = (uint64_t)(irk_time_t)(irk_now_us() - vorher);
    dus_done = 1;
    for (;;) work_us(100);
}

static void cyclic_us_task(void)
{
    while (!cus_stop) { cus_ticks++; irk_yield(); }
    for (;;) work_us(100);
}

static void test_microseconds(void)
{
    static char s1[64000], s2[64000];
    irk_task_t tc;
    int i;

    SECTION("Mikrosekunden -- irk_delay_us() und 500-Hz-Zyklus");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    dus_done = 0; dus_woke_after = 0; cus_ticks = 0; cus_stop = 0;

    irk_init(1);
    irk_task_create(delay_us_task, 1, s1, sizeof(s1), "us_delay");
    for (i = 0; i < 200 && !dus_done; i++) work_us(100);

    printf("  irk_delay_us(2500) wachte nach %lu us auf (Zeitscheibe %lu us)\n",
           (unsigned long)dus_woke_after, (unsigned long)IRK_MIN_TIMESLICE_US);
    CHECK(dus_done, "Task ist nie aufgewacht");
    CHECK(dus_woke_after >= 2500 &&
          dus_woke_after <= 2500 + IRK_MIN_TIMESLICE_US + 300,
          "Wecklatenz ausserhalb der Erwartung: %lu us",
          (unsigned long)dus_woke_after);

    tc = irk_task_create(cyclic_us_task, 1, s2, sizeof(s2), "us_zyklus");
    irk_task_set_cyclic_us(tc, 2000);

    /* Ueber die Uhr messen, nicht ueber Schleifendurchlaeufe: auch die
       aufgewachte Delay-Task stellt die Testuhr weiter. */
    {
        irk_time_t start = irk_now_us();
        while ((irk_time_t)(irk_now_us() - start) < 1000000u) work_us(100);
    }

    printf("  Zyklus 2000 us: %d Aktivierungen in 1 s (erwartet ~500)\n", cus_ticks);
    CHECK(cus_ticks >= 475 && cus_ticks <= 525,
          "erwartet ~500 Aktivierungen, gezaehlt %d", cus_ticks);

    cus_stop = 1;
    irk_deinit();
}


/*========================================================================*\
 *  14. Ueberlauf der Uhr -- ausdruecklich, unabhaengig von IRK_TEST_T0
 *
 *  Die Uhr startet 0,5 s vor dem Ueberlauf von irk_time_t -- je nach
 *  Uebersetzung dem 32- oder dem 64-Bit-Ueberlauf. Ueber den Wrap hinweg
 *  muessen Fair Share, eine Wartefrist und ein Zyklus unbeeindruckt
 *  weiterlaufen.
\*========================================================================*/

static volatile int      wr_stop, wr_sleep_done, wr_ticks;
static volatile long     wr_runs[3];
static volatile uint64_t wr_sleep_us;

static void wr_a(void) { while (!wr_stop) { wr_runs[1]++; work_1ms(); } for (;;) work_1ms(); }
static void wr_b(void) { while (!wr_stop) { wr_runs[2]++; work_1ms(); } for (;;) work_1ms(); }

static void wr_sleeper(void)
{
    irk_time_t vorher = irk_now_us();
    irk_delay(700);
    wr_sleep_us   = (uint64_t)(irk_time_t)(irk_now_us() - vorher);
    wr_sleep_done = 1;
    for (;;) work_1ms();
}

static void wr_zyklus(void) { for (;;) { wr_ticks++; irk_yield(); } }

static void test_clock_wrap(void)
{
    static char s1[64000], s2[64000], s3[64000], s4[64000];
    irk_task_t ta, tb, tz;
    irk_time_t start;

    SECTION("Ueberlauf der Uhr -- Fair Share, Frist und Zyklus ueber den Wrap");

    irk_test_clock_enable(1);
    irk_test_clock_set_us((uint64_t)IRK_TIME_MAX - 499999ULL);
    wr_stop = 0; wr_sleep_done = 0; wr_ticks = 0; wr_sleep_us = 0;
    wr_runs[1] = wr_runs[2] = 0;

    irk_init(1);
    ta = irk_task_create(wr_a,      1, s1, sizeof(s1), "wr_a");
    tb = irk_task_create(wr_b,      2, s2, sizeof(s2), "wr_b");
    irk_task_create(wr_sleeper,     1, s3, sizeof(s3), "wr_sleep");
    tz = irk_task_create(wr_zyklus, 1, s4, sizeof(s4), "wr_zyklus");
    irk_task_set_cyclic(tz, 10);

    start = irk_now_us();
    while ((irk_time_t)(irk_now_us() - start) < 2000000u) work_1ms();
    wr_stop = 1;

    printf("  Uhr jetzt bei %llu (uebergelaufen: %s)\n",
           (unsigned long long)irk_now_us(),
           (uint64_t)irk_now_us() < 1000000000ULL ? "ja" : "NEIN");
    printf("  Frist 700 ms ueber den Wrap: %lu us\n", (unsigned long)wr_sleep_us);
    printf("  Laufzeiten prio1=%lu prio2=%lu ms, Zyklus 10 ms: %d Takte in 2 s\n",
           (unsigned long)irk_task_runtime(ta), (unsigned long)irk_task_runtime(tb),
           wr_ticks);

    CHECK((uint64_t)irk_now_us() < 1000000000ULL,
          "Testaufbau: die Uhr ist gar nicht uebergelaufen");
    CHECK(wr_sleep_done && wr_sleep_us >= 700000u && wr_sleep_us <= 720000u,
          "Frist ueber den Wrap falsch: %lu us", (unsigned long)wr_sleep_us);
    CHECK(ratio_ok(irk_task_runtime(tb), irk_task_runtime(ta), 2.0, 10.0),
          "Fair Share ueber den Wrap gestoert: %.2f",
          (double)irk_task_runtime(tb) / (double)irk_task_runtime(ta));
    CHECK(wr_ticks >= 180 && wr_ticks <= 220,
          "Zyklus ueber den Wrap gestoert: %d Takte", wr_ticks);

    irk_deinit();
}


/*========================================================================*\
 *  15. Fair-Share-Konten ueber lange Zeit
 *
 *  Nach rund 30 s simulierter Zeit erreicht das kleinste Konto die
 *  Absenkschwelle, in 120 s also mehrfach. Gemessen wird das Verhaeltnis
 *  nur in der zweiten Minute -- ginge beim Absenken Rueckstand verloren,
 *  saehe man es hier.
\*========================================================================*/

static volatile int nz_stop;

static void nz_task(void) { while (!nz_stop) work_1ms(); for (;;) work_1ms(); }

static void test_normalize(void)
{
    static char s1[64000], s2[64000], s3[64000];
    irk_task_t a, b, c;
    irk_time_t start;
    uint32_t a1, b1, c1, da, db, dc;

    SECTION("Fair-Share-Konten -- gemeinsames Absenken erhaelt die Verhaeltnisse");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    nz_stop = 0;

    irk_init(1);
    a = irk_task_create(nz_task, 1, s1, sizeof(s1), "nz1");
    b = irk_task_create(nz_task, 2, s2, sizeof(s2), "nz2");
    c = irk_task_create(nz_task, 3, s3, sizeof(s3), "nz3");

    start = irk_now_us();
    while ((irk_time_t)(irk_now_us() - start) < 60000000u) work_1ms();
    a1 = irk_task_runtime(a); b1 = irk_task_runtime(b); c1 = irk_task_runtime(c);
    while ((irk_time_t)(irk_now_us() - start) < 120000000u) work_1ms();
    da = irk_task_runtime(a) - a1;
    db = irk_task_runtime(b) - b1;
    dc = irk_task_runtime(c) - c1;
    nz_stop = 1;

    printf("  zweite Minute: prio1=%lu prio2=%lu prio3=%lu ms\n",
           (unsigned long)da, (unsigned long)db, (unsigned long)dc);

    CHECK(ratio_ok(db, da, 2.0, 5.0), "prio2/prio1 = %.3f", (double)db / da);
    CHECK(ratio_ok(dc, da, 3.0, 5.0), "prio3/prio1 = %.3f", (double)dc / da);

    irk_deinit();
}


/*========================================================================*\
 *  16. Zyklische Task, die blockiert
 *
 *  Eine zyklische Task, die gerade auf etwas anderes wartet (Semaphor,
 *  Queue, Frist, Anhalten), darf bei ihrer Faelligkeit NICHT aktiviert
 *  werden. Sonst kehrt z.B. irk_sema_wait() zurueck, ohne dass die Task
 *  das Semaphor bekommen hat, und steht zugleich noch in dessen
 *  Warteschlange -- gegenseitiger Ausschluss verletzt, Schlange kaputt.
\*========================================================================*/

#define SEM_ZYK 1

static volatile int zb_halter_drin, zb_verletzt, zb_bekommen, zb_stop;
static volatile int zb_ticks;

static void zb_halter(void)
{
    int i;
    irk_sema_wait(SEM_ZYK);
    zb_halter_drin = 1;
    for (i = 0; i < 50; i++) work_1ms();      /* 50 ms festhalten */
    zb_halter_drin = 0;
    irk_sema_signal(SEM_ZYK);
    for (;;) work_1ms();
}

static void zb_zyklisch(void)
{
    for (;;) {
        irk_sema_wait(SEM_ZYK);
        if (zb_halter_drin) zb_verletzt++;    /* darf nie passieren */
        zb_bekommen++;
        irk_sema_signal(SEM_ZYK);
        irk_yield();                           /* bis zum naechsten Takt */
    }
}

static void zb_zaehler(void) { for (;;) { zb_ticks++; irk_yield(); } }

static void test_cyclic_blocked(void)
{
    static char s1[64000], s2[64000], s3[64000];
    irk_task_t th, tz, tc;
    int i, vorher;

    SECTION("Zyklische Task, die blockiert -- keine Aktivierung waehrend des Wartens");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    zb_halter_drin = zb_verletzt = zb_bekommen = zb_stop = zb_ticks = 0;

    irk_init(1);
    irk_sema_init(SEM_ZYK, 1);

    th = irk_task_create(zb_halter, 1, s1, sizeof(s1), "halter");
    for (i = 0; i < 5; i++) work_1ms();       /* Halter holt sich das Semaphor */
    CHECK(zb_halter_drin == 1, "Testaufbau: Halter hat das Semaphor nicht");

    tz = irk_task_create(zb_zyklisch, 1, s2, sizeof(s2), "zyklisch");
    irk_task_set_cyclic(tz, 10);

    for (i = 0; i < 300; i++) work_1ms();

    printf("  zyklische Task im Abschnitt, waehrend der Halter drin war: %d (soll 0)\n",
           zb_verletzt);
    printf("  zyklische Task hat das Semaphor danach %d-mal sauber bekommen\n",
           zb_bekommen);
    printf("  Semaphorzaehler am Ende: %d (soll 1)\n", irk_sema_count(SEM_ZYK));

    CHECK(zb_verletzt == 0, "gegenseitiger Ausschluss verletzt (%d-mal)", zb_verletzt);
    CHECK(zb_bekommen > 5,  "zyklische Task kam danach nicht mehr dran (%d)", zb_bekommen);
    CHECK(irk_sema_count(SEM_ZYK) == 1, "Semaphor inkonsistent: Zaehler %d",
          irk_sema_count(SEM_ZYK));
    (void)th;

    /* --- angehaltene zyklische Task darf nicht takten --- */
    tc = irk_task_create(zb_zaehler, 1, s3, sizeof(s3), "zaehler");
    irk_task_set_cyclic(tc, 10);
    for (i = 0; i < 100; i++) work_1ms();
    irk_task_suspend(tc);
    vorher = zb_ticks;
    for (i = 0; i < 200; i++) work_1ms();
    printf("  angehaltene zyklische Task: %d Takte waehrend des Anhaltens (soll 0)\n",
           zb_ticks - vorher);
    CHECK(zb_ticks == vorher, "angehaltene zyklische Task taktet weiter (%d)",
          zb_ticks - vorher);

    irk_task_resume(tc);
    vorher = zb_ticks;
    for (i = 0; i < 200; i++) work_1ms();
    CHECK(zb_ticks > vorher, "zyklische Task taktet nach dem Fortsetzen nicht");

    irk_deinit();
}


/*========================================================================*\
 *  17. Zyklische Task mit Startversatz
 *
 *  Periode 100 ms, erste Aktivierung 30 ms nach dem Aufruf. Die weiteren
 *  Aktivierungen muessen im festen Raster 30, 130, 230 ... ms liegen.
\*========================================================================*/

static volatile int      sv_n;
static uint32_t          sv_zeit[16];
static irk_time_t        sv_t0;

static void sv_task(void)
{
    for (;;) {
        if (sv_n < 16) sv_zeit[sv_n] = (uint32_t)(irk_time_t)(irk_now_us() - sv_t0);
        sv_n++;
        irk_yield();
    }
}

static void test_cyclic_offset(void)
{
    static char st[64000];
    irk_task_t t;
    int k, raster_ok = 1;

    SECTION("Zyklische Task mit Startversatz -- festes Raster ab Aufruf + Versatz");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    sv_n = 0;

    irk_init(1);
    t = irk_task_create(sv_task, 1, st, sizeof(st), "versatz");

    sv_t0 = irk_now_us();
    irk_task_set_cyclic_at(t, 100, 30);

    while ((irk_time_t)(irk_now_us() - sv_t0) < 1050000u) work_1ms();

    printf("  Aktivierungen in 1,05 s: %d (erwartet 11)\n", sv_n);
    printf("  erste Zeitpunkte (ms): ");
    for (k = 0; k < sv_n && k < 5; k++) printf("%lu ", (unsigned long)(sv_zeit[k] / 1000u));
    printf("\n");

    for (k = 0; k < sv_n && k < 11; k++) {
        uint32_t soll = 30000u + (uint32_t)k * 100000u;
        if (sv_zeit[k] < soll || sv_zeit[k] > soll + 2000u) raster_ok = 0;
    }
    CHECK(sv_n == 11, "erwartet 11 Aktivierungen, waren %d", sv_n);
    CHECK(sv_n > 0 && sv_zeit[0] >= 30000u && sv_zeit[0] <= 32000u,
          "erste Aktivierung nicht nach 30 ms: %lu us", (unsigned long)sv_zeit[0]);
    CHECK(raster_ok, "Aktivierungen liegen nicht im Raster 30 + k*100 ms");

    irk_deinit();
}


/*========================================================================*\
 *  18. Nockenschaltwerk: zwei zyklische Tasks, gleiche Periode, versetzt
 *
 *  "An" startet sofort, "Aus" 500 ms spaeter, Periode je 1 s. Ueber 10 s
 *  muss zwischen An und Aus in jedem Takt 500 ms liegen, ohne Drift.
\*========================================================================*/

static volatile int      nw_lampe, nw_an, nw_aus;
static volatile uint32_t nw_abstand_min, nw_abstand_max, nw_abstand_erst, nw_abstand_letzt;
static irk_time_t        nw_zeit_an;

static void nw_an_task(void)
{
    for (;;) {
        nw_lampe   = 1;
        nw_zeit_an = irk_now_us();
        nw_an++;
        irk_yield();
    }
}

static void nw_aus_task(void)
{
    for (;;) {
        uint32_t d = (uint32_t)(irk_time_t)(irk_now_us() - nw_zeit_an);
        nw_lampe = 0;
        if (nw_aus == 0) { nw_abstand_min = nw_abstand_max = nw_abstand_erst = d; }
        if (d < nw_abstand_min) nw_abstand_min = d;
        if (d > nw_abstand_max) nw_abstand_max = d;
        nw_abstand_letzt = d;
        nw_aus++;
        irk_yield();
    }
}

static void test_cam_switch(void)
{
    static char s1[64000], s2[64000];
    irk_task_t ta, tb;
    irk_time_t start;
    long an_proben = 0, proben = 0;

    SECTION("Nockenschaltwerk -- An/Aus phasenstarr ueber 10 s");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    nw_lampe = nw_an = nw_aus = 0;

    irk_init(1);
    ta = irk_task_create(nw_an_task,  1, s1, sizeof(s1), "nocke_an");
    tb = irk_task_create(nw_aus_task, 1, s2, sizeof(s2), "nocke_aus");

    irk_task_set_cyclic_at(ta, 1000,   0);
    irk_task_set_cyclic_at(tb, 1000, 500);

    start = irk_now_us();
    while ((irk_time_t)(irk_now_us() - start) < 10200000u) {
        work_1ms();
        proben++;
        if (nw_lampe) an_proben++;
    }

    printf("  An-Flanken %d, Aus-Flanken %d\n", nw_an, nw_aus);
    printf("  Abstand An->Aus: min %lu, max %lu, erster %lu, letzter %lu us\n",
           (unsigned long)nw_abstand_min, (unsigned long)nw_abstand_max,
           (unsigned long)nw_abstand_erst, (unsigned long)nw_abstand_letzt);
    printf("  Lampe an: %.1f %% der Zeit\n", 100.0 * an_proben / proben);

    CHECK(nw_an >= 10 && nw_aus >= 10, "zu wenige Flanken: an %d, aus %d", nw_an, nw_aus);
    CHECK(nw_abstand_min >= 498000u && nw_abstand_max <= 502000u,
          "Abstand An->Aus nicht 500 ms: %lu..%lu us",
          (unsigned long)nw_abstand_min, (unsigned long)nw_abstand_max);
    {
        long drift = (long)nw_abstand_letzt - (long)nw_abstand_erst;
        if (drift < 0) drift = -drift;
        CHECK(drift <= 2000, "Drift zwischen erstem und letztem Takt: %ld us", drift);
    }
    CHECK(an_proben * 100 >= proben * 47 && an_proben * 100 <= proben * 53,
          "Tastverhaeltnis nicht ~50 %%: %.1f %%", 100.0 * an_proben / proben);

    irk_deinit();
}


#if IRK_MIN_TIMESLICE_US == 0
/*========================================================================*\
 *  19. irk_yield() heisst "fertig" -- kurze Tasks kommen oft dran
 *
 *  Eine Fuellstandsabfrage (20 us Arbeit, Prioritaet 1) neben einer
 *  Dauerlast (Prioritaet 255). Ihr Anteil ist 1/256 = 0,39 %. Weil sie
 *  nach 20 us abgibt, kommt sie etwa alle 20 us / 0,39 % = 5,1 ms dran --
 *  ganz ohne Interrupt. Mit einer Mindest-Zeitscheibe von 1 ms waeren es
 *  255 ms; deshalb laeuft der Test nur ohne.
\*========================================================================*/

static volatile long       fs_laeufe;
static volatile irk_time_t fs_letzt, fs_abstand_max;

static void fuellstand_task(void)
{
    for (;;) {
        irk_time_t jetzt = irk_now_us();
        if (fs_laeufe > 0 && (irk_time_t)(jetzt - fs_letzt) > fs_abstand_max)
            fs_abstand_max = (irk_time_t)(jetzt - fs_letzt);
        fs_letzt = jetzt;
        fs_laeufe++;
        work_us(20);                     /* Fuellstand lesen, LED setzen */
    }
}

static void test_short_task(void)
{
    static char s1[64000];
    irk_task_t  tf;
    irk_time_t  start, lauf_us;

    SECTION("irk_yield() heisst fertig -- 20-us-Task bei 0,39 % Anteil");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    fs_laeufe = 0; fs_letzt = 0; fs_abstand_max = 0;

    irk_init(255);                       /* Haupttask = Dauerlast */
    tf = irk_task_create(fuellstand_task, 1, s1, sizeof(s1), "fuellstand");

    start = irk_now_us();
    while ((irk_time_t)(irk_now_us() - start) < 1000000u) work_us(1000);
    lauf_us = irk_task_runtime_us(tf);

    printf("  Abfragen in 1 s: %ld (erwartet ~195), groesster Abstand %lu us\n",
           fs_laeufe, (unsigned long)fs_abstand_max);
    printf("  Rechenzeit der Abfrage: %lu us = %.2f %% (Soll 0,39 %%)\n",
           (unsigned long)lauf_us, lauf_us / 10000.0);

    CHECK(fs_laeufe >= 150 && fs_laeufe <= 250,
          "Abfrage kam %ld-mal dran statt ~195", fs_laeufe);
    CHECK(fs_abstand_max <= 7000u,
          "groesster Abstand %lu us statt ~5100", (unsigned long)fs_abstand_max);
    CHECK(lauf_us >= 3000u && lauf_us <= 5000u,
          "Rechenzeit %lu us statt ~3900", (unsigned long)lauf_us);

    irk_deinit();
}


/*========================================================================*\
 *  20. Prioritaeten mit 16 Bit -- 1 : 9999 ergibt 0,01 %
 *
 *  Mit 8 Bit war 1 : 255 (0,39 %) das Aeusserste. Eine Task mit 10 us je
 *  Lauf und Prioritaet 1 neben einer Dauerlast mit 9999 bekommt 1/10000
 *  der Zeit: in 10 s genau 1 ms, also rund 100 Laeufe, einer alle 100 ms.
\*========================================================================*/

static volatile long pz_laeufe;

static void pz_task(void)
{
    for (;;) { pz_laeufe++; work_us(10); }
}

static void test_wide_prio(void)
{
    static char s1[64000];
    irk_task_t  tk;
    irk_time_t  start, lauf_us, haupt_us;

    SECTION("Prioritaet 16 Bit -- 1 : 9999 ergibt 0,01 %");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    pz_laeufe = 0;

    irk_init(9999);                      /* Haupttask = Dauerlast */
    tk = irk_task_create(pz_task, 1, s1, sizeof(s1), "winzig");

    CHECK(irk_task_get_prio(IRK_MAIN_TASK) == 9999,
          "Haupttask hat Prioritaet %u statt 9999", (unsigned)irk_task_get_prio(IRK_MAIN_TASK));
    irk_task_set_prio(tk, 65535);
    CHECK(irk_task_get_prio(tk) == 65535,
          "Prioritaet 65535 wurde zu %u", (unsigned)irk_task_get_prio(tk));
    irk_task_set_prio(tk, 1);

    start = irk_now_us();
    while ((irk_time_t)(irk_now_us() - start) < 10000000u) work_us(1000);
    lauf_us  = irk_task_runtime_us(tk);
    haupt_us = irk_task_runtime_us(IRK_MAIN_TASK);

    printf("  Laeufe in 10 s: %ld (erwartet ~100)\n", pz_laeufe);
    printf("  Rechenzeit: winzig %lu us, Dauerlast %lu us -> Anteil %.4f %% (Soll 0,0100 %%)\n",
           (unsigned long)lauf_us, (unsigned long)haupt_us,
           100.0 * (double)lauf_us / (double)(lauf_us + haupt_us));

    CHECK(pz_laeufe >= 80 && pz_laeufe <= 120, "%ld Laeufe statt ~100", pz_laeufe);
    CHECK(lauf_us >= 800u && lauf_us <= 1200u, "Rechenzeit %lu us statt ~1000",
          (unsigned long)lauf_us);

    irk_deinit();
}


/*========================================================================*\
 *  21. Aufwachen nach irk_delay() ordnet das Konto ein
 *
 *  30 s lang schlafen eine Task mit Prioritaet 1 und zwei Rechner
 *  (256, 768) in irk_delay(10), nur main rechnet. Jedes Aufwachen kostet
 *  40 us -- bei Prioritaet 1 aber das 256-fache aufs Konto. Danach rechnen
 *  die Rechner, und die Prio-1-Task soll blitzen. Wer aufwacht, muss dabei
 *  auf den aktuellen Stand gehoben werden: sonst kaemen die Rechner mit
 *  ihrem alten, niedrigen Konto zurueck und sperrten die anderen
 *  minutenlang aus.
\*========================================================================*/

static volatile int  fb_phase;
static volatile long fb_blitze;

static void fb_blitz(void)                          /* Prioritaet 1 */
{
    for (;;) {
        if (fb_phase < 2) { irk_test_clock_add_us(40); irk_delay(10); continue; }
        irk_test_clock_add_us(40);                  /* an  */
        irk_delay(2);
        irk_test_clock_add_us(40);                  /* aus */
        fb_blitze++;
        irk_yield();
    }
}

static void fb_rechner(void)                        /* Prioritaet 256 / 768 */
{
    for (;;) {
        if (fb_phase < 2) { irk_test_clock_add_us(40); irk_delay(10); continue; }
        work_us(20);
    }
}

static void test_wake_after_delay(void)
{
    static char s1[64000], s2[64000], s3[64000];
    irk_task_t  tb, r1, r3;
    irk_time_t  t0, dauer, z1, z3, zb, d1, d3, db;

    SECTION("Aufwachen nach irk_delay() -- niemand wird ausgesperrt");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    fb_phase = 0;
    fb_blitze = 0;

    irk_init(256);
    tb = irk_task_create(fb_blitz,   1,   s1, sizeof(s1), "blitz");
    r1 = irk_task_create(fb_rechner, 256, s2, sizeof(s2), "r1");
    r3 = irk_task_create(fb_rechner, 768, s3, sizeof(s3), "r3");

    fb_phase = 1;                                   /* 30 s: nur main rechnet */
    t0 = irk_now_us();
    while ((irk_time_t)(irk_now_us() - t0) < 30000000u) work_us(1000);

    fb_phase = 2;                                   /* 10 s: Rechner + Blitz  */
    z1 = irk_task_runtime_us(r1);
    z3 = irk_task_runtime_us(r3);
    zb = irk_task_runtime_us(tb);
    t0 = irk_now_us();
    /* main stellt die Uhr selbst vor: schliefe es, und steckten gerade alle
       anderen in irk_delay(), stuende die steuerbare Uhr still. */
    while ((irk_time_t)(irk_now_us() - t0) < 10000000u) work_us(10);
    dauer = irk_now_us() - t0;
    d1 = irk_task_runtime_us(r1) - z1;
    d3 = irk_task_runtime_us(r3) - z3;
    db = irk_task_runtime_us(tb) - zb;

    printf("  Phase 2 dauerte %lu ms (Soll 10000; laenger heisst: main ausgesperrt)\n",
           (unsigned long)(dauer / 1000u));
    printf("  Blitze %ld (erwartet ~100), Rechenzeit blitz %lu us, r1 %lu ms, r3 %lu ms\n",
           fb_blitze, (unsigned long)db, (unsigned long)(d1 / 1000u), (unsigned long)(d3 / 1000u));

    CHECK(fb_blitze >= 60 && fb_blitze <= 140,
          "Prio-1-Task: %ld Blitze statt ~100", fb_blitze);
    CHECK(dauer < 10100000u,
          "Phase 2 dauerte %lu ms statt 10000 -- main war ausgesperrt",
          (unsigned long)(dauer / 1000u));
    CHECK(d1 > 0 && d3 * 100u / d1 >= 280 && d3 * 100u / d1 <= 320,
          "Rechner 768 : 256 = %lu (x100) statt 300",
          (unsigned long)(d1 ? d3 * 100u / d1 : 0u));

    irk_deinit();
}


/*========================================================================*\
 *  22. Aufwachen auf einem leeren Kern
 *
 *  10 s ist der Kern leer -- alle Tasks schlafen in irk_delay(10), auch
 *  main. Die Task mit Prioritaet 1 zahlt dabei fuer jedes Aufwachen das
 *  256-fache. Danach rechnen die Rechner, und sie soll blitzen. Bezug
 *  beim Aufwachen ist deshalb mindestens das Kern-Minimum: ohne einen
 *  solchen Bezug -- niemand sonst ist lauffaehig -- behielten die Rechner
 *  ihr niedriges Konto und sperrten die Prio-1-Task aus.
 *
 *  Der Host-Port stellt die steuerbare Uhr im Leerlauf weiter; sonst
 *  stuende sie hier still.
\*========================================================================*/

static volatile int  lk_phase;
static volatile long lk_blitze;

static void lk_blitz(void)                          /* Prioritaet 1 */
{
    for (;;) {
        if (lk_phase != 2) { irk_test_clock_add_us(40); irk_delay(10); continue; }
        irk_test_clock_add_us(40);                  /* an  */
        irk_delay(2);
        irk_test_clock_add_us(40);                  /* aus */
        lk_blitze++;
        irk_yield();
    }
}

static void lk_rechner(void)                        /* Prioritaet 256 / 768 */
{
    for (;;) {
        if (lk_phase != 2) { irk_test_clock_add_us(40); irk_delay(10); continue; }
        work_us(20);
    }
}

/* Wie die Schreiber im Sketch, die in diesem Block ruhen: sie wachen nur
   kurz auf. Ohne sie ist beim Aufwachen der Rechner oft zufaellig eine
   andere Task lauffaehig, und der Fehler zeigt sich nicht. */
static void lk_schlaefer(void)                      /* Prioritaet 256 */
{
    for (;;) { irk_test_clock_add_us(40); irk_delay(10); }
}

static void test_wake_on_idle_core(void)
{
    static char s1[64000], s2[64000], s3[64000], s4[64000], s5[64000];
    irk_task_t  tb, r1, r3;
    irk_time_t  z1, z3, zb, d1, d3, db;
    int         s;

    SECTION("Aufwachen auf leerem Kern -- Bezug auch ohne andere lauffaehige Task");

    irk_test_clock_enable(1);
    irk_test_clock_set_us(IRK_TEST_T0);
    lk_phase  = 0;
    lk_blitze = 0;

    irk_init(256);
    tb = irk_task_create(lk_blitz,   1,   s1, sizeof(s1), "blitz");
    r1 = irk_task_create(lk_rechner, 256, s2, sizeof(s2), "r1");
    r3 = irk_task_create(lk_rechner, 768, s3, sizeof(s3), "r3");
    irk_task_create(lk_schlaefer, 256, s4, sizeof(s4), "s1");
    irk_task_create(lk_schlaefer, 256, s5, sizeof(s5), "s2");

    lk_phase = 1;
    irk_delay(10000);                               /* 10 s: Kern leer */

    lk_phase = 2;
    z1 = irk_task_runtime_us(r1);
    z3 = irk_task_runtime_us(r3);
    zb = irk_task_runtime_us(tb);
    for (s = 0; s < 10; s++) irk_delay(1000);       /* 10 s: Rechner + Blitz */
    d1 = irk_task_runtime_us(r1) - z1;
    d3 = irk_task_runtime_us(r3) - z3;
    db = irk_task_runtime_us(tb) - zb;

    printf("  Blitze in 10 s: %ld (erwartet ~120), Rechenzeit blitz %lu us, r1 %lu ms, r3 %lu ms\n",
           lk_blitze, (unsigned long)db, (unsigned long)(d1 / 1000u), (unsigned long)(d3 / 1000u));

    CHECK(lk_blitze >= 80 && lk_blitze <= 160,
          "Prio-1-Task: %ld Blitze statt ~120 -- Vorsprung auf leerem Kern", lk_blitze);
    CHECK(d1 > 0 && d3 * 100u / d1 >= 280 && d3 * 100u / d1 <= 320,
          "Rechner 768 : 256 = %lu (x100) statt 300",
          (unsigned long)(d1 ? d3 * 100u / d1 : 0u));

    irk_deinit();
}
#endif


/*========================================================================*\
 *  main
\*========================================================================*/

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* Ausgabe sofort sichtbar */
    printf("========================================================\n");
    printf(" IRKernel -- Testlauf der Scheduling-Politik\n");
    printf("========================================================\n");

    test_fair_share();
    test_cyclic();
    test_cyclic_to_normal();
    test_prio_zero();
    test_semaphore();
    test_delay();
    test_self_kill();
    test_slot_reuse();
    test_queue();
    test_queue_nonblocking();
    test_kill_while_waiting();
    test_stack_watch();
    test_microseconds();
    test_clock_wrap();
    test_normalize();
    test_cyclic_blocked();
    test_cyclic_offset();
    test_cam_switch();
#if IRK_MIN_TIMESLICE_US == 0
    test_short_task();
    test_wide_prio();
    test_wake_after_delay();
    test_wake_on_idle_core();
#endif

    printf("\n========================================================\n");
    printf(" %d Pruefungen, %d Fehler\n", tests_run, tests_failed);
    printf("========================================================\n");

    return tests_failed ? 1 : 0;
}
