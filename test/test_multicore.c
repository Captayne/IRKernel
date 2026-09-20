/*========================================================================*\
 *
 *  test_multicore.c  --  IRKernel mit zwei echt parallel laufenden Kernen
 *
 *  Auf dem PC ist jeder "Kern" ein Betriebssystem-Thread mit eigenen
 *  Fibers. Beide laufen wirklich gleichzeitig -- Wettlaeufe auf den
 *  gemeinsamen Kerneldaten wuerden hier also tatsaechlich auftreten.
 *
 *  Gemessen wird mit der echten Uhr, die Toleranzen sind entsprechend
 *  grosszuegig.
 *
 *  Bauen:  gcc -std=c99 -pthread -DIRK_MAX_CORES=2 test/test_multicore.c
 *              src/IRKernel.c src/port/irk_port_host.c
 *
\*========================================================================*/

#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "../src/IRKernel.h"

#if IRK_MAX_CORES < 2
#  error "test_multicore.c braucht -DIRK_MAX_CORES=2"
#endif

extern void irk_test_set_core(int c);
extern void irk_test_set_isr(int on);
extern volatile int irk_test_error_count;
extern volatile int irk_test_last_error;

#define STK 16384                       /* Host braucht mindestens 4096 */


/*========================================================================*\
 *  Testgeruest
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

/* Kern 1 = eigener Thread, der ein Szenario ausfuehrt */
typedef void (*szenario_fn)(void);
static szenario_fn kern1_szenario;

static void *kern1_thread(void *unbenutzt)
{
    (void)unbenutzt;
    irk_test_set_core(1);
    kern1_szenario();
    return NULL;
}

static pthread_t starte_kern1(szenario_fn f)
{
    pthread_t th;
    kern1_szenario = f;
    pthread_create(&th, NULL, kern1_thread, NULL);
    return th;
}

/* Kern 0 muss waehrend eines Szenarios selbst weiter abgeben, sonst laufen
   seine Tasks nicht -- also nie blockierend auf Kern 1 warten. */
static void kern0_warte_auf(volatile int *fertig, unsigned max_ms)
{
    irk_time_t start = irk_now_us();
    while (!*fertig && (irk_time_t)(irk_now_us() - start) < (irk_time_t)max_ms * 1000u) {
        irk_delay(2);
    }
}

static void schlaf_task(void) { for (;;) irk_delay(10); }


/*========================================================================*\
 *  1. Autostart und feste Handles der Haupttasks
\*========================================================================*/

static volatile int a_self1, a_core1, a_task1, a_taskcore1, a_fertig;

static void a_kern1(void)
{
    a_self1     = irk_task_self();          /* startet Kern 1 automatisch */
    a_core1     = irk_core_id();
    a_task1     = irk_task_create(schlaf_task, 1, NULL, STK, "a_k1");
    a_taskcore1 = irk_task_core((irk_task_t)a_task1);
    irk_delay(20);
    a_fertig = 1;
    irk_deinit();
}

static void test_autostart(void)
{
    pthread_t  th;
    irk_task_t self0;

    SECTION("Autostart -- beide Kerne ohne irk_init(), feste Haupttask-Handles");

    a_fertig = 0;
    self0 = irk_task_self();                /* startet Kern 0 automatisch */
    th = starte_kern1(a_kern1);
    kern0_warte_auf(&a_fertig, 3000);
    pthread_join(th, NULL);

    printf("  Kern 0: Haupttask %d   Kern 1: Haupttask %d, Kernnummer %d\n",
           self0, a_self1, a_core1);
    printf("  Task auf Kern 1 angelegt: Handle %d, laeuft auf Kern %d\n",
           a_task1, a_taskcore1);

    CHECK(self0 == IRK_MAIN_TASK_OF(0), "Haupttask Kern 0 hat Handle %d statt 1", self0);
    CHECK(a_self1 == IRK_MAIN_TASK_OF(1), "Haupttask Kern 1 hat Handle %d statt 2", a_self1);
    CHECK(a_core1 == 1, "Kern 1 meldet Kernnummer %d", a_core1);
    CHECK(a_task1 > IRK_MAX_CORES, "Task auf Kern 1 hat ungueltiges Handle %d", a_task1);
    CHECK(a_taskcore1 == 1, "Task von Kern 1 laeuft angeblich auf Kern %d", a_taskcore1);
    CHECK(irk_task_core(IRK_MAIN_TASK) == 0, "Haupttask Kern 0 laeuft nicht auf Kern 0");

    irk_deinit();
}


