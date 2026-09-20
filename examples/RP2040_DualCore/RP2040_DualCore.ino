/*========================================================================*\
 *
 *  RP2040_DualCore  --  IRKernel auf beiden Kernen des RP2040
 *
 *  Board:   Waveshare RP2040 Zero (RGB-LED an GPIO 16), jedes andere
 *           RP2040/RP2350-Board geht auch -- dann ohne LED.
 *  Kern:    arduino-pico (Earle Philhower), Library "Adafruit NeoPixel"
 *  Monitor: 115200 Baud
 *
 *  Aufbau
 *  ------
 *  Der Test laeuft in Bloecken von je 10 s. In jedem Block ist nur die
 *  Funktion aktiv, um die es geht -- in der Ausgabe und auf der LED sieht
 *  man also genau eine Sache. Zum Schluss laufen alle gleichzeitig.
 *
 *    Block 0  Anlauf (2 s): Handles, Kerne, Prioritaeten    LED aus
 *    Block 1  Queue Kern 0 -> Kern 1                         matt cyan
 *    Block 2  Wecken ueber die Kerngrenze                    blauer Blitz
 *    Block 3  Semaphor ueber beide Kerne (und ohne)          matt violett
 *    Block 4  Fair Share 256 : 768, Takt unter Volllast      matt gelb
 *    Block 5  Prioritaet 1 neben Volllast                    weiss flackernd
 *    Block 6  Kernelaufrufe aus dem Interrupt                matt orange
 *    Block 7  alles gleichzeitig, Rechenzeit je Task         gemischt
 *    Ende     Zusammenfassung                                gruen / rot
 *
 *  Wer macht was
 *  -------------
 *     Kern 0                                 Kern 1
 *     erzeuger   (zyklisch 10 ms) --Queue-->  empfaenger
 *     ausloeser  (zyklisch 500 ms) --Sema-->  wecker
 *     schreiber x2  <--- ein Semaphor --->    schreiber x2
 *     frei          <--- KEIN Semaphor -->    frei
 *                                             rechner  Prio 256 und 768
 *                                             blitz    Prio 1
 *     Timer-Interrupt                         led      (zyklisch 20 ms)
 *     loop(): Ablauf, Ausgabe  <--Queue---    loop1(): Bericht je Sekunde
 *
 *  setup()/loop() laufen auf Kern 0, setup1()/loop1() auf Kern 1. Jeder
 *  Kern hat seinen eigenen Scheduler; Tasks bleiben auf dem Kern, der sie
 *  anlegt. Semaphore und Queues sind gemeinsam.
 *
 *  Alle normalen Prioritaeten sind Vielfache von P_BASIS = 256. Die
 *  Verhaeltnisse sind dieselben wie mit 1, 2, 3 -- aber darunter ist Platz
 *  fuer eine absolut niedrige Task mit Prioritaet 1.
 *
 *  Alle Tasks werden am Anfang angelegt. Wessen Block gerade nicht laeuft,
 *  der schlaeft mit irk_delay(10). Warum nicht irk_task_suspend()? Ein
 *  Schreiber, der mitten im Semaphor angehalten wird, blockierte es fuer
 *  alle. So prueft jede Task selbst, an einer Stelle, an der sie nichts
 *  festhaelt.
 *
 *  Die wichtigste Regel im Mehrkernbetrieb
 *  ---------------------------------------
 *  Innerhalb EINES Kerns gilt weiter: zwischen zwei irk_yield() funkt
 *  niemand dazwischen. Zwischen den Kernen gilt das NICHT -- die rechnen
 *  wirklich gleichzeitig. Daten, die Tasks beider Kerne anfassen, brauchen
 *  ein Semaphor oder eine Queue. Block 3 zeigt, warum.
 *
\*========================================================================*/

#include <IRKernel.h>
#include <irk_port.h>               /* nur fuer den Fehler-Haken irk_port_error() */
#include <pico/time.h>              /* Hardware-Timer fuer Block 6                 */
#include <Adafruit_NeoPixel.h>

#ifndef PIN_NEOPIXEL
#define PIN_NEOPIXEL 16
#endif

/* Alle Datentypen GANZ OBEN: die Arduino-IDE setzt ihre automatisch
   erzeugten Funktionsprototypen vor die erste Funktion -- Typen, die
   erst danach kommen, kennt sie dort noch nicht. */

struct Messwert {                   /* Kern 0 -> Kern 1 */
    uint32_t nummer;
    uint64_t zeit_us;
};

struct Bericht {                    /* Kern 1 -> Kern 0 */
    char text[96];
};

struct Konto {                      /* Invariante:  b == 2 * a */
    uint32_t a;
    uint32_t b;
};

struct BlitzMax {                   /* der laengste Blitz eines Blocks    */
    uint32_t seit_us;               /* Einschalten, ab Blockbeginn        */
    uint32_t dauer_us;              /* weiss                              */
    uint32_t abstand_us;            /* zum vorigen Einschalten, 0 = erster */
    uint32_t show_us;               /* Dauer von led_zeigen() beim Ein    */
    uint64_t blitz_an, rechner_an;  /* Konten beim Einschalten            */
    uint64_t blitz_aus, rechner_aus;/* Konten beim Ausschalten            */
};                                  /* Rechner: das kleinere der beiden   */

/* Stand aller Zaehler, genommen zu Beginn und am Ende jedes Blocks */
struct Stand {
    irk_time_t wann;
    uint32_t   empfangen, verworfen, folgefehler;
    uint32_t   weckungen;
    uint32_t   beitrag0, beitrag1, konto_verl, frei_verl;
    uint32_t   blitze;
    uint32_t   isr_versuche, isr_durch, fehler_isr;
    uint32_t   fehler_sonst, kern_verl;
    irk_time_t zeit[IRK_MAX_TASKS + 1];         /* Rechenzeit je Task */
    uint32_t   laeufe[IRK_MAX_TASKS + 1];       /* Laeufe je Task     */
};


/*========================================================================*\
 *  Konstanten und gemeinsame Objekte
\*========================================================================*/

#define SEM_KONTO   0               /* schuetzt konto                 */
#define SEM_WECK    1               /* Kern 0 weckt Kern 1            */
#define SEM_ISR     2               /* Ziel der Interrupt-Versuche    */

#define STK         1536
#define LED_HELL    40
#define LED_MATT    3

#define ANLAUF_MS   2000
#define BLOCK_MS    10000

#define P_BASIS     256             /* Vielfaches fuer normale Prioritaeten */

