/*========================================================================*\
 *
 *  irk_port_cortexm.c  --  IRKernel-Kontextwechsel fuer ARM Cortex-M
 *
 *  Deckt ab:
 *      ARMv6-M   Cortex-M0, M0+        (SAMD21, RP2040, STM32F0/G0, nRF51)
 *      ARMv7-M   Cortex-M3, M4, M7     (STM32F1/F4, nRF52, Teensy)
 *      ARMv8-M   Cortex-M23, M33       (RP2350, STM32L5/U5)
 *
 *  Gesichert werden nur die callee-saved Register des AAPCS. Bei
 *  vorhandener FPU kommen s16..s31 dazu -- die sind ebenfalls
 *  callee-saved und wuerden sonst beim Wechsel verlorengehen.
 *
 *  Der Clou steckt in der letzten Instruktion: "pop {..., pc}" laedt den
 *  Programmzaehler aus dem *neuen* Stack. Die Funktion kehrt also nicht
 *  dorthin zurueck, wo sie aufgerufen wurde, sondern dorthin, wo die
 *  neue Task zuletzt stehengeblieben ist.
 *
 *  ACHTUNG bei ARMv6-M: "push {r4-r11}" gibt es dort nicht, PUSH kann nur
 *  r0-r7 und LR. r8..r11 muessen deshalb ueber die unteren Register
 *  umgeladen werden.
 *
\*========================================================================*/

#if defined(__arm__) && (defined(__ARM_ARCH_6M__)      || \
                         defined(__ARM_ARCH_7M__)      || \
                         defined(__ARM_ARCH_7EM__)     || \
                         defined(__ARM_ARCH_8M_BASE__) || \
                         defined(__ARM_ARCH_8M_MAIN__))

#include "../irk_port.h"


/*------------------------------------------------------------------------*\
 *  Rahmengroesse
 *
 *  Ohne FPU:  r4..r11 + lr                     =  9 Worte
 *  Mit  FPU:  zusaetzlich s16..s31             = 25 Worte
 *
 *  Die Ruecksprungadresse liegt in beiden Faellen im *letzten* Wort des
 *  Rahmens, also an der hoechsten Adresse -- der Rest wird mit 0
 *  vorbelegt. Damit ist die Kontexterzeugung fuer alle Varianten
 *  dieselbe Schleife.
\*------------------------------------------------------------------------*/

#if defined(__ARM_FP) && (__ARM_FP > 0)
#  define IRK_CTX_HAS_FPU  1
#  define IRK_CTX_WORDS    25
#else
#  define IRK_CTX_HAS_FPU  0
#  define IRK_CTX_WORDS     9
#endif


size_t irk_ctx_min_stack(void)
{
    /* Kontextrahmen plus Reserve fuer den ersten Funktionsrahmen und
       -- falls die Tasks auf dem MSP laufen -- fuer eine Interruptlage. */
    return (IRK_CTX_WORDS * 4u) + 128u;
}


void *irk_ctx_create(void *stack, size_t size, void (*entry)(void))
{
    uint32_t *sp;
    unsigned  i;

    if (stack == NULL || size < irk_ctx_min_stack()) return NULL;

    /* Oberkante auf 8 Byte ausrichten (AAPCS verlangt das an
       oeffentlichen Schnittstellen). */
    sp = (uint32_t *)(((uintptr_t)stack + size) & ~(uintptr_t)7);

    /* Ruecksprungadresse.
       Bit 0 MUSS gesetzt sein: "pop {...,pc}" fuehrt auf M-Profil einen
       Interworking-Sprung aus, und ein geloeschtes Bit 0 bedeutete
       ARM-Modus -- den es hier nicht gibt. Das quittiert der Kern mit
       einem UsageFault (INVSTATE). C-Funktionszeiger tragen das Bit
       ohnehin; das ODER ist die Absicherung. */
    *--sp = ((uint32_t)entry) | 1u;

    /* Restlichen Rahmen nullen (r4..r11 bzw. zusaetzlich s16..s31) */
    for (i = 0; i < (IRK_CTX_WORDS - 1u); i++) *--sp = 0;

    return (void *)sp;
}


