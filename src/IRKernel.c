/*========================================================================*\
 *
 *  IRKernel.c  --  Kern des IRKernel
 *
 *  Neuimplementierung fuer kleine Mikrocontroller, nach dem Entwurf von
 *  Andreas Keibel (IRF Dortmund, 1993).
 *
 *  Die Scheduling-Politik ist die des urspruenglichen Entwurfs; der
 *  Unterbau ist neu:
 *
 *    - der Kontextwechsel          (in Assembler, je Architektur,
 *                                   siehe irk_port.h)
 *    - die Semaphor-Warteschlangen (intrusiv, ohne Allokation)
 *    - alle Dimensionen            (siehe irk_config.h)
 *
 *  ----------------------------------------------------------------------
 *  MEHRKERNBETRIEB  (IRK_MAX_CORES > 1, z.B. RP2040 / RP2350)
 *  ----------------------------------------------------------------------
 *
 *  Jeder Kern hat seinen eigenen Scheduler. Eine Task laeuft immer auf dem
 *  Kern, der sie angelegt hat, und wechselt ihn nie. Gemeinsam sind:
 *
 *    - die Tasktabelle -- Handles sind global eindeutig
 *    - Semaphore und Queues
 *
 *  Grundsatz: Ein Kontextwechsel findet NUR auf dem Kern der Task statt.
 *  Alles, was ein anderer Kern tut -- wecken, anhalten, loeschen, eine
 *  Nachricht schicken --, ist eine Zustandsaenderung im gemeinsamen
 *  Speicher. Der Scheduler der Task sieht sie beim naechsten Durchlauf und
 *  handelt selbst. Kein Kern fasst je den Stack einer fremden Task an.
 *
 *  Den gemeinsamen Teil schuetzt eine kernuebergreifende Sperre (IRK_LOCK).
 *  Sie wird NIE ueber einen Kontextwechsel gehalten: unter der Sperre
 *  Zustand aendern und Wartebit setzen, Sperre freigeben, dann abgeben.
 *  Weckt der andere Kern dazwischen, sieht der Scheduler das Bit schon
 *  geloescht -- ein Weckruf kann nicht verloren gehen.
 *
 *  Anwenderfunktionen (Rueckrufe, Fehlermeldungen) werden grundsaetzlich
 *  AUSSERHALB der Sperre aufgerufen.
 *
 *  Bei IRK_MAX_CORES == 1 entfallen Sperre und Kernabfrage vollstaendig.
 *
\*========================================================================*/

#include <string.h>

#include "IRKernel.h"
#include "irk_port.h"

#if IRK_ENABLE_MALLOC_STACKS
#include <stdlib.h>
#endif


/*========================================================================*\
 *  Sperre und Kernabfrage
\*========================================================================*/

#if IRK_MAX_CORES > 1
#  define IRK_LOCK()      irk_port_xlock()
#  define IRK_UNLOCK()    irk_port_xunlock()
#  define IRK_CORE_ID()   irk_port_core_id()
#else
#  define IRK_LOCK()      ((void)0)
#  define IRK_UNLOCK()    ((void)0)
#  define IRK_CORE_ID()   0u
#endif


/*========================================================================*\
 *  Taskdescriptor  --  gemeinsam fuer alle Kerne
\*========================================================================*/

typedef struct {
    void       *sp;           /* Kontext: der gesicherte Stackpointer     */
    void       *stack;        /* Basis des Taskstacks                     */
    size_t      stack_size;

    void      (*func)(void);  /* Taskfunktion                             */

    uint16_t    status;       /* IRK_RUNNING / IRK_WAIT_* / IRK_KILLED    */
    irk_prio_t  priority;     /* 0 = nicht einplanen, sonst 1..IRK_MAX_PRIO*/
    uint8_t     core;         /* Kern, auf dem die Task laeuft -- fest    */

    /* Alle Zeiten in Mikrosekunden. Fristen werden als Start + Dauer
       gefuehrt, nie als Zielzeitpunkt: "jetzt - Start >= Dauer" ist in
       vorzeichenloser Arithmetik ueber jeden Ueberlauf hinweg exakt. */

    irk_time_t  vruntime;     /* Fair-Share-Konto, siehe irk_charge()     */
    irk_prio_t  vrem;         /* Divisionsrest des Kontos, < priority     */
    irk_time_t  runtime;      /* verbrauchte Rechenzeit gesamt (Statistik)*/
    irk_time_t  slice;        /* Laufzeit seit der letzten Zuteilung      */
    irk_time_t  wait_start;   /* IRK_WAIT_TIME: Beginn der Wartezeit      */
    irk_time_t  wait_len;     /*                Dauer                     */
    irk_time_t  kill_start;   /* IRK_WAIT_DEAD: Beginn der Todesfrist     */
    irk_time_t  kill_len;     /*                Dauer                     */

#if IRK_ENABLE_CYCLIC
    irk_time_t  cycle_ref;    /* Faelligkeit der naechsten Aktivierung
                                 minus eine Periode                       */
    irk_time_t  cycle_time;   /* Periode                                  */
    irk_time_t  cycle_start_len; /* Startversatz ab cycle_ref; 0 = der Takt
                                 laeuft bereits                           */
#endif

    uint8_t     q_next;       /* naechste Task in einer Warteschlange     */
    uint8_t     owns_stack;   /* Stack wurde vom Kernel allokiert         */
    uint8_t     zombie;       /* geloescht, aber ihr Kern hat noch nicht
                                 von ihr weggeschaltet -- ihr Stack ist
                                 noch in Benutzung, der Slot gesperrt     */
    uint8_t     needs_align;  /* beim naechsten Scheduler-Durchlauf ihres
                                 Kerns in die Fair-Share-Ordnung einordnen*/
    void       *wait_obj;     /* Semaphor bzw. Queue, auf die gewartet
                                 wird -- NULL, wenn die Task nicht in
                                 einer Warteschlange haengt              */
#if IRK_ENABLE_STACKCHECK
    uint8_t     stack_warned; /* Fruehwarnung fuer diese Task schon
                                 ausgeloest                              */
#endif

#if IRK_ENABLE_NAMES
    const char *name;
#endif
#if IRK_ENABLE_STATS
    uint32_t    calls;        /* wie oft die Task zugeteilt bekam         */
#endif
} irk_tcb;


/*========================================================================*\
 *  Semaphor  --  gemeinsam fuer alle Kerne
\*========================================================================*/

typedef struct {
    int16_t  count;           /* < 0  =>  so viele Tasks warten           */
    int16_t  init;            /* Initialwert                              */
    uint8_t  q_head;          /* laengstwartende Task,  0 = leer          */
    uint8_t  q_tail;
    uint8_t  valid;
} irk_sema_ctl;


/*========================================================================*\
 *  Zustand je Kern  --  nur vom eigenen Kern angefasst
\*========================================================================*/

#if IRK_ENABLE_WATCH
typedef struct {
    int    (*cond)(void);
    void   (*action)(void);
    uint8_t  mode;
    uint8_t  used;
    int      last;
} irk_watch_entry;
#endif

typedef struct {
    uint8_t     started;
    uint8_t     current;      /* laufende Task dieses Kerns               */
    uint8_t     lockcnt;      /* irk_lock(): Scheduler gesperrt           */
    uint8_t     dead_prev;    /* zuletzt verlassene, geloeschte Task --
                                 wird nach dem Wechsel freigegeben        */
    irk_time_t  t0;           /* Uhr beim Start dieses Kerns              */
    irk_time_t  now;          /* Uhr jetzt (absolut)                      */
    irk_time_t  last;         /* Uhr beim letzten Scheduler-Durchlauf     */
    irk_time_t  vmin;         /* Kern-Minimum der Fair-Share-
                                 Konten: folgt der jeweils gewaehlten Task
                                 und sinkt nie (ausser beim Normalisieren) */
#if IRK_ENABLE_STACKCHECK
    uint8_t     watch_next;   /* Stack-Fruehwarnung: naechste Task        */
    irk_time_t  watch_last;
#endif
#if IRK_ENABLE_WATCH
    irk_watch_entry watches[IRK_MAX_WATCH];
#endif
} irk_core_t;


/*========================================================================*\
 *  Kernelzustand
\*========================================================================*/

/* Gemeinsam. Die Plaetze 1 .. IRK_MAX_CORES sind fest fuer die Haupttasks
   der Kerne reserviert: Kern 0 hat Handle 1, Kern 1 Handle 2. So bleibt
   IRK_MAIN_TASK immer die Haupttask von Kern 0 -- egal, welcher Kern
   zuerst startet. */
static irk_tcb       tasks[IRK_MAX_TASKS + 1];   /* Index 0 bleibt frei   */
static irk_sema_ctl  semas[IRK_MAX_SEMAPHORES];
static uint8_t       irk_ntasks = 0;             /* hoechster belegter Idx*/

static irk_core_t    irk_cores[IRK_MAX_CORES];

#if IRK_ENABLE_STACKCHECK
static size_t             watch_min_free = 0;    /* gemeinsam eingestellt */
static irk_stack_alarm_fn watch_cb       = NULL;
#endif

/* Groesste Laufzeit, die einer Task fuer EINE Zuteilung angerechnet wird.
   Begrenzt den Kontozuwachs je Lauf auf 2^22 * 256 = 2^30 und damit den
   Abstand zwischen zwei lauffaehigen Tasks. Eine Task, die laenger als
   gut 4 Sekunden nicht abgibt, ist ohnehin nicht kooperativ.            */
#define IRK_SLICE_CHARGE_MAX   ((irk_time_t)1 << 22)

/* Erreicht das kleinste Konto diese Schwelle, wird von allen Konten
   derselbe Betrag abgezogen (siehe irk_normalize).
   So bleiben die Konten unabhaengig von der Laufzeit im Bereich.      */
#define IRK_VRUNTIME_NORMALIZE ((irk_time_t)1 << 30)

/* Laengste Einzelfrist. Bei 32-Bit-Zeit knapp unter der halben
   Zaehlerbreite, damit "jetzt - Start" nie mehrdeutig wird.            */
