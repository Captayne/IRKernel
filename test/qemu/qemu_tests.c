/*========================================================================*\
 *
 *  qemu_tests.c  --  Gemeinsamer Testkoerper fuer die QEMU-Laeufe
 *
 *  Wird von avr_main.c und arm_main.c eingebunden. Erwartet von dort:
 *
 *      void put(const char *s);      Textausgabe
 *      volatile uint32_t sw_ms;      Software-Uhr (ersetzt millis())
 *
 *  Warum eine Software-Uhr statt der echten Zeit: QEMU emuliert die
 *  Timer der Zielchips nur teilweise, und der Arduino-Kern ist hier
 *  ohnehin nicht dabei. Die Uhr wird von den Tasks selbst gestellt --
 *  jede "Arbeitseinheit" ist genau eine Millisekunde. Damit haengt der
 *  Test an nichts als am Kontextwechsel, und genau der soll geprueft
 *  werden.
 *
\*========================================================================*/

#include "../../src/IRKernel.h"
#include "../../src/irk_port.h"

/* --- vom jeweiligen main.c bereitzustellen --- */
static void put(const char *s);
extern volatile uint32_t sw_ms;


/*------------------------------------------------------------------------*\
 *  Kleine Ausgabehelfer  --  kein printf, das waere auf AVR zu teuer
\*------------------------------------------------------------------------*/

static void put_u32(uint32_t v)
{
    char b[12];
    int  i = 11;
    b[i--] = 0;
    if (v == 0) b[i--] = '0';
    while (v) { b[i--] = (char)('0' + (v % 10u)); v /= 10u; }
    put(&b[i + 1]);
}

static uint16_t geprueft = 0;
static uint16_t fehler   = 0;

static void pruefe(const char *name, int ok)
{
    geprueft++;
    put(ok ? "  ok    " : "  FEHLER ");
    put(name);
    put("\r\n");
    if (!ok) fehler++;
}

static void pruefe_zahl(const char *name, int ok, uint32_t wert)
{
    geprueft++;
    put(ok ? "  ok    " : "  FEHLER ");
    put(name);
    put(" = ");
    put_u32(wert);
    put("\r\n");
    if (!ok) fehler++;
}

/* Eine Millisekunde "Arbeit", danach die CPU abgeben. */
static void arbeite(void)
{
    sw_ms++;
    irk_yield();
}

/* Begrenzte Wartschleife -- ein kaputter Scheduler ergibt einen
   Fehlschlag, keinen Haenger. */
static int warte_bis(volatile uint8_t *flagge, uint8_t wert, uint16_t max)
{
    uint16_t i;
    for (i = 0; i < max; i++) {
        if (*flagge == wert) return 1;
        arbeite();
    }
    return 0;
}

static void alle_killen(void)
{
    uint8_t t;
    for (t = 2; t <= irk_task_count(); t++) {
        if (!(irk_task_status(t) & IRK_KILLED)) irk_task_kill(t);
    }
    arbeite();
}


/*------------------------------------------------------------------------*\
 *  Stacks  --  auf AVR knapp bemessen, dort zaehlt jedes Byte
\*------------------------------------------------------------------------*/

#if defined(__AVR__)
  #define STK 176
#else
  #define STK 512
#endif

static uint8_t stack_a[STK];
static uint8_t stack_b[STK];
static uint8_t stack_c[STK];


/*========================================================================*\
 *  1  Findet ueberhaupt ein Kontextwechsel statt?
\*========================================================================*/

static volatile uint8_t t1_lief = 0;

static void t1(void)
{
    for (;;) { t1_lief = 1; arbeite(); }
}

static void test_wechsel(void)
{
    t1_lief = 0;
    irk_task_create(t1, 1, stack_a, STK, "t1");
    pruefe("Kontextwechsel findet statt", warte_bis(&t1_lief, 1, 200));
    alle_killen();
}


/*========================================================================*\
 *  2  Ueberleben die callee-saved Register den Wechsel?
 *
 *  Die entscheidende Pruefung. Die acht Werte sind ueber den
 *  irk_yield()-Aufruf hinweg lebendig, der Compiler muss sie deshalb in
 *  callee-saved Register legen -- auf AVR r2..r17, auf ARM r4..r11.
 *  Genau die muss irk_ctx_switch() retten.
\*========================================================================*/

static volatile uint8_t  t2_status = 0;   /* 0=laeuft 1=ok 2=Fehler */
static volatile uint32_t t2_schlecht = 0;

