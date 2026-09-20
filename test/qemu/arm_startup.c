/*========================================================================*\
 *
 *  arm_startup.c  --  Minimaler Bare-metal-Start fuer die QEMU-Laeufe
 *
 *  Vektortabelle, Reset-Handler und Fehler-Handler. Bewusst winzig
 *  gehalten -- hier soll nichts ablenken.
 *
 *  Die Fehler-Handler sind kein Beiwerk: waere der Kontextwechsel
 *  fehlerhaft (etwa ein geloeschtes Thumb-Bit in der Ruecksprung-
 *  adresse), landet der Kern im HardFault. Statt still zu haengen,
 *  meldet er sich dann ueber Semihosting und beendet die Maschine.
 *
\*========================================================================*/

#include <stdint.h>

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;

int  main(void);
void Reset_Handler(void);
static void Default_Handler(void);
static void Fault_Handler(void);


/*------------------------------------------------------------------------*\
 *  Semihosting  --  reicht hier fuer Ausgabe und Ausstieg
\*------------------------------------------------------------------------*/

#define SYS_WRITE0  0x04
#define SYS_EXIT    0x18

static int sh_call(int op, void *arg)
{
    register int   r0 __asm__("r0") = op;
    register void *r1 __asm__("r1") = arg;
    __asm__ volatile ("bkpt #0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

void sh_write0(const char *s) { sh_call(SYS_WRITE0, (void *)s); }
void sh_exit(int code)
{
    /* 0x20026 = ADP_Stopped_ApplicationExit */
    sh_call(SYS_EXIT, (void *)(uintptr_t)(code ? 0x20023 : 0x20026));
    for (;;) { }
}


/*------------------------------------------------------------------------*\
 *  Winzige libc-Ersatzteile
 *
 *  Der Testrahmen wird freistehend gebaut (-nostdlib), damit nichts aus
 *  newlib hineingezogen wird. Der Kernel braucht nur diese drei.
\*------------------------------------------------------------------------*/

void *memset(void *d, int c, unsigned long n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
    return d;
}

int strcmp(const char *a, const char *b)
{
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}


/*------------------------------------------------------------------------*\
 *  Vektortabelle
\*------------------------------------------------------------------------*/

__attribute__((section(".isr_vector"), used))
void (* const g_vectors[])(void) = {
    (void (*)(void))&_estack,   /*  0  Anfangs-Stackpointer   */
    Reset_Handler,              /*  1  Reset                  */
    Fault_Handler,              /*  2  NMI                    */
    Fault_Handler,              /*  3  HardFault              */
    Fault_Handler,              /*  4  MemManage              */
    Fault_Handler,              /*  5  BusFault               */
    Fault_Handler,              /*  6  UsageFault             */
    0, 0, 0, 0,                 /*  7..10 reserviert          */
    Default_Handler,            /* 11  SVCall                 */
    Default_Handler,            /* 12  DebugMon               */
    0,                          /* 13  reserviert             */
    Default_Handler,            /* 14  PendSV                 */
    Default_Handler,            /* 15  SysTick                */
    /* Ab hier die geraetespezifischen Interrupts -- werden nicht
       benutzt, ein paar Plaetze genuegen. */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler
};


/*------------------------------------------------------------------------*\
 *  Handler
\*------------------------------------------------------------------------*/

static void Default_Handler(void)
{
    sh_write0("\r\n*** unerwarteter Interrupt ***\r\n");
    sh_exit(1);
}

static void Fault_Handler(void)
{
    /* Genau hier landet man, wenn der Kontextwechsel eine ungueltige
       Ruecksprungadresse hinterlaesst -- etwa mit geloeschtem Thumb-Bit
       (UsageFault INVSTATE) oder einem Stackpointer ins Nirgendwo. */
    sh_write0("\r\n*** FAULT -- Kontextwechsel vermutlich fehlerhaft ***\r\n");
    sh_exit(1);
}

void Reset_Handler(void)
{
#if defined(__ARM_FP) && (__ARM_FP > 0)
    /* FPU freischalten (CPACR, CP10/CP11 auf volle Rechte).
       Ohne das quittiert schon das erste "vpush" mit einem UsageFault. */
    *(volatile uint32_t *)0xE000ED88 |= (0xFu << 20);
    __asm__ volatile ("dsb" ::: "memory");
    __asm__ volatile ("isb" ::: "memory");
#endif

    {   /* .data aus dem Flash ins RAM kopieren */
        uint32_t *src = &_sidata, *dst = &_sdata;
        while (dst < &_edata) *dst++ = *src++;
    }
    {   /* .bss nullen */
        uint32_t *p = &_sbss;
        while (p < &_ebss) *p++ = 0;
    }

    main();

    sh_write0("\r\nmain() ist zurueckgekehrt\r\n");
    sh_exit(0);
}