#define B_PAUSE     -1              /* zwischen den Bloecken */
#define B_ANLAUF    0
#define B_QUEUE     1
#define B_WECKEN    2
#define B_SEMA      3
#define B_FAIR      4
#define B_PRIO1     5
#define B_ISR       6
#define B_ALLES     7
#define B_ENDE      8
#define B_ANZAHL    8               /* Bloecke 0 .. 7 */

static const char *const block_name[B_ANZAHL] = {
    "Anlauf",
    "Queue Kern 0 -> Kern 1",
    "Wecken ueber die Kerngrenze",
    "Semaphor ueber beide Kerne",
    "Fair Share und Takt unter Volllast",
    "Prioritaet 1 neben Volllast",
    "Kernelaufrufe aus dem Interrupt",
    "Alles gleichzeitig",
};

static irk_queue_t q_mess;          static Messwert q_mess_speicher[8];
static irk_queue_t q_bericht;       static Bericht  q_bericht_speicher[4];

static Konto konto;                 /* mit Semaphor  */
static Konto frei;                  /* ohne Semaphor -- absichtlich falsch */

static volatile bool bereit   = false;   /* Kern 0 hat alles eingerichtet */
static volatile int  block    = B_PAUSE; /* laufender Block               */
static volatile int  ergebnis = 0;       /* 0 laeuft, 1 bestanden, 2 Fehler */

/* Handles und Kernnummern -- nie als feste Zahlen annehmen */
static volatile irk_task_t kern0_haupt, kern1_haupt;
static volatile uint8_t    kern0_id = 99, kern1_id = 99;
static irk_task_t t_erzeuger, t_empfaenger, t_rechner1, t_rechner3, t_blitz;

/* Zaehler. Jeder wird nur von EINEM Kern (oder einem Interrupt) geschrieben.
   Die Maxima setzt Kern 0 zu Blockbeginn zurueck, solange ihre Tasks ruhen. */
static volatile uint32_t anlegefehler[2], kern_verletzungen[2];
static volatile uint32_t q_empfangen, q_folgefehler, q_latenz_max;     /* Kern 1 */
static volatile uint32_t q_verworfen, takt_abw_max;                    /* Kern 0 */
static volatile uint32_t weckungen, weck_latenz_max;                   /* Kern 1 */
static volatile uint64_t weck_zeit_us;                                 /* Kern 0 */
static volatile uint64_t blitz_bis_us;                                 /* Kern 1 */
static volatile bool     weiss;                                        /* Kern 1 */
static volatile uint32_t blitze, weiss_max_us, blitz_abstand_max_us;   /* Kern 1 */
static volatile uint32_t weiss_min_us = 0xFFFFFFFFu;                   /* Kern 1 */
static volatile uint32_t beitrag[2], konto_verletzungen;               /* unter SEM_KONTO */
static volatile uint32_t frei_verletzungen[2];
static volatile uint32_t isr_versuche, isr_durch, fehler_isr;          /* Interrupt */
static volatile uint32_t fehler_sonst;
static volatile int      fehler_letzt, fehler_detail;

static Stand             stand_a, stand_e;          /* Blockbeginn, Blockende */
static repeating_timer_t isr_timer;
static int               pruef_block;
static int               blk_ok[B_ANZAHL], blk_fehler[B_ANZAHL];

/* Der laengste Blitz eines Blocks (Kern 1 schreibt, Kern 0 liest erst nach
   Blockende) */
static BlitzMax            blitz_max;
static volatile irk_time_t block_start_us;

static uint8_t stk_erzeuger[STK], stk_ausloeser[STK], stk_schreiber0a[STK], stk_schreiber0b[STK];
static uint8_t stk_empfaenger[STK], stk_wecker[STK], stk_rechner1[STK], stk_rechner3[STK];
static uint8_t stk_schreiber1a[STK], stk_schreiber1b[STK], stk_led[STK];
static uint8_t stk_frei0[STK], stk_frei1[STK], stk_blitz[STK];

Adafruit_NeoPixel led(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);


/*========================================================================*\
 *  Fehler-Haken
 *
 *  Die Library meldet Fehler ueber irk_port_error(). Ihre eigene Fassung
 *  ist "weak" -- diese hier ersetzt sie. Sie kann auch aus einem
 *  Interrupt heraus gerufen werden, darf also nur Zaehler anfassen.
\*========================================================================*/

void irk_port_error(int code, int detail)
{
    if (code == IRK_ERR_IN_ISR) {
        fehler_isr++;
    } else {
        fehler_sonst++;
        fehler_letzt  = code;
        fehler_detail = detail;
    }
}


/*========================================================================*\
 *  Hilfen
\*========================================================================*/

/* Ist Block b gerade dran? Im letzten Block laeuft alles. */
static inline bool laeuft(int b)
{
    int x = block;
    return x == b || x == B_ALLES;
}

static irk_task_t anlegen(void (*fn)(void), irk_prio_t prio, uint8_t *stk, const char *name)
{
    irk_task_t t = irk_task_create(fn, prio, stk, STK, name);
    if (t == IRK_NO_TASK) anlegefehler[irk_core_id()]++;
    return t;
}

/* Jede Task fragt regelmaessig nach: laufe ich noch auf meinem Kern? */
static inline void kern_pruefen(uint8_t soll)
{
    if (irk_core_id() != soll) kern_verletzungen[soll]++;
}

static void stand_nehmen(Stand &s)
{
    s.wann         = irk_now_us();
    s.empfangen    = q_empfangen;
    s.verworfen    = q_verworfen;
    s.folgefehler  = q_folgefehler;
    s.weckungen    = weckungen;
    s.beitrag0     = beitrag[0];
    s.beitrag1     = beitrag[1];
    s.konto_verl   = konto_verletzungen;
    s.frei_verl    = frei_verletzungen[0] + frei_verletzungen[1];
    s.blitze       = blitze;
    s.isr_versuche = isr_versuche;
    s.isr_durch    = isr_durch;
    s.fehler_isr   = fehler_isr;
    s.fehler_sonst = fehler_sonst;
    s.kern_verl    = kern_verletzungen[0] + kern_verletzungen[1];
    for (uint8_t t = 1; t <= IRK_MAX_TASKS; t++) {
        s.zeit[t]   = irk_task_runtime_us(t);       /* ungueltig -> 0 */
        s.laeufe[t] = irk_task_calls(t);
    }
}

/* Aenderung eines Zaehlers im Block */
#define D(feld)  (stand_e.feld - stand_a.feld)

static uint64_t zeit_im_block(irk_task_t t)
{
    return (uint64_t)(stand_e.zeit[t] - stand_a.zeit[t]);
}


