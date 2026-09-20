/*========================================================================*\
 *
 *  irk_port_avr.c  --  IRKernel-Kontextwechsel fuer AVR
 *
 *  Getestet gedacht fuer ATmega328P, ATmega1284P, ATmega2560.
 *
 *  Auf AVR legt der CALL-Befehl die Ruecksprungadresse per Hardware auf
 *  den Stack. Der Kontextwechsel braucht deshalb keinerlei Trickserei
 *  mit dem Programmzaehler: das abschliessende RET holt sich die Adresse
 *  vom *neuen* Stack und springt damit in den neuen Kontext.
 *
 *  Zu sichern sind nur die callee-saved Register des avr-gcc-ABI:
 *      r2 .. r17, r28, r29        (18 Byte)
 *  Alles Uebrige hat der Compiler an der Aufrufstelle bereits gerettet,
 *  weil der Wechsel aus einer normalen C-Funktion heraus geschieht.
 *
\*========================================================================*/

#if defined(__AVR__)

#include <avr/io.h>
#include <avr/interrupt.h>

#include "../irk_port.h"


/*------------------------------------------------------------------------*\
 *  Aufbau des gesicherten Kontexts, von der niedrigsten Adresse aufwaerts:
 *
 *      SP+1  .. SP+2    r29, r28
 *      SP+3  .. SP+18   r17 .. r2
 *      SP+19 ..         Ruecksprungadresse, hoechstwertiges Byte zuerst
 *
 *  (SP zeigt auf AVR stets auf die naechste *freie* Zelle.)
\*------------------------------------------------------------------------*/

#if defined(__AVR_3_BYTE_PC__)
#  define IRK_PCBYTES  3         /* ATmega2560 und groesser */
#else
#  define IRK_PCBYTES  2
#endif

#define IRK_CTX_BYTES  (18 + IRK_PCBYTES)


size_t irk_ctx_min_stack(void)
{
    /* Kontext plus etwas Luft fuer den ersten Funktionsrahmen.
       Weniger als das ist auch bei sparsamsten Tasks nicht tragfaehig. */
    return IRK_CTX_BYTES + 40u;
}


void *irk_ctx_create(void *stack, size_t size, void (*entry)(void))
{
    uint8_t  *p;
    uint16_t  addr;
    uint8_t   i;

    if (stack == NULL || size < irk_ctx_min_stack()) return NULL;

    /* Hoechste nutzbare Zelle des Bereichs */
    p = (uint8_t *)stack + size - 1;

    /* Ruecksprungadresse ablegen. RET liest sie hoechstwertiges Byte
       zuerst, also liegt das niederwertigste Byte an der *hoechsten*
       Adresse.
       Funktionszeiger sind bei avr-gcc bereits Wortadressen und damit
       genau das, was RET in den PC laedt -- nicht umrechnen. */
    addr = (uint16_t)entry;

    *p-- = (uint8_t)(addr & 0xFF);
    *p-- = (uint8_t)((addr >> 8) & 0xFF);
#if IRK_PCBYTES == 3
    /* avr-gcc haelt Funktionszeiger auch auf dem ATmega2560 16 bittig und
       erzeugt bei Bedarf Trampoline im unteren Flash. Das hoechste
       PC-Byte ist daher stets 0. */
    *p-- = 0;
#endif

    /* r2..r17, r28, r29  --  alle mit 0 vorbelegen */
    for (i = 0; i < 18; i++) *p-- = 0;

    /* p zeigt jetzt auf die naechste freie Zelle: das ist der SP. */
    return (void *)p;
}


/*------------------------------------------------------------------------*\
 *  Der Kontextwechsel
 *
 *  Aufrufkonvention avr-gcc:
 *      void **old_sp   ->  r25:r24
 *      void  *new_sp   ->  r23:r22
\*------------------------------------------------------------------------*/