#if IRK_TIME_64
#  define IRK_MAX_WAIT_US      ((irk_time_t)1 << 62)
#else
#  define IRK_MAX_WAIT_US      ((irk_time_t)0x7FFFFFFFUL)
#endif

/* Vorwaertsdeklarationen */
static irk_core_t *irk_enter(void);
static void        irk_kill_locked(uint8_t t);


/*========================================================================*\
 *  Interne Helfer
\*========================================================================*/

static void irk_fail(int code, int detail)
{
    irk_port_error(code, detail);
}

static int irk_valid(uint8_t t)
{
    return (t >= 1) && (t <= irk_ntasks) && !(tasks[t].status & IRK_KILLED);
}

static int irk_is_main(uint8_t t)
{
    return (t >= 1) && (t <= IRK_MAX_CORES);
}

/* Lauffaehig ist eine Task, wenn sie auf nichts wartet. IRK_WAIT_DEAD
   zaehlt dabei nicht als Wartegrund: die Task laeuft bis zu ihrem
   Todeszeitpunkt ganz normal weiter. */
static int irk_runnable(uint8_t t)
{
    uint16_t st = tasks[t].status;
    return (st == IRK_RUNNING) || (st == IRK_WAIT_DEAD);
}

/*------------------------------------------------------------------------*\
 *  Fair Share  --  virtuelle Laufzeit
 *
 *  Jede Task fuehrt ein Konto vruntime. Laeuft sie, waechst es um
 *
 *        Laufzeit * 256 / Prioritaet
 *
 *  Dran ist stets die Task mit dem KLEINSTEN Konto -- die, die relativ zu
 *  ihrer Prioritaet am meisten im Rueckstand ist. Prioritaet 3 laesst das
 *  Konto dreimal langsamer wachsen als Prioritaet 1, die Task kommt also
 *  dreimal so oft dran.
 *
 *  Im Mehrkernbetrieb bildet jeder Kern seine eigene Fair-Share-Ordnung:
 *  verglichen werden immer nur Konten von Tasks desselben Kerns, und nur
 *  der eigene Kern liest und schreibt sie.
 *
 *  Der Entwurf von 1993 suchte das Maximum von  jetzt - runtime/prio.
 *  Da "jetzt" in dieser Schleife konstant ist, ist das dasselbe wie das
 *  Minimum von runtime/prio. Das Konto hier ist genau diese
 *  Groesse, nur laufend fortgeschrieben statt jedes Mal neu dividiert.
 *
 *  Warum so und nicht mehr "runtime / prio":
 *    - keine Division beim Auswaehlen, keine Multiplikation beim Wecken
 *      (die alte Formel  kleinstes_Verhaeltnis * prio  lief bei
 *      Mikrosekunden nach wenigen Minuten ueber)
 *    - der Divisionsrest wird je Task mitgetragen (vrem), es geht also
 *      auch bei Prioritaet 65535 und kurzen Zeitscheiben nichts verloren
 *    - Ueberlauf wird durch gemeinsames ABZIEHEN abgefangen (irk_normalize),
 *      nicht durch Halbieren -- die Abstaende bleiben exakt erhalten
\*------------------------------------------------------------------------*/

/* Kleinstes Konto unter allen lauffaehigen, eingeplanten Tasks des Kerns
   core ausser except. Rueckgabe 0, wenn es keine solche Task gibt. */
static int irk_min_vruntime(uint8_t core, uint8_t except, irk_time_t *out)
{
    uint8_t    t;
    int        found = 0;
    irk_time_t best  = 0;

    for (t = 1; t <= irk_ntasks; t++) {
        if (t == except) continue;
        if (tasks[t].core != core) continue;
        if (!irk_runnable(t)) continue;
        if (tasks[t].priority == 0) continue;
        if (!found || tasks[t].vruntime < best) {
            best  = tasks[t].vruntime;
            found = 1;
        }
    }
    if (found) *out = best;
    return found;
}

/* Verbrauchte Laufzeit auf das Konto einer Task buchen. */
static void irk_charge(uint8_t t, irk_time_t slice)
{
    irk_prio_t p = tasks[t].priority;
    irk_time_t acc;

    if (p == 0) p = 1;                  /* 0: nicht eingeplant      */
    if (slice > IRK_SLICE_CHARGE_MAX) slice = IRK_SLICE_CHARGE_MAX;

    /* Auch bei 32-Bit-Zeit ohne Ueberlauf: slice ist hoechstens 2^22,
       mal 256 also 2^30, plus ein Rest unter 2^16. */
    acc                = slice * 256u + tasks[t].vrem;
    tasks[t].vruntime += acc / p;
    tasks[t].vrem      = (irk_prio_t)(acc % p);
}

/* Ueberlaufschutz: erreicht das kleinste Konto eines Kerns die Schwelle,
   wird von ALLEN Konten dieses Kerns derselbe Betrag abgezogen. Die
   Reihenfolge und alle Abstaende bleiben dabei exakt erhalten.

   Warum das sicher ist: eine lauffaehige Task kann das kleinste Konto um
   hoechstens einen Kontozuwachs (2^30) ueberholen, bevor wieder die
   kleinste drankommt. Alle lauffaehigen Konten liegen damit stets unter
   Schwelle + 2^30 = 2^31 -- auch bei 32-Bit-Zeit weit weg vom Ueberlauf.

   Konten nicht lauffaehiger Tasks, die unter dem Abzugsbetrag liegen,
   werden auf 0 gesetzt. Das ist unschaedlich: beim Wecken hebt irk_align()
   sie ohnehin auf den aktuellen Stand.
   So bleiben die Konten unabhaengig von der Laufzeit im Bereich.      */
static void irk_normalize(uint8_t core)
{
    irk_time_t base;
    uint8_t    t;

    if (!irk_min_vruntime(core, 0, &base)) return;
    if (base < IRK_VRUNTIME_NORMALIZE) return;

    for (t = 1; t <= irk_ntasks; t++) {
        if (tasks[t].core != core) continue;
        tasks[t].vruntime = (tasks[t].vruntime > base)
                          ? tasks[t].vruntime - base : 0;
    }
    irk_cores[core].vmin = (irk_cores[core].vmin > base)
                         ? irk_cores[core].vmin - base : 0;
}

/* Task in die Fair-Share-Ordnung ihres Kerns aufnehmen -- beim Anlegen,
   beim Wecken, beim Wechsel aus dem Zyklusbetrieb. Sie bekommt das
   GROESSERE von eigenem und kleinstem Konto: fuer ihre Wartezeit wird sie
   nicht mit einem Schwall Rechenzeit entschaedigt, und durch kurzes
   Schlafen kann sie sich auch keinen Vorsprung erschleichen.

   Darf nur vom Kern der Task selbst gerufen werden, denn nur der liest
   die Konten konsistent. Andere Kerne setzen stattdessen needs_align.

   Bezug ist mindestens das Kern-Minimum vmin des Kerns. Zaehlte nur das
   kleinste Konto der ANDEREN lauffaehigen Tasks, behielte eine Task auf
   einem fast leeren Kern ihr altes Konto -- und sperrte nach dem
   Aufwachen die anderen aus, bis der Abstand aufgeholt ist. */
static void irk_align(uint8_t t)
{
    irk_time_t m;
    irk_time_t boden = irk_cores[tasks[t].core].vmin;   /* gilt auch, wenn
                                                           niemand sonst laeuft */
    if (irk_min_vruntime(tasks[t].core, t, &m) && m > boden) boden = m;
    if (tasks[t].vruntime < boden) tasks[t].vruntime = boden;
}


/*------------------------------------------------------------------------*\
 *  Semaphor-Warteschlangen  --  intrusiv, ohne jede Allokation
 *
 *  Ein Wartevorgang darf keinen Speicher anfordern muessen: auf einem
 *  Mikrocontroller waere ein Fehlschlag jederzeit moeglich.
 *
 *  Da eine Task ohnehin nur auf genau ein Semaphor zugleich warten kann,
 *  genuegt ein einziger uint8_t je Task: die Warteschlange wird durch
 *  die Taskdescriptoren selbst gefaedelt. Keine Allokation, O(1) beim
 *  Einreihen, FIFO-Reihenfolge.
 *
 *  Alle Helfer hier setzen voraus, dass die Sperre gehalten wird.
\*------------------------------------------------------------------------*/

static void irk_q_push(uint8_t *head, uint8_t *tail, uint8_t t)
{
    tasks[t].q_next = 0;
    if (*head == 0) *head = t;
    else            tasks[*tail].q_next = t;
    *tail = t;
}

static uint8_t irk_q_pop(uint8_t *head, uint8_t *tail)
{
    uint8_t t = *head;
    if (t != 0) {
        *head = tasks[t].q_next;
        if (*head == 0) *tail = 0;
        tasks[t].q_next = 0;
    }
    return t;
}

/* Entfernt eine bestimmte Task aus einer Warteschlange.
   Rueckgabe: 1, wenn sie darin stand. */
static int irk_q_unlink(uint8_t *head, uint8_t *tail, uint8_t t)
{
    uint8_t cur  = *head;
    uint8_t prev = 0;

    while (cur != 0 && cur != t) {
        prev = cur;
        cur  = tasks[cur].q_next;
    }
    if (cur == 0) return 0;                     /* nicht in dieser Schlange */

    if (prev == 0) *head              = tasks[cur].q_next;
    else           tasks[prev].q_next = tasks[cur].q_next;

    if (*tail == cur) *tail = prev;
    tasks[cur].q_next = 0;
    return 1;
}

/* Loest eine Task aus der Warteschlange, in der sie gerade haengt.
   Wird beim Loeschen einer wartenden Task gebraucht, damit dort kein
   Eintrag auf eine Task zurueckbleibt, die es nicht mehr gibt.

   Statt alle Semaphore und Queues abzusuchen, merkt sich jede Task in
   wait_obj, worauf sie wartet -- der Statuszustand sagt, um welche Art
   Objekt es sich handelt. */
