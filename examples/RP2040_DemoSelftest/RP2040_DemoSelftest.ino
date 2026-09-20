/*========================================================================*\
 *
 *  RP2040_DemoSelftest  --  IRKernel auf dem RP2040 zum Abschauen
 *
 *  Ein Sketch, der alles Wesentliche an einer Stelle zeigt und sich am
 *  Ende selbst prueft. Gedacht als Vorlage: Kapitel heraussuchen,
 *  abschauen, in das eigene Projekt uebernehmen.
 *
 *  Board:   Raspberry Pi Pico / Pico W / Waveshare RP2040 / RP2350
 *  Kern:    arduino-pico (Earle Philhower)
 *  Monitor: 115200 Baud
 *
 *  ----------------------------------------------------------------------
 *  ARCHITEKTUR DIESES BEISPIELS
 *  ----------------------------------------------------------------------
 *
 *     Core 0                                     Core 1
 *     ======                                     ======
 *     IRKernel mit allen Tasks                   setup1() / loop1()
 *       - drei Zaehler     (Fair Share 1:2:3)    "Messdatenerfassung":
 *       - Regler           (zyklisch, 10 ms)       erzeugt alle 10 ms
 *       - zwei Schreiber   (Semaphor)              einen Messwert
 *       - ein Pruefer      (Semaphor)
 *       - Erzeuger/Verbr.  (IRKernel-Queue)     <-- rp2040.fifo  (32 Bit)
 *       - Core-1-Empfaenger                     <-- queue_t      (Struktur)
 *       - Stapelfresser    (Stack-Fruehwarnung)
 *       - sechs LED-Nocken (zyklisch mit Startversatz)
 *       - loop() = Haupttask, einzige mit Serial
 *
 *  Grundregeln, die sich daraus ergeben:
 *
 *   1. IRKernel laeuft NUR auf Core 0. Aus loop1() niemals irk_*-Funktionen
 *      aufrufen -- der Kernel ist nicht fuer zwei Kerne abgesichert.
 *
 *   2. Die Kerne reden nur ueber die dafuer gebauten Kanaele miteinander
 *      (rp2040.fifo, queue_t). Nicht ueber gemeinsame Variablen, ausser
 *      fuer einfache Zaehler zur Anzeige.
 *
 *   3. Auf Core 0 wird NIE blockierend auf Core 1 gewartet. Blockiert eine
 *      IRKernel-Task, blockiert der ganze Kern -- kooperativ heisst, niemand
 *      kommt dran, solange nicht abgegeben wird. Also immer die
 *      nicht-blockierenden Varianten plus irk_delay().
 *
 *   4. Nur eine Task benutzt Serial (hier: loop()). Das spart Stack in allen
 *      anderen Tasks und verhindert zerhackte Ausgaben.
 *
\*========================================================================*/

#include <IRKernel.h>
#include <pico/util/queue.h>        /* queue_t aus dem Pico-SDK */
#include <Adafruit_NeoPixel.h>      /* RGB-LED des RP2040-Zero */

/* Datentyp fuer die Uebertragung Core 1 -> Core 0 (Kapitel 6).
   Steht bewusst GANZ OBEN: die Arduino-IDE erzeugt fuer .ino-Dateien
   automatisch Funktionsprototypen und setzt sie vor die erste Funktion.
   Ein Prototyp wie pruefsumme(const Messung&) braucht den Typ dann
   schon -- steht die Struktur weiter unten, bricht die Uebersetzung ab. */
struct Messung {
    uint32_t nr;
    int32_t  wert;
    uint32_t pruefsumme;
};

static inline uint32_t pruefsumme(const Messung &m)
{
    return m.nr ^ (uint32_t)m.wert ^ 0xA5A5A5A5u;
}

#if !defined(ARDUINO_ARCH_RP2040)
#  error "Dieses Beispiel ist fuer RP2040/RP2350 mit dem arduino-pico-Kern."
#endif


/* ----------------------------------------------------------------------
   RGB-LED  (Waveshare RP2040-Zero: WS2812 an GPIO 16)

   Steht bewusst UNTER struct Messung: hier folgen Funktionen, und
   vor die erste Funktion setzt die Arduino-IDE ihre automatisch erzeugten
   Prototypen. Stuenden sie weiter oben, kaeme der Prototyp von
   pruefsumme() wieder vor die Struktur.

   PIN_NEOPIXEL definiert nur die Boardvariante "Waveshare RP2040 Zero".
   Ist ein anderes Board eingestellt, wird GPIO 16 angenommen.
   Farben vertauscht? Dann NEO_RGB statt NEO_GRB.
   ---------------------------------------------------------------------- */

