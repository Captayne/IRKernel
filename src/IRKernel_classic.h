/*========================================================================*\
 *
 *  IRKernel_classic.h  --  Die Originalnamen von 1993
 *
 *  Bildet die Bezeichner des urspruenglichen Kernels auf die
 *  heutige API ab. Gedacht fuer zweierlei:
 *
 *    - bestehenden Code von damals uebersetzen, ohne ihn anzufassen
 *    - Wiedererkennungswert fuer alle, die den Kernel von damals kennen
 *
 *  Warum die Umbenennung ueberhaupt noetig war: Bezeichner wie Done(),
 *  RUNNING oder KILLED sind als globale Symbole in einer Library nicht
 *  haltbar -- sie kollidieren frueher oder spaeter mit fremdem Code.
 *  Deshalb ist irk_* der Normalfall und dieser Header die Ausnahme.
 *
 *  ACHTUNG: Diesen Header nur einbinden, wenn wirklich noetig, und dann
 *  moeglichst spaet -- die Makros hier sind bewusst grob.
 *
\*========================================================================*/

#ifndef IRKERNEL_CLASSIC_H
#define IRKERNEL_CLASSIC_H

#include "IRKernel.h"


/*------------------------------------------------------------------------*\
 *  Taskzustaende
 *
 *  Die Zahlenwerte weichen vom Original ab (dort 1,2,4,8,16,64,256,8192).
 *  Wer sie numerisch vergleicht statt die Makros zu benutzen, muss das
 *  anpassen -- der Kernel selbst tut das nirgends.
\*------------------------------------------------------------------------*/

#define RUNNING          IRK_RUNNING
#define WAITFORTIME      IRK_WAIT_TIME
#define WAITFORSEMAIRD   IRK_WAIT_SEMA
#define WAITFORTSKGOON   IRK_WAIT_RESUME
#define WAITFORDEAD      IRK_WAIT_DEAD
#define WAITFORCYCLE     IRK_WAIT_CYCLE
#define KILLED           IRK_KILLED


/*------------------------------------------------------------------------*\
 *  Kernel starten und beenden
\*------------------------------------------------------------------------*/

#define Init_threader(prio)      irk_init((irk_prio_t)(prio))
#define Deinit_threader()        irk_deinit()


/*------------------------------------------------------------------------*\
 *  Tasks
 *
 *  Init_task() hatte im Original keine Stackangabe -- die Groesse stand
 *  fest in STACKSIZE. Hier wird der Default aus irk_config.h benutzt und
 *  der Stack, sofern die Plattform es erlaubt, dynamisch besorgt. Auf
 *  AVR muss stattdessen irk_task_create() mit eigenem Stack gerufen
 *  werden.
\*------------------------------------------------------------------------*/

#define Init_task(name, func, prio) \
        irk_task_create((func), (irk_prio_t)(prio), NULL, 0, (name))

#define Done()                   irk_yield()

#define Run_task_no(tsk, flag)   irk_task_resume((irk_task_t)(tsk))
#define Stop_task_no(tsk, flag)  irk_task_suspend((irk_task_t)(tsk))
#define Stop_task(flag)          irk_task_suspend(irk_task_self())

#define Kill_task_now(tsk)       irk_task_kill((irk_task_t)(tsk))
#define Kill_task_at(tsk, t)     irk_task_kill_at((irk_task_t)(tsk), (t))
#define Kill_task_after(tsk, t)  irk_task_kill_after((irk_task_t)(tsk), (t))

#define Stop_task_for_ms(tsk, d) irk_task_delay((irk_task_t)(tsk), (d))
#define MT_delay(d)              irk_delay(d)

#define Get_task_handle()        irk_task_self()
#define Get_task_status(tsk)     irk_task_status((irk_task_t)(tsk))
#define Get_task_prio()          irk_task_get_prio(irk_task_self())
#define Change_task_prio(t, p)   irk_task_set_prio((irk_task_t)(t), (irk_prio_t)(p))

#if IRK_ENABLE_NAMES
#define Get_task_name(tsk)       irk_task_name((irk_task_t)(tsk))
#define Get_task_No(name)        irk_task_by_name(name)
#endif


/*------------------------------------------------------------------------*\
 *  Zyklische Tasks
\*------------------------------------------------------------------------*/

#if IRK_ENABLE_CYCLIC
#define Activate_task_cyclically(t, p)  irk_task_set_cyclic((irk_task_t)(t), (p))
#define Deactivate_cyclic(t, p)         irk_task_set_normal((irk_task_t)(t), (uint8_t)(p))
#endif


/*------------------------------------------------------------------------*\
 *  Semaphore
 *
 *  Im Original gab es getrennte "externe" (IRDATA-) und "interne"
 *  Semaphore. Der Unterschied lag allein in der Verwaltung, nicht in der
 *  Semantik -- hier gibt es nur noch eine Sorte.
\*------------------------------------------------------------------------*/

#define EXT_semini(sema, ini)    irk_sema_init((irk_sema_t)(sema), (int16_t)(ini))
#define Wait_for_ext_sema(sema)  irk_sema_wait((irk_sema_t)(sema))
#define Fin_ext_sema(sema)       irk_sema_signal((irk_sema_t)(sema))

#define WaitforINTsema(sema)     irk_sema_wait((irk_sema_t)(sema))
#define FinishedINTsema(sema)    irk_sema_signal((irk_sema_t)(sema))


/*------------------------------------------------------------------------*\
 *  Sonstiges
\*------------------------------------------------------------------------*/

#define Disable_IRkernel()       irk_lock()
#define Enable_IRkernel()        irk_unlock()
#define Getms()                  ((uint32_t)(irk_now_us() / 1000u))

#if IRK_ENABLE_WATCH
#define Init_debug_function(c, f, t)  irk_watch_add((c), (f), (uint8_t)(t))
#define Kill_debug_function(c, f, t)  irk_watch_remove((c), (f))
#define ALWAYS    IRK_WATCH_ALWAYS
#define ONCE      IRK_WATCH_ONCE
#define LEVEL     IRK_WATCH_LEVEL
#define TRIGGER   IRK_WATCH_EDGE
#define POSITIVE  IRK_WATCH_POSITIVE
#define NEGATIVE  IRK_WATCH_NEGATIVE
#endif


/*------------------------------------------------------------------------*\
 *  Nicht uebernommen
 *
 *  Aus dem Original fehlen hier bewusst:
 *
 *    Send_msg / Wait_for_msg / Get_glob_msg / Kill_tsk_msg
 *        Das Message-Passing-System. Es beruht durchgehend auf malloc()
 *        und passt in dieser Form nicht auf einen Mikrocontroller. Eine
 *        allokationsfreie Neufassung ist vorgesehen.
 *
 *    Tmalloc / Trealloc / Free_tsk_mem / Free_Total_Task_Mem
 *        Tasklokale Speicherverwaltung. Auf Zielen mit wenigen Kilobyte
 *        RAM ist ein zweiter Heap keine gute Idee.
 *
 *    Set_extern_index
 *        Der Haken, ueber den die Steuerung ihren IRDATA-Taskindex mitgefuehrt
 *        hat. Wird erst wieder gebraucht, wenn die IRDATA-Schicht
 *        portiert wird.
 *
\*------------------------------------------------------------------------*/

#endif /* IRKERNEL_CLASSIC_H */
