/*========================================================================*\
 *
 *  irk_port.h  --  Portierungsschicht des IRKernel
 *
 *  Alles, was der Kernel von der Hardware braucht, steht hier. Wer eine
 *  neue Architektur anbindet, implementiert genau diese Funktionen und
 *  muss den Scheduler nicht anfassen.
 *
 *  Vorhandene Backends in src/port/ :
 *      irk_port_avr.c        AVR (ATmega328/1284/2560, ...)
 *      irk_port_cortexm.c    ARM Cortex-M0/M0+/M3/M4/M33
 *      irk_port_host.c       PC (Windows-Fibers / POSIX-ucontext), fuer Tests
 *
\*========================================================================*/

#ifndef IRK_PORT_H
#define IRK_PORT_H

#include <stddef.h>
#include <stdint.h>

#include "irk_config.h"

#ifdef __cplusplus
extern "C" {
#endif


/*------------------------------------------------------------------------*\
 *  0. Initialisierung
 *
 *  Wird als Erstes aus irk_init() gerufen. Hier gehoert alles hin, was
 *  die Plattform braucht, bevor der erste Kontextwechsel stattfinden
 *  kann. Darf leer sein.
\*------------------------------------------------------------------------*/

void irk_port_init(void);


/*------------------------------------------------------------------------*\
 *  1. Kontextwechsel
 *
 *  Der gesamte Kontext einer Task ist ihr Stackpointer -- mehr braucht der
 *  Taskdescriptor nicht. Der Wechsel wird immer aus einer normalen
 *  C-Funktion heraus aufgerufen (nie aus einem Interrupt), deshalb hat der
 *  Compiler die caller-saved Register an der Aufrufstelle bereits selbst
 *  gerettet: zu sichern sind nur die callee-saved Register.
 *
 *  Der Entwurf von 1993 schrieb den Stackpointer stattdessen direkt in
 *  eine jmp_buf und brauchte dafuer je Compiler eine andere Magic-Zahl.
\*------------------------------------------------------------------------*/

/* Erzeugt einen lauffaehigen Kontext auf dem uebergebenen Stackbereich.
 *
 *   stack  Basisadresse des Speicherbereichs (niedrigste Adresse)
 *   size   Groesse in Byte
 *   entry  Einsprungfunktion. Sie darf niemals zurueckkehren --
 *          der Kernel ruft sie ueber ein Trampolin auf, das das absichert.
 *
 * Rueckgabe: der initiale Stackpointer, wie ihn irk_ctx_switch() erwartet,
 *            oder NULL, wenn der Bereich zu klein ist.
 */
void *irk_ctx_create(void *stack, size_t size, void (*entry)(void));

/* Sichert den laufenden Kontext nach *old_sp und setzt new_sp fort.
 * Kehrt erst zurueck, wenn spaeter wieder auf den gesicherten Kontext
 * umgeschaltet wird. */
void irk_ctx_switch(void **old_sp, void *new_sp);

/* Minimal noetiger Platz fuer einen Kontext, in Byte. Der Kernel prueft
 * damit, ob eine angeforderte Stackgroesse ueberhaupt tragfaehig ist. */
size_t irk_ctx_min_stack(void);


/*------------------------------------------------------------------------*\
 *  2. Zeit
\*------------------------------------------------------------------------*/

/* Mikrosekunden seit Systemstart, monoton, als irk_time_t.
 *
 * Bei 64-Bit-Zeit (IRK_TIME_64) muss der Wert wirklich 64 Bit breit sein.
 * Liefert die Plattform nur 32 Bit (Arduino micros()), erweitert das
 * Backend den Zaehler selbst, indem es Ueberlaeufe mitzaehlt -- dafuer
 * muss irk_port_micros() nur oefter als alle 71 Minuten gerufen werden,
 * was der Scheduler von selbst erledigt.
 *
 * Bei 32-Bit-Zeit darf der Wert ganz normal ueberlaufen: der Kernel
 * vergleicht nur Differenzen und ist darauf ausgelegt. */
irk_time_t irk_port_micros(void);


/*------------------------------------------------------------------------*\
 *  3. Kritische Abschnitte
 *
 *  Nur noetig, wo der Kernel gegen Interrupts geschuetzt werden muss --
 *  etwa beim Schreiben des Stackpointers auf AVR, wo das nicht atomar ist.
 *  Muss schachtelbar sein.
\*------------------------------------------------------------------------*/

void irk_port_crit_enter(void);
void irk_port_crit_exit(void);


/*------------------------------------------------------------------------*\
 *  3a. Interrupts und Mehrkernbetrieb
\*------------------------------------------------------------------------*/

/* 1, wenn gerade ein Interrupt-Handler laeuft, sonst 0.
 * IRKernel-Funktionen lehnen Aufrufe aus Interrupts ab. Auf Cortex-M ist
 * das eine Abfrage des IPSR-Registers. Wo die Plattform es nicht erkennen
 * kann (AVR), liefert die Funktion 0. */
int irk_port_in_isr(void);

/* Nur bei IRK_MAX_CORES > 1 gebraucht:
 *
 * irk_port_core_id()   Nummer des Kerns, auf dem der Aufrufer laeuft
 *                      (0 .. IRK_MAX_CORES-1). Muss aus jedem Kontext
 *                      korrekt sein, auch waehrend eines Taskwechsels.
 *
 * irk_port_xlock()     Kernuebergreifende Sperre fuer die gemeinsamen
 * irk_port_xunlock()   Kerneldaten. Muss je Kern schachtelbar sein und
 *                      waehrend sie gehalten wird die Interrupts des
 *                      eigenen Kerns sperren. Wird nie ueber einen
 *                      Kontextwechsel gehalten und nur sehr kurz.        */
#if IRK_MAX_CORES > 1
uint8_t irk_port_core_id(void);
void    irk_port_xlock(void);
void    irk_port_xunlock(void);
#endif


/*------------------------------------------------------------------------*\
 *  4. Diagnose
\*------------------------------------------------------------------------*/

/* Wird bei einem Kernelfehler aufgerufen (Codes siehe IRKernel.h).
 * Die Default-Implementierung gibt den Code aus und kehrt zurueck; wer
 * lieber anhalten moechte, ueberschreibt sie -- sie ist schwach gebunden,
 * wo der Compiler das unterstuetzt. */
void irk_port_error(int code, int detail);


/*------------------------------------------------------------------------*\
 *  5. Leerlauf
\*------------------------------------------------------------------------*/

/* Wird gerufen, wenn keine Task lauffaehig ist und nur die Zeit
 * weiterlaufen muss. Guter Platz fuer sleep/WFI. Darf leer sein --
 * dann wird gepollt. */
void irk_port_idle(void);


#ifdef __cplusplus
}
#endif

#endif /* IRK_PORT_H */
