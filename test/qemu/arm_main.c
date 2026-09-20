/*========================================================================*\
 *
 *  arm_main.c  --  QEMU-Testrahmen fuer ARM Cortex-M
 *
 *  Bare metal, ohne jedes Betriebssystem. Ausgabe ueber Semihosting,
 *  damit kein UART-Treiber je Maschine noetig ist.
 *
 *  Wie beim AVR-Rahmen kommt die Zeit aus einem Softwarezaehler: der
 *  Test soll den Kontextwechsel pruefen, nicht die Zeitbasis.
 *
 *  Bauen und starten:  test/qemu/run_qemu.sh
 *
\*========================================================================*/

#include <stdint.h>

#include "../../src/IRKernel.h"
#include "../../src/irk_port.h"

/* aus arm_startup.c */
void sh_write0(const char *s);
void sh_exit(int code);


/*------------------------------------------------------------------------*\
 *  Ausgabe
 *
 *  Semihosting kennt nur ganze, nullterminierte Zeichenketten, daher
 *  wird hier gepuffert und bei Zeilenende oder vollem Puffer geleert.
\*------------------------------------------------------------------------*/

static char  aus_puffer[128];
static int   aus_n = 0;

static void aus_leeren(void)
{
    if (aus_n == 0) return;
    aus_puffer[aus_n] = 0;
    sh_write0(aus_puffer);
    aus_n = 0;
}

static void put(const char *s)
{
    while (*s) {
        aus_puffer[aus_n++] = *s;
        if (*s == '\n' || aus_n >= (int)sizeof(aus_puffer) - 2) aus_leeren();
        s++;
    }
}


/*------------------------------------------------------------------------*\
 *  Portierungsschicht, soweit nicht in irk_port_cortexm.c
\*------------------------------------------------------------------------*/

volatile uint32_t sw_ms = 0;

void     irk_port_init(void)           { }
irk_time_t irk_port_micros(void)       { return (irk_time_t)sw_ms * 1000u; }
void     irk_port_idle(void)           { sw_ms++; }

void irk_port_error(int code, int detail)
{
    char b[8];
    put("  [Kernelfehler ");
    b[0] = (char)('0' + (code / 10) % 10);
    b[1] = (char)('0' + code % 10);
    b[2] = '/';
    b[3] = (char)('0' + (detail / 10) % 10);
    b[4] = (char)('0' + detail % 10);
    b[5] = ']'; b[6] = '\n'; b[7] = 0;
    put(b);
}


/*------------------------------------------------------------------------*\
 *  Gemeinsamer Testkoerper
\*------------------------------------------------------------------------*/

#include "qemu_tests.c"


int main(void)
{
#if defined(__ARM_ARCH_6M__)
    put("\n[Cortex-M0 / ARMv6-M]\n");
#elif defined(__ARM_ARCH_8M_MAIN__)
    put("\n[Cortex-M33 / ARMv8-M Mainline]\n");
#elif defined(__ARM_ARCH_7EM__)
    put("\n[Cortex-M4 / ARMv7E-M]\n");
#else
    put("\n[Cortex-M3 / ARMv7-M]\n");
#endif
#if defined(__ARM_FP) && (__ARM_FP > 0)
    put("[FPU aktiv -- s16..s31 werden mitgesichert]\n");
#else
    put("[ohne FPU]\n");
#endif

    tests_ausfuehren();

    aus_leeren();
    sh_exit(fehler ? 1 : 0);
    return 0;
}
