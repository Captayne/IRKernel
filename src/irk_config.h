/*========================================================================*\
 *
 *  irk_config.h  --  Kompilierzeit-Dimensionierung des IRKernel
 *
 *  Jede Dimension laesst sich vor dem Einbinden der Library ueberschreiben,
 *  z.B. per Build-Flag  -DIRK_MAX_TASKS=4  oder durch ein eigenes
 *  irk_config_local.h (siehe unten).
 *
 *  Der Entwurf von 1993 war fuer einen PC dimensioniert (64 Tasks,
 *  1023 Semaphore) und braeuchte in dieser Form ~13 KB statisches RAM.
 *  Ein ATmega328 hat 2 KB. Alle Dimensionen sind daher konfigurierbar.
 *
 *  Herkunft: der Kernel einer PC-Robotersteuerung, Institut fuer Roboterforschung,
 *            Universitaet Dortmund, Andreas Keibel, Okt. 1993
 *
\*========================================================================*/

#ifndef IRK_CONFIG_H
#define IRK_CONFIG_H

/* Erlaubt es, alle Defaults in einem projektlokalen Header zu ueberschreiben,
   ohne diese Datei zu aendern. Anlegen und -DIRK_HAVE_LOCAL_CONFIG setzen. */
#ifdef IRK_HAVE_LOCAL_CONFIG
#include "irk_config_local.h"
#endif


/*------------------------------------------------------------------------*\
 *  Groesse der Taskverwaltung
\*------------------------------------------------------------------------*/

/* Maximale Zahl gleichzeitig existierender Tasks, die Haupttask
   eingeschlossen. Kosten: ~40 Byte (8 Bit) bzw. ~64 Byte (32 Bit) je Task.
   Gueltig 2..254 -- Tasknummern werden in uint8_t gefuehrt, 0 = "keine". */
#ifndef IRK_MAX_TASKS
#  if defined(__AVR__)
#    define IRK_MAX_TASKS        6
#  else
#    define IRK_MAX_TASKS       24
#  endif
#endif

/* Zahl der Prozessorkerne, auf denen IRKernel laufen kann.
 *
 * Jeder Kern bekommt seinen eigenen Scheduler; eine Task laeuft immer auf
 * dem Kern, der sie angelegt hat. Tasktabelle, Semaphore und Queues sind
 * gemeinsam -- eine Task auf Kern 1 kann auf ein Semaphor warten, das eine
 * Task auf Kern 0 freigibt. Der Kernel startet auf jedem Kern automatisch
 * beim ersten Aufruf einer seiner Funktionen.
 *
 * Bei 1 entfallen Sperre und Kernabfrage vollstaendig -- kein Aufwand.  */
#ifndef IRK_MAX_CORES
#  if defined(ARDUINO_ARCH_RP2040)
#    define IRK_MAX_CORES        2
#  else
#    define IRK_MAX_CORES        1
#  endif
#endif

/* Default-Stackgroesse fuer irk_task_create(), wenn kein Stack uebergeben
   wird. Auf AVR sehr knapp bemessen -- im Zweifel per Task angeben. */
#ifndef IRK_DEFAULT_STACK
#  if defined(__AVR__)
#    define IRK_DEFAULT_STACK  192
#  else
#    define IRK_DEFAULT_STACK  512
#  endif
#endif

/* Darf irk_task_create() den Taskstack selbst per malloc() besorgen,
   wenn kein Stack uebergeben wird?
   Auf AVR bewusst aus: dort soll jeder Stack ein statisches Array sein,
   damit der Linker den Verbrauch kennt und nicht erst zur Laufzeit
   auffaellt, dass das RAM nicht reicht. */
#ifndef IRK_ENABLE_MALLOC_STACKS
#  if defined(__AVR__)
#    define IRK_ENABLE_MALLOC_STACKS  0
#  else
#    define IRK_ENABLE_MALLOC_STACKS  1
#  endif
#endif


/*------------------------------------------------------------------------*\
 *  Semaphore
\*------------------------------------------------------------------------*/

/* Anzahl Zaehlsemaphore. Kosten je Semaphor: 4 Byte (8 Bit) / 8 Byte (32 Bit).
   Die Warteschlangen liegen intrusiv in den Taskdescriptoren und kosten
   nichts extra -- anders als im Original, das je Wartevorgang einen
   Ringlistenknoten per malloc() angelegt hat. */
#ifndef IRK_MAX_SEMAPHORES
#  if defined(__AVR__)
#    define IRK_MAX_SEMAPHORES   8
#  else
#    define IRK_MAX_SEMAPHORES  32
#  endif
#endif


/*------------------------------------------------------------------------*\
 *  Scheduling-Verhalten
\*------------------------------------------------------------------------*/

