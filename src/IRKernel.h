/*========================================================================*\
 *
 *  IRKernel  --  Stackful-kooperatives Multitasking mit
 *                prioritaetsproportionaler Rechenzeitverteilung
 *
 *  Herkunft:  PC-Robotersteuerung, Modul irkernel.c
 *             Institut fuer Roboterforschung, Universitaet Dortmund
 *             Andreas Keibel, Oktober 1993
 *
 *  Was diesen Kernel von den ueblichen kooperativen Schedulern
 *  unterscheidet:
 *
 *   - Stackful.   Jede Task hat einen eigenen Stack. Eine Task darf
 *                 mitten in einer Funktion, auf beliebiger Aufruftiefe,
 *                 die CPU abgeben und spaeter mit intakten lokalen
 *                 Variablen weiterlaufen. Callback-basierte Scheduler
 *                 koennen das prinzipbedingt nicht.
 *
 *   - Fair Share. Die Prioritaet einer Task bestimmt ihren *Anteil* an
 *                 der Rechenzeit, nicht ihren Vorrang. Eine Task mit
 *                 Prioritaet 3 bekommt dreimal so viel CPU-Zeit wie eine
 *                 mit Prioritaet 1 -- sie verdraengt sie nicht.
 *                 Es verhungert also niemand.
 *
 *   - Zyklisch.   Tasks koennen periodisch aktiviert werden und haben
 *                 dann Vorrang vor allem anderen. Das ist der Baustein
 *                 fuer Regelkreise.
 *
 *   - Kooperativ. Zwischen zwei irk_yield()-Aufrufen kann niemand
 *                 dazwischenfunken. Geteilte Variablen brauchen keine
 *                 Mutexe, und es gibt keine Race Conditions.
 *
 *  Wer bestehenden Code des urspruenglichen Kernels uebersetzen will, bindet zusaetzlich
 *  IRKernel_classic.h ein -- das bildet die Originalnamen (Init_task,
 *  Done, RUNNING, ...) auf die hier definierten ab.
 *
\*========================================================================*/

#ifndef IRKERNEL_H
#define IRKERNEL_H

#include <stddef.h>
#include <stdint.h>

#include "irk_config.h"