static void irk_q_detach(uint8_t t)
{
    uint16_t st = tasks[t].status;
    void    *o  = tasks[t].wait_obj;

    if (o == NULL) return;

    if (st & IRK_WAIT_SEMA) {
        irk_sema_ctl *s = (irk_sema_ctl *)o;
        if (irk_q_unlink(&s->q_head, &s->q_tail, t)) s->count++;
    } else if (st & IRK_WAIT_QSEND) {
        irk_queue_t *q = (irk_queue_t *)o;
        irk_q_unlink(&q->q_send, &q->q_send_tail, t);
    } else if (st & IRK_WAIT_QRECV) {
        irk_queue_t *q = (irk_queue_t *)o;
        irk_q_unlink(&q->q_recv, &q->q_recv_tail, t);
    }
    tasks[t].wait_obj = NULL;
}


/*------------------------------------------------------------------------*\
 *  Stackueberwachung
\*------------------------------------------------------------------------*/

#if IRK_ENABLE_STACKCHECK

static void irk_stack_paint(void *stack, size_t size)
{
    memset(stack, IRK_STACK_PATTERN, size);
}

/* Wertet das Fuellmuster am unteren Ende des Stacks aus.
   Prueft nur den eigenen, gerade laufenden Stack -- ohne Sperre. */
static void irk_stack_check(uint8_t t)
{
    const uint8_t *p = (const uint8_t *)tasks[t].stack;
    if (p == NULL) return;
    if (p[0] != IRK_STACK_PATTERN || p[1] != IRK_STACK_PATTERN) {
        irk_fail(IRK_ERR_STACK_OVERFLOW, t);
    }
}

size_t irk_stack_free(irk_task_t tsk)
{
    const uint8_t *p;
    size_t         n = 0;

    if (!irk_valid(tsk) || tasks[tsk].stack == NULL) return 0;

    p = (const uint8_t *)tasks[tsk].stack;
    while (n < tasks[tsk].stack_size && p[n] == IRK_STACK_PATTERN) n++;
    return n;
}


/*------------------------------------------------------------------------*\
 *  Stack-Fruehwarnung
 *
 *  Der Kanarienvogel in irk_stack_check() schlaegt erst an, wenn der
 *  Stack schon uebergelaufen ist. Die Fruehwarnung meldet dagegen, sobald
 *  die Reserve unter eine Schwelle faellt -- solange also noch nichts
 *  zerstoert ist.
 *
 *  Um den Scheduler nicht zu belasten, prueft jeder Kern reihum nur eine
 *  seiner Tasks je Intervall, und von ihrem Stack nur die untersten
 *  min_free Byte: sind die alle noch eingefaerbt, reicht die Reserve.
\*------------------------------------------------------------------------*/

int irk_stack_watch(size_t min_free, irk_stack_alarm_fn callback)
{
    irk_core_t *K = irk_enter();
    uint8_t     t;

    if (K == NULL) return -1;

    IRK_LOCK();
    watch_min_free = min_free;
    watch_cb       = callback;
    K->watch_last  = K->now;
    for (t = 1; t <= irk_ntasks; t++) tasks[t].stack_warned = 0;
    IRK_UNLOCK();
    return 0;
}

/* Unter der Sperre: pruefen und das Ergebnis nur MERKEN. Den Rueckruf
   ruft der Scheduler danach ausserhalb der Sperre auf. */
static void irk_stack_watch_run(irk_core_t *K, uint8_t core,
                                uint8_t *alarm_t, size_t *alarm_n)
{
    const uint8_t *p;
    size_t         n, grenze;
    uint8_t        t;

    if (watch_cb == NULL || watch_min_free == 0) return;
    if ((irk_time_t)(K->now - K->watch_last) <
        (irk_time_t)IRK_STACK_WATCH_INTERVAL_MS * 1000u) return;
    K->watch_last = K->now;

    /* Reihum weiter; die Haupttasks haben keinen eingefaerbten Stack. */
    if (K->watch_next <= IRK_MAX_CORES || K->watch_next > irk_ntasks) {
        K->watch_next = IRK_MAX_CORES + 1u;
    }
    t = K->watch_next++;

    if (t > irk_ntasks || tasks[t].core != core) return;
    if (tasks[t].status & IRK_KILLED) return;
    if (tasks[t].stack == NULL || tasks[t].stack_warned) return;

    p      = (const uint8_t *)tasks[t].stack;
    grenze = (watch_min_free < tasks[t].stack_size) ? watch_min_free
                                                    : tasks[t].stack_size;

    for (n = 0; n < grenze; n++) {
        if (p[n] != IRK_STACK_PATTERN) break;
    }

    if (n < grenze) {
        /* n ist jetzt genau der nie benutzte Rest vom Stackboden an */
        tasks[t].stack_warned = 1;
        *alarm_t = t;
        *alarm_n = n;
    }
}

#else
#  define irk_stack_paint(s, n)   ((void)0)
#  define irk_stack_check(t)      ((void)0)
#endif


/*------------------------------------------------------------------------*\
 *  Ueberwachungsfunktionen  --  je Kern, ausserhalb der Sperre gerufen
\*------------------------------------------------------------------------*/

#if IRK_ENABLE_WATCH

static void irk_watch_run(irk_core_t *K)
{
    uint8_t i;
    for (i = 0; i < IRK_MAX_WATCH; i++) {
        int now, fire;
        irk_watch_entry *w = &K->watches[i];
        if (!w->used) continue;

        now  = w->cond();
        fire = (w->mode & IRK_WATCH_EDGE) ? (now != w->last) : (now != 0);

        if (w->mode & IRK_WATCH_NEGATIVE) {
            fire = (w->mode & IRK_WATCH_EDGE) ? (now < w->last) : (now == 0);
        } else if (w->mode & IRK_WATCH_EDGE) {
            fire = (now > w->last);
        }
        w->last = now;

        if (fire) {
            if (w->mode & IRK_WATCH_ONCE) w->used = 0;
            w->action();
        }
    }
}

int irk_watch_add(int (*cond)(void), void (*action)(void), uint8_t mode)
{
    irk_core_t *K = irk_enter();
    uint8_t     i;

    if (K == NULL) return -1;
    if (cond == NULL || action == NULL) return -1;
    for (i = 0; i < IRK_MAX_WATCH; i++) {
        if (!K->watches[i].used) {
            K->watches[i].cond   = cond;
            K->watches[i].action = action;
            K->watches[i].mode   = mode;
            K->watches[i].last   = 0;
            K->watches[i].used   = 1;
            return 0;
        }
    }
    return -1;
}

int irk_watch_remove(int (*cond)(void), void (*action)(void))
{
    irk_core_t *K = irk_enter();
    uint8_t     i;

    if (K == NULL) return -1;
    for (i = 0; i < IRK_MAX_WATCH; i++) {
        if (K->watches[i].used &&
            K->watches[i].cond == cond && K->watches[i].action == action) {
            K->watches[i].used = 0;
            return 0;
        }
    }
    return 1;
}

#else
#  define irk_watch_run(K)   ((void)0)
#endif


/*========================================================================*\
 *  Kerne: Eintritt, Start und Ende
\*========================================================================*/

/* Startet den Kernel auf Kern c. Der dort laufende Code (auf Arduino:
   setup()/loop() bzw. setup1()/loop1()) wird zur Haupttask mit Handle
   c + 1. Startet der erste Kern ueberhaupt, werden auch die gemeinsamen
   Tabellen frisch aufgesetzt. */
static void irk_start_core(uint8_t c, irk_prio_t prio)
{
    irk_core_t *K = &irk_cores[c];
    uint8_t     m = (uint8_t)(c + 1u);
    uint8_t     i, laeuft_schon = 0;

    /* Die Portierungsschicht zuerst: auf dem PC etwa muss jeder Kern
       seinen Thread in einen Fiber verwandeln, bevor er umschalten kann. */
    irk_port_init();

    if (prio < 1) prio = 1;

    IRK_LOCK();

    for (i = 0; i < IRK_MAX_CORES; i++) {
        if (irk_cores[i].started) laeuft_schon = 1;
    }
    if (!laeuft_schon) {
        memset(tasks, 0, sizeof(tasks));
        memset(semas, 0, sizeof(semas));
        /* Haupttask-Plaetze der noch nicht gestarteten Kerne als unbelegt
           kennzeichnen -- sie sind reserviert und werden nie vergeben. */
        for (i = 1; i <= IRK_MAX_CORES; i++) tasks[i].status = IRK_KILLED;
        irk_ntasks = IRK_MAX_CORES;
#if IRK_ENABLE_STACKCHECK
        watch_min_free = 0;
        watch_cb       = NULL;
#endif
    }

    memset(K, 0, sizeof(*K));
    memset(&tasks[m], 0, sizeof(irk_tcb));

    /* Die Haupttask laeuft bereits -- sie braucht weder Stack noch
       Kontext, sie benutzt den vorhandenen. */
    tasks[m].core     = c;
    tasks[m].priority = prio;
    tasks[m].status   = IRK_RUNNING;
#if IRK_ENABLE_NAMES
    tasks[m].name     = "main";
#endif

    K->current = m;
    K->t0      = irk_port_micros();
    K->now     = K->t0;
    K->last    = K->t0;
    K->started = 1;

    IRK_UNLOCK();
}

/* Eintritt in jede oeffentliche Funktion.
 *
 *   - Aufrufe aus einem Interrupt werden abgelehnt: sie koennten den
 *     Scheduler mitten in einer Aenderung unterbrechen.
 *   - Laeuft der Kernel auf diesem Kern noch nicht, wird er hier
 *     automatisch gestartet -- mit Prioritaet 1 fuer die Haupttask.
 *
 * Rueckgabe: der Zustand des aufrufenden Kerns, oder NULL bei Fehler. */
static irk_core_t *irk_enter(void)
{
    uint8_t c;

    if (irk_port_in_isr()) {
        irk_fail(IRK_ERR_IN_ISR, 0);
        return NULL;
    }
    c = (uint8_t)IRK_CORE_ID();
    if (c >= IRK_MAX_CORES) {
        irk_fail(IRK_ERR_WRONG_CORE, c);
        return NULL;
    }
    if (!irk_cores[c].started) irk_start_core(c, 1);
    return &irk_cores[c];
}