/*========================================================================*\
 *  2. Gemeinsames Semaphor ueber beide Kerne
\*========================================================================*/

#define SEM_GEM 0

static volatile int s_stop, s_drin, s_max, s_ein[2], s_fertig;

static void s_arbeiter(void)
{
    int k = irk_core_id();
    while (!s_stop) {
        int i;
        irk_sema_wait(SEM_GEM);
        s_drin++;
        if (s_drin > s_max) s_max = s_drin;
        s_ein[k]++;
        for (i = 0; i < 3; i++) irk_delay_us(150);   /* im Abschnitt abgeben */
        s_drin--;
        irk_sema_signal(SEM_GEM);
        irk_delay_us(100);
    }
    for (;;) irk_delay(10);
}

static void s_kern1(void)
{
    irk_task_create(s_arbeiter, 1, NULL, STK, "s_k1a");
    irk_task_create(s_arbeiter, 1, NULL, STK, "s_k1b");
    while (!s_stop) irk_delay(5);
    irk_delay(50);
    s_fertig = 1;
    irk_deinit();
}

static void test_shared_sema(void)
{
    pthread_t th;

    SECTION("Gemeinsames Semaphor -- je zwei Tasks auf beiden Kernen");

    s_stop = s_drin = s_max = s_fertig = 0;
    s_ein[0] = s_ein[1] = 0;

    irk_sema_init(SEM_GEM, 1);
    irk_task_create(s_arbeiter, 1, NULL, STK, "s_k0a");
    irk_task_create(s_arbeiter, 1, NULL, STK, "s_k0b");

    th = starte_kern1(s_kern1);
    irk_delay(1000);
    s_stop = 1;
    kern0_warte_auf(&s_fertig, 3000);
    pthread_join(th, NULL);
    irk_delay(50);

    printf("  Eintritte Kern 0: %d, Kern 1: %d, max. gleichzeitig: %d, Zaehler: %d\n",
           s_ein[0], s_ein[1], s_max, irk_sema_count(SEM_GEM));

    CHECK(s_max == 1, "gegenseitiger Ausschluss verletzt: %d gleichzeitig", s_max);
    CHECK(s_ein[0] > 20 && s_ein[1] > 20,
          "ein Kern kam kaum dran: %d / %d", s_ein[0], s_ein[1]);
    CHECK(irk_sema_count(SEM_GEM) == 1, "Semaphor inkonsistent: Zaehler %d",
          irk_sema_count(SEM_GEM));

    irk_deinit();
}


/*========================================================================*\
 *  3. Queue von Kern 0 nach Kern 1
\*========================================================================*/

#define XQ_N 2000

static irk_queue_t       xq;
static uint32_t          xq_speicher[4];
static volatile int      xq_empf, xq_folge_ok, xq_voll, xq_fertig, xq_stop;

static void xq_erzeuger(void)
{
    uint32_t n;
    for (n = 0; n < XQ_N && !xq_stop; n++) {
        if (irk_queue_count(&xq) >= 4) xq_voll = 1;
        irk_queue_send(&xq, &n);
    }
    for (;;) irk_delay(10);
}