#ifdef __cplusplus
extern "C" {
#endif


/*========================================================================*\
 *  Typen
\*========================================================================*/

/* Task-Handle. 0 ist immer ungueltig. Handles sind ueber alle Kerne hinweg
 * eindeutig. Die Haupttasks haben feste Handles: Kern 0 hat 1, Kern 1 hat 2
 * -- unabhaengig davon, welcher Kern zuerst startet. */
typedef uint8_t irk_task_t;

#define IRK_NO_TASK             ((irk_task_t)0)
#define IRK_MAIN_TASK           ((irk_task_t)1)
#define IRK_MAIN_TASK_OF(core)  ((irk_task_t)((core) + 1))

/* Semaphor-Handle: Index 0 .. IRK_MAX_SEMAPHORES-1 */
typedef uint8_t irk_sema_t;

/* Prioritaet: 0 = nicht einplanen, sonst 1 .. IRK_MAX_PRIO (hoechstens
 * 65535). Sie bestimmt den ANTEIL an der Rechenzeit: 1 neben 9999 ergibt
 * 0,01 %. Seit 2.0 16 Bit breit, vorher 8 Bit (hoechstens 1 : 255). */
typedef uint16_t irk_prio_t;


/*------------------------------------------------------------------------*\
 *  Taskzustaende.  Bitmaske: RUNNING (0) heisst "kein Grund zu warten".
 *  Eine Task ist genau dann lauffaehig, wenn ihr Zustand 0 ist.
\*------------------------------------------------------------------------*/

#define IRK_RUNNING          0x0000u  /* lauffaehig                        */
#define IRK_WAIT_TIME        0x0001u  /* wartet auf Ablauf einer Frist     */
#define IRK_WAIT_SEMA        0x0002u  /* wartet auf ein Semaphor           */
#define IRK_WAIT_RESUME      0x0004u  /* angehalten, wartet auf Fortsetzung*/
#define IRK_WAIT_DEAD        0x0008u  /* wartet auf den Zeitpunkt des Todes*/
#define IRK_WAIT_CYCLE       0x0010u  /* wartet auf zyklische Reaktivierung*/
#define IRK_WAIT_QSEND       0x0020u  /* wartet auf Platz in einer Queue   */
#define IRK_WAIT_QRECV       0x0040u  /* wartet auf Daten aus einer Queue  */
#define IRK_WAIT_USER        0x0080u  /* frei fuer aufsetzende Schichten   */
#define IRK_KILLED           0x8000u  /* geloescht, Slot wiederverwendbar  */


/*------------------------------------------------------------------------*\
 *  Fehlercodes, wie sie an irk_port_error() gemeldet werden
\*------------------------------------------------------------------------*/

#define IRK_ERR_NONE             0
#define IRK_ERR_NO_MEMORY        1   /* kein Slot / Stack zu klein         */
#define IRK_ERR_TOO_MANY_TASKS   2   /* IRK_MAX_TASKS erschoepft           */
#define IRK_ERR_BAD_TASK         3   /* ungueltiges Task-Handle            */
#define IRK_ERR_BAD_SEMA         4   /* ungueltiges Semaphor-Handle        */
#define IRK_ERR_SEMA_UNINIT      5   /* Semaphor nicht initialisiert       */
#define IRK_ERR_SIGNAL_NO_WAIT   6   /* zu oft freigegeben                 */
#define IRK_ERR_DEADLOCK         7   /* keine Task mehr lauffaehig         */
#define IRK_ERR_STACK_OVERFLOW   8   /* Stackueberlauf erkannt             */
#define IRK_ERR_NOT_INIT         9   /* Kernel nicht initialisiert         */
#define IRK_ERR_KILL_MAIN       10   /* Versuch, eine Haupttask zu loeschen*/
#define IRK_ERR_WRONG_CORE      11   /* Aufruf von einem unbekannten Kern  */
#define IRK_ERR_IN_ISR          12   /* Aufruf aus einem Interrupt-Handler */


/*========================================================================*\
 *  Kernel starten und beenden
\*========================================================================*/

/* Initialisiert den Kernel auf dem aufrufenden Kern und meldet den dort
 * laufenden Code (auf Arduino: setup()/loop(), auf Kern 1 setup1()/loop1())
 * als Haupttask mit der Prioritaet prio an.
 *
 * Der Aufruf ist NICHT noetig: jede IRKernel-Funktion startet den Kernel
 * auf ihrem Kern beim ersten Aufruf automatisch, mit Prioritaet 1. Wer eine
 * andere Prioritaet fuer die Haupttask will, ruft irk_init() -- auch
 * nachtraeglich.
 *
 * MEHRKERNBETRIEB (IRK_MAX_CORES > 1)
 *   Jeder Kern hat seinen eigenen Scheduler. Eine Task laeuft auf dem
 *   Kern, der sie anlegt, und wechselt ihn nie. Handles, Semaphore und
 *   Queues sind gemeinsam: Tasks verschiedener Kerne koennen einander
 *   anhalten, fortsetzen, loeschen und ueber Semaphore und Queues
 *   aufeinander warten. Betrifft ein Aufruf eine Task, die gerade auf dem
 *   anderen Kern rechnet, wirkt er bei deren naechstem irk_yield().
 *
 * INTERRUPTS
 *   Interrupts laufen jederzeit. Aus einem Interrupt-Handler heraus darf
 *   aber keine IRKernel-Funktion gerufen werden (Ausnahme: irk_now_us());
 *   solche Aufrufe werden mit IRK_ERR_IN_ISR abgelehnt.
 *
 * Rueckgabe: 0 = ok, sonst Fehlercode. Mehrfachaufruf ist harmlos.  */
int irk_init(irk_prio_t prio);

/* Faehrt den Kernel herunter: loescht alle Tasks ausser der Haupttask.
 * Darf nur aus der Haupttask heraus gerufen werden. */
int irk_deinit(void);

/* 1, wenn der Kernel auf dem aufrufenden Kern laeuft. Startet ihn nicht. */
int irk_is_running(void);

/* Nummer des aufrufenden Kerns (0 im Einkernbetrieb). */
uint8_t irk_core_id(void);

/* Kern, auf dem eine Task laeuft, oder 0xFF fuer ein ungueltiges Handle. */
uint8_t irk_task_core(irk_task_t tsk);


/*========================================================================*\
 *  Tasks anlegen und steuern
\*========================================================================*/

/* Legt eine Task an und macht sie sofort lauffaehig.
 *
 *   func    Taskfunktion. Laeuft ueblicherweise als Endlosschleife; kehrt
 *           sie zurueck, wird die Task automatisch geloescht.
 *   prio    1 .. IRK_MAX_PRIO. Bestimmt den Anteil an der Rechenzeit.
 *           0 bedeutet: angelegt, aber nicht eingeplant.
 *   stack   Speicher fuer den Taskstack, oder NULL fuer statische
 *           Zuteilung aus dem kernel-eigenen Vorrat (nur wenn die
 *           Plattform das unterstuetzt -- auf AVR immer selbst mitgeben).
 *   size    Groesse von stack in Byte.
 *   name    Klartextname oder NULL. Wird nur bei IRK_ENABLE_NAMES
 *           gespeichert, sonst ignoriert.
 *
 * Rueckgabe: Task-Handle, oder IRK_NO_TASK bei Fehler.
 */
irk_task_t irk_task_create(void (*func)(void), irk_prio_t prio,
                           void *stack, size_t size, const char *name);

/* Gibt die CPU ab.  DAS ist die zentrale Funktion des Kernels.
 *
 * Bedeutet: "Ich bin fuer jetzt fertig" -- im Original hiess sie Done().
 * Prueft, ob eine andere Task dringlicher Rechenzeit braucht, und
 * schaltet gegebenenfalls um. Ist keine andere Task dran, kehrt der
 * Aufruf sofort zurueck. Weckt nebenbei Tasks, deren Frist abgelaufen
 * ist, und aktiviert faellige zyklische Tasks.
 *
 * Eine Task, die nur kurz etwas prueft und dann abgibt, kommt so sehr
 * oft dran: wie oft, ergibt sich aus ihrem Anteil geteilt durch die Dauer
 * eines Durchlaufs. (Mit IRK_MIN_TIMESLICE_US > 0 laesst sich eine
 * Mindestlaufzeit vor dem Abgeben erzwingen, siehe irk_config.h.)
 *
 * NICHT zum Warten verwenden:  while (!fertig) irk_yield();
 * Eine so kreisende Task gilt als lauffaehig und sammelt Anspruch an.
 * Rechnet eine gleich priorisierte Task 2 s am Stueck, kreist die
 * wartende danach ebenfalls 2 s -- genau ihr fairer Anteil. Wer wartet,
 * nimmt irk_delay(), ein Semaphor oder eine Queue: beim Aufwachen wird das
 * Konto auf den aktuellen Stand gehoben, ohne Nachforderung.
 */
void irk_yield(void);

/* Haelt eine Task an (Grund: IRK_WAIT_RESUME). Haelt sie sich selbst an,
 * kehrt der Aufruf erst zurueck, wenn sie fortgesetzt wurde. */
int irk_task_suspend(irk_task_t tsk);

/* Setzt eine mit irk_task_suspend() angehaltene Task fort. */
int irk_task_resume(irk_task_t tsk);

/* Loescht eine Task sofort und gibt ihren Slot frei. Loescht eine Task
 * sich selbst, kehrt der Aufruf nicht zurueck. Die Haupttask laesst sich
 * nicht loeschen. */
int irk_task_kill(irk_task_t tsk);

/* Loescht die Task zum angegebenen Zeitpunkt (absolut, ms seit
 * Kernelstart) bzw. nach Ablauf der angegebenen Frist.
 * Bei 32-Bit-Zeit (AVR) liegt die groesste moegliche Frist bei rund
 * 71 Minuten; laengere Angaben werden darauf begrenzt. */
int irk_task_kill_at(irk_task_t tsk, uint32_t when_ms);
int irk_task_kill_after(irk_task_t tsk, uint32_t delay_ms);
int irk_task_kill_after_us(irk_task_t tsk, irk_time_t delay_us);

/* Setzt die laufende Task fuer delay_ms Millisekunden aus und gibt
 * solange die CPU ab. Der kooperative Ersatz fuer delay().
 * Beliebig lange Zeiten sind moeglich, auch bei 32-Bit-Zeit. */
void irk_delay(uint32_t delay_ms);

/* Wie irk_delay(), aber in Mikrosekunden.
 * Wie genau die Frist getroffen wird, haengt davon ab, wie oft die
 * uebrigen Tasks irk_yield() aufrufen -- kooperativ heisst, niemand wird
 * mitten in seiner Arbeit unterbrochen. */
void irk_delay_us(irk_time_t delay_us);

/* Wie irk_delay() bzw. irk_delay_us(), aber fuer eine beliebige Task.
 * Bei 32-Bit-Zeit auf rund 71 Minuten begrenzt. */
int irk_task_delay(irk_task_t tsk, uint32_t delay_ms);
int irk_task_delay_us(irk_task_t tsk, irk_time_t delay_us);

/* Aktuelle Kernelzeit in Mikrosekunden (laeuft bei 32-Bit-Zeit ueber). */
irk_time_t irk_now_us(void);

/* Aendert die Prioritaet. Wirkt ab sofort auf den kuenftigen Anteil an
 * der Rechenzeit; der bisherige Stand in der Fair-Share-Ordnung bleibt
 * erhalten. */
int irk_task_set_prio(irk_task_t tsk, irk_prio_t prio);

irk_prio_t irk_task_get_prio(irk_task_t tsk);
uint16_t   irk_task_status(irk_task_t tsk);
irk_task_t irk_task_self(void);
uint8_t    irk_task_count(void);

/* Gesamte bisher verbrauchte Rechenzeit der Task, in Millisekunden bzw.
 * Mikrosekunden. Reine Statistik -- die Fair-Share-Ordnung benutzt ein
 * eigenes, ueberlauffestes Konto. Bei 32-Bit-Zeit laeuft der
 * Mikrosekundenwert nach 71 Minuten Rechenzeit der Task ueber. */
uint32_t   irk_task_runtime(irk_task_t tsk);
irk_time_t irk_task_runtime_us(irk_task_t tsk);

/* Wie oft die Task bisher an die Reihe kam (Laeufe). Zusammen mit der
 * Rechenzeit ergibt das die mittlere Dauer eines Laufs -- nuetzlich zum
 * Profilieren. 0, wenn IRK_ENABLE_STATS aus ist (Standard auf AVR). */
uint32_t   irk_task_calls(irk_task_t tsk);

/* Fair-Share-Konto der Task. Nur zum Profilieren und Nachvollziehen des
 * Schedulers: der Wert ist nur im Vergleich mit Tasks DESSELBEN Kerns
 * sinnvoll, und er wird beim Normalisieren abgesenkt. Dran ist, wer das
 * kleinste Konto hat. 0 bei ungueltigem Handle. */
irk_time_t irk_task_vruntime(irk_task_t tsk);

#if IRK_ENABLE_NAMES
const char *irk_task_name(irk_task_t tsk);
irk_task_t  irk_task_by_name(const char *name);
#endif


/*========================================================================*\
 *  Zyklische Tasks
 *
 *  Eine zyklische Task wird je Periode genau einmal aktiviert, und zwar
 *  mit Vorrang vor allen nicht-zyklischen Tasks. Zwischen den
 *  Aktivierungen belegt sie keine Rechenzeit.
 *
 *  Die Faelligkeit rueckt je Aktivierung um genau eine Periode vor -- der
 *  Takt driftet also nicht. Kommt die Task einmal mehrere Perioden zu
 *  spaet dran, werden die verpassten Takte UEBERSPRUNGEN statt in einem
 *  Schwall nachgeholt; die Phasenlage bleibt dabei erhalten.
 *
 *  Typische Anwendung: ein Regelkreis, der einen festen Takt braucht,
 *  waehrend im Hintergrund Bedienung und Kommunikation laufen.
\*========================================================================*/

#if IRK_ENABLE_CYCLIC
int irk_task_set_cyclic(irk_task_t tsk, uint32_t period_ms);
int irk_task_set_cyclic_us(irk_task_t tsk, irk_time_t period_us);

/* Wie oben, aber mit Startversatz: die erste Aktivierung erfolgt erst
 * start_after_ms bzw. start_after_us nach dem Aufruf, alle weiteren dann im
 * festen Raster  Aufruf + Versatz + k * Periode.
 *
 * Damit lassen sich Schaltvorgaenge phasenstarr koppeln -- wie bei einem
 * Nockenschaltwerk. Beispiel: eine LED 0,7 s an, 3 s aus:
 *
 *     irk_task_set_cyclic_at(t_rot_an,  3700,   0);   // schaltet ein
 *     irk_task_set_cyclic_at(t_rot_aus, 3700, 700);   // schaltet aus
 *
 * Beide Tasks haengen am selben Takt und koennen nicht gegeneinander
 * driften. start_after = 0 entspricht irk_task_set_cyclic().
 * Bei 32-Bit-Zeit ist der Versatz auf rund 35 Minuten begrenzt. */
int irk_task_set_cyclic_at(irk_task_t tsk, uint32_t period_ms, uint32_t start_after_ms);
int irk_task_set_cyclic_at_us(irk_task_t tsk, irk_time_t period_us,
                              irk_time_t start_after_us);
int irk_task_set_normal(irk_task_t tsk, irk_prio_t prio);
#endif


/*========================================================================*\
 *  Zaehlsemaphore
 *
 *  Anders als im Original brauchen die Warteschlangen keinerlei
 *  dynamischen Speicher -- sie sind intrusiv durch die Taskdescriptoren
 *  gefaedelt. Die Reihenfolge ist FIFO.
\*========================================================================*/

/* Initialisiert ein Semaphor mit dem Zaehlerstand count.
 * count = 1 ergibt ein binaeres Semaphor (Mutex). */
int irk_sema_init(irk_sema_t sema, int16_t count);

/* Belegt eine Einheit. Ist keine frei, wird die Task angehalten und
 * erst fortgesetzt, wenn eine Einheit freigegeben wurde. */
int irk_sema_wait(irk_sema_t sema);

/* Gibt eine Einheit frei und weckt die laengstwartende Task. */
int irk_sema_signal(irk_sema_t sema);

/* Belegt eine Einheit nur, wenn sofort eine frei ist.
 * Rueckgabe: 1 = belegt, 0 = war nicht frei. Blockiert nie. */
int irk_sema_try_wait(irk_sema_t sema);

/* Aktueller Zaehlerstand. Negativ bedeutet: so viele Tasks warten. */
int16_t irk_sema_count(irk_sema_t sema);


/*========================================================================*\
 *  Queues  --  Inter-Task-Kommunikation
 *
 *  Eine Queue befoerdert Elemente fester Groesse zwischen Tasks. Sender
 *  und Empfaenger muessen nicht gleichzeitig bereit sein; die Queue
 *  entkoppelt sie bis zu ihrer Kapazitaet.
 *
 *  Den Speicher stellt der Aufrufer -- die Library allokiert nichts:
 *
 *      static uint16_t  puffer[8];
 *      static irk_queue_t q;
 *
 *      irk_queue_init(&q, puffer, sizeof(uint16_t), 8);
 *
 *      uint16_t wert = 42;
 *      irk_queue_send(&q, &wert);      // blockiert, wenn voll
 *
 *      uint16_t empfangen;
 *      irk_queue_recv(&q, &empfangen); // blockiert, wenn leer
 *
 *  Das tritt an die Stelle der Nachrichten des urspruenglichen Entwurfs,
 *  die je Nachricht Speicher anforderten -- auf einem Mikrocontroller
 *  nicht tragbar.
\*========================================================================*/

typedef struct {
    uint8_t *buf;          /* vom Aufrufer gestellter Speicher          */
    uint8_t  item_size;    /* Groesse eines Elements in Byte            */
    uint8_t  capacity;     /* Zahl der Plaetze                          */
    uint8_t  count;        /* aktuell belegte Plaetze                   */
    uint8_t  head;         /* naechstes zu lesendes Element             */
    uint8_t  tail;         /* naechster freier Platz                    */
    uint8_t  q_send;       /* Warteschlange der Sender   (0 = leer)     */
    uint8_t  q_send_tail;
    uint8_t  q_recv;       /* Warteschlange der Empfaenger (0 = leer)   */
    uint8_t  q_recv_tail;
    uint8_t  valid;
} irk_queue_t;

/* Richtet eine Queue auf dem uebergebenen Speicher ein.
 * storage muss mindestens item_size * capacity Byte gross sein. */
int irk_queue_init(irk_queue_t *q, void *storage,
                   uint8_t item_size, uint8_t capacity);

/* Stellt ein Element ein. Ist die Queue voll, wartet die Task, bis
 * wieder Platz ist. */
int irk_queue_send(irk_queue_t *q, const void *item);

/* Entnimmt ein Element. Ist die Queue leer, wartet die Task, bis eines
 * eintrifft. */
int irk_queue_recv(irk_queue_t *q, void *item);

/* Wie oben, aber ohne je zu blockieren.
 * Rueckgabe: 1 = ausgefuehrt, 0 = ging gerade nicht. */
int irk_queue_try_send(irk_queue_t *q, const void *item);
int irk_queue_try_recv(irk_queue_t *q, void *item);

/* Zahl der aktuell eingestellten Elemente. */
uint8_t irk_queue_count(const irk_queue_t *q);

/* Verwirft alle Elemente. Wartende Sender werden dabei geweckt. */
int irk_queue_flush(irk_queue_t *q);


/*========================================================================*\
 *  Ueberwachungsfunktionen
 *
 *  Der Scheduler prueft bei jedem Durchlauf die Bedingung cond() und
 *  ruft bei Erfuellung action(). Im Original Init_debug_function().
\*========================================================================*/

#if IRK_ENABLE_WATCH

#define IRK_WATCH_ALWAYS    0x00  /* bei jeder Erfuellung             */
#define IRK_WATCH_ONCE      0x01  /* nur beim ersten Mal              */
#define IRK_WATCH_LEVEL     0x00  /* pegelgetriggert                  */
#define IRK_WATCH_EDGE      0x02  /* flankengetriggert                */
#define IRK_WATCH_POSITIVE  0x00  /* auf steigend / wahr              */
#define IRK_WATCH_NEGATIVE  0x04  /* auf fallend / falsch             */

int irk_watch_add(int (*cond)(void), void (*action)(void), uint8_t mode);
int irk_watch_remove(int (*cond)(void), void (*action)(void));

#endif


/*========================================================================*\
 *  Stackueberwachung
\*========================================================================*/

#if IRK_ENABLE_STACKCHECK
/* Kleinster jemals beobachteter freier Stackrest der Task, in Byte.
 * Ermittelt ueber das beim Anlegen eingefaerbte Muster. */
size_t irk_stack_free(irk_task_t tsk);

/* Rueckruf der Stack-Fruehwarnung: Task und ihr nie benutzter Rest. */
typedef void (*irk_stack_alarm_fn)(irk_task_t tsk, size_t free_bytes);

/* Stack-Fruehwarnung.
 *
 * Meldet, sobald bei einer Task weniger als min_free Byte Stack nie
 * benutzt wurden -- also BEVOR der Stack ueberlaeuft, nicht erst danach.
 *
 *   min_free  Warnschwelle in Byte, z.B. 128 oder 256
 *   callback  wird je Task hoechstens EINMAL gerufen; NULL schaltet die
 *             Fruehwarnung ab
 *
 * Ein erneuter Aufruf schaerft die Warnung fuer alle Tasks wieder.
 *
 * Arbeitsweise: der Scheduler prueft alle IRK_STACK_WATCH_INTERVAL_MS
 * reihum eine Task, und zwar nur die untersten min_free Byte ihres
 * Stacks. Das kostet praktisch keine Rechenzeit.
 *
 * WICHTIG zum Rueckruf: er laeuft innerhalb des Schedulers, auf dem Stack
 * der gerade laufenden Task -- das kann genau die knappe sein. Also kurz
 * halten: Flag oder Zaehler setzen, die Ausgabe spaeter aus einer Task
 * erledigen. Kein Serial.print(), kein irk_yield(), kein irk_delay().
 *
 * Nicht erfasst wird die Haupttask (setup/loop): sie benutzt den Stack
 * des Systems, den der Kernel nicht eingefaerbt hat.
 *
 * Rueckgabe: 0
 */
int irk_stack_watch(size_t min_free, irk_stack_alarm_fn callback);
#endif


/*========================================================================*\
 *  Scheduler voruebergehend sperren
 *
 *  Zwischen irk_lock() und irk_unlock() schaltet irk_yield() nicht um.
 *  Schachtelbar. Fuer Faelle, in denen eine Folge von Operationen nicht
 *  unterbrochen werden darf, obwohl sie irk_yield() enthaelt.
\*========================================================================*/

void irk_lock(void);
void irk_unlock(void);


/*========================================================================*\
 *  Lizenz  --  abstrakter Haken
 *
 *  Meldet, ob diese Uebersetzung von IRKernel unter einer gueltigen
 *  Lizenz benutzt wird.
 *
 *  Der Kernel selbst fragt das NIE ab und richtet sein Verhalten nicht
 *  danach. Ein Scheduler, der die Arbeit verweigert, haelt im Zweifel
 *  eine Maschine an -- das waere die falsche Antwort auf eine
 *  Vertragsfrage. Die Auskunft ist zum Melden da: im Baulauf, im
 *  Firmware-Manifest, in einer Diagnoseausgabe. Durchgesetzt wird eine
 *  Lizenz beim Beziehen des Kernels, nicht zur Laufzeit.
 *
 *  Die Vorgabe ist schwach gebunden und liefert 1. Eine kommerzielle
 *  Auslieferung legt eine eigene Uebersetzungseinheit daneben, die sie
 *  verdraengt; wer IRKernel frei benutzt, merkt von alledem nichts.
 *
 *  Rueckgabe: 1 gueltig, 0 nicht. Ein int wie bei irk_is_running() --
 *  der Header kommt ohne <stdbool.h> aus.
\*========================================================================*/

int irk_license_valid(void);


#ifdef __cplusplus
}
#endif

#endif /* IRKERNEL_H */