int irk_init(irk_prio_t prio)
{
    uint8_t c;

    if (irk_port_in_isr()) { irk_fail(IRK_ERR_IN_ISR, 0); return IRK_ERR_IN_ISR; }
    c = (uint8_t)IRK_CORE_ID();
    if (c >= IRK_MAX_CORES) { irk_fail(IRK_ERR_WRONG_CORE, c); return IRK_ERR_WRONG_CORE; }

    if (!irk_cores[c].started) {
        irk_start_core(c, prio);
    } else {
        /* Schon gestartet (etwa automatisch): nur die Prioritaet der
           Haupttask nachtragen. */
        if (prio < 1) prio = 1;
        tasks[c + 1u].priority = prio;
    }
    return 0;
}

int irk_deinit(void)
{
    irk_core_t *K;
    uint8_t     c, t, i, laeuft_noch = 0;

    c = (uint8_t)IRK_CORE_ID();
    if (c >= IRK_MAX_CORES) return IRK_ERR_WRONG_CORE;
    K = &irk_cores[c];

    if (!K->started) return 0;
    if (K->current != c + 1u) return IRK_ERR_BAD_TASK;

    IRK_LOCK();
    for (t = IRK_MAX_CORES + 1u; t <= irk_ntasks; t++) {
        if (tasks[t].core != c) continue;
        if (!(tasks[t].status & IRK_KILLED)) irk_kill_locked(t);
#if IRK_ENABLE_MALLOC_STACKS
        if (tasks[t].owns_stack && tasks[t].stack && !tasks[t].zombie) {
            free(tasks[t].stack);
            tasks[t].stack      = NULL;
            tasks[t].owns_stack = 0;
        }
#endif
    }
    tasks[c + 1u].status = IRK_KILLED;
    K->started = 0;

    for (i = 0; i < IRK_MAX_CORES; i++) {
        if (irk_cores[i].started) laeuft_noch = 1;
    }
    if (!laeuft_noch) irk_ntasks = IRK_MAX_CORES;
    IRK_UNLOCK();
    return 0;
}

int irk_is_running(void)
{
    uint8_t c = (uint8_t)IRK_CORE_ID();
    return (c < IRK_MAX_CORES) ? irk_cores[c].started : 0;
}

uint8_t irk_core_id(void)
{
    return (uint8_t)IRK_CORE_ID();
}

uint8_t irk_task_core(irk_task_t tsk)
{
    return irk_valid(tsk) ? tasks[tsk].core : 0xFFu;
}


/*========================================================================*\
 *  Tasks anlegen
\*========================================================================*/

/* Nach jedem Kontextwechsel, auf der Seite der Task, die jetzt laeuft:
   war die zuvor verlassene Task geloescht, ist ihr Stack erst jetzt
   wirklich frei -- vorher lief der Wechsel ja noch auf ihm. */
static void irk_after_switch(irk_core_t *K)
{
    if (K->dead_prev == 0) return;
    IRK_LOCK();
    tasks[K->dead_prev].zombie = 0;
    K->dead_prev = 0;
    IRK_UNLOCK();
}

/* Trampolin: jede Task startet hier. Kehrt die Taskfunktion zurueck,
   loescht sich die Task selbst -- und irk_task_kill() kehrt dann
   seinerseits nicht zurueck. */
static void irk_trampoline(void)
{
    irk_core_t *K = &irk_cores[IRK_CORE_ID()];

    irk_after_switch(K);          /* der erste Lauf kommt aus einem Wechsel */
    tasks[K->current].func();
    irk_task_kill(K->current);

    /* Unerreichbar -- irk_task_kill() auf die eigene Task kehrt nie
       zurueck. Falls doch, nicht in den Nirvana-Stack laufen. */
    for (;;) irk_yield();
}

irk_task_t irk_task_create(void (*func)(void), irk_prio_t prio,
                           void *stack, size_t size, const char *name)
{
    irk_core_t *K = irk_enter();
    uint8_t     c, t;
    uint8_t     owns     = 0;
    void       *alt_stack = NULL;
    void       *sp;

    if (K == NULL) return IRK_NO_TASK;
    if (func == NULL) { irk_fail(IRK_ERR_BAD_TASK, 0); return IRK_NO_TASK; }
    c = (uint8_t)(K - irk_cores);

    if (size == 0) size = IRK_DEFAULT_STACK;

    if (stack == NULL) {
#if IRK_ENABLE_MALLOC_STACKS
        stack = malloc(size);
        owns  = 1;
#endif
        if (stack == NULL) { irk_fail(IRK_ERR_NO_MEMORY, 0); return IRK_NO_TASK; }
    }
    if (size < irk_ctx_min_stack()) {
#if IRK_ENABLE_MALLOC_STACKS
        if (owns) free(stack);
#endif
        irk_fail(IRK_ERR_NO_MEMORY, 0);
        return IRK_NO_TASK;
    }

    IRK_LOCK();

    /* Freien Slot suchen: erst geloeschte wiederverwenden, dann anhaengen.
       Nur wirklich tote Slots -- ein Zombie laeuft auf seinem Kern noch.
       Ein linearer Suchlauf ueber hoechstens IRK_MAX_TASKS Eintraege ist
       billiger, als eine Liste freier Slots an RAM kostet. */
    for (t = IRK_MAX_CORES + 1u; t <= irk_ntasks; t++) {
        if ((tasks[t].status & IRK_KILLED) && !tasks[t].zombie) break;
    }
    if (t > irk_ntasks) {
        if (irk_ntasks >= IRK_MAX_TASKS) {
            IRK_UNLOCK();
#if IRK_ENABLE_MALLOC_STACKS
            if (owns) free(stack);
#endif
            irk_fail(IRK_ERR_TOO_MANY_TASKS, 0);
            return IRK_NO_TASK;
        }
        t = ++irk_ntasks;
    } else if (tasks[t].owns_stack) {
        alt_stack = tasks[t].stack;           /* nach der Sperre freigeben */
    }

    /* Slot reservieren: als Zombie markiert greift ihn niemand an, bis
       der Kontext fertig ist. */
    memset(&tasks[t], 0, sizeof(irk_tcb));
    tasks[t].status = IRK_KILLED;
    tasks[t].zombie = 1;

    IRK_UNLOCK();

#if IRK_ENABLE_MALLOC_STACKS
    if (alt_stack) free(alt_stack);
#else
    (void)alt_stack;
#endif

    irk_stack_paint(stack, size);
    sp = irk_ctx_create(stack, size, irk_trampoline);

    IRK_LOCK();
    if (sp == NULL) {
        tasks[t].zombie = 0;                  /* Slot wieder freigeben */
        IRK_UNLOCK();
#if IRK_ENABLE_MALLOC_STACKS
        if (owns) free(stack);
#endif
        irk_fail(IRK_ERR_NO_MEMORY, t);
        return IRK_NO_TASK;
    }

    tasks[t].sp         = sp;
    tasks[t].func       = func;
    tasks[t].stack      = stack;
    tasks[t].stack_size = size;
    tasks[t].owns_stack = owns;
    tasks[t].core       = c;
#if IRK_MAX_PRIO < 65535
    if (prio > IRK_MAX_PRIO) prio = IRK_MAX_PRIO;
#endif
    tasks[t].priority   = prio;
#if IRK_ENABLE_NAMES
    tasks[t].name       = name;
#else
    (void)name;
#endif
    tasks[t].zombie     = 0;
    tasks[t].status     = IRK_RUNNING;

    /* Auf den aktuellen Fairness-Stand setzen, damit die neue Task nicht
       schlagartig alle Rechenzeit an sich zieht. Die Task gehoert zu
       diesem Kern, das Einordnen ist also hier erlaubt. */
    irk_align(t);

    IRK_UNLOCK();
    return (irk_task_t)t;
}


/*========================================================================*\
 *  Der Scheduler  --  je Kern
\*========================================================================*/

/* Unter der Sperre: eine Task dieses Kerns im Scheduler-Durchlauf pruefen
   -- einordnen, Fristen, Todeszeitpunkt, Zyklus. Das ist alles, was der
   andere Kern an einer Task dieses Kerns gleichzeitig aendern darf
   (wecken, anhalten, loeschen, Fristen setzen); deshalb nur hier und nur
   fuer diese eine Task gesperrt. */