/*========================================================================*\
 *  Block 1  Queue von Kern 0 nach Kern 1
 *
 *  Der Erzeuger ist zyklisch: alle 10 ms ein Messwert mit Zeitstempel.
 *  Er sendet NICHT blockierend -- eine Regelschleife darf nicht haengen,
 *  weil der Empfaenger mal nicht nachkommt. Verlorene Werte werden gezaehlt.
 *
 *  Der Erzeuger misst zugleich seine Taktgenauigkeit (Block 4: haelt
 *  Kern 0 seinen Takt, waehrend Kern 1 voll ausgelastet ist?).
\*========================================================================*/

static void erzeuger(void)                          /* Kern 0, zyklisch 10 ms */
{
    uint32_t   nummer = 0;
    irk_time_t letzte = 0;

    for (;;) {
        irk_time_t jetzt = irk_now_us();
        bool       an    = laeuft(B_QUEUE) || laeuft(B_FAIR);

        if (an && letzte != 0) {
            long abw = (long)(jetzt - letzte) - 10000L;
            if (abw < 0) abw = -abw;
            if ((uint32_t)abw > takt_abw_max) takt_abw_max = (uint32_t)abw;
        }
        letzte = jetzt;

        if (an) {
            Messwert m = { nummer++, (uint64_t)jetzt };
            if (irk_queue_try_send(&q_mess, &m) != 1) q_verworfen++;
        }

        kern_pruefen(0);
        irk_yield();                                /* bis zum naechsten Takt */
    }
}

static void empfaenger(void)                        /* Kern 1 */
{
    uint32_t erwartet = 0;
    Messwert m;

    for (;;) {
        irk_queue_recv(&q_mess, &m);                /* blockiert, wenn leer */

        if (m.nummer != erwartet) q_folgefehler++;
        erwartet = m.nummer + 1;
        q_empfangen++;

        uint32_t lat = (uint32_t)(irk_now_us() - m.zeit_us);
        if (lat > q_latenz_max) q_latenz_max = lat;

        kern_pruefen(1);
    }
}


/*========================================================================*\
 *  Block 2  Wecken ueber die Kerngrenze
 *
 *  Der Wecker auf Kern 1 wartet auf ein Semaphor mit Zaehler 0. Kern 0
 *  gibt es alle 500 ms frei. Die Freigabe ist nur eine Zustandsaenderung;
 *  umgeschaltet wird von Kern 1 selbst, bei seinem naechsten irk_yield().
 *
 *  Freigegeben wird nur, wenn der Wecker auch wirklich wartet (Zaehler < 0)
 *  -- sonst waere es ein "zu oft freigegeben" (IRK_ERR_SIGNAL_NO_WAIT).
\*========================================================================*/
static void ausloeser(void)                         /* Kern 0, zyklisch 500 ms */
{
    for (;;) {
        if (laeuft(B_WECKEN) && irk_sema_count(SEM_WECK) < 0) {
            weck_zeit_us = irk_now_us();
            irk_sema_signal(SEM_WECK);
        }
        kern_pruefen(0);
        irk_yield();
    }
}

static void wecker(void)                            /* Kern 1 */
{
    for (;;) {
        irk_sema_wait(SEM_WECK);
        irk_time_t jetzt = irk_now_us();

        weckungen++;
        uint32_t lat = (uint32_t)(jetzt - weck_zeit_us);
        if (lat > weck_latenz_max) weck_latenz_max = lat;

        blitz_bis_us = jetzt + 60000u;              /* 60 ms blau */
        kern_pruefen(1);
    }
}


/*========================================================================*\
 *  Block 3  Ein Semaphor fuer beide Kerne -- und zum Vergleich keines
 *
 *  Ein Konto wird in zwei Schritten geaendert. Zwischen den Schritten ist
 *  die Invariante b == 2a kurz verletzt.
 *
 *    konto  -- vier Schreiber, zwei je Kern, geschuetzt durch SEM_KONTO:
 *              darf NIE verletzt gesehen werden
 *    frei   -- je ein Schreiber pro Kern, ungeschuetzt:
 *              der andere Kern sieht die Luecke
 *
 *  Auf EINEM Kern waere "frei" voellig in Ordnung, weil zwischen den beiden
 *  Schritten kein irk_yield() steht. Zwischen zwei Kernen hilft das nichts.
 *
 *  Die freien Schreiber sind eigene Tasks mit unterschiedlichem Takt
 *  (1000 / 1300 us). Haengt der ungeschuetzte Teil direkt hinter dem
 *  geschuetzten, ordnet das Semaphor ihn nebenbei mit -- die Schreiber
 *  laufen dann im Gleichtakt, und der Vergleich zeigt zufaellig nichts.
 *
 *  Der Block wird oben in der Schleife abgefragt -- nie, waehrend das
 *  Semaphor gehalten wird.
\*========================================================================*/
static void schreiben(uint8_t kern)
{
    for (;;) {
        if (!laeuft(B_SEMA)) { irk_delay(10); continue; }

        irk_sema_wait(SEM_KONTO);
        if (konto.b != 2 * konto.a) konto_verletzungen++;
        konto.a++;
        delayMicroseconds(20);                      /* Luecke absichtlich weit */
        konto.b += 2;
        beitrag[kern]++;
        irk_sema_signal(SEM_KONTO);

        kern_pruefen(kern);
        irk_delay(1);
    }
}

static void schreiber0(void) { schreiben(0); }
static void schreiber1(void) { schreiben(1); }

static void frei_schreiben(uint8_t kern, irk_time_t pause_us)
{
    for (;;) {
        if (!laeuft(B_SEMA)) { irk_delay(10); continue; }

        if (frei.b != 2 * frei.a) {                 /* ohne Schutz */
            frei_verletzungen[kern]++;
            frei.b = 2 * frei.a;                    /* reparieren, weiterzaehlen */
        }
        frei.a++;
        delayMicroseconds(20);
        frei.b += 2;

        kern_pruefen(kern);
        irk_delay_us(pause_us);
    }
}

static void frei_schreiber0(void) { frei_schreiben(0, 1000); }
static void frei_schreiber1(void) { frei_schreiben(1, 1300); }