static void t2(void)
{
    volatile uint32_t saat = 0x9E37u;      /* volatile: nicht vorausrechenbar */

    uint32_t a = saat *  3u;
    uint32_t b = saat *  5u;
    uint32_t c = saat *  7u;
    uint32_t d = saat * 11u;
    uint32_t e = saat * 13u;
    uint32_t f = saat * 17u;
    uint32_t g = saat * 19u;
    uint32_t h = saat * 23u;

    { uint8_t i; for (i = 0; i < 10; i++) arbeite(); }

    {
        uint32_t s = saat;
        if      (a != s *  3u) { t2_schlecht = 1; t2_status = 2; }
        else if (b != s *  5u) { t2_schlecht = 2; t2_status = 2; }
        else if (c != s *  7u) { t2_schlecht = 3; t2_status = 2; }
        else if (d != s * 11u) { t2_schlecht = 4; t2_status = 2; }
        else if (e != s * 13u) { t2_schlecht = 5; t2_status = 2; }
        else if (f != s * 17u) { t2_schlecht = 6; t2_status = 2; }
        else if (g != s * 19u) { t2_schlecht = 7; t2_status = 2; }
        else if (h != s * 23u) { t2_schlecht = 8; t2_status = 2; }
        else                   { t2_status = 1; }
    }
    for (;;) arbeite();
}

static void test_register(void)
{
    t2_status = 0; t2_schlecht = 0;
    irk_task_create(t2, 1, stack_a, STK, "t2");

    if (!warte_bis(&t2_status, 1, 300) && t2_status != 2) {
        pruefe("callee-saved Register ueberleben", 0);
    } else if (t2_status == 1) {
        pruefe("callee-saved Register ueberleben", 1);
    } else {
        pruefe_zahl("Register verfaelscht, Wert Nr", 0, t2_schlecht);
    }
    alle_killen();
}


/*========================================================================*\
 *  3  Ueberlebt eine tiefe Aufrufkette den Wechsel?
\*========================================================================*/

static volatile uint8_t  t3_status = 0;
static volatile uint32_t t3_summe  = 0;

static uint32_t rekursion(uint8_t tiefe)
{
    uint32_t eigen = (uint32_t)tiefe * 100u + 7u;
    if (tiefe == 0) { arbeite(); return eigen; }
    return eigen + rekursion((uint8_t)(tiefe - 1));
}

static void t3(void)
{
    t3_summe  = rekursion(7);           /* Soll: 100*28 + 8*7 = 2856 */
    t3_status = (t3_summe == 2856u) ? 1 : 2;
    for (;;) arbeite();
}

static void test_aufrufkette(void)
{
    t3_status = 0; t3_summe = 0;
    irk_task_create(t3, 1, stack_a, STK, "t3");

    if (!warte_bis(&t3_status, 1, 300) && t3_status != 2) {
        pruefe("tiefe Aufrufkette ueberlebt", 0);
    } else if (t3_status == 1) {
        pruefe("tiefe Aufrufkette ueberlebt", 1);
    } else {
        pruefe_zahl("Pruefsumme falsch (soll 2856)", 0, t3_summe);
    }
    alle_killen();
}


/*========================================================================*\
 *  4  Haben die Tasks wirklich getrennte Stacks?
\*========================================================================*/

static volatile uint8_t t4_ok[2]     = { 0, 0 };
static volatile uint8_t t4_fertig[2] = { 0, 0 };

static void t4_body(uint8_t nr)
{
    uint8_t feld[12];
    uint8_t i, gut = 1;

    for (i = 0; i < 12; i++) feld[i] = (uint8_t)(nr * 12u + i + 1u);
    for (i = 0; i < 8; i++)  arbeite();
    for (i = 0; i < 12; i++) {
        if (feld[i] != (uint8_t)(nr * 12u + i + 1u)) { gut = 0; break; }
    }
    t4_ok[nr]     = gut;
    t4_fertig[nr] = 1;
    for (;;) arbeite();
}

static void t4a(void) { t4_body(0); }
static void t4b(void) { t4_body(1); }

static void test_getrennte_stacks(void)
{
    uint16_t i;
    t4_ok[0] = t4_ok[1] = 0;
    t4_fertig[0] = t4_fertig[1] = 0;

    irk_task_create(t4a, 1, stack_a, STK, "t4a");
    irk_task_create(t4b, 1, stack_b, STK, "t4b");

    for (i = 0; i < 300 && !(t4_fertig[0] && t4_fertig[1]); i++) arbeite();

    pruefe("Tasks haben getrennte Stacks",
           t4_fertig[0] && t4_fertig[1] && t4_ok[0] && t4_ok[1]);
    alle_killen();
}


/*========================================================================*\
 *  5  Fair Share  --  1 : 2 : 3
\*========================================================================*/

static volatile uint32_t fz[3];
static volatile uint8_t  f_lauf;

static void fa(void) { while (f_lauf) { fz[0]++; arbeite(); } for(;;) arbeite(); }
static void fb(void) { while (f_lauf) { fz[1]++; arbeite(); } for(;;) arbeite(); }
static void fc(void) { while (f_lauf) { fz[2]++; arbeite(); } for(;;) arbeite(); }