static void irk_scan_locked(irk_core_t *K, uint8_t c, uint8_t t,
                            uint8_t *waiting_for_time, uint8_t *cyclic_due)
{
    uint16_t st;

    if (tasks[t].core != c) return;     /* Slot inzwischen neu vergeben */
    st = tasks[t].status;
    if (st & IRK_KILLED) return;

    /* Vorgemerkt beim Wecken oder von einem anderen Kern: erst hier, auf
       dem eigenen Kern, in die Fair-Share-Ordnung einordnen -- nur hier
       ist das Lesen der Konten sicher. */
    if (tasks[t].needs_align) {
        if (irk_runnable(t)) irk_align(t);
        tasks[t].needs_align = 0;
    }

    if (st == IRK_RUNNING) return;

    if (st & IRK_WAIT_TIME) {
        if ((irk_time_t)(K->now - tasks[t].wait_start) >= tasks[t].wait_len) {
            tasks[t].status &= (uint16_t)~IRK_WAIT_TIME;
            /* Auch nach Ablauf einer Frist einordnen -- wie beim Wecken
               ueber Semaphor, Queue oder Fortsetzen: sonst behielte eine
               schlafende Task ihr altes Konto und sperrte nach dem
               Aufwachen die anderen aus, bis der Abstand aufgeholt ist. */
            if (irk_runnable(t)) irk_align(t);
        } else {
            (*waiting_for_time)++;
        }
    }

    if ((st & IRK_WAIT_DEAD) &&
        (irk_time_t)(K->now - tasks[t].kill_start) >= tasks[t].kill_len) {
        irk_kill_locked(t);
        return;
    }

#if IRK_MAX_CORES > 1
    /* Mehrkernbetrieb: eine Task, die auf ein Semaphor, eine Queue oder
       ihr Fortsetzen wartet, kann der ANDERE Kern jederzeit wecken. Sie
       gilt deshalb nicht als festgefahren -- sonst meldete dieser Kern
       einen Deadlock, obwohl nur der andere noch nicht so weit ist. */
    if (st & (IRK_WAIT_SEMA | IRK_WAIT_QSEND | IRK_WAIT_QRECV |
              IRK_WAIT_RESUME)) {
        (*waiting_for_time)++;
    }
#endif

#if IRK_ENABLE_CYCLIC
    /* Nur aktivieren, wenn die Task auf NICHTS ANDERES wartet: eine
       zyklische Task, die in irk_sema_wait(), irk_queue_recv() oder
       irk_delay() steckt oder angehalten ist, darf bei Faelligkeit nicht
       hineinspringen -- sie kaeme sonst zurueck, ohne das Semaphor zu
       haben, und stuende noch in dessen Warteschlange. IRK_WAIT_DEAD zaehlt wie ueberall nicht als
       Wartegrund. Eine so blockierte Task zaehlt auch nicht als "wartet
       auf Zeit" -- sonst verdeckte sie einen echten Deadlock.            */
    if ((tasks[t].status & (uint16_t)~IRK_WAIT_DEAD) == IRK_WAIT_CYCLE) {
        (*waiting_for_time)++;

        /* Startversatz (irk_task_set_cyclic_at): cycle_ref ist hier noch
           der Aufrufzeitpunkt. Ist der Versatz abgelaufen, wird die
           Referenz auf  Startzeitpunkt - eine Periode  gesetzt. Die erste
           Aktivierung ist dann sofort faellig, und alle weiteren liegen im
           festen Raster ab dem Startzeitpunkt -- auch wenn die Task zum
           Startzeitpunkt gerade blockiert war und erst spaeter hier
           ankommt. */
        if (tasks[t].cycle_start_len != 0 &&
            (irk_time_t)(K->now - tasks[t].cycle_ref) >= tasks[t].cycle_start_len) {
            tasks[t].cycle_ref      += tasks[t].cycle_start_len;
            tasks[t].cycle_ref      -= tasks[t].cycle_time;
            tasks[t].cycle_start_len = 0;
        }

        if (!*cyclic_due && tasks[t].cycle_start_len == 0) {
            irk_time_t seit = K->now - tasks[t].cycle_ref;
            if (seit >= tasks[t].cycle_time) {
                /* Faelligkeit um ganze Perioden vorruecken: normal genau
                   eine. War die Task mehrere Perioden zu spaet, werden die
                   verpassten uebersprungen statt in einem Schwall
                   nachgeholt -- die Phasenlage bleibt. Frueher stand hier
                   Aktivierungen * Periode ; das Produkt lief bei
                   Mikrosekunden nach 71 min ueber. */
                irk_time_t n = seit / tasks[t].cycle_time;
                tasks[t].cycle_ref += n * tasks[t].cycle_time;
                *cyclic_due = t;
            }
        }
    }
#endif
}

void irk_yield(void)
{
    irk_core_t *K = irk_enter();
    uint8_t     c, cur, prev, chosen;
    uint8_t     waiting_for_time;
    uint8_t     deadlock    = 0;
    uint16_t    dead_cycles = 0;
    uint8_t     leer        = 0;   /* letzte Auswahlrunde fand niemanden */
    irk_time_t  slice;
#if IRK_ENABLE_STACKCHECK
    uint8_t     alarm_t = 0;
    size_t      alarm_n = 0;
#endif

    if (K == NULL)  return;
    if (K->lockcnt) return;        /* Scheduler ist gesperrt */
    c = (uint8_t)(K - irk_cores);

    /*--- Verstrichene Zeit der laufenden Task zuschlagen ----------------*/

    IRK_LOCK();

    /* "jetzt - zuletzt" in vorzeichenloser Arithmetik ist ueber jeden
       Zaehlerueberlauf hinweg exakt -- hier braucht es keinen Sonderfall. */
    K->now = irk_port_micros();
    slice  = K->now - K->last;
    cur    = K->current;

    tasks[cur].runtime += slice;
    tasks[cur].slice   += slice;
    irk_charge(cur, slice);
    K->last = K->now;

    /* Die Mindest-Zeitscheibe ist konfigurierbar, standardmaessig aber
       aus: irk_yield() heisst "fuer jetzt fertig", und den Aufwand der
       Runde traegt ohnehin die Task, die danach laeuft. Wer nach jeder
       Anweisung abgibt -- eine Interpreterschleife etwa -- schaltet sie
       ein.                                                              */
#if IRK_MIN_TIMESLICE_US > 0
    if (irk_runnable(cur) && tasks[cur].slice < IRK_MIN_TIMESLICE_US) {
        IRK_UNLOCK();
        return;
    }
#endif

#if IRK_ENABLE_STACKCHECK
    irk_stack_watch_run(K, c, &alarm_t, &alarm_n);
#endif
    IRK_UNLOCK();

    /* Die Fair-Share-Konten schreibt nur dieser Kern -- ohne Sperre. */
    irk_normalize(c);              /* Ueberlaufschutz der Fair-Share-Konten */

    /*--- Anwendercode ausserhalb der Sperre -----------------------------*/

    irk_stack_check(cur);
#if IRK_ENABLE_STACKCHECK
    if (alarm_t != 0 && watch_cb != NULL) watch_cb((irk_task_t)alarm_t, alarm_n);
#endif
    irk_watch_run(K);

    prev = K->current;

    /*--- Auswahlschleife ------------------------------------------------
     *  Sie laeuft so lange, wie keine einzige Task lauffaehig ist. Warten
     *  Tasks auf eine Frist, ist das voellig in Ordnung -- dann muss nur
     *  die Zeit vergehen. Wartet dagegen niemand auf Zeit und ist
     *  trotzdem niemand lauffaehig, liegt ein echter Deadlock vor.
     *
     *  Gesperrt wird nur, was der andere Kern aendern darf, und nur kurz:
     *  der Blick auf jeweils EINE Task (irk_scan_locked) und am Ende das
     *  Festlegen der gewaehlten. Die Fair-Share-Auswahl liest nur Konten
     *  dieses Kerns und laeuft ohne Sperre. Weil der andere Kern die
     *  gewaehlte Task inzwischen angehalten oder geloescht haben kann, wird
     *  die Wahl beim Festlegen unter der Sperre noch einmal geprueft.
     *  So haelt ein Kern, der staendig abgibt, den anderen nicht auf.
     *------------------------------------------------------------------*/
    for (;;) {
        uint8_t    t;
        uint8_t    cyclic_due = 0;
        irk_time_t best_vr    = 0;
        int        gilt;

        K->now = irk_port_micros();

        /* Zeitbuchung: die naechste Task bekommt alles ab dem Eintritt in
           dieses irk_yield() -- Normalisieren, Pruefungen, Auswahl,
           Umschalten. Nur Leerlauf, in dem niemand lauffaehig war, geht
           keine Task etwas an: dann beginnt ihre Buchung erst jetzt.
           (Frueher begann sie immer erst hier; die Zeit davor bekam
           niemand -- auf dem RP2040 gut 5 us je Aufruf.) */
        if (leer) K->last = K->now;
        leer = 0;

        waiting_for_time = 0;
        chosen           = 0;

        /*--- Fristen pruefen: wecken, toeten, zyklisch aktivieren ------*/
        for (t = 1; t <= irk_ntasks; t++) {
            if (tasks[t].core != c) continue;   /* Vorfilter ohne Sperre */
            IRK_LOCK();
            irk_scan_locked(K, c, t, &waiting_for_time, &cyclic_due);
            IRK_UNLOCK();
        }

#if IRK_ENABLE_CYCLIC
        /* Eine faellige zyklische Task hat Vorrang vor allem anderen. */
        if (cyclic_due) chosen = cyclic_due;
#endif

        /*--- Fair Share: die Task mit dem groessten Rueckstand ---------*/
        if (chosen == 0) {
            for (t = 1; t <= irk_ntasks; t++) {
                if (tasks[t].core != c) continue;
                if (!irk_runnable(t)) continue;
                if (tasks[t].priority == 0) continue;   /* nicht einplanen */
                /* Eben vom anderen Kern geweckt, noch nicht eingeordnet:
                   ihr Konto ist veraltet. Sie kommt in der naechsten
                   Runde dran, nach irk_align(). */
                if (tasks[t].needs_align) continue;

                /* Kleinstes Konto gewinnt. "<=" waehlt bei Gleichstand die
                   zuletzt gepruefte Task und laesst gleichrangige Tasks so
                   reihum drankommen. */
                if (chosen == 0 || tasks[t].vruntime <= best_vr) {
                    best_vr = tasks[t].vruntime;
                    chosen  = t;
                }
            }
        }

        if (chosen != 0) {
            /* Festlegen unter der Sperre. Gilt die Wahl noch? Die Task
               muss noch diesem Kern gehoeren (Slot nicht neu vergeben) und
               noch lauffaehig bzw. noch zyklisch faellig sein. */
            IRK_LOCK();
            if (chosen == cyclic_due) {
                gilt = (tasks[chosen].status & (uint16_t)~IRK_WAIT_DEAD) == IRK_WAIT_CYCLE;
            } else {
                gilt = irk_runnable(chosen);
            }
            if (gilt && tasks[chosen].core == c) {
                /* Kern-Minimum nachziehen: es folgt dem Konto der
                   eben gewaehlten Task und sinkt nie. Laeuft eine Task
                   allein, folgt es ihr -- wer spaeter aufwacht, wird darauf
                   gehoben. Zyklische Tasks laufen ausserhalb von Fair Share. */
                if (chosen != cyclic_due && best_vr > K->vmin) K->vmin = best_vr;
                break;                                      /* Sperre bleibt */
            }
            IRK_UNLOCK();
            continue;                                       /* neue Runde */
        }

        /*--- Niemand lauffaehig --------------------------------------*/
        leer = 1;
        if (waiting_for_time == 0) {
            if (++dead_cycles > IRK_DEADLOCK_CYCLES) {
                /* Echter Deadlock: Haupttask dieses Kerns erzwingen und
                   -- nach der Sperre -- melden. */
                IRK_LOCK();
                tasks[c + 1u].status = IRK_RUNNING;
                chosen   = (uint8_t)(c + 1u);
                deadlock = 1;
                break;                                      /* Sperre bleibt */
            }
        } else {
            /* Es laeuft eine Frist, oder der andere Kern kann wecken:
               kurz ruhen -- ohne Sperre. */
            dead_cycles = 0;
            irk_port_idle();
        }
    }

    /*--- Umschalten (unter der Sperre) ----------------------------------*/

    tasks[chosen].slice = 0;     /* K->last bleibt: siehe Zeitbuchung oben */
#if IRK_ENABLE_STATS
    tasks[chosen].calls++;
#endif

    if (chosen == prev) {                     /* dieselbe Task, nichts tun*/
        IRK_UNLOCK();
        if (deadlock) irk_fail(IRK_ERR_DEADLOCK, c);
        return;
    }

    K->current = chosen;
    if (tasks[prev].status & IRK_KILLED) {
        /* Die verlassene Task ist tot. Ihr Slot bleibt gesperrt, bis der
           Wechsel wirklich vollzogen ist -- siehe irk_after_switch(). */
        K->dead_prev = prev;
    }

    IRK_UNLOCK();                             /* nie ueber den Wechsel!   */

    if (deadlock) irk_fail(IRK_ERR_DEADLOCK, c);

    irk_ctx_switch(&tasks[prev].sp, tasks[chosen].sp);

    /* Hier geht es weiter, wenn spaeter wieder auf "prev" umgeschaltet
       wird -- immer auf demselben Kern, Tasks wechseln ihn nie.         */
    irk_after_switch(K);
}