/*========================================================================*\
 *  Block 4  Fair Share auf Kern 1
 *
 *  Zwei Rechner ohne jede Pause, Prioritaet 256 und 768. Verglichen wird
 *  ihre Rechenzeit, wie der Kernel sie misst (irk_task_runtime_us) --
 *  erwartet ist 1 : 3. Der Scheduler von Kern 1 verteilt nur die Zeit von
 *  Kern 1; Kern 0 bemerkt die Volllast nicht (Taktgenauigkeit des
 *  Erzeugers).
 *
 *  Warum nicht Schleifendurchlaeufe zaehlen? Jeder Durchlauf gibt ab, und
 *  eine Runde, in der dieselbe Task gleich wieder drankommt, ist ohne
 *  Kontextwechsel billiger. Der hoeher priorisierte Rechner kommt oefter
 *  mehrmals hintereinander dran und schafft so in derselben Zeit mehr
 *  Durchlaeufe -- gezaehlt ergab das 1 : 3,14 bei exakt 1 : 3 Zeit.
\*========================================================================*/
static void rechnen(void)
{
    for (;;) {
        if (!(laeuft(B_FAIR) || laeuft(B_PRIO1))) { irk_delay(10); continue; }
        kern_pruefen(1);
        irk_yield();
    }
}

static void rechnen1(void) { rechnen(); }
static void rechnen3(void) { rechnen(); }


/*========================================================================*\
 *  LED auf Kern 1
 *
 *  Zwei Tasks steuern die LED: led_task (Blockfarbe, blauer Blitz) und
 *  blitz_task (weiss). Beide laufen auf Kern 1 und rufen led_zeigen() --
 *  zwischen zwei irk_yield() kommt keine der anderen dazwischen, ein Mutex
 *  ist also nicht noetig. Weiss hat Vorrang.
\*========================================================================*/

static uint32_t led_alt = 0xFFFFFFFFu;

static void led_zeigen(void)                        /* nur Kern 1 */
{
    uint8_t r = 0, g = 0, b = 0;
    int     x = block;

    switch (x) {
        case B_QUEUE: g = LED_MATT; b = LED_MATT; break;          /* cyan    */
        case B_SEMA:  r = LED_MATT; b = LED_MATT; break;          /* violett */
        case B_FAIR:  r = LED_MATT; g = LED_MATT; break;          /* gelb    */
        case B_ISR:   r = 4;        g = 1;        break;          /* orange  */
        case B_ALLES: r = LED_MATT; g = LED_MATT; break;          /* gelb    */
        case B_ENDE:  if (ergebnis == 1) g = LED_HELL; else r = LED_HELL; break;
        default:      break;                      /* Anlauf, Wecken, Prio 1 */
    }
    if (irk_now_us() < blitz_bis_us) b = LED_HELL;
    if (weiss) {
        uint8_t w = (x == B_ALLES) ? 8 : LED_HELL;  /* im Gesamtblock gedimmt */
        r = g = b = w;
    }

    uint32_t farbe = led.Color(r, g, b);
    if (farbe != led_alt) {
        led.setPixelColor(0, farbe);
        led.show();
        led_alt = farbe;
    }
}

static void led_task(void)                          /* Kern 1, zyklisch 20 ms */
{
    for (;;) {
        led_zeigen();
        irk_yield();
    }
}


/*========================================================================*\
 *  Block 5  Absolut niedrige Prioritaet: weiss blitzen mit Prioritaet 1
 *
 *  Die Blitz-Task schaltet weiss ein, wartet mit irk_delay(2) und schaltet
 *  wieder aus. Sie laeuft auf dem voll ausgelasteten Kern 1 neben den
 *  Rechnern (256 + 768 = 1024). Ihr Anteil ist damit rund 1/1025 = 0,1 %.
 *
 *  Warum die LED trotzdem nicht nur 2 ms weiss ist:
 *    - Ein Lauf der Blitz-Task (LED setzen, abgeben) kostet auf dem RP2040
 *      gemessen etwa 29 us. Bei Prioritaet 1 waechst ihr Konto dabei um
 *      29 * 256 / 1 = 7424.
 *    - Die Konten der Rechner wachsen zusammen um 256/1024 = 1/4 je us, in
 *      der sie rechnen. Bis sie 7424 aufgeholt haben, vergehen ~30 ms.
 *    - Nach irk_delay(2) ist die Task zwar lauffaehig, beim Aufwachen wird
 *      ihr Konto aber nur ANGEHOBEN, nie gesenkt. Es liegt noch ueber dem
 *      der Rechner -- sie kommt erst nach rund 30-35 ms wieder dran.
 *  Ergebnis: ~35 ms weiss, ~30 ms dunkel, ein Flackern statt kurzer Blitze.
 *  Gemessen: Vorsprung beim Einschalten genau 0, +15 k Konto je Blitz.
 *  (Im Gesamtblock 7 rechnen die Rechner nur einen Teil der Zeit -- dann
 *  dauert es entsprechend laenger.)
 *
 *  Das ist richtig so: irk_delay(2) heisst "fruehestens nach 2 ms", und
 *  eine Task mit 0,1 % Anteil bekommt 0,1 %. Wie oft sie drankommt, ergibt
 *  sich aus Anteil geteilt durch die Dauer eines Laufs. Wer einen EXAKTEN
 *  2-ms-Blitz braucht, nimmt eine zyklische Task (Vorrang vor Fair Share).
\*========================================================================*/

static void blitz_task(void)                        /* Kern 1, Prioritaet 1 */
{
    irk_time_t letzter_an = 0;

    for (;;) {
        if (!laeuft(B_PRIO1)) { letzter_an = 0; irk_delay(10); continue; }

        /* Konten beim Einschalten -- fuer den laengsten Blitz */
        uint64_t kb_an = (uint64_t)irk_task_vruntime(t_blitz);
        uint64_t k1    = (uint64_t)irk_task_vruntime(t_rechner1);
        uint64_t k3    = (uint64_t)irk_task_vruntime(t_rechner3);
        uint64_t kr_an = (k1 < k3) ? k1 : k3;

        irk_time_t an = irk_now_us();
        weiss = true;
        led_zeigen();
        uint32_t show = (uint32_t)(irk_now_us() - an);  /* haengt die LED-Ausgabe? */

        irk_delay(2);                               /* fruehestens 2 ms */

        irk_time_t aus = irk_now_us();
        weiss = false;
        led_zeigen();
        blitze++;

        uint32_t dauer = (uint32_t)(aus - an);
        if (dauer < weiss_min_us) weiss_min_us = dauer;
        if (dauer > weiss_max_us) weiss_max_us = dauer;
        if (letzter_an != 0) {
            uint32_t abstand = (uint32_t)(an - letzter_an);
            if (abstand > blitz_abstand_max_us) blitz_abstand_max_us = abstand;
        }
        if (dauer > blitz_max.dauer_us) {           /* neuer laengster Blitz */
            k1 = (uint64_t)irk_task_vruntime(t_rechner1);
            k3 = (uint64_t)irk_task_vruntime(t_rechner3);
            blitz_max.seit_us     = (uint32_t)(an - block_start_us);
            blitz_max.dauer_us    = dauer;
            blitz_max.abstand_us  = letzter_an ? (uint32_t)(an - letzter_an) : 0u;
            blitz_max.show_us     = show;
            blitz_max.blitz_an    = kb_an;
            blitz_max.rechner_an  = kr_an;
            blitz_max.blitz_aus   = (uint64_t)irk_task_vruntime(t_blitz);
            blitz_max.rechner_aus = (k1 < k3) ? k1 : k3;
        }
        letzter_an = an;

        kern_pruefen(1);
        irk_yield();                                /* fertig -- bis zum naechsten Mal */
    }
}