static void test_fairshare(void)
{
    uint16_t i;
    irk_task_t a, b, c;

    fz[0] = fz[1] = fz[2] = 0;
    f_lauf = 1;

    a = irk_task_create(fa, 1, stack_a, STK, "f1");
    b = irk_task_create(fb, 2, stack_b, STK, "f2");
    c = irk_task_create(fc, 3, stack_c, STK, "f3");

    for (i = 0; i < 2000; i++) arbeite();
    f_lauf = 0;
    for (i = 0; i < 20; i++) arbeite();

    {
        uint32_t r1 = irk_task_runtime(a);
        uint32_t r2 = irk_task_runtime(b);
        uint32_t r3 = irk_task_runtime(c);
        int ok = 0;

        put("        Laufzeiten: ");
        put_u32(r1); put(" / "); put_u32(r2); put(" / "); put_u32(r3);
        put("  (soll 1:2:3)\r\n");

        if (r1 > 50) {
            uint32_t v2 = (r2 * 100u) / r1;      /* in Hundertsteln */
            uint32_t v3 = (r3 * 100u) / r1;
            ok = (v2 > 170 && v2 < 230) && (v3 > 260 && v3 < 340);
        }
        pruefe("Fair Share haelt das Verhaeltnis 1:2:3", ok);
    }
    alle_killen();
}


/*========================================================================*\
 *  6  Semaphor  --  gegenseitiger Ausschluss
\*========================================================================*/

static volatile uint8_t s_drin = 0, s_max = 0, s_lauf = 0;

static void s_body(void)
{
    while (s_lauf) {
        irk_sema_wait(0);
        s_drin++;
        if (s_drin > s_max) s_max = s_drin;
        arbeite();
        s_drin--;
        irk_sema_signal(0);
        arbeite();
    }
    for (;;) arbeite();
}

static void test_semaphor(void)
{
    uint16_t i;
    s_drin = s_max = 0;
    s_lauf = 1;
    irk_sema_init(0, 1);

    irk_task_create(s_body, 1, stack_a, STK, "s1");
    irk_task_create(s_body, 1, stack_b, STK, "s2");
    irk_task_create(s_body, 1, stack_c, STK, "s3");

    for (i = 0; i < 400; i++) arbeite();
    s_lauf = 0;
    for (i = 0; i < 20; i++) arbeite();

    pruefe_zahl("Semaphor: max. gleichzeitig im Abschnitt", s_max == 1, s_max);
    alle_killen();
}


/*========================================================================*\
 *  7  Queue  --  FIFO und Blockieren bei voll
\*========================================================================*/

static irk_queue_t  q;
static uint16_t     q_speicher[4];
static volatile uint16_t q_ges = 0, q_emp = 0;
static volatile uint8_t  q_folge_ok = 1, q_war_voll = 0, q_lauf = 0;

static void q_send(void)
{
    uint16_t n = 0;
    while (q_lauf) {
        if (irk_queue_count(&q) >= 4) q_war_voll = 1;
        irk_queue_send(&q, &n);
        q_ges++; n++;
        arbeite();
    }
    for (;;) arbeite();
}

static void q_recv(void)
{
    uint16_t erwartet = 0, wert;
    while (q_lauf) {
        irk_queue_recv(&q, &wert);
        if (wert != erwartet) q_folge_ok = 0;
        erwartet++; q_emp++;
        arbeite(); arbeite(); arbeite();
    }
    for (;;) arbeite();
}

static void test_queue(void)
{
    uint16_t i;
    q_ges = q_emp = 0; q_folge_ok = 1; q_war_voll = 0; q_lauf = 1;
    irk_queue_init(&q, q_speicher, sizeof(uint16_t), 4);

    irk_task_create(q_send, 1, stack_a, STK, "qs");
    irk_task_create(q_recv, 1, stack_b, STK, "qe");

    for (i = 0; i < 500; i++) arbeite();
    q_lauf = 0;
    for (i = 0; i < 20; i++) arbeite();

    put("        gesendet "); put_u32(q_ges);
    put(", empfangen ");      put_u32(q_emp);
    put(", Vorsprung ");      put_u32((uint32_t)(q_ges - q_emp));
    put("\r\n");

    pruefe("Queue: FIFO-Reihenfolge", q_folge_ok && q_emp > 10);
    pruefe("Queue: Sender blockiert bei voll",
           q_war_voll && (uint16_t)(q_ges - q_emp) <= 5);
    alle_killen();
}


/*========================================================================*\
 *  Ablauf
\*========================================================================*/

static void tests_ausfuehren(void)
{
    put("\r\n========================================\r\n");
    put(" IRKernel  --  QEMU-Lauf\r\n");
    put("========================================\r\n");

    if (irk_init(1) != 0) {
        put("  KERNEL START FEHLGESCHLAGEN\r\n");
        return;
    }

    put("  -- Kontextwechsel --\r\n");
    test_wechsel();
    test_register();
    test_aufrufkette();
    test_getrennte_stacks();

    put("  -- Scheduling --\r\n");
    test_fairshare();

    put("  -- Synchronisation --\r\n");
    test_semaphor();

    put("  -- Inter-Task-Kommunikation --\r\n");
    test_queue();

    put("========================================\r\n");
    put(" ERGEBNIS: ");
    put_u32(geprueft);
    put(" Pruefungen, ");
    put_u32(fehler);
    put(" Fehler\r\n");
    put("========================================\r\n");
    put(fehler ? "FERTIG-MIT-FEHLERN\r\n" : "FERTIG-OHNE-FEHLER\r\n");
}