/*========================================================================*\
 *  Task anhalten, fortsetzen, loeschen
 *
 *  Alle Funktionen hier duerfen eine Task auf JEDEM Kern betreffen. Sie
 *  aendern nur deren Zustand; betrifft das eine Task, die gerade auf dem
 *  anderen Kern laeuft, wirkt es bei deren naechstem irk_yield().
\*========================================================================*/

/* Unter der Sperre: Wartebit loeschen. Ist die Task danach lauffaehig,
   wird sie zum Einordnen vorgemerkt -- einordnen darf nur ihr eigener
   Kern, und der tut es im naechsten Scheduler-Durchlauf. */
static int irk_unblock_locked(uint8_t t, uint16_t reason)
{
    if (irk_runnable(t)) return 0;                    /* laeuft schon */

    tasks[t].status &= (uint16_t)~reason;

    if (irk_runnable(t)) {
        tasks[t].needs_align = 1;
        tasks[t].slice       = 0;
        return 1;
    }
    return 0;
}

/* Unter der Sperre: Task als geloescht eintragen und aus einer
   Warteschlange loesen. Laeuft sie gerade auf ihrem Kern, bleibt ihr Slot
   als Zombie gesperrt, bis dieser Kern von ihr weggeschaltet hat -- ihr
   Stack ist bis dahin noch in Benutzung. */
static void irk_kill_locked(uint8_t t)
{
    uint8_t k = tasks[t].core;

    irk_q_detach(t);
    tasks[t].status      = IRK_KILLED;
    tasks[t].needs_align = 0;
    tasks[t].zombie      = (uint8_t)(irk_cores[k].started &&
                                     irk_cores[k].current == t);
}

/* Wartebit setzen. Betrifft es die eigene, laufende Task, wird sofort
   abgegeben -- erst nachdem die Sperre freigegeben ist. */
static int irk_block(irk_core_t *K, uint8_t t, uint16_t reason)
{
    IRK_LOCK();
    if (!irk_valid(t)) { IRK_UNLOCK(); irk_fail(IRK_ERR_BAD_TASK, t); return -1; }

    tasks[t].status |= reason;

    if (t == K->current) {
        /* Mindest-Zeitscheibe umgehen: diese Task darf jetzt nicht
           weiterlaufen, egal wie kurz sie schon lief. */
        tasks[t].slice = IRK_MIN_TIMESLICE_US;
        IRK_UNLOCK();
        irk_yield();
        return 0;
    }
    IRK_UNLOCK();
    return 0;
}

int irk_task_suspend(irk_task_t tsk)
{
    irk_core_t *K = irk_enter();
    if (K == NULL) return -1;
    return irk_block(K, tsk, IRK_WAIT_RESUME);
}

int irk_task_resume(irk_task_t tsk)
{
    int r;

    if (irk_enter() == NULL) return -1;

    IRK_LOCK();
    if (!irk_valid(tsk)) { IRK_UNLOCK(); irk_fail(IRK_ERR_BAD_TASK, tsk); return -1; }
    r = irk_unblock_locked(tsk, IRK_WAIT_RESUME);
    IRK_UNLOCK();
    return r;
}

int irk_task_kill(irk_task_t tsk)
{
    irk_core_t *K = irk_enter();

    if (K == NULL) return -1;
    if (irk_is_main(tsk)) { irk_fail(IRK_ERR_KILL_MAIN, tsk); return -1; }

    IRK_LOCK();
    if (!irk_valid(tsk)) { IRK_UNLOCK(); irk_fail(IRK_ERR_BAD_TASK, tsk); return -1; }

    /* Aus der Warteschlange loesen, in der sie ggf. haengt -- sonst
       bliebe dort ein Eintrag auf eine Task stehen, die es nicht mehr
       gibt. Das erledigt irk_kill_locked().

       Der Stack wird bewusst NICHT hier freigegeben: loescht sich die
       Task selbst, laeuft der folgende irk_yield() noch auf genau diesem
       Stack. Freigabe erfolgt bei Wiederverwendung des Slots oder in
       irk_deinit(). */
    irk_kill_locked(tsk);

    if (tsk == K->current) {
        /* Bedingungslos abgeben: in einer bereits toten Task darf nicht
           weitergelaufen werden. */
        tasks[tsk].slice = IRK_MIN_TIMESLICE_US;
        IRK_UNLOCK();
        for (;;) {
            irk_yield();        /* kehrt nie zurueck */
        }
    }
    IRK_UNLOCK();
    return 1;
}

/* Millisekunden in Mikrosekunden, bei 32-Bit-Zeit auf die laengste
   Einzelfrist begrenzt statt ueberzulaufen. */
static irk_time_t irk_ms_to_us(uint32_t ms)
{
#if IRK_TIME_64
    return (irk_time_t)ms * 1000u;
#else
    if (ms > (uint32_t)(IRK_MAX_WAIT_US / 1000u)) return IRK_MAX_WAIT_US;
    return (irk_time_t)ms * 1000u;
#endif
}

static irk_time_t irk_clamp_wait(irk_time_t us)
{
    return (us > IRK_MAX_WAIT_US) ? IRK_MAX_WAIT_US : us;
}

/* Darf von ueberall gerufen werden, auch aus Interrupts. */
irk_time_t irk_now_us(void)
{
    return irk_port_micros();
}

int irk_task_kill_after_us(irk_task_t tsk, irk_time_t delay_us)
{
    if (irk_enter() == NULL) return -1;
    if (irk_is_main(tsk)) { irk_fail(IRK_ERR_KILL_MAIN, tsk); return -1; }

    IRK_LOCK();
    if (!irk_valid(tsk)) { IRK_UNLOCK(); irk_fail(IRK_ERR_BAD_TASK, tsk); return -1; }

    tasks[tsk].kill_start = irk_port_micros();
    tasks[tsk].kill_len   = irk_clamp_wait(delay_us);

    /* Das Wartebit setzen, ohne die Task anzuhalten: sie soll bis zu
       ihrem Todeszeitpunkt ganz normal weiterlaufen. Der Scheduler
       behandelt IRK_WAIT_DEAD deshalb als lauffaehig. */
    tasks[tsk].status |= IRK_WAIT_DEAD;
    IRK_UNLOCK();
    return 1;
}

int irk_task_kill_after(irk_task_t tsk, uint32_t delay_ms)
{
    return irk_task_kill_after_us(tsk, irk_ms_to_us(delay_ms));
}

int irk_task_kill_at(irk_task_t tsk, uint32_t when_ms)
{
    /* Absolute Angabe in ms seit Start dieses Kerns in eine Frist
       umrechnen. Liegt der Zeitpunkt schon zurueck, stirbt die Task beim
       naechsten Scheduler-Durchlauf. */
    irk_core_t *K = irk_enter();
    uint32_t    jetzt_ms;

    if (K == NULL) return -1;
    jetzt_ms = (uint32_t)((irk_time_t)(irk_port_micros() - K->t0) / 1000u);
    return irk_task_kill_after(tsk, (when_ms > jetzt_ms) ? when_ms - jetzt_ms : 0);
}

int irk_task_delay_us(irk_task_t tsk, irk_time_t delay_us)
{
    irk_core_t *K = irk_enter();

    if (K == NULL) return -1;

    IRK_LOCK();
    if (!irk_valid(tsk)) { IRK_UNLOCK(); irk_fail(IRK_ERR_BAD_TASK, tsk); return -1; }

    /* Start + Dauer statt Zielzeitpunkt: der Scheduler prueft
       "jetzt - Start >= Dauer", und das ist ueberlauffest. */
    tasks[tsk].wait_start = irk_port_micros();
    tasks[tsk].wait_len   = irk_clamp_wait(delay_us);
    IRK_UNLOCK();

    return irk_block(K, tsk, IRK_WAIT_TIME);
}

int irk_task_delay(irk_task_t tsk, uint32_t delay_ms)
{
    return irk_task_delay_us(tsk, irk_ms_to_us(delay_ms));
}