static void xq_verbraucher(void)
{
    uint32_t erwartet = 0, wert;
    while (xq_empf < XQ_N && !xq_stop) {
        irk_queue_recv(&xq, &wert);
        if (wert != erwartet) xq_folge_ok = 0;
        erwartet = wert + 1u;
        xq_empf++;
        irk_delay_us(100);                       /* langsamer als der Erzeuger */
    }
    xq_fertig = 1;
    for (;;) irk_delay(10);
}

static volatile int xq_k1_fertig;

static void xq_kern1(void)
{
    irk_task_create(xq_verbraucher, 1, NULL, STK, "xq_empf");
    while (!xq_fertig && !xq_stop) irk_delay(5);
    irk_delay(20);
    xq_k1_fertig = 1;
    irk_deinit();
}

static void test_queue_cross(void)
{
    pthread_t th;

    SECTION("Queue von Kern 0 nach Kern 1 -- FIFO und Blockieren bei voll");

    xq_empf = xq_voll = xq_fertig = xq_stop = xq_k1_fertig = 0;
    xq_folge_ok = 1;

    irk_queue_init(&xq, xq_speicher, sizeof(uint32_t), 4);
    irk_task_create(xq_erzeuger, 1, NULL, STK, "xq_send");

    th = starte_kern1(xq_kern1);
    kern0_warte_auf(&xq_fertig, 8000);
    xq_stop = 1;
    kern0_warte_auf(&xq_k1_fertig, 3000);
    pthread_join(th, NULL);

    printf("  empfangen %d von %d, Reihenfolge %s, Queue lief voll: %s\n",
           xq_empf, XQ_N, xq_folge_ok ? "korrekt" : "VERLETZT", xq_voll ? "ja" : "nein");

    CHECK(xq_empf == XQ_N, "nur %d von %d Werten angekommen", xq_empf, XQ_N);
    CHECK(xq_folge_ok, "FIFO-Reihenfolge ueber die Kerngrenze verletzt");
    CHECK(xq_voll, "Queue lief nie voll -- Blockieren ungeprueft");

    irk_deinit();
}


/*========================================================================*\
 *  4. Wecken ueber die Kerngrenze -- ohne falschen Deadlock
 *
 *  Die Haupttask von Kern 1 blockiert selbst auf einem Semaphor, und Kern 1
 *  hat sonst nichts zu tun. Im Einkernbetrieb waere das ein Deadlock; hier
 *  kann Kern 0 das Semaphor freigeben, also darf keiner gemeldet werden.
\*========================================================================*/

#define SEM_WECK 1

static volatile uint64_t w_geweckt_us, w_signal_us;
static volatile int      w_fertig;

static void w_kern1(void)
{
    irk_sema_wait(SEM_WECK);
    w_geweckt_us = irk_now_us();
    w_fertig = 1;
    irk_deinit();
}

static void test_wake_cross(void)
{
    pthread_t th;
    int       fehler_vorher;
    long      latenz_us;

    SECTION("Wecken ueber die Kerngrenze -- und kein falscher Deadlock");

    w_fertig = 0;
    irk_sema_init(SEM_WECK, 0);
    fehler_vorher = irk_test_error_count;

    th = starte_kern1(w_kern1);
    irk_delay(300);                              /* Kern 1 wartet so lange */
    w_signal_us = irk_now_us();
    irk_sema_signal(SEM_WECK);
    kern0_warte_auf(&w_fertig, 3000);
    pthread_join(th, NULL);

    latenz_us = (long)(w_geweckt_us - w_signal_us);
    printf("  Kern 1 geweckt: %s, Latenz %ld us, gemeldete Kernelfehler: %d\n",
           w_fertig ? "ja" : "NEIN", latenz_us, irk_test_error_count - fehler_vorher);

    CHECK(w_fertig, "Kern 1 wurde nie geweckt");
    CHECK(latenz_us >= 0 && latenz_us < 50000, "Weckverzug %ld us", latenz_us);
    CHECK(irk_test_error_count == fehler_vorher,
          "Kernelfehler gemeldet (zuletzt Code %d) -- falscher Deadlock?",
          irk_test_last_error);

    irk_deinit();
}