/*------------------------------------------------------------------------*\
 *  Der Kontextwechsel
 *
 *  AAPCS:   r0 = old_sp (Zeiger),   r1 = new_sp (Wert)
\*------------------------------------------------------------------------*/

#if defined(__ARM_ARCH_6M__) || defined(__ARM_ARCH_8M_BASE__)

/*---- ARMv6-M / ARMv8-M Baseline: PUSH nur mit r0-r7 ----*/

__attribute__((naked, noinline))
void irk_ctx_switch(void **old_sp __attribute__((unused)),
                    void  *new_sp __attribute__((unused)))
{
    __asm__ __volatile__ (
        "push   {r4-r7, lr}     \n"   /* untere callee-saved + Ruecksprung */
        "mov    r4, r8          \n"   /* obere ueber die unteren retten    */
        "mov    r5, r9          \n"
        "mov    r6, r10         \n"
        "mov    r7, r11         \n"
        "push   {r4-r7}         \n"
        "mov    r2, sp          \n"
        "str    r2, [r0]        \n"   /* alten SP festhalten               */
        "mov    sp, r1          \n"   /* neuen SP laden                    */
        "pop    {r4-r7}         \n"
        "mov    r8,  r4         \n"
        "mov    r9,  r5         \n"
        "mov    r10, r6         \n"
        "mov    r11, r7         \n"
        "pop    {r4-r7, pc}     \n"   /* Sprung in den neuen Kontext       */
        ::: "memory"
    );
}

#else

/*---- ARMv7-M / ARMv8-M Mainline ----*/

__attribute__((naked, noinline))
void irk_ctx_switch(void **old_sp __attribute__((unused)),
                    void  *new_sp __attribute__((unused)))
{
    __asm__ __volatile__ (
        "push   {r4-r11, lr}    \n"
#if IRK_CTX_HAS_FPU
        "vpush  {s16-s31}       \n"   /* callee-saved FPU-Register         */
#endif
        "str    sp, [r0]        \n"   /* alten SP festhalten               */
        "mov    sp, r1          \n"   /* neuen SP laden                    */
#if IRK_CTX_HAS_FPU
        "vpop   {s16-s31}       \n"
#endif
        "pop    {r4-r11, pc}    \n"   /* Sprung in den neuen Kontext       */
        ::: "memory"
    );
}

#endif


/*------------------------------------------------------------------------*\
 *  Kritische Abschnitte  --  schachtelbar
\*------------------------------------------------------------------------*/

static uint32_t irk_crit_depth = 0;
static uint32_t irk_crit_primask = 0;

void irk_port_crit_enter(void)
{
    uint32_t pm;
    __asm__ __volatile__ ("mrs %0, primask" : "=r"(pm));
    __asm__ __volatile__ ("cpsid i" ::: "memory");
    if (irk_crit_depth == 0) irk_crit_primask = pm;
    irk_crit_depth++;
}

void irk_port_crit_exit(void)
{
    if (irk_crit_depth == 0) return;
    if (--irk_crit_depth == 0) {
        if (irk_crit_primask == 0) {
            __asm__ __volatile__ ("cpsie i" ::: "memory");
        }
    }
}

/*------------------------------------------------------------------------*\
 *  Interrupt-Erkennung
 *
 *  Das IPSR-Register enthaelt die Nummer der gerade laufenden Ausnahme;
 *  0 bedeutet normaler Programmablauf (Thread-Modus). Ein einziger
 *  Registerzugriff, auf jedem Kern der eigene Wert.
\*------------------------------------------------------------------------*/

int irk_port_in_isr(void)
{
    uint32_t ipsr;
    __asm__ __volatile__ ("mrs %0, ipsr" : "=r"(ipsr));
    return (ipsr & 0x1FFu) != 0u;
}

#endif /* Cortex-M */