void irk_delay_us(irk_time_t delay_us)
{
    irk_core_t *K = irk_enter();

    if (K == NULL) return;

    /* Laengere Fristen als die groesste Einzelfrist in Stuecken warten --
       das betrifft nur 32-Bit-Zeit. */
    while (delay_us > IRK_MAX_WAIT_US) {
        irk_task_delay_us(K->current, IRK_MAX_WAIT_US);
        delay_us -= IRK_MAX_WAIT_US;
    }
    irk_task_delay_us(K->current, delay_us);
}

void irk_delay(uint32_t delay_ms)
{
#if IRK_TIME_64
    irk_delay_us((irk_time_t)delay_ms * 1000u);
#else
    /* 32-Bit-Zeit: in Stuecken von 2.000.000 ms (33 min) warten, damit die
       Umrechnung in Mikrosekunden nie ueberlaeuft. Beliebig lange Zeiten
       sind so moeglich. */
    while (delay_ms > 2000000u) {
        irk_delay_us(2000000000u);
        delay_ms -= 2000000u;
    }
    irk_delay_us((irk_time_t)delay_ms * 1000u);
#endif
}


/*========================================================================*\
 *  Prioritaeten und Abfragen
\*========================================================================*/

int irk_task_set_prio(irk_task_t tsk, irk_prio_t prio)
{
    irk_prio_t alt;

    if (irk_enter() == NULL) return -1;

#if IRK_MAX_PRIO < 65535
    if (prio > IRK_MAX_PRIO) prio = IRK_MAX_PRIO;
#endif

    IRK_LOCK();
    if (!irk_valid(tsk)) { IRK_UNLOCK(); irk_fail(IRK_ERR_BAD_TASK, tsk); return -1; }

    /* Das Konto bleibt, wie es ist: es ist bereits auf die Prioritaet
       normiert, die neue Prioritaet wirkt nur auf kuenftige Laufzeit.
       Kommt die Task aus Prioritaet 0 (nicht eingeplant), ist ihr Konto
       veraltet -- dann wird sie zum Einordnen vorgemerkt. */
    alt = tasks[tsk].priority;
    tasks[tsk].priority = prio;
    tasks[tsk].vrem     = 0;           /* Rest war auf die alte Prio bezogen */
    if (alt == 0 && prio > 0 && irk_runnable(tsk)) tasks[tsk].needs_align = 1;

    IRK_UNLOCK();
    return 0;
}

irk_prio_t irk_task_get_prio(irk_task_t tsk)
{
    return irk_valid(tsk) ? tasks[tsk].priority : 0;
}

uint16_t irk_task_status(irk_task_t tsk)
{
    if (tsk < 1 || tsk > irk_ntasks) return IRK_KILLED;
    return tasks[tsk].status;
}

irk_task_t irk_task_self(void)
{
    irk_core_t *K = irk_enter();
    return (K != NULL) ? (irk_task_t)K->current : IRK_NO_TASK;
}

uint8_t irk_task_count(void)
{
    return irk_ntasks;
}

uint32_t irk_task_runtime(irk_task_t tsk)
{
    uint32_t ms = 0;
    IRK_LOCK();                       /* 64-Bit-Wert nicht zerrissen lesen */
    if (irk_valid(tsk)) ms = (uint32_t)(tasks[tsk].runtime / 1000u);
    IRK_UNLOCK();
    return ms;
}

irk_time_t irk_task_runtime_us(irk_task_t tsk)
{
    irk_time_t us = 0;
    IRK_LOCK();
    if (irk_valid(tsk)) us = tasks[tsk].runtime;
    IRK_UNLOCK();
    return us;
}

irk_time_t irk_task_vruntime(irk_task_t tsk)
{
    irk_time_t v = 0;
    IRK_LOCK();
    if (irk_valid(tsk)) v = tasks[tsk].vruntime;
    IRK_UNLOCK();
    return v;
}

uint32_t irk_task_calls(irk_task_t tsk)
{
#if IRK_ENABLE_STATS
    uint32_t n = 0;
    IRK_LOCK();
    if (irk_valid(tsk)) n = (uint32_t)tasks[tsk].calls;
    IRK_UNLOCK();
    return n;
#else
    (void)tsk;
    return 0;
#endif
}

#if IRK_ENABLE_NAMES
const char *irk_task_name(irk_task_t tsk)
{
    return irk_valid(tsk) ? tasks[tsk].name : NULL;
}

irk_task_t irk_task_by_name(const char *name)
{
    uint8_t t;
    if (name == NULL) return IRK_NO_TASK;
    for (t = 1; t <= irk_ntasks; t++) {
        if (!(tasks[t].status & IRK_KILLED) && tasks[t].name &&
            strcmp(tasks[t].name, name) == 0) {
            return (irk_task_t)t;
        }
    }
    return IRK_NO_TASK;
}
#endif


/*========================================================================*\
 *  Zyklische Tasks
\*========================================================================*/

#if IRK_ENABLE_CYCLIC

int irk_task_set_cyclic_at_us(irk_task_t tsk, irk_time_t period_us,
                              irk_time_t start_after_us)
{
    irk_core_t *K = irk_enter();

    if (K == NULL) return -1;

    IRK_LOCK();
    if (!irk_valid(tsk)) { IRK_UNLOCK(); irk_fail(IRK_ERR_BAD_TASK, tsk); return -1; }
    if (period_us == 0 || period_us > IRK_MAX_WAIT_US) { IRK_UNLOCK(); return -1; }

    tasks[tsk].cycle_time = period_us;

    if (start_after_us == 0) {
        /* Referenz eine Periode in der Vergangenheit: die erste Aktivierung
           ist sofort faellig. Die Subtraktion darf ueberlaufen -- sie wird
           nur als Differenz "jetzt - Referenz" ausgewertet. */
        tasks[tsk].cycle_ref       = irk_port_micros() - period_us;
        tasks[tsk].cycle_start_len = 0;
    } else {
        /* Startversatz wie eine Frist: Aufrufzeitpunkt plus Dauer. Der
           Scheduler loest daraus beim Ablauf das feste Raster auf. */
        tasks[tsk].cycle_ref       = irk_port_micros();
        tasks[tsk].cycle_start_len = irk_clamp_wait(start_after_us);
    }
    /* Bit HINZUFUEGEN, nicht den Status ueberschreiben: steht die Task
       gerade in einer Warteschlange, muss ihr Wartegrund erhalten bleiben
       (siehe irk_wake_due()). */
    tasks[tsk].status |= IRK_WAIT_CYCLE;

    if (tsk == K->current) {
        tasks[tsk].slice = IRK_MIN_TIMESLICE_US;
        IRK_UNLOCK();
        irk_yield();
        return 1;
    }
    IRK_UNLOCK();
    return 1;
}

int irk_task_set_cyclic_us(irk_task_t tsk, irk_time_t period_us)
{
    return irk_task_set_cyclic_at_us(tsk, period_us, 0);
}

int irk_task_set_cyclic(irk_task_t tsk, uint32_t period_ms)
{
    if (period_ms == 0) return -1;
    return irk_task_set_cyclic_at_us(tsk, irk_ms_to_us(period_ms), 0);
}

int irk_task_set_cyclic_at(irk_task_t tsk, uint32_t period_ms, uint32_t start_after_ms)
{
    if (period_ms == 0) return -1;
    return irk_task_set_cyclic_at_us(tsk, irk_ms_to_us(period_ms),
                                     irk_ms_to_us(start_after_ms));
}

/* Beendet den zyklischen Betrieb. Das Zyklus-Bit wird geloescht, nicht
   vom Status abgezogen: sonst blieben fremde Wartegruende zurueck. */
int irk_task_set_normal(irk_task_t tsk, irk_prio_t prio)
{
    if (irk_enter() == NULL) return -1;

#if IRK_MAX_PRIO < 65535
    if (prio > IRK_MAX_PRIO) prio = IRK_MAX_PRIO;
#endif

    IRK_LOCK();
    if (!irk_valid(tsk)) { IRK_UNLOCK(); irk_fail(IRK_ERR_BAD_TASK, tsk); return -1; }
    if (!(tasks[tsk].status & IRK_WAIT_CYCLE)) { IRK_UNLOCK(); return 0; }

    tasks[tsk].status  &= (uint16_t)~IRK_WAIT_CYCLE;
    tasks[tsk].priority = prio;
    tasks[tsk].vrem     = 0;
    if (irk_runnable(tsk)) tasks[tsk].needs_align = 1;

    IRK_UNLOCK();
    return 1;
}

#endif /* IRK_ENABLE_CYCLIC */


/*========================================================================*\
 *  Zaehlsemaphore  --  gemeinsam fuer alle Kerne
 *
 *  Eine Task auf Kern 1 darf auf ein Semaphor warten, das eine Task auf
 *  Kern 0 freigibt. Muster beim Warten: unter der Sperre einreihen und
 *  Wartebit setzen, Sperre freigeben, DANN abgeben. Gibt der andere Kern
 *  das Semaphor genau dazwischen frei, ist das Bit schon wieder geloescht
 *  und irk_yield() kehrt sofort zurueck -- kein verlorener Weckruf.
\*========================================================================*/

int irk_sema_init(irk_sema_t sema, int16_t count)
{
    if (irk_enter() == NULL) return -1;
    if (sema >= IRK_MAX_SEMAPHORES) { irk_fail(IRK_ERR_BAD_SEMA, sema); return -1; }

    IRK_LOCK();
    if (semas[sema].valid && semas[sema].q_head != 0) {
        /* Neuinitialisierung, waehrend noch Tasks warten -- die wuerden
           sonst nie wieder geweckt. */
        IRK_UNLOCK();
        irk_fail(IRK_ERR_BAD_SEMA, sema);
        return -1;
    }
    semas[sema].count  = count;
    semas[sema].init   = count;
    semas[sema].q_head = 0;
    semas[sema].q_tail = 0;
    semas[sema].valid  = 1;
    IRK_UNLOCK();
    return 0;
}