/* Die Parameter werden nicht als C-Variablen benutzt, sondern ueber die
   Aufrufkonvention direkt in den Registern gelesen -- daher "unused". */
__attribute__((naked, noinline))
void irk_ctx_switch(void **old_sp __attribute__((unused)),
                    void  *new_sp __attribute__((unused)))
{
    __asm__ __volatile__ (
        /* --- callee-saved Register des alten Kontexts sichern --- */
        "push r2              \n\t"
        "push r3              \n\t"
        "push r4              \n\t"
        "push r5              \n\t"
        "push r6              \n\t"
        "push r7              \n\t"
        "push r8              \n\t"
        "push r9              \n\t"
        "push r10             \n\t"
        "push r11             \n\t"
        "push r12             \n\t"
        "push r13             \n\t"
        "push r14             \n\t"
        "push r15             \n\t"
        "push r16             \n\t"
        "push r17             \n\t"
        "push r28             \n\t"
        "push r29             \n\t"

        /* --- alten Stackpointer nach *old_sp schreiben --- */
        "movw r26, r24        \n\t"   /* X = old_sp                       */
        "in   r0,  __SP_L__   \n\t"
        "st   X+,  r0         \n\t"
        "in   r0,  __SP_H__   \n\t"
        "st   X,   r0         \n\t"

        /* --- neuen Stackpointer laden ---
           Das Schreiben von SP ist nicht atomar: waehrend SPH schon neu
           und SPL noch alt ist, wuerde ein Interrupt auf einen wilden
           Stack zugreifen. Deshalb mit gesperrten Interrupts, und das
           I-Flag danach exakt so wiederherstellen, wie es vorher war. */
        "in   r0,  __SREG__   \n\t"
        "cli                  \n\t"
        "out  __SP_H__, r23   \n\t"
        "out  __SP_L__, r22   \n\t"
        "out  __SREG__, r0    \n\t"

        /* --- Register des neuen Kontexts holen --- */
        "pop  r29             \n\t"
        "pop  r28             \n\t"
        "pop  r17             \n\t"
        "pop  r16             \n\t"
        "pop  r15             \n\t"
        "pop  r14             \n\t"
        "pop  r13             \n\t"
        "pop  r12             \n\t"
        "pop  r11             \n\t"
        "pop  r10             \n\t"
        "pop  r9              \n\t"
        "pop  r8              \n\t"
        "pop  r7              \n\t"
        "pop  r6              \n\t"
        "pop  r5              \n\t"
        "pop  r4              \n\t"
        "pop  r3              \n\t"
        "pop  r2              \n\t"

        /* RET holt die Ruecksprungadresse vom neuen Stack --
           das ist der eigentliche Sprung in den neuen Kontext. */
        "ret                  \n\t"
    );
}


/*------------------------------------------------------------------------*\
 *  Kritische Abschnitte  --  schachtelbar
\*------------------------------------------------------------------------*/

static uint8_t irk_crit_depth = 0;
static uint8_t irk_crit_sreg  = 0;

void irk_port_crit_enter(void)
{
    uint8_t s = SREG;
    cli();
    if (irk_crit_depth == 0) irk_crit_sreg = s;
    irk_crit_depth++;
}

void irk_port_crit_exit(void)
{
    if (irk_crit_depth == 0) return;
    if (--irk_crit_depth == 0) SREG = irk_crit_sreg;
}

/*------------------------------------------------------------------------*\
 *  Interrupt-Erkennung
 *
 *  AVR bietet kein Register, das zuverlaessig anzeigt, ob gerade ein
 *  Interrupt-Handler laeuft -- das I-Bit ist auch in jedem cli()-Abschnitt
 *  geloescht. Die Pruefung entfaellt hier deshalb; Aufrufe aus Interrupts
 *  bleiben trotzdem verboten.
\*------------------------------------------------------------------------*/

int irk_port_in_isr(void)
{
    return 0;
}

#endif /* __AVR__ */