/*------------------------------------------------------------------------*\
 *  Zeitbasis
 *
 *  Der Kernel rechnet intern durchgehend in Mikrosekunden. Der Datentyp
 *  dafuer ist irk_time_t:
 *
 *    64 Bit  (Default auf ARM und PC)
 *        Ueberlauf nach rund 584.000 Jahren -- praktisch nie.
 *
 *    32 Bit  (Default auf AVR)
 *        Ueberlauf alle 71,6 Minuten. Das ist bewusst so: 64-Bit-Rechnung
 *        kostet auf AVR spuerbar Flash und RAM. Der Kernel ist ueberlauffest
 *        gebaut -- er vergleicht nie Zeitpunkte, sondern nur vorzeichenlose
 *        Differenzen "jetzt - Start". Die einzige Grenze: eine EINZELNE
 *        Frist darf hoechstens gut 71 Minuten lang sein. irk_delay() mit
 *        laengeren Zeiten zerlegt der Kernel selbst in Teilstuecke.
\*------------------------------------------------------------------------*/

#include <stdint.h>

#ifndef IRK_TIME_64
#  if defined(__AVR__)
#    define IRK_TIME_64   0
#  else
#    define IRK_TIME_64   1
#  endif
#endif

#if IRK_TIME_64
typedef uint64_t irk_time_t;
#  define IRK_TIME_MAX  0xFFFFFFFFFFFFFFFFULL
#else
typedef uint32_t irk_time_t;
#  define IRK_TIME_MAX  0xFFFFFFFFUL
#endif

/* Minimale Zeitscheibe in Mikrosekunden -- standardmaessig AUS (0).

   irk_yield() heisst "ich bin fuer jetzt fertig" (im Original Done()) und
   gibt deshalb sofort ab. Niemand ruft es mitten in einer Rechnung auf,
   deren Ergebnis noch gebraucht wird. So kann eine Task, die nur kurz
   einen Fuellstand liest, sehr oft drankommen, ohne mehr als ihren Anteil
   zu verbrauchen.

   Den Verwaltungsaufwand bezahlt dabei, wer ihn verursacht: jeder Lauf
   bekommt die Scheduler-Runde davor angerechnet. Wer staendig abgibt,
   verbraucht also nur das eigene Konto.

   Wer alten Code hat, der irk_yield() in einer Rechenschleife ruft (etwa
   nach jeder interpretierten Anweisung), setzt hier einen Wert > 0: dann
   kehrt irk_yield() sofort zurueck, bis die Task so lange lief.
   Blockierende Aufrufe (irk_delay, Semaphore, Queues) geben immer ab.
   Fruehere Projekte, die IRK_MIN_TIMESLICE_MS setzen, gehen weiter.   */
#ifndef IRK_MIN_TIMESLICE_US
#  ifdef IRK_MIN_TIMESLICE_MS
#    define IRK_MIN_TIMESLICE_US   ((IRK_MIN_TIMESLICE_MS) * 1000UL)
#  else
#    define IRK_MIN_TIMESLICE_US   0UL
#  endif
#endif
#ifndef IRK_MIN_TIMESLICE_MS
#  define IRK_MIN_TIMESLICE_MS     ((IRK_MIN_TIMESLICE_US) / 1000UL)
#endif

/* Nach so vielen aufeinanderfolgenden Scheduler-Durchlaeufen ohne eine
   einzige lauffaehige Task gilt die Lage als Deadlock: der Kernel weckt
   die Haupttask und meldet IRK_ERR_DEADLOCK. */
#ifndef IRK_DEADLOCK_CYCLES
#  define IRK_DEADLOCK_CYCLES   50
#endif

/* Hoechste zulaessige Prioritaet. Prioritaet 0 bedeutet "nicht einplanen",
   1 ist die niedrigste normale Prioritaet. Hoechstens 65535 (irk_prio_t
   ist 16 Bit breit); groessere Werte werden darauf begrenzt. */
#ifndef IRK_MAX_PRIO
#  define IRK_MAX_PRIO         65535
#endif


/*------------------------------------------------------------------------*\
 *  Optionale Module  --  kosten Flash und RAM, per Default aus auf AVR
\*------------------------------------------------------------------------*/

/* Zyklische Tasks: periodische Reaktivierung mit Vorrang vor allen
   anderen. Das Kernstueck fuer Regelkreise -- im Zweifel anlassen. */
#ifndef IRK_ENABLE_CYCLIC
#  define IRK_ENABLE_CYCLIC      1
#endif

/* Namensbasierte Tasksuche (irk_task_by_name, irk_task_name).
   Kostet einen Zeiger je Task plus die Stringliterale. */
#ifndef IRK_ENABLE_NAMES
#  if defined(__AVR__)
#    define IRK_ENABLE_NAMES     0
#  else
#    define IRK_ENABLE_NAMES     1
#  endif
#endif

/* Ueberwachungsfunktionen (Bedingung + Reaktion, vom Scheduler geprueft).
   Im Original Init_debug_function() -- ein selbstgebauter Watchpoint. */