/*========================================================================*\
 *  Block 6  Aufruf aus einem Interrupt
 *
 *  Interrupts laufen ganz normal weiter. Ein Kernelaufruf AUS einem
 *  Interrupt-Handler wird aber abgelehnt -- er koennte den Scheduler mitten
 *  in einer Aenderung erwischen. Richtig macht man es so: im Interrupt nur
 *  ein Flag setzen, eine Task holt es ab.
\*========================================================================*/

static bool isr_rueckruf(repeating_timer_t *rt)
{
    (void)rt;
    if (!laeuft(B_ISR)) return true;
    isr_versuche++;
    if (irk_sema_signal(SEM_ISR) == 0) isr_durch++;   /* darf nie klappen */
    return true;
}


/*========================================================================*\
 *  Ausgabe und Pruefung
\*========================================================================*/

static void pruefen(bool ok, const __FlashStringHelper *text, long wert)
{
    Serial.print(ok ? F("  OK      ") : F("  FEHLER  "));
    Serial.print(text);
    Serial.print(F(": "));
    Serial.println(wert);
    if (ok) blk_ok[pruef_block]++; else blk_fehler[pruef_block]++;
}

static void info(const __FlashStringHelper *text, long wert)
{
    Serial.print(F("          "));
    Serial.print(text);
    Serial.print(F(": "));
    Serial.println(wert);
}

static void kernelfehler_pruefen(void)
{
    uint32_t n = D(fehler_sonst);
    pruefen(n == 0, F("keine Kernelfehler in diesem Block"), (long)n);
    if (n) {
        info(F("letzter Fehlercode"), fehler_letzt);
        info(F("Detail"), fehler_detail);
    }
}

/* Rechenzeit Prio 768 : Prio 256, mal 100 */
static long verhaeltnis_rechner(void)
{
    uint64_t z1 = zeit_im_block(t_rechner1);
    uint64_t z3 = zeit_im_block(t_rechner3);
    return z1 ? (long)(z3 * 100u / z1) : 0;
}

/* Rechenzeit einer Task im Block, in Millionsteln der Blockdauer */
static long ppm_im_block(irk_task_t t)
{
    irk_time_t dauer = stand_e.wann - stand_a.wann;
    return dauer ? (long)(zeit_im_block(t) * 1000000u / dauer) : 0;
}

static void profil_ausgeben(void)
{
    char       zeile[100];
    irk_time_t fenster = stand_e.wann - stand_a.wann;
    if (fenster == 0) fenster = 1;

    Serial.println();
    Serial.print(F("  Rechenzeit je Task im Block ("));
    Serial.print((unsigned long)(fenster / 1000u));
    Serial.println(F(" ms)"));

    for (uint8_t k = 0; k < 2; k++) {
        uint64_t summe = 0;

        Serial.println();
        snprintf(zeile, sizeof(zeile),
                 "  Kern %u  Task       Hdl Prio  Rechenzeit   Anteil     Laeufe  us/Lauf",
                 (unsigned)k);
        Serial.println(zeile);

        for (uint8_t t = 1; t <= IRK_MAX_TASKS; t++) {
            if (irk_task_status(t) & IRK_KILLED) continue;
            if (irk_task_core(t) != k) continue;

            uint64_t    zeit     = zeit_im_block(t);
            uint32_t    laeufe   = stand_e.laeufe[t] - stand_a.laeufe[t];
            uint32_t    promille = (uint32_t)(zeit * 1000u / fenster);
            const char *name     = irk_task_name(t);
            summe += zeit;

            snprintf(zeile, sizeof(zeile),
                     "    %-12s %3u %4u %8lu ms  %3lu.%lu %%  %9lu  %7lu",
                     name ? name : "?", (unsigned)t, (unsigned)irk_task_get_prio(t),
                     (unsigned long)(zeit / 1000u),
                     (unsigned long)(promille / 10u), (unsigned long)(promille % 10u),
                     (unsigned long)laeufe,
                     (unsigned long)(laeufe ? zeit / laeufe : 0u));
            Serial.println(zeile);
        }

        uint32_t promille = (uint32_t)(summe * 1000u / fenster);
        snprintf(zeile, sizeof(zeile), "    %-21s %8lu ms  %3lu.%lu %%", "Summe",
                 (unsigned long)(summe / 1000u),
                 (unsigned long)(promille / 10u), (unsigned long)(promille % 10u));
        Serial.println(zeile);
        snprintf(zeile, sizeof(zeile), "    %-21s %8ld ms", "nicht zugerechnet",
                 (long)(fenster / 1000u) - (long)(summe / 1000u));
        Serial.println(zeile);
    }
}


/*========================================================================*\
 *  Die Bloecke
\*========================================================================*/

static void block_beschreiben(int b)
{
    switch (b) {
    case B_ANLAUF:
        Serial.println(F("  Beide Kerne starten ihren Scheduler selbst. Geprueft: feste"));
        Serial.println(F("  Haupttask-Handles, jede Task auf ihrem Kern, 16-Bit-Prioritaeten."));
        break;
    case B_QUEUE:
        Serial.println(F("  Kern 0 schickt alle 10 ms einen Messwert (zyklisch, nicht blockierend),"));
        Serial.println(F("  Kern 1 empfaengt blockierend. Geprueft: lueckenlos, in Reihenfolge."));
        break;
    case B_WECKEN:
        Serial.println(F("  Kern 0 gibt alle 500 ms ein Semaphor frei, auf das Kern 1 wartet."));
        Serial.println(F("  Die LED blitzt bei jeder Weckung blau."));
        break;
    case B_SEMA:
        Serial.println(F("  Je zwei Schreiber pro Kern aendern ein Konto unter einem Semaphor,"));
        Serial.println(F("  je einer ohne. Mit Semaphor darf nichts passieren -- ohne passiert es."));
        break;
    case B_FAIR:
        Serial.println(F("  Zwei Rechner auf Kern 1 ohne Pause, Prioritaet 256 und 768: Soll 1 : 3."));
        Serial.println(F("  Kern 0 haelt dabei seinen 10-ms-Takt."));
        break;
    case B_PRIO1:
        Serial.println(F("  Eine Task mit Prioritaet 1 blitzt weiss (irk_delay(2)) neben den Rechnern."));
        Serial.println(F("  Sie bekommt ~0,1 %: sie verhungert nicht, bleibt aber ~35 ms weiss --"));
        Serial.println(F("  irk_delay heisst 'fruehestens', und dran ist sie nach ihrem Anteil."));
        break;
    case B_ISR:
        Serial.println(F("  Ein Timer ruft alle 100 ms irk_sema_signal() aus dem Interrupt."));
        Serial.println(F("  Jeder Aufruf muss abgelehnt werden (IRK_ERR_IN_ISR)."));
        break;
    case B_ALLES:
        Serial.println(F("  Alle Tasks gleichzeitig. Am Ende die Rechenzeit jeder Task."));
        break;
    }
}