/*========================================================================*\
 *  5. Task des anderen Kerns anhalten, fortsetzen, loeschen
\*========================================================================*/

static volatile long       c_zaehler;
static volatile irk_task_t c_task;
static volatile long       c_a, c_b, c_c, c_d, c_e;
static volatile int        c_tot, c_fertig;

static void c_zaehl(void) { for (;;) { c_zaehler++; irk_yield(); } }

static void c_kern1(void)
{
    while (c_zaehler < 1000) irk_delay(1);

    irk_task_suspend(c_task);
    irk_delay(20);                /* wirkt beim naechsten Abgeben der Task */
    c_a = c_zaehler;
    irk_delay(100);
    c_b = c_zaehler;              /* angehalten: darf sich nicht bewegen */

    irk_task_resume(c_task);
    irk_delay(100);
    c_c = c_zaehler;              /* laeuft wieder */

    irk_task_kill(c_task);
    irk_delay(50);
    c_tot = (irk_task_status(c_task) & IRK_KILLED) ? 1 : 0;
    c_d = c_zaehler;
    irk_delay(100);
    c_e = c_zaehler;              /* geloescht: steht */

    c_fertig = 1;
    irk_deinit();
}

static void test_control_cross(void)
{
    pthread_t  th;
    irk_task_t neu;

    SECTION("Kern 1 haelt eine Task von Kern 0 an, setzt sie fort, loescht sie");

    c_zaehler = 0;
    c_fertig  = 0;
    c_task = irk_task_create(c_zaehl, 1, NULL, STK, "zaehler");

    th = starte_kern1(c_kern1);
    kern0_warte_auf(&c_fertig, 5000);
    pthread_join(th, NULL);

    neu = irk_task_create(schlaf_task, 1, NULL, STK, "nachfolger");

    printf("  angehalten: %ld -> %ld, fortgesetzt: %ld, geloescht: %ld -> %ld\n",
           c_a, c_b, c_c, c_d, c_e);
    printf("  Slot der geloeschten Task (%d) neu vergeben an: %d\n", c_task, neu);

    CHECK(c_fertig, "Szenario auf Kern 1 wurde nicht fertig");
    CHECK(c_b == c_a, "angehaltene Task zaehlt weiter: %ld -> %ld", c_a, c_b);
    CHECK(c_c > c_b, "fortgesetzte Task zaehlt nicht");
    CHECK(c_tot, "Task ist nicht als geloescht eingetragen");
    CHECK(c_e == c_d, "geloeschte Task zaehlt weiter: %ld -> %ld", c_d, c_e);
    CHECK(neu == c_task, "Slot wurde nicht wiederverwendet (%d statt %d)", neu, c_task);

    irk_deinit();
}


/*========================================================================*\
 *  6. Wettlauf: beide Kerne legen gleichzeitig kurzlebige Tasks an
 *
 *  Jede Task kehrt sofort zurueck und loescht sich damit selbst. Ihr Slot
 *  ist erst frei, wenn ihr Kern von ihr weggeschaltet hat; der andere Kern
 *  darf ihn bis dahin nicht vergeben. Genau das trifft die Zombie-Logik.
\*========================================================================*/

#define R_N 400

static volatile int r_liefen[2], r_fehl[2], r_fertig;

static void r_kurz(void) { r_liefen[irk_core_id()]++; }

static void r_schleife(int k)
{
    int i;
    for (i = 0; i < R_N; i++) {
        if (irk_task_create(r_kurz, 1, NULL, STK, "kurz") == IRK_NO_TASK) r_fehl[k]++;
        irk_delay(1);
    }
    irk_delay(20);
}

static void r_kern1(void)
{
    r_schleife(1);
    r_fertig = 1;
    irk_deinit();
}