#ifndef IRK_ENABLE_WATCH
#  if defined(__AVR__)
#    define IRK_ENABLE_WATCH     0
#  else
#    define IRK_ENABLE_WATCH     1
#  endif
#endif

/* Maximale Zahl gleichzeitiger Ueberwachungen (nur bei IRK_ENABLE_WATCH). */
#ifndef IRK_MAX_WATCH
#  define IRK_MAX_WATCH          4
#endif

/* Laufzeitstatistik je Task (Aufrufzaehler). Die Gesamtlaufzeit TaskTime
   wird immer gefuehrt, sie ist Teil der Scheduling-Politik. */
#ifndef IRK_ENABLE_STATS
#  if defined(__AVR__)
#    define IRK_ENABLE_STATS     0
#  else
#    define IRK_ENABLE_STATS     1
#  endif
#endif

/* Stack-Ueberwachung: faerbt neue Stacks mit IRK_STACK_PATTERN ein und
   erlaubt irk_stack_free() sowie eine Warnung bei Ueberlauf.
   Im Original war das Feld "stackoverflow" vorgesehen, wurde aber nie
   gesetzt -- siehe README, "Behobene Fehler". */
#ifndef IRK_ENABLE_STACKCHECK
#  define IRK_ENABLE_STACKCHECK  1
#endif

#ifndef IRK_STACK_PATTERN
#  define IRK_STACK_PATTERN   0xA5
#endif

/* Abstand, in dem die Stack-Fruehwarnung (irk_stack_watch) die naechste
   Task prueft. Reihum je eine Task pro Intervall -- bei n Tasks wird jede
   also etwa alle n * IRK_STACK_WATCH_INTERVAL_MS Millisekunden geprueft. */
#ifndef IRK_STACK_WATCH_INTERVAL_MS
#  define IRK_STACK_WATCH_INTERVAL_MS  10
#endif


/*------------------------------------------------------------------------*\
 *  Mehrere loop()-Funktionen (IRKernel_loops.h)
\*------------------------------------------------------------------------*/

/* Wie viele zusaetzliche Schleifen (loop1, loop2, ...) hoechstens moeglich
   sind. Jede kostet einen Taskplatz und einen Stack, auch wenn der Sketch
   sie nicht benutzt -- der Stack wird fest reserviert.
   Hoechstens 16: so weit reichen die Deklarationen in IRKernel_loops.h.
   Die Haupttask zaehlt bei IRK_MAX_TASKS mit -- auf AVR bleiben davon
   also hoechstens 5 Schleifen uebrig. */
#ifndef IRK_LOOP_MAX
#  if defined(__AVR__)
#    define IRK_LOOP_MAX         4
#  else
#    define IRK_LOOP_MAX         8
#  endif
#endif

/* Stack je Schleife in Byte. Reicht er nicht, stuerzt die Schleife ab --
   mit IRK_ENABLE_STACKCHECK zeigt irk_stack_free() die Reserve. */
#ifndef IRK_LOOP_STACK
#  if defined(__AVR__)
#    define IRK_LOOP_STACK       256
#  else
#    define IRK_LOOP_STACK       1024
#  endif
#endif


/*------------------------------------------------------------------------*\
 *  Diagnose
\*------------------------------------------------------------------------*/

/* Der Kernel gibt selbst nichts aus. Fehler (IRK_ERR_...) meldet er an
   genau einer Stelle: irk_port_error(code, detail).

   Auf Arduino ist diese Meldung standardmaessig stumm. Zwei Wege, sie zu
   sehen:
     - hier IRK_DEBUG_SERIAL definieren: der Port schreibt dann
       "[IRKernel] Fehler <code> (<detail>)" auf Serial
     - im Sketch eine eigene  void irk_port_error(int code, int detail)
       schreiben (die des Ports ist "weak") -- sie kann auch aus einem
       Interrupt heraus gerufen werden, darf also nur Zaehler/Flags setzen

   ACHTUNG: ein #define IRK_DEBUG_SERIAL im Sketch reicht NICHT -- die
   Arduino-IDE uebersetzt die Library getrennt vom Sketch. */
/* #define IRK_DEBUG_SERIAL */


/*------------------------------------------------------------------------*\
 *  Plausibilitaetspruefung der Konfiguration
\*------------------------------------------------------------------------*/

#if IRK_MAX_TASKS < 2
#  error "IRK_MAX_TASKS muss mindestens 2 sein (Haupttask + eine Task)"
#endif
#if IRK_MAX_TASKS > 254
#  error "IRK_MAX_TASKS darf hoechstens 254 sein (Tasknummern sind uint8_t)"
#endif
#if IRK_MAX_SEMAPHORES > 255
#  error "IRK_MAX_SEMAPHORES darf hoechstens 255 sein"
#endif

#endif /* IRK_CONFIG_H */