static void block_beginnen(int b)
{
    /* Maxima zuruecksetzen, solange die Tasks, die sie schreiben, ruhen */
    q_latenz_max         = 0;
    weck_latenz_max      = 0;
    takt_abw_max         = 0;
    weiss_min_us         = 0xFFFFFFFFu;
    weiss_max_us         = 0;
    blitz_abstand_max_us = 0;

    Serial.println();
    Serial.print(F("--- Block "));
    Serial.print(b);
    Serial.print(F("/7: "));
    Serial.print(block_name[b]);
    Serial.print(F(" ("));
    Serial.print((b == B_ANLAUF ? ANLAUF_MS : BLOCK_MS) / 1000);
    Serial.println(F(" s) ---"));
    block_beschreiben(b);

    blitz_max.dauer_us = 0;                         /* Blitz-Task ruht gerade */
    block_start_us     = irk_now_us();
    stand_nehmen(stand_a);
    block = b;
}

/* Der laengste Blitz des Blocks: wann, wie lange, und die Konten beim Ein-
   und Ausschalten. "Vorsprung" ist Konto Blitz minus kleineres Rechner-
   Konto, in Tausend.
     - Vorsprung beim Einschalten ~0 und beim Ausschalten ~0, aber lange
       weiss: die Rechner mussten nur den Zuwachs dieses einen Laufs
       aufholen -- die Dauer liegt dann an einem teuren Lauf.
     - Vorsprung beim Einschalten deutlich > 0: die Blitz-Task hatte schon
       Vorsprung und musste auf die Rechner warten.
     - led.show() lang: die LED-Ausgabe selbst hat gehangen. */
static void blitz_max_ausgeben(void)
{
    char zeile[100];
    const BlitzMax &m = blitz_max;

    if (m.dauer_us == 0) return;
    long vor_an  = (long)(((int64_t)m.blitz_an  - (int64_t)m.rechner_an)  / 1000);
    long vor_aus = (long)(((int64_t)m.blitz_aus - (int64_t)m.rechner_aus) / 1000);

    Serial.println(F("  Laengster Blitz im Block:"));
    snprintf(zeile, sizeof(zeile), "     ab Beginn %lu ms, weiss %lu us, Abstand %lu us, led.show() %lu us",
             (unsigned long)(m.seit_us / 1000u), (unsigned long)m.dauer_us,
             (unsigned long)m.abstand_us, (unsigned long)m.show_us);
    Serial.println(zeile);
    snprintf(zeile, sizeof(zeile), "     Einschalten:  Konto Blitz %lu k, Rechner %lu k, Vorsprung %ld k",
             (unsigned long)(m.blitz_an / 1000u), (unsigned long)(m.rechner_an / 1000u), vor_an);
    Serial.println(zeile);
    snprintf(zeile, sizeof(zeile), "     Ausschalten:  Konto Blitz %lu k, Rechner %lu k, Vorsprung %ld k",
             (unsigned long)(m.blitz_aus / 1000u), (unsigned long)(m.rechner_aus / 1000u), vor_aus);
    Serial.println(zeile);
}

static void auswerten_anlauf(void)
{
    pruefen(kern0_haupt == IRK_MAIN_TASK_OF(0) && kern0_id == 0,
            F("Kern 0 -- Haupttask-Handle (Soll 1)"), kern0_haupt);
    pruefen(kern1_haupt == IRK_MAIN_TASK_OF(1) && kern1_id == 1,
            F("Kern 1 -- Haupttask-Handle (Soll 2)"), kern1_haupt);
    pruefen(anlegefehler[0] + anlegefehler[1] == 0,
            F("alle Tasks angelegt -- Fehlschlaege"), (long)(anlegefehler[0] + anlegefehler[1]));

    bool orte = irk_task_core(t_erzeuger) == 0 && irk_task_core(t_empfaenger) == 1
             && irk_task_core(t_rechner1) == 1 && irk_task_core(t_rechner3) == 1
             && irk_task_core(t_blitz) == 1;
    pruefen(orte, F("jede Task auf dem Kern, der sie angelegt hat"), orte ? 1 : 0);

    pruefen(irk_task_get_prio(t_rechner3) == 3 * P_BASIS && irk_task_get_prio(t_blitz) == 1,
            F("16-Bit-Prioritaeten -- Rechner 3 (Soll 768)"), (long)irk_task_get_prio(t_rechner3));
}

static void auswerten_queue(void)
{
    uint32_t n = D(empfangen);
    pruefen(n >= 950 && n <= 1050, F("Werte in 10 s (Soll 1000)"), (long)n);
    pruefen(D(verworfen) + D(folgefehler) == 0,
            F("verloren oder vertauscht"), (long)(D(verworfen) + D(folgefehler)));
    pruefen(q_latenz_max < 20000, F("groesste Laufzeit durch die Queue (us)"), (long)q_latenz_max);
}

static void auswerten_wecken(void)
{
    uint32_t n = D(weckungen);
    pruefen(n >= 18 && n <= 21, F("Weckungen in 10 s (Soll 20)"), (long)n);
    pruefen(weck_latenz_max < 20000, F("groesste Weckverzoegerung (us)"), (long)weck_latenz_max);
}

static void auswerten_sema(void)
{
    pruefen(D(konto_verl) == 0, F("mit Semaphor -- Verletzungen"), (long)D(konto_verl));
    pruefen(D(beitrag0) > 100 && D(beitrag1) > 100,
            F("beide Kerne kommen ans Semaphor -- Buchungen Kern 0"), (long)D(beitrag0));
    info(F("Buchungen Kern 1"), (long)D(beitrag1));
    info(F("OHNE Semaphor -- Verletzungen (zeigt, warum man es braucht)"), (long)D(frei_verl));
}

