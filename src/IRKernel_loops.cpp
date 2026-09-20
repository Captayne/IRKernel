/*========================================================================*\
 *
 *  IRKernel_loops.cpp  --  Umsetzung zu IRKernel_loops.h
 *
 *  Jede vom Sketch definierte loopN() bekommt eine eigene Task mit
 *  eigenem Stack. Die Task ruft einmal setupN() und danach in einer
 *  Endlosschleife loopN(), mit irk_yield() dazwischen.
 *
 *  Der Header deklariert bis loop16. Wie viele davon wirklich moeglich
 *  sind, sagt IRK_LOOP_MAX in irk_config.h -- so viele Stacks und
 *  Einsprungfunktionen entstehen hier. Jede Einsprungfunktion steht
 *  einzeln hinter einer Bedingung, damit bei kleinem IRK_LOOP_MAX keine
 *  ungenutzten Funktionen uebrig bleiben.
 *
\*========================================================================*/

#if defined(ARDUINO)

/* Im Wrapper selbst soll delay() das echte delay() bleiben. */
#define IRK_LOOPS_NO_DELAY_MACRO
#include "IRKernel_loops.h"

/*------------------------------------------------------------------------*\
 *  Tabellen: Adresse 0 heisst "vom Sketch nicht definiert"
\*------------------------------------------------------------------------*/

typedef void (*irk_loop_fn)(void);

static const irk_loop_fn irk_setups[IRK_LOOP_CEILING] = {
    setup1,  setup2,  setup3,  setup4,  setup5,  setup6,  setup7,  setup8,
    setup9,  setup10, setup11, setup12, setup13, setup14, setup15, setup16
};

static const irk_loop_fn irk_loops[IRK_LOOP_CEILING] = {
    loop1,   loop2,   loop3,   loop4,   loop5,   loop6,   loop7,   loop8,
    loop9,   loop10,  loop11,  loop12,  loop13,  loop14,  loop15,  loop16
};

static uint8_t irk_loop_stapel[IRK_LOOP_MAX][IRK_LOOP_STACK];

/*------------------------------------------------------------------------*\
 *  Der Rumpf jeder Task
\*------------------------------------------------------------------------*/

static void irk_loop_ausfuehren(uint8_t i)
{
    uint8_t n = (uint8_t)(IRK_LOOP_FIRST - 1 + i);   /* 0-basierter Index */

    if (n >= IRK_LOOP_CEILING) return;
    if (irk_setups[n]) irk_setups[n]();

    for (;;) {
        if (irk_loops[n]) irk_loops[n]();
        irk_yield();                    /* hier steckt das Abgeben */
    }
}

/* Eine Einsprungfunktion je Platz -- irk_task_create() nimmt keine
   Argumente entgegen. */
#if IRK_LOOP_MAX >  0
static void irk_loop_t0(void)  { irk_loop_ausfuehren(0);  }
#endif
#if IRK_LOOP_MAX >  1
static void irk_loop_t1(void)  { irk_loop_ausfuehren(1);  }
#endif
#if IRK_LOOP_MAX >  2
static void irk_loop_t2(void)  { irk_loop_ausfuehren(2);  }
#endif
#if IRK_LOOP_MAX >  3
static void irk_loop_t3(void)  { irk_loop_ausfuehren(3);  }
#endif
#if IRK_LOOP_MAX >  4
static void irk_loop_t4(void)  { irk_loop_ausfuehren(4);  }
#endif
#if IRK_LOOP_MAX >  5
static void irk_loop_t5(void)  { irk_loop_ausfuehren(5);  }
#endif
#if IRK_LOOP_MAX >  6
static void irk_loop_t6(void)  { irk_loop_ausfuehren(6);  }
#endif
#if IRK_LOOP_MAX >  7
static void irk_loop_t7(void)  { irk_loop_ausfuehren(7);  }
#endif
#if IRK_LOOP_MAX >  8
static void irk_loop_t8(void)  { irk_loop_ausfuehren(8);  }
#endif
#if IRK_LOOP_MAX >  9
static void irk_loop_t9(void)  { irk_loop_ausfuehren(9);  }
#endif
#if IRK_LOOP_MAX > 10
static void irk_loop_t10(void) { irk_loop_ausfuehren(10); }
#endif
#if IRK_LOOP_MAX > 11
static void irk_loop_t11(void) { irk_loop_ausfuehren(11); }
#endif
#if IRK_LOOP_MAX > 12
static void irk_loop_t12(void) { irk_loop_ausfuehren(12); }
#endif
#if IRK_LOOP_MAX > 13
static void irk_loop_t13(void) { irk_loop_ausfuehren(13); }
#endif
#if IRK_LOOP_MAX > 14
static void irk_loop_t14(void) { irk_loop_ausfuehren(14); }
#endif
#if IRK_LOOP_MAX > 15
static void irk_loop_t15(void) { irk_loop_ausfuehren(15); }
#endif