int irk_sema_wait(irk_sema_t sema)
{
    irk_core_t *K = irk_enter();
    uint8_t     cur;

    if (K == NULL) return -1;
    if (sema >= IRK_MAX_SEMAPHORES) { irk_fail(IRK_ERR_BAD_SEMA, sema); return -1; }

    IRK_LOCK();
    if (!semas[sema].valid) { IRK_UNLOCK(); irk_fail(IRK_ERR_SEMA_UNINIT, sema); return -1; }

    semas[sema].count--;

    if (semas[sema].count < 0) {
        cur = K->current;
        tasks[cur].wait_obj = &semas[sema];
        irk_q_push(&semas[sema].q_head, &semas[sema].q_tail, cur);
        tasks[cur].status |= IRK_WAIT_SEMA;
        tasks[cur].slice   = IRK_MIN_TIMESLICE_US;
        IRK_UNLOCK();
        irk_yield();                  /* kehrt erst zurueck, wenn geweckt */
        return 0;
    }
    IRK_UNLOCK();
    return 0;
}

int irk_sema_try_wait(irk_sema_t sema)
{
    int r = 0;

    if (irk_enter() == NULL) return -1;
    if (sema >= IRK_MAX_SEMAPHORES) { irk_fail(IRK_ERR_BAD_SEMA, sema); return -1; }

    IRK_LOCK();
    if (!semas[sema].valid) { IRK_UNLOCK(); irk_fail(IRK_ERR_SEMA_UNINIT, sema); return -1; }
    if (semas[sema].count > 0) {
        semas[sema].count--;
        r = 1;
    }
    IRK_UNLOCK();
    return r;
}

int irk_sema_signal(irk_sema_t sema)
{
    uint8_t t;

    if (irk_enter() == NULL) return -1;
    if (sema >= IRK_MAX_SEMAPHORES) { irk_fail(IRK_ERR_BAD_SEMA, sema); return -1; }

    IRK_LOCK();
    if (!semas[sema].valid) { IRK_UNLOCK(); irk_fail(IRK_ERR_SEMA_UNINIT, sema); return -1; }

    if (semas[sema].count >= semas[sema].init) {
        IRK_UNLOCK();
        irk_fail(IRK_ERR_SIGNAL_NO_WAIT, sema);
        return -1;
    }

    semas[sema].count++;

    if (semas[sema].count <= 0) {
        t = irk_q_pop(&semas[sema].q_head, &semas[sema].q_tail);
        if (t != 0) {
            tasks[t].wait_obj = NULL;
            irk_unblock_locked(t, IRK_WAIT_SEMA);
        }
    }
    IRK_UNLOCK();
    return 0;
}

int16_t irk_sema_count(irk_sema_t sema)
{
    if (sema >= IRK_MAX_SEMAPHORES) return 0;
    return semas[sema].count;
}


/*========================================================================*\
 *  Queues  --  Inter-Task-Kommunikation, gemeinsam fuer alle Kerne
 *
 *  An der Stelle der Nachrichten des urspruenglichen Entwurfs, die je
 *  Nachricht Speicher anforderten -- auf einem Mikrocontroller keine
 *  Option.
 *
 *  Hier stellt der Aufrufer den Speicher, die Queue verwaltet ihn nur.
 *  Die beiden Warteschlangen (Sender, die auf Platz warten; Empfaenger,
 *  die auf Daten warten) haengen wie bei den Semaphoren intrusiv in den
 *  Taskdescriptoren. Erzeuger und Verbraucher duerfen auf verschiedenen
 *  Kernen laufen.
\*========================================================================*/

/* Kopiert item_size Byte -- memcpy waere hier auf kleinen Elementen
   oft der teurere Weg, und die Groesse steht erst zur Laufzeit fest. */
static void irk_copy(uint8_t *dst, const uint8_t *src, uint8_t n)
{
    while (n--) *dst++ = *src++;
}

/* Unter der Sperre: weckt die laengstwartende Task einer Schlange. */
static void irk_wake_one(uint8_t *head, uint8_t *tail, uint16_t reason)
{
    uint8_t t = irk_q_pop(head, tail);
    if (t != 0) {
        tasks[t].wait_obj = NULL;
        irk_unblock_locked(t, reason);
    }
}

int irk_queue_init(irk_queue_t *q, void *storage,
                   uint8_t item_size, uint8_t capacity)
{
    if (irk_enter() == NULL) return -1;
    if (q == NULL || storage == NULL) return -1;
    if (item_size == 0 || capacity == 0) return -1;

    IRK_LOCK();
    q->buf         = (uint8_t *)storage;
    q->item_size   = item_size;
    q->capacity    = capacity;
    q->count       = 0;
    q->head        = 0;
    q->tail        = 0;
    q->q_send      = 0;
    q->q_send_tail = 0;
    q->q_recv      = 0;
    q->q_recv_tail = 0;
    q->valid       = 1;
    IRK_UNLOCK();
    return 0;
}

int irk_queue_send(irk_queue_t *q, const void *item)
{
    irk_core_t *K = irk_enter();
    uint8_t     cur;

    if (K == NULL) return -1;
    if (q == NULL || item == NULL) return -1;

    IRK_LOCK();
    if (!q->valid) { IRK_UNLOCK(); return -1; }

    /* Als Schleife, nicht als einfache Abfrage: nach dem Wecken koennte
       eine andere Task -- auch auf dem anderen Kern -- den freigewordenen
       Platz schon belegt haben. */
    while (q->count >= q->capacity) {
        cur = K->current;
        tasks[cur].wait_obj = q;
        irk_q_push(&q->q_send, &q->q_send_tail, cur);
        tasks[cur].status |= IRK_WAIT_QSEND;
        tasks[cur].slice   = IRK_MIN_TIMESLICE_US;
        IRK_UNLOCK();
        irk_yield();
        IRK_LOCK();
        if (!q->valid) { IRK_UNLOCK(); return -1; }
    }

    irk_copy(q->buf + (uint16_t)q->tail * q->item_size,
             (const uint8_t *)item, q->item_size);
    q->tail = (uint8_t)((q->tail + 1u) % q->capacity);
    q->count++;

    irk_wake_one(&q->q_recv, &q->q_recv_tail, IRK_WAIT_QRECV);
    IRK_UNLOCK();
    return 0;
}

int irk_queue_recv(irk_queue_t *q, void *item)
{
    irk_core_t *K = irk_enter();
    uint8_t     cur;

    if (K == NULL) return -1;
    if (q == NULL || item == NULL) return -1;

    IRK_LOCK();
    if (!q->valid) { IRK_UNLOCK(); return -1; }

    while (q->count == 0) {
        cur = K->current;
        tasks[cur].wait_obj = q;
        irk_q_push(&q->q_recv, &q->q_recv_tail, cur);
        tasks[cur].status |= IRK_WAIT_QRECV;
        tasks[cur].slice   = IRK_MIN_TIMESLICE_US;
        IRK_UNLOCK();
        irk_yield();
        IRK_LOCK();
        if (!q->valid) { IRK_UNLOCK(); return -1; }
    }

    irk_copy((uint8_t *)item,
             q->buf + (uint16_t)q->head * q->item_size, q->item_size);
    q->head = (uint8_t)((q->head + 1u) % q->capacity);
    q->count--;

    irk_wake_one(&q->q_send, &q->q_send_tail, IRK_WAIT_QSEND);
    IRK_UNLOCK();
    return 0;
}

int irk_queue_try_send(irk_queue_t *q, const void *item)
{
    if (irk_enter() == NULL) return -1;
    if (q == NULL || item == NULL) return -1;

    IRK_LOCK();
    if (!q->valid) { IRK_UNLOCK(); return -1; }
    if (q->count >= q->capacity) { IRK_UNLOCK(); return 0; }

    irk_copy(q->buf + (uint16_t)q->tail * q->item_size,
             (const uint8_t *)item, q->item_size);
    q->tail = (uint8_t)((q->tail + 1u) % q->capacity);
    q->count++;

    irk_wake_one(&q->q_recv, &q->q_recv_tail, IRK_WAIT_QRECV);
    IRK_UNLOCK();
    return 1;
}

int irk_queue_try_recv(irk_queue_t *q, void *item)
{
    if (irk_enter() == NULL) return -1;
    if (q == NULL || item == NULL) return -1;

    IRK_LOCK();
    if (!q->valid) { IRK_UNLOCK(); return -1; }
    if (q->count == 0) { IRK_UNLOCK(); return 0; }

    irk_copy((uint8_t *)item,
             q->buf + (uint16_t)q->head * q->item_size, q->item_size);
    q->head = (uint8_t)((q->head + 1u) % q->capacity);
    q->count--;

    irk_wake_one(&q->q_send, &q->q_send_tail, IRK_WAIT_QSEND);
    IRK_UNLOCK();
    return 1;
}

uint8_t irk_queue_count(const irk_queue_t *q)
{
    return (q != NULL && q->valid) ? q->count : 0;
}

int irk_queue_flush(irk_queue_t *q)
{
    if (irk_enter() == NULL) return -1;
    if (q == NULL) return -1;

    IRK_LOCK();
    if (!q->valid) { IRK_UNLOCK(); return -1; }

    q->count = 0;
    q->head  = 0;
    q->tail  = 0;

    /* Alle wartenden Sender wecken -- es ist jetzt reichlich Platz. */
    while (q->q_send != 0) {
        irk_wake_one(&q->q_send, &q->q_send_tail, IRK_WAIT_QSEND);
    }
    IRK_UNLOCK();
    return 0;
}


/*========================================================================*\
 *  Scheduler sperren  --  je Kern
\*========================================================================*/

void irk_lock(void)
{
    irk_core_t *K = irk_enter();
    if (K != NULL && K->lockcnt < 255) K->lockcnt++;
}

void irk_unlock(void)
{
    irk_core_t *K = irk_enter();
    if (K != NULL && K->lockcnt > 0) K->lockcnt--;
}


/*========================================================================*\
 *  Lizenz  --  Vorgabe
\*========================================================================*/

/* Schwach gebunden: ohne eigene Pruefung gilt die Benutzung als gueltig.
   Der Kernel ruft diese Funktion nirgends auf -- warum, steht in
   IRKernel.h. */
#if defined(__GNUC__)
__attribute__((weak))
#endif
int irk_license_valid(void)
{
    return 1;
}