static void auswerten_fair(void)
{
    long v = verhaeltnis_rechner();
    pruefen(v >= 285 && v <= 315,
            F("Rechenzeit Prio 768 : Prio 256 (x100, Soll 300)"), v);
    info(F("Rechenzeit Prio 256 (ms)"), (long)(zeit_im_block(t_rechner1) / 1000u));
    info(F("Rechenzeit Prio 768 (ms)"), (long)(zeit_im_block(t_rechner3) / 1000u));
    pruefen(takt_abw_max < 5000, F("10-ms-Takt auf Kern 0 -- groesste Abweichung (us)"),
            (long)takt_abw_max);
}

static void auswerten_prio1(void)
{
    uint32_t n = D(blitze);
    pruefen(n >= 20, F("Prio-1-Task verhungert nicht -- Blitze in 10 s"), (long)n);
    pruefen(n > 0 && weiss_min_us >= 2000,
            F("kuerzeste Weiss-Dauer (us, Soll >= 2000)"), (long)weiss_min_us);
    info(F("laengste Weiss-Dauer (us, erwartet um 35000)"), (long)weiss_max_us);
    info(F("groesster Abstand zweier Blitze (us)"), (long)blitz_abstand_max_us);
    info(F("Rechenzeit-Anteil (ppm, erwartet um 950)"), ppm_im_block(t_blitz));
    info(F("Rechner dabei weiter Prio 768 : 256 (x100)"), verhaeltnis_rechner());
    blitz_max_ausgeben();
}

static void auswerten_isr(void)
{
    uint32_t n = D(isr_versuche);
    pruefen(n >= 90 && D(isr_durch) == 0 && D(fehler_isr) == n && irk_sema_count(SEM_ISR) == 0,
            F("alle Aufrufe aus dem Interrupt abgelehnt -- Versuche"), (long)n);
    info(F("abgelehnt mit IRK_ERR_IN_ISR"), (long)D(fehler_isr));
}

static void auswerten_alles(void)
{
    uint32_t q = D(empfangen);
    pruefen(q >= 950 && q <= 1050 && D(verworfen) + D(folgefehler) == 0,
            F("Queue -- Werte, lueckenlos (Soll 1000)"), (long)q);
    pruefen(D(weckungen) >= 18, F("Wecken -- Weckungen (Soll 20)"), (long)D(weckungen));
    pruefen(D(konto_verl) == 0, F("Semaphor -- Verletzungen"), (long)D(konto_verl));
    info(F("ohne Semaphor -- Verletzungen"), (long)D(frei_verl));

    long v = verhaeltnis_rechner();
    pruefen(v >= 285 && v <= 315, F("Fair Share -- Prio 768 : 256 (x100, Soll 300)"), v);
    pruefen(takt_abw_max < 5000, F("Takt Kern 0 -- groesste Abweichung (us)"), (long)takt_abw_max);

    uint32_t n = D(blitze);
    pruefen(n >= 5 && weiss_min_us >= 2000, F("Prio-1-Task -- Blitze"), (long)n);
    blitz_max_ausgeben();

    uint32_t i = D(isr_versuche);
    pruefen(i >= 90 && D(isr_durch) == 0 && D(fehler_isr) == i,
            F("Interrupt -- alle Aufrufe abgelehnt"), (long)i);

    pruefen(stand_e.kern_verl == 0,
            F("Tasks blieben die ganze Zeit auf ihrem Kern -- Verletzungen"), (long)stand_e.kern_verl);

    profil_ausgeben();
}

static void block_auswerten(int b)
{
    pruef_block = b;
    Serial.println(F("  Ergebnis:"));

    switch (b) {
        case B_ANLAUF: auswerten_anlauf(); break;
        case B_QUEUE:  auswerten_queue();  break;
        case B_WECKEN: auswerten_wecken(); break;
        case B_SEMA:   auswerten_sema();   break;
        case B_FAIR:   auswerten_fair();   break;
        case B_PRIO1:  auswerten_prio1();  break;
        case B_ISR:    auswerten_isr();    break;
        case B_ALLES:  auswerten_alles();  break;
    }
    kernelfehler_pruefen();
}

static void zusammenfassung(void)
{
    char zeile[80];
    int  ok = 0, fehler = 0;

    Serial.println();
    Serial.println(F("=== Zusammenfassung ==="));
    for (int b = 0; b < B_ANZAHL; b++) {
        snprintf(zeile, sizeof(zeile), "  Block %d  %-36s %2d OK  %2d Fehler",
                 b, block_name[b], blk_ok[b], blk_fehler[b]);
        Serial.println(zeile);
        ok     += blk_ok[b];
        fehler += blk_fehler[b];
    }
    Serial.println();
    Serial.print(F("=== "));
    Serial.print(ok);
    Serial.print(F(" bestanden, "));
    Serial.print(fehler);
    Serial.println(F(" Fehler ==="));
    Serial.println(F("(Test beendet -- Reset startet ihn neu.)"));

    ergebnis = fehler ? 2 : 1;
    block    = B_ENDE;                              /* LED gruen oder rot */
}

/* Wartet ms Millisekunden und gibt dabei die Berichte von Kern 1 aus. */
static void warten_und_ausgeben(uint32_t ms)
{
    uint32_t start = millis();
    Bericht  b;

    do {
        while (irk_queue_try_recv(&q_bericht, &b) == 1) Serial.println(b.text);
        irk_delay(20);
    } while (millis() - start < ms);
    while (irk_queue_try_recv(&q_bericht, &b) == 1) Serial.println(b.text);
}