static const irk_loop_fn irk_loop_tramp[IRK_LOOP_MAX] = {
    irk_loop_t0,
#if IRK_LOOP_MAX >  1
    irk_loop_t1,
#endif
#if IRK_LOOP_MAX >  2
    irk_loop_t2,
#endif
#if IRK_LOOP_MAX >  3
    irk_loop_t3,
#endif
#if IRK_LOOP_MAX >  4
    irk_loop_t4,
#endif
#if IRK_LOOP_MAX >  5
    irk_loop_t5,
#endif
#if IRK_LOOP_MAX >  6
    irk_loop_t6,
#endif
#if IRK_LOOP_MAX >  7
    irk_loop_t7,
#endif
#if IRK_LOOP_MAX >  8
    irk_loop_t8,
#endif
#if IRK_LOOP_MAX >  9
    irk_loop_t9,
#endif
#if IRK_LOOP_MAX > 10
    irk_loop_t10,
#endif
#if IRK_LOOP_MAX > 11
    irk_loop_t11,
#endif
#if IRK_LOOP_MAX > 12
    irk_loop_t12,
#endif
#if IRK_LOOP_MAX > 13
    irk_loop_t13,
#endif
#if IRK_LOOP_MAX > 14
    irk_loop_t14,
#endif
#if IRK_LOOP_MAX > 15
    irk_loop_t15,
#endif
};

#if IRK_ENABLE_NAMES
static const char *const irk_loop_namen[IRK_LOOP_CEILING] = {
    "loop1",  "loop2",  "loop3",  "loop4",  "loop5",  "loop6",  "loop7",  "loop8",
    "loop9",  "loop10", "loop11", "loop12", "loop13", "loop14", "loop15", "loop16"
};
#endif

/*------------------------------------------------------------------------*\
 *  Start
\*------------------------------------------------------------------------*/

uint8_t irk_loops_begin(void)
{
    static bool gestartet = false;
    uint8_t     angelegt  = 0;

    if (gestartet) return 0;
    gestartet = true;

    for (uint8_t i = 0; i < IRK_LOOP_MAX; i++) {
        uint8_t n = (uint8_t)(IRK_LOOP_FIRST - 1 + i);
        if (n >= IRK_LOOP_CEILING) break;
        if (!irk_setups[n] && !irk_loops[n]) continue;   /* nicht benutzt */

        irk_task_t t = irk_task_create(irk_loop_tramp[i], 1,
                                       irk_loop_stapel[i], IRK_LOOP_STACK,
#if IRK_ENABLE_NAMES
                                       irk_loop_namen[n]);
#else
                                       NULL);
#endif
        if (t != IRK_NO_TASK) angelegt++;
    }
    return angelegt;
}

/*------------------------------------------------------------------------*\
 *  setup() und loop() -- nur, wenn der Sketch sie nicht selbst hat
\*------------------------------------------------------------------------*/

__attribute__((weak)) void setup(void)
{
    irk_loops_begin();
}

__attribute__((weak)) void loop(void)
{
    irk_yield();                        /* Haupttask gibt nur ab */
}

#endif /* ARDUINO */