static void test_race(void)
{
    pthread_t th;
    int       fehler_vorher;

    SECTION("Wettlauf -- je 400 kurzlebige Tasks auf beiden Kernen gleichzeitig");

    r_liefen[0] = r_liefen[1] = r_fehl[0] = r_fehl[1] = r_fertig = 0;
    fehler_vorher = irk_test_error_count;

    irk_task_self();                              /* Kern 0 starten */
    th = starte_kern1(r_kern1);
    r_schleife(0);
    kern0_warte_auf(&r_fertig, 10000);
    pthread_join(th, NULL);

    printf("  gelaufen: Kern 0 %d, Kern 1 %d (je %d); Anlegen gescheitert: %d / %d\n",
           r_liefen[0], r_liefen[1], R_N, r_fehl[0], r_fehl[1]);
    printf("  hoechster belegter Slot: %d von %d\n", irk_task_count(), IRK_MAX_TASKS);

    CHECK(r_liefen[0] == R_N && r_liefen[1] == R_N,
          "nicht alle Tasks gelaufen: %d / %d", r_liefen[0], r_liefen[1]);
    CHECK(r_fehl[0] == 0 && r_fehl[1] == 0,
          "Anlegen gescheitert: %d / %d", r_fehl[0], r_fehl[1]);
    CHECK(irk_test_error_count == fehler_vorher,
          "Kernelfehler gemeldet (zuletzt Code %d)", irk_test_last_error);

    irk_deinit();
}


/*========================================================================*\
 *  7. Aufruf aus einem Interrupt wird abgelehnt
\*========================================================================*/

static void test_isr(void)
{
    int        r;
    irk_time_t z;

    SECTION("Aufruf aus einem Interrupt wird abgelehnt");

    irk_sema_init(SEM_GEM, 1);

    irk_test_set_isr(1);
    r = irk_sema_signal(SEM_GEM);
    z = irk_now_us();                             /* ausdruecklich erlaubt */
    irk_test_set_isr(0);

    printf("  irk_sema_signal() aus Interrupt: Rueckgabe %d, Fehlercode %d\n",
           r, irk_test_last_error);

    CHECK(r == -1, "Aufruf aus Interrupt wurde ausgefuehrt");
    CHECK(irk_test_last_error == IRK_ERR_IN_ISR, "falscher Fehlercode %d",
          irk_test_last_error);
    CHECK(z != 0, "irk_now_us() lieferte aus dem Interrupt 0");
    CHECK(irk_sema_count(SEM_GEM) == 1, "Semaphor wurde trotz Ablehnung veraendert");

    irk_deinit();
}


/*========================================================================*\
 *  8. Aufruf von einem unbekannten Kern wird abgelehnt
\*========================================================================*/

static volatile int u_self, u_fehler;

static void *u_thread(void *unbenutzt)
{
    (void)unbenutzt;
    irk_test_set_core(5);
    u_self   = irk_task_self();
    u_fehler = irk_test_last_error;
    return NULL;
}

static void test_wrong_core(void)
{
    pthread_t th;

    SECTION("Aufruf von einem unbekannten Kern wird abgelehnt");

    pthread_create(&th, NULL, u_thread, NULL);
    pthread_join(th, NULL);

    printf("  irk_task_self() auf Kern 5: %d, Fehlercode %d\n", u_self, u_fehler);

    CHECK(u_self == IRK_NO_TASK, "Kern 5 bekam Handle %d", u_self);
    CHECK(u_fehler == IRK_ERR_WRONG_CORE, "falscher Fehlercode %d", u_fehler);
}


int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("========================================================\n");
    printf(" IRKernel -- zwei Kerne echt parallel\n");
    printf("========================================================\n");

    test_autostart();
    test_shared_sema();
    test_queue_cross();
    test_wake_cross();
    test_control_cross();
    test_race();
    test_isr();
    test_wrong_core();

    printf("\n========================================================\n");
    printf(" %d Pruefungen, %d Fehler\n", tests_run, tests_failed);
    printf("========================================================\n");
    return tests_failed ? 1 : 0;
}