/*========================================================================*\
 *  Kern 0
\*========================================================================*/

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 3000) delay(10);

    Serial.println(F("IRKernel -- zwei Kerne, ein Kernel"));
    Serial.println(F("8 Bloecke, je 10 s (Anlauf 2 s), dann die Zusammenfassung."));

    /* irk_init() waere nicht noetig -- der erste Aufruf startet den
       Scheduler von Kern 0 mit Prioritaet 1 fuer die Haupttask. Hier soll
       aber nur die Blitz-Task Prioritaet 1 haben, also P_BASIS. */
    irk_init(P_BASIS);
    kern0_haupt = irk_task_self();
    kern0_id    = irk_core_id();

    /* Gemeinsame Objekte VOR allem anderen einrichten -- Kern 1 wartet
       darauf (siehe setup1). */
    irk_sema_init(SEM_KONTO, 1);
    irk_sema_init(SEM_WECK,  0);
    irk_sema_init(SEM_ISR,   0);
    irk_queue_init(&q_mess,    q_mess_speicher,    sizeof(Messwert), 8);
    irk_queue_init(&q_bericht, q_bericht_speicher, sizeof(Bericht),  4);

    t_erzeuger = anlegen(erzeuger, 2 * P_BASIS, stk_erzeuger, "erzeuger");
    irk_task_set_cyclic(t_erzeuger, 10);

    irk_task_t t = anlegen(ausloeser, P_BASIS, stk_ausloeser, "ausloeser");
    irk_task_set_cyclic(t, 500);

    anlegen(schreiber0, P_BASIS, stk_schreiber0a, "schreib0a");
    anlegen(schreiber0, P_BASIS, stk_schreiber0b, "schreib0b");
    anlegen(frei_schreiber0, P_BASIS, stk_frei0, "frei0");

    add_repeating_timer_ms(100, isr_rueckruf, NULL, &isr_timer);

    bereit = true;
}

void loop()
{
    /* Die Haupttask von Kern 0 fuehrt den Ablauf und ist die einzige, die
       auf die serielle Schnittstelle schreibt. Kern 1 schickt ihr seine
       Texte per Queue. */
    static bool fertig = false;

    if (!fertig) {
        for (int b = B_ANLAUF; b <= B_ALLES; b++) {
            block_beginnen(b);
            warten_und_ausgeben(b == B_ANLAUF ? ANLAUF_MS : BLOCK_MS);
            stand_nehmen(stand_e);
            block = B_PAUSE;                        /* Ruhe waehrend der Ausgabe */
            warten_und_ausgeben(50);                /* letzte Berichte abholen   */
            block_auswerten(b);
        }
        zusammenfassung();
        fertig = true;
    }
    warten_und_ausgeben(1000);
}


/*========================================================================*\
 *  Kern 1
\*========================================================================*/

void setup1()
{
    /* setup() und setup1() laufen gleichzeitig los. Erst weitermachen,
       wenn Kern 0 Semaphore und Queues eingerichtet hat. */
    while (!bereit) delay(1);

    irk_init(P_BASIS);                              /* startet Kern 1 */
    kern1_haupt = irk_task_self();
    kern1_id    = irk_core_id();

    led.begin();

    t_empfaenger = anlegen(empfaenger, 2 * P_BASIS, stk_empfaenger, "empfaenger");
    anlegen(wecker, 2 * P_BASIS, stk_wecker, "wecker");
    anlegen(schreiber1, P_BASIS, stk_schreiber1a, "schreib1a");
    anlegen(schreiber1, P_BASIS, stk_schreiber1b, "schreib1b");
    anlegen(frei_schreiber1, P_BASIS, stk_frei1, "frei1");

    t_rechner1 = anlegen(rechnen1, 1 * P_BASIS, stk_rechner1, "rechner1");
    t_rechner3 = anlegen(rechnen3, 3 * P_BASIS, stk_rechner3, "rechner3");

    irk_task_t t = anlegen(led_task, P_BASIS, stk_led, "led");
    irk_task_set_cyclic(t, 20);

    t_blitz = anlegen(blitz_task, 1, stk_blitz, "blitz");  /* absolut niedrig */
}

void loop1()
{
    /* Jede Sekunde ein Bericht zum laufenden Block, gezaehlt ab Blockbeginn
       (stand_a nimmt Kern 0, bevor er den Block setzt). */
    static uint64_t z1_alt = 0, z3_alt = 0;

    irk_delay(1000);

    uint64_t z1 = (uint64_t)irk_task_runtime_us(t_rechner1);
    uint64_t z3 = (uint64_t)irk_task_runtime_us(t_rechner3);
    uint64_t d1 = z1 - z1_alt, d3 = z3 - z3_alt;
    uint32_t x100 = d1 ? (uint32_t)(d3 * 100u / d1) : 0;
    z1_alt = z1;
    z3_alt = z3;

    Bericht b;
    int     x = block;

    switch (x) {
    case B_QUEUE:
        snprintf(b.text, sizeof(b.text), "  [Kern 1] Messwerte im Block %lu, groesste Laufzeit %lu us",
                 (unsigned long)(q_empfangen - stand_a.empfangen), (unsigned long)q_latenz_max);
        break;
    case B_WECKEN:
        snprintf(b.text, sizeof(b.text), "  [Kern 1] Weckungen im Block %lu, groesste Verzoegerung %lu us",
                 (unsigned long)(weckungen - stand_a.weckungen), (unsigned long)weck_latenz_max);
        break;
    case B_SEMA:
        snprintf(b.text, sizeof(b.text), "  [Kern 1] Buchungen Kern 0: %lu  Kern 1: %lu  ohne Semaphor verletzt: %lu",
                 (unsigned long)(beitrag[0] - stand_a.beitrag0),
                 (unsigned long)(beitrag[1] - stand_a.beitrag1),
                 (unsigned long)(frei_verletzungen[0] + frei_verletzungen[1] - stand_a.frei_verl));
        break;
    case B_FAIR:
        snprintf(b.text, sizeof(b.text), "  [Kern 1] Rechenzeit Prio 256 : 768 = 1 : %lu.%02lu",
                 (unsigned long)(x100 / 100u), (unsigned long)(x100 % 100u));
        break;
    case B_PRIO1:
        snprintf(b.text, sizeof(b.text), "  [Kern 1] Blitze im Block %lu, laengste Weiss-Dauer %lu us",
                 (unsigned long)(blitze - stand_a.blitze), (unsigned long)weiss_max_us);
        break;
    case B_ISR:
        snprintf(b.text, sizeof(b.text), "  [Kern 1] Interrupt-Versuche %lu, abgelehnt %lu",
                 (unsigned long)(isr_versuche - stand_a.isr_versuche),
                 (unsigned long)(fehler_isr - stand_a.fehler_isr));
        break;
    case B_ALLES:
        snprintf(b.text, sizeof(b.text), "  [Kern 1] Messw. %lu  Weck. %lu  Blitze %lu  Rechenzeit 1 : %lu.%02lu",
                 (unsigned long)(q_empfangen - stand_a.empfangen),
                 (unsigned long)(weckungen - stand_a.weckungen),
                 (unsigned long)(blitze - stand_a.blitze),
                 (unsigned long)(x100 / 100u), (unsigned long)(x100 % 100u));
        break;
    default:
        return;                                     /* Anlauf, Pause, Ende */
    }
    irk_queue_send(&q_bericht, &b);                 /* Kern 0 gibt es aus */
}