#ifndef PIN_NEOPIXEL
#  define PIN_NEOPIXEL 16
#endif

Adafruit_NeoPixel led(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

/* ----------------------------------------------------------------------
   Die LED als Nockenschaltwerk

   Je Farbe zwei zyklische Tasks mit GLEICHER Periode: die "an"-Task
   startet sofort, die "aus"-Task um die Leuchtdauer versetzt. Beide
   haengen am selben Raster (Aufruf + Versatz + k * Periode) und koennen
   nicht gegeneinander driften.

       Farbe   Periode   an nach   aus nach
       gruen   1000 ms      0        500 ms
       rot     3700 ms      0        700 ms
       blau    7000 ms      0        200 ms

   Die drei Farben laufen voellig unabhaengig voneinander; das Gesamtmuster
   wiederholt sich erst nach 259 Sekunden.

   Muster jeder Nocke: EINEN kurzen Schritt tun, dann irk_yield() -- das
   heisst bei einer zyklischen Task "schlafen bis zum naechsten Takt".
   Kein irk_delay() in zyklischen Tasks.

   Jede Task setzt nur ihren eigenen Farbanteil. led_ausgeben() setzt die
   Anteile zusammen. Einen Mutex braucht es nicht: led_ausgeben() gibt
   nirgends die CPU ab, also kann niemand dazwischenfunken.
   ---------------------------------------------------------------------- */

#define LED_HELL  20                    /* Helligkeit je Farbanteil (0..255) */

static volatile uint8_t led_r = 0, led_g = 0, led_b = 0;

static void led_ausgeben(void)
{
    led.setPixelColor(0, led.Color(led_r, led_g, led_b));
    led.show();
}

static void gruen_an(void)  { for (;;) { led_g = LED_HELL; led_ausgeben(); irk_yield(); } }
static void gruen_aus(void) { for (;;) { led_g = 0;        led_ausgeben(); irk_yield(); } }
static void rot_an(void)    { for (;;) { led_r = LED_HELL; led_ausgeben(); irk_yield(); } }
static void rot_aus(void)   { for (;;) { led_r = 0;        led_ausgeben(); irk_yield(); } }
static void blau_an(void)   { for (;;) { led_b = LED_HELL; led_ausgeben(); irk_yield(); } }
static void blau_aus(void)  { for (;;) { led_b = 0;        led_ausgeben(); irk_yield(); } }


/*========================================================================*\
 *  KAPITEL 1  --  Stacks
 *
 *  Jede IRKernel-Task braucht ihren eigenen Stack. Auf dem RP2040 gilt:
 *  nicht unter ~400 Byte, auch fuer triviale Tasks. Grund: die Tasks
 *  laufen auf dem Haupt-Stackpointer (MSP), und jeder Interrupt -- USB,
 *  Timer, bei Pico W auch WLAN -- legt seinen Rahmen auf den Stack der
 *  Task, die gerade laeuft. Mit WLAN lieber 1024 Byte und mehr.
 *
 *  RAM ist auf dem RP2040 reichlich da (264 KB), also nicht geizen.
\*========================================================================*/

#define STK  1024

static uint8_t stk_z1[STK], stk_z2[STK], stk_z3[STK];
static uint8_t stk_regler[STK];
static uint8_t stk_schreibA[STK], stk_schreibB[STK], stk_pruefer[STK];
static uint8_t stk_qprod[STK], stk_qcons[STK];
static uint8_t stk_core1rx[STK];

/* Handles merken, um spaeter Stackreserve und Laufzeit abzufragen */
struct TaskInfo { irk_task_t h; const char *name; };
static TaskInfo taskliste[20];
static uint8_t  taskanzahl = 0;

static irk_task_t anlegen(void (*fn)(void), irk_prio_t prio,
                          uint8_t *stack, const char *name)
{
    irk_task_t h = irk_task_create(fn, prio, stack, STK, name);
    if (taskanzahl < 20) taskliste[taskanzahl++] = { h, name };
    return h;
}


/*========================================================================*\
 *  KAPITEL 2  --  Fair Share
 *
 *  Drei Tasks zaehlen, so schnell sie duerfen. Prioritaet = ANTEIL an der
 *  Rechenzeit, nicht Vorrang: Prio 3 bekommt dreimal so viel wie Prio 1,
 *  verdraengt sie aber nicht. Niemand verhungert.
 *
 *  Beachte: irk_yield() in einer Schleife ohne echte Arbeit gibt die CPU
 *  erst nach Ablauf der Mindest-Zeitscheibe (1 ms) wirklich ab. Fuer
 *  "rechne, so viel du kannst" ist das genau richtig. Fuer "warte auf
 *  etwas" dagegen irk_delay() oder ein Semaphor nehmen.
\*========================================================================*/

static volatile uint32_t zaehler[3];

static void zaehler1(void) { for (;;) { zaehler[0]++; irk_yield(); } }
static void zaehler2(void) { for (;;) { zaehler[1]++; irk_yield(); } }
static void zaehler3(void) { for (;;) { zaehler[2]++; irk_yield(); } }


/*========================================================================*\
 *  KAPITEL 3  --  Zyklische Task: der Regeltakt
 *
 *  Eine zyklische Task laeuft nicht nach Anteilen, sondern nach der Uhr:
 *  alle PERIODE_MS genau einmal, und dann VOR allen anderen Tasks.
 *  Zwischen den Aktivierungen belegt sie keine Rechenzeit.
 *
 *  Muster:  for (;;) { ein Regelschritt;  irk_yield(); }
 *           irk_yield() heisst hier "schlafen bis zur naechsten Periode".
 *
 *  Wichtig: der Regelschritt muss deutlich kuerzer sein als die Periode,
 *  sonst gibt es nichts mehr, was man verteilen koennte.
\*========================================================================*/

#define PERIODE_MS  10

static volatile uint32_t regler_takte   = 0;
static volatile uint32_t regler_jitter_us = 0;   /* groesste Abweichung */
static volatile float    regler_istwert = 0.0f;

static void regler(void)
{
    uint32_t letzter = 0;
    float    sollwert = 100.0f;

    for (;;) {
        /* --- Jitter messen: wie genau trifft der Takt? --- */
        uint32_t jetzt = micros();
        if (letzter != 0 && regler_takte > 5) {          /* Anlauf ignorieren */
            int32_t abw = (int32_t)(jetzt - letzter) - PERIODE_MS * 1000;
            if (abw < 0) abw = -abw;
            if ((uint32_t)abw > regler_jitter_us) regler_jitter_us = (uint32_t)abw;
        }
        letzter = jetzt;
        regler_takte++;

        /* --- Der eigentliche Regelschritt ---
           Hier nur zur Anschauung: P-Regler auf eine simulierte Strecke
           erster Ordnung. In einem echten Projekt stuende hier: Istwert
           lesen, Regelgesetz, Stellgroesse ausgeben. */
        float fehler      = sollwert - regler_istwert;
        float stellgroesse = 2.0f * fehler;
        regler_istwert   += (stellgroesse - regler_istwert) * 0.02f;

        irk_yield();          /* schlafen bis zur naechsten Periode */
    }
}


/*========================================================================*\
 *  KAPITEL 4  --  Semaphor: wann man eins braucht
 *
 *  Kooperativ heisst: zwischen zwei irk_yield() funkt niemand dazwischen.
 *  Einfache Variablen brauchen deshalb KEINEN Schutz.
 *
 *  Ein Semaphor wird erst noetig, wenn eine zusammengehoerige Aenderung
 *  ueber ein irk_yield() hinweg reicht. Hier: ein Wertepaar mit der
 *  Invariante  b == 2 * a.  Die Schreiber aendern a, geben ABSICHTLICH
 *  mittendrin ab und setzen danach b. Ohne Semaphor saehe der Pruefer
 *  dazwischen einen inkonsistenten Zustand.
\*========================================================================*/

#define SEM_PAAR  0

static struct { uint32_t a; uint32_t b; } paar = { 0, 0 };

static volatile uint32_t sem_geprueft = 0;
static volatile uint32_t sem_verletzt = 0;

static void schreiber(void)
{
    for (;;) {
        irk_sema_wait(SEM_PAAR);        /* Paar exklusiv belegen          */
        paar.a++;
        irk_yield();                    /* absichtlich mitten in der
                                           Aenderung abgeben              */
        paar.b = paar.a * 2u;
        irk_sema_signal(SEM_PAAR);      /* freigeben                      */

        irk_delay(2);
    }
}

static void pruefer(void)
{
    for (;;) {
        irk_sema_wait(SEM_PAAR);
        if (paar.b != paar.a * 2u) sem_verletzt++;
        sem_geprueft++;
        irk_sema_signal(SEM_PAAR);

        irk_delay(1);
    }
}


/*========================================================================*\
 *  KAPITEL 5  --  IRKernel-Queue: Kommunikation zwischen Tasks auf Core 0
 *
 *  Den Speicher stellt der Aufrufer, die Library allokiert nichts.
 *  irk_queue_send() blockiert, wenn die Queue voll ist,
 *  irk_queue_recv() blockiert, wenn sie leer ist -- beides gibt dabei die
 *  CPU ab, die anderen Tasks laufen weiter.
 *
 *  Hier ist der Erzeuger absichtlich schneller als der Verbraucher. Die
 *  Queue laeuft voll, und der Erzeuger wird automatisch gebremst.
\*========================================================================*/

static irk_queue_t        iq;
static uint16_t           iq_speicher[8];

static volatile uint32_t  iq_gesendet   = 0;
static volatile uint32_t  iq_empfangen  = 0;
static volatile uint32_t  iq_reihenfolge_falsch = 0;

static void q_erzeuger(void)
{
    uint16_t nr = 0;
    for (;;) {
        irk_queue_send(&iq, &nr);       /* blockiert bei voller Queue */
        iq_gesendet++;
        nr++;
        irk_delay(1);
    }
}

static void q_verbraucher(void)
{
    uint16_t erwartet = 0, wert;
    for (;;) {
        irk_queue_recv(&iq, &wert);     /* blockiert bei leerer Queue */
        if (wert != erwartet) iq_reihenfolge_falsch++;
        erwartet = (uint16_t)(wert + 1u);
        iq_empfangen++;
        irk_delay(3);                   /* absichtlich langsamer */
    }
}


/*========================================================================*\
 *  KAPITEL 6  --  Core 1: Messdatenerfassung
 *
 *  setup1() und loop1() laufen auf dem zweiten Kern, voellig unabhaengig
 *  von IRKernel. Hier darf ganz normal delay() stehen.
 *
 *  Zwei Wege zu Core 0:
 *
 *   rp2040.fifo   Hardware-Postfach, transportiert einzelne 32-Bit-Werte.
 *                 push_nb()/pop_nb() blockieren nie.
 *                 ACHTUNG: arduino-pico benutzt die FIFO intern selbst,
 *                 z.B. fuer rp2040.idleOtherCore() beim Flash-Schreiben
 *                 (EEPROM, LittleFS). Wer das nutzt, nimmt fuer eigene
 *                 Daten besser queue_t.
 *
 *   queue_t       Aus dem Pico-SDK. Transportiert ganze Strukturen und ist
 *                 zwischen den Kernen abgesichert. queue_try_add() und
 *                 queue_try_remove() blockieren nie.
\*========================================================================*/

/* struct Messung und pruefsumme() stehen ganz oben, siehe dort. */

static queue_t              core1_queue;
static volatile bool        core1_queue_bereit = false;

/* Nur zur Anzeige von Core 1 beschrieben, von Core 0 gelesen. Einzelne
   32-Bit-Werte lesen ist auf dem RP2040 unkritisch; fuer echte Daten die
   Kanaele oben benutzen. */
static volatile uint32_t    core1_fifo_verworfen  = 0;
static volatile uint32_t    core1_queue_verworfen = 0;

void setup1()
{
    /* Warten, bis Core 0 die Queue eingerichtet hat. setup() und setup1()
       laufen gleichzeitig los -- die Reihenfolge ist nicht garantiert. */
    while (!core1_queue_bereit) delay(1);
}

void loop1()
{
    static uint32_t nr = 0;
    nr++;

    /* Weg 1: nur die laufende Nummer ueber die Hardware-FIFO */
    if (!rp2040.fifo.push_nb(nr)) core1_fifo_verworfen++;

    /* Weg 2: eine ganze Messung ueber queue_t */
    Messung m;
    m.nr         = nr;
    m.wert       = (int32_t)(nr * 7u) - 1000;
    m.pruefsumme = pruefsumme(m);
    if (!queue_try_add(&core1_queue, &m)) core1_queue_verworfen++;

    delay(10);                          /* 100 Messungen pro Sekunde */
}


/*========================================================================*\
 *  KAPITEL 7  --  Core 0 empfaengt von Core 1
 *
 *  Eine ganz normale IRKernel-Task. Sie leert beide Kanaele, OHNE zu
 *  blockieren, und gibt danach mit irk_delay() die CPU ab.
 *
 *  Die Wartezeit bestimmt die Latenz: bei 5 ms und 100 Messungen pro
 *  Sekunde liegen nie mehr als ein, zwei Werte an. Die FIFO fasst nur
 *  wenige Eintraege -- wer seltener abholt, riskiert Verluste.
\*========================================================================*/

static volatile uint32_t rx_fifo_anzahl  = 0;
static volatile uint32_t rx_fifo_luecken = 0;
static volatile uint32_t rx_queue_anzahl = 0;
static volatile uint32_t rx_queue_kaputt = 0;
static volatile int32_t  rx_letzter_wert = 0;

static void core1_empfaenger(void)
{
    uint32_t letzte_nr = 0;

    for (;;) {
        /* --- Weg 1: FIFO leeren --- */
        uint32_t w;
        while (rp2040.fifo.pop_nb(&w)) {
            if (letzte_nr != 0 && w != letzte_nr + 1u) rx_fifo_luecken++;
            letzte_nr = w;
            rx_fifo_anzahl++;
        }

        /* --- Weg 2: queue_t leeren --- */
        Messung m;
        while (queue_try_remove(&core1_queue, &m)) {
            if (m.pruefsumme != pruefsumme(m)) rx_queue_kaputt++;
            rx_letzter_wert = m.wert;
            rx_queue_anzahl++;
        }

        irk_delay(5);                   /* nichts mehr da -> abgeben */
    }
}


/*========================================================================*\
 *  KAPITEL 8  --  Pico W: WLAN und Bluetooth  (nur als Hinweis)
 *
 *  Nicht der RP2040 funkt, sondern der CYW43439 daneben -- mit eigenen
 *  Prozessoren und eigener Firmware. Auf dem RP2040 laufen nur Treiber,
 *  lwIP (TCP/IP) und BTstack (Bluetooth), und zwar auf dem Kern, der
 *  WiFi.begin() aufruft.
 *
 *  Im arduino-pico-Kern wird der Treiber interruptgesteuert im Hintergrund
 *  bedient. Man muss also nichts pollen. Fuer IRKernel heisst das:
 *
 *    - WLAN-Funktionen nur von Core 0 aus aufrufen, am besten aus EINER
 *      Task ("Netzwerktask").
 *    - Stacks aller Tasks groesser machen (>= 1024 Byte), weil die
 *      Hintergrund-Interrupts auf dem jeweils laufenden Taskstack landen.
 *    - Blockierende Netzwerkaufrufe (connect, readBytes mit Timeout, ...)
 *      halten den ganzen Kern an. Lieber available() abfragen und dazwischen
 *      irk_delay() -- dasselbe Muster wie in Kapitel 7.
 *
 *  Skizze einer Netzwerktask:
 *
 *      #include <WiFi.h>
 *      static void netzwerk(void) {
 *          WiFi.begin("SSID", "Passwort");
 *          while (WiFi.status() != WL_CONNECTED) irk_delay(100);
 *          WiFiServer server(80);
 *          server.begin();
 *          for (;;) {
 *              WiFiClient c = server.accept();
 *              if (c) { ... kurz bedienen ...; c.stop(); }
 *              irk_delay(10);
 *          }
 *      }
\*========================================================================*/


/*========================================================================*\
 *  KAPITEL 9  --  Stack-Fruehwarnung
 *
 *  irk_stack_watch(schwelle, rueckruf) meldet jede Task, deren nie
 *  benutzter Stackrest unter die Schwelle faellt -- BEVOR er ueberlaeuft.
 *  Je Task genau einmal.
 *
 *  Der Rueckruf laeuft im Scheduler, auf dem Stack der gerade laufenden
 *  Task, und das kann genau die knappe sein. Deshalb hier nur MERKEN,
 *  die Ausgabe macht loop() (siehe unten). Kein Serial, kein irk_delay().
 *
 *  Zum Nachweis auf echter Hardware frisst sich eine Task absichtlich
 *  rekursiv in ihren Stack, bis nur noch rund 240 Byte frei sind. Das
 *  liegt unter der Warnschwelle von 256 Byte, aber weit vor einem echten
 *  Ueberlauf. Erwartet: genau diese Task wird gemeldet, keine andere.
\*========================================================================*/

#define STACK_WARNSCHWELLE  256

static volatile irk_task_t warn_task[8];
static volatile uint16_t   warn_frei[8];
static volatile uint8_t    warn_anzahl = 0;

static void stack_alarm(irk_task_t tsk, size_t frei)
{
    /* Laeuft im Scheduler: nur merken, nichts ausgeben. */
    if (warn_anzahl < 8) {
        warn_task[warn_anzahl] = tsk;
        warn_frei[warn_anzahl] = (uint16_t)frei;
        warn_anzahl++;
    }
}

static uint8_t           stk_fresser[STK];
static irk_task_t        t_fresser     = 0;
static volatile uint8_t  fresser_los   = 0;
static volatile uint16_t fresser_tiefe = 0;

/* Jede Ebene belegt einen kleinen Puffer und steigt tiefer, bis die
   Reserve unter 240 Byte faellt. Danach sofort zurueck -- nicht in der
   Tiefe verweilen, damit kein Interrupt den Rest aufbraucht. */
static uint16_t fressen(uint16_t tiefe)
{
    volatile uint8_t puffer[48];
    for (uint8_t i = 0; i < sizeof(puffer); i++) puffer[i] = i;

    if (irk_stack_free(irk_task_self()) < 240 || tiefe > 200) return tiefe;

    uint16_t erreicht = fressen((uint16_t)(tiefe + 1u));
    return (uint16_t)(erreicht + (puffer[0] & 0u));   /* verhindert Endrekursion */
}

static void stapelfresser(void)
{
    for (;;) {
        if (fresser_los && fresser_tiefe == 0) fresser_tiefe = fressen(1);
        irk_delay(20);
    }
}

static const char *taskname(irk_task_t h)
{
    for (uint8_t i = 0; i < taskanzahl; i++)
        if (taskliste[i].h == h) return taskliste[i].name;
    return "?";
}


/*========================================================================*\
 *  KAPITEL 9  --  Selbsttest und Anzeige
\*========================================================================*/

#define MESSDAUER_S  5                  /* ueber so viele Sekunden werten */

static uint32_t mess_z0[3], mess_takte0;
static uint16_t geprueft = 0, fehler = 0;

static void pruefe(bool ok, const char *text)
{
    geprueft++;
    if (!ok) fehler++;
    Serial.print(ok ? F("  ok      ") : F("  FEHLER  "));
    Serial.println(text);
}

static void status_ausgeben(uint32_t sekunde)
{
    Serial.print(F("[")); Serial.print(sekunde); Serial.print(F(" s]  "));
    Serial.print(F("Zaehler "));
    Serial.print(zaehler[0]); Serial.print('/');
    Serial.print(zaehler[1]); Serial.print('/');
    Serial.print(zaehler[2]);
    Serial.print(F("   Regler ")); Serial.print(regler_takte);
    Serial.print(F(" Takte, Istwert ")); Serial.print(regler_istwert, 1);
    Serial.print(F("   Core1 fifo=")); Serial.print(rx_fifo_anzahl);
    Serial.print(F(" queue=")); Serial.print(rx_queue_anzahl);
    Serial.print(F(" letzter Wert=")); Serial.println(rx_letzter_wert);
}

static void auswerten(void)
{
    uint32_t d[3];
    for (int i = 0; i < 3; i++) d[i] = zaehler[i] - mess_z0[i];
    uint32_t takte = regler_takte - mess_takte0;

    Serial.println();
    Serial.println(F("========================================"));
    Serial.println(F(" Selbsttest"));
    Serial.println(F("========================================"));

    /* --- Fair Share --- */
    float v2 = d[0] ? (float)d[1] / d[0] : 0;
    float v3 = d[0] ? (float)d[2] / d[0] : 0;
    Serial.print(F("  Verhaeltnis 1 : ")); Serial.print(v2, 3);
    Serial.print(F(" : ")); Serial.println(v3, 3);
    pruefe(v2 > 1.85f && v2 < 2.15f && v3 > 2.8f && v3 < 3.2f,
           "Fair Share 1:2:3");

    /* --- Zyklischer Regler --- */
    uint32_t soll = MESSDAUER_S * 1000u / PERIODE_MS;
    Serial.print(F("  Regler: ")); Serial.print(takte);
    Serial.print(F(" Takte (soll ")); Serial.print(soll);
    Serial.print(F("), max. Jitter ")); Serial.print(regler_jitter_us);
    Serial.println(F(" us"));
    pruefe(takte >= soll * 95u / 100u && takte <= soll * 105u / 100u,
           "zyklische Task haelt den Takt");
    /* Die Zeitbasis des Kernels ist millis(), also 1 ms Aufloesung. Dazu
       kommt bis zu eine Zeitscheibe, bis die gerade laufende Task abgibt.
       Mehr als rund 2 ms Jitter ist daher nicht zu erwarten; 3 ms Grenze
       laesst Luft fuer USB-Interrupts. */
    pruefe(regler_jitter_us <= 3000u, "Jitter unter 3 ms");

    /* --- Semaphor --- */
    Serial.print(F("  Semaphor: ")); Serial.print(sem_geprueft);
    Serial.print(F(" Pruefungen, ")); Serial.print(sem_verletzt);
    Serial.println(F(" Verletzungen der Invariante"));
    pruefe(sem_geprueft > 100 && sem_verletzt == 0,
           "Semaphor schuetzt die zusammengehoerige Aenderung");

    /* --- IRKernel-Queue --- */
    Serial.print(F("  Queue: gesendet ")); Serial.print(iq_gesendet);
    Serial.print(F(", empfangen ")); Serial.print(iq_empfangen);
    Serial.print(F(", Vorsprung ")); Serial.println(iq_gesendet - iq_empfangen);
    pruefe(iq_empfangen > 100 && iq_reihenfolge_falsch == 0,
           "IRKernel-Queue: FIFO-Reihenfolge");
    pruefe(iq_gesendet - iq_empfangen <= 8u + 1u,
           "IRKernel-Queue: Erzeuger wird bei voller Queue gebremst");

    /* --- Core 1 --- */
    Serial.print(F("  Core1 -> Core0: fifo ")); Serial.print(rx_fifo_anzahl);
    Serial.print(F(" (Luecken ")); Serial.print(rx_fifo_luecken);
    Serial.print(F(", verworfen ")); Serial.print(core1_fifo_verworfen);
    Serial.print(F("), queue_t ")); Serial.print(rx_queue_anzahl);
    Serial.print(F(" (defekt ")); Serial.print(rx_queue_kaputt);
    Serial.print(F(", verworfen ")); Serial.print(core1_queue_verworfen);
    Serial.println(F(")"));
    pruefe(rx_fifo_anzahl > 100 && rx_fifo_luecken <= core1_fifo_verworfen,
           "rp2040.fifo: Werte kommen lueckenlos an");
    pruefe(rx_queue_anzahl > 100 && rx_queue_kaputt == 0,
           "queue_t: Strukturen kommen unbeschaedigt an");

    /* --- Stack-Fruehwarnung --- */
    bool fresser_gemeldet = false, andere_gemeldet = false;
    for (uint8_t i = 0; i < warn_anzahl; i++) {
        if (warn_task[i] == t_fresser) fresser_gemeldet = true;
        else                           andere_gemeldet  = true;
    }
    Serial.print(F("  Stapelfresser: Tiefe ")); Serial.print(fresser_tiefe);
    Serial.print(F(", Warnungen gesamt ")); Serial.println(warn_anzahl);
    pruefe(fresser_gemeldet && !andere_gemeldet,
           "Stack-Fruehwarnung meldet genau die knappe Task");

    /* --- Stackreserve --- */
#if IRK_ENABLE_STACKCHECK
    Serial.println(F("  Stackreserve (nie benutzte Byte):"));
    bool stacks_ok = true;
    for (uint8_t i = 0; i < taskanzahl; i++) {
        size_t frei = irk_stack_free(taskliste[i].h);
        Serial.print(F("    ")); Serial.print(taskliste[i].name);
        Serial.print(F(": ")); Serial.println((unsigned long)frei);
        if (taskliste[i].h == t_fresser) {
            Serial.println(F("      (absichtlich knapp, siehe Kapitel 9)"));
            continue;
        }
        if (frei < 128) stacks_ok = false;
    }
    pruefe(stacks_ok, "alle Tasks haben mindestens 128 Byte Reserve");
#endif

    Serial.println(F("========================================"));
    Serial.print(F(" ERGEBNIS: ")); Serial.print(geprueft);
    Serial.print(F(" Pruefungen, ")); Serial.print(fehler);
    Serial.println(F(" Fehler"));
    Serial.println(F("========================================"));
    Serial.println();
}


/*========================================================================*\
 *  setup() und loop()  --  laufen auf Core 0
\*========================================================================*/

/* Stacks der sechs LED-Nocken */
static uint8_t stk_gruen_an[STK], stk_gruen_aus[STK];
static uint8_t stk_rot_an[STK],   stk_rot_aus[STK];
static uint8_t stk_blau_an[STK],  stk_blau_aus[STK];

void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { }

    Serial.println();
    Serial.println(F("IRKernel  --  RP2040 Demo und Selbsttest"));
    Serial.print  (F("Messung ueber ")); Serial.print(MESSDAUER_S);
    Serial.println(F(" Sekunden, danach Auswertung."));
    Serial.println();

    /* Queue fuer Core 1 zuerst einrichten, dann Core 1 freigeben */
    queue_init(&core1_queue, sizeof(Messung), 16);
    core1_queue_bereit = true;

    /* Kernel starten -- die Haupttask (setup/loop) bekommt Prioritaet 1 */
    irk_init(1);

    /* Semaphor und Queue vor den Tasks einrichten, die sie benutzen */
    irk_sema_init(SEM_PAAR, 1);                       /* binaer = Mutex */
    irk_queue_init(&iq, iq_speicher, sizeof(uint16_t), 8);

    /* Tasks anlegen */
    anlegen(zaehler1, 1, stk_z1, "zaehler1");
    anlegen(zaehler2, 2, stk_z2, "zaehler2");
    anlegen(zaehler3, 3, stk_z3, "zaehler3");

    irk_task_t t_regler = anlegen(regler, 1, stk_regler, "regler");
    irk_task_set_cyclic(t_regler, PERIODE_MS);       /* ab jetzt nach der Uhr */

    anlegen(schreiber, 1, stk_schreibA, "schreiberA");
    anlegen(schreiber, 1, stk_schreibB, "schreiberB");
    anlegen(pruefer,   1, stk_pruefer,  "pruefer");

    anlegen(q_erzeuger,    1, stk_qprod, "q_erzeuger");
    anlegen(q_verbraucher, 1, stk_qcons, "q_verbraucher");

    anlegen(core1_empfaenger, 2, stk_core1rx, "core1_rx");

    /* RGB-LED als Nockenschaltwerk: je Farbe "an" sofort, "aus" versetzt,
       beide mit derselben Periode (siehe Beschreibung oben) */
    led.begin();
    led_ausgeben();                                   /* LED definiert aus */

    irk_task_t tl;
    tl = anlegen(gruen_an,  1, stk_gruen_an,  "gruen_an");  irk_task_set_cyclic_at(tl, 1000,   0);
    tl = anlegen(gruen_aus, 1, stk_gruen_aus, "gruen_aus"); irk_task_set_cyclic_at(tl, 1000, 500);
    tl = anlegen(rot_an,    1, stk_rot_an,    "rot_an");    irk_task_set_cyclic_at(tl, 3700,   0);
    tl = anlegen(rot_aus,   1, stk_rot_aus,   "rot_aus");   irk_task_set_cyclic_at(tl, 3700, 700);
    tl = anlegen(blau_an,   1, stk_blau_an,   "blau_an");   irk_task_set_cyclic_at(tl, 7000,   0);
    tl = anlegen(blau_aus,  1, stk_blau_aus,  "blau_aus");  irk_task_set_cyclic_at(tl, 7000, 200);

    t_fresser = anlegen(stapelfresser, 1, stk_fresser, "stapelfresser");

    /* Fruehwarnung fuer alle Tasks scharf schalten */
    irk_stack_watch(STACK_WARNSCHWELLE, stack_alarm);
}

void loop()
{
    static uint32_t sekunde = 0;

    /* irk_delay() statt delay(): die Haupttask schlaeft, alle anderen
       Tasks laufen weiter. Mit delay() stuende der ganze Kern still. */
    irk_delay(1000);
    sekunde++;

    if (sekunde == 2) fresser_los = 1;                /* Kapitel 9 */

    if (sekunde == 1) {                               /* Anlauf abwarten */
        for (int i = 0; i < 3; i++) mess_z0[i] = zaehler[i];
        mess_takte0 = regler_takte;
    }

    status_ausgeben(sekunde);

    /* Stack-Warnungen hier ausgeben -- nicht im Rueckruf selbst */
    static uint8_t gemeldet = 0;
    while (gemeldet < warn_anzahl) {
        Serial.print(F("  STACK-WARNUNG: Task "));
        Serial.print(taskname(warn_task[gemeldet]));
        Serial.print(F(" hat nur noch "));
        Serial.print(warn_frei[gemeldet]);
        Serial.println(F(" Byte nie benutzt"));
        gemeldet++;
    }

    if (sekunde == 1 + MESSDAUER_S) auswerten();
}
