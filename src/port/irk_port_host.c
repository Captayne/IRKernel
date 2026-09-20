/*========================================================================*\
 *
 *  irk_port_host.c  --  IRKernel-Backend fuer den PC
 *
 *  Nur fuer Tests und Entwicklung gedacht, nicht fuer den Zielbetrieb.
 *  Erlaubt es, die Scheduling-Politik auf dem Entwicklungsrechner
 *  auszufuehren und zu pruefen, ohne Hardware zu flashen.
 *
 *  Kontextwechsel:
 *      Windows   ->  Fibers        (CreateFiber / SwitchToFiber)
 *      POSIX     ->  ucontext      (makecontext / swapcontext)
 *
 *  In beiden Faellen verwaltet das Betriebssystem den Taskstack selbst;
 *  der an irk_ctx_create() uebergebene Puffer bleibt ungenutzt. Die
 *  Stackueberwachung liefert auf dem Host deshalb keine sinnvollen Werte.
 *
\*========================================================================*/

#if defined(_WIN32) || defined(__unix__) || defined(__APPLE__)

#include <stdio.h>
#include <stdlib.h>

#include "../irk_port.h"

/* Fuer die Zeitmessung im Test soll die Uhr steuerbar sein: siehe
   irk_test_clock_set() ganz unten. */
static uint64_t host_fake_us   = 0;
static int      host_use_fake  = 0;


/*========================================================================*\
 *  Kontextwechsel
\*========================================================================*/

#if defined(_WIN32)

#include <windows.h>

static VOID CALLBACK irk_fiber_proc(LPVOID param)
{
    void (*entry)(void) = (void (*)(void))param;
    entry();
    /* Eine Taskfunktion darf nicht zurueckkehren; der Kernel faengt das
       im Trampolin ab. Sicherheitshalber hier nicht weiterlaufen. */
    for (;;) Sleep(1000);
}

void *irk_ctx_create(void *stack, size_t size, void (*entry)(void))
{
    (void)stack;
    return (void *)CreateFiber(size, irk_fiber_proc, (LPVOID)entry);
}

void irk_ctx_switch(void **old_sp, void *new_sp)
{
    /* Aktuellen Kontext festhalten -- entspricht dem "str sp,[r0]" der
       Assembler-Backends. Beim ersten Wechsel liefert das den durch
       irk_port_init() erzeugten Fiber der Haupttask. */
    *old_sp = GetCurrentFiber();
    SwitchToFiber(new_sp);
}

size_t irk_ctx_min_stack(void) { return 4096; }

static void irk_ctx_init(void)
{
    /* Den laufenden Thread in einen Fiber verwandeln, sonst laesst sich
       nicht von ihm wegschalten. GetCurrentFiber() taugt zur Pruefung
       nicht -- es liefert auf einem Nicht-Fiber-Thread undefinierten
       Muell. IsThreadAFiber() ist der dokumentierte Weg. */
    if (!IsThreadAFiber()) {
        ConvertThreadToFiber(NULL);
    }
}

static uint64_t host_real_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER        c;

    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);

    /* In zwei Schritten, damit c * 1000000 nicht ueberlaeuft */
    return (uint64_t)(c.QuadPart / freq.QuadPart) * 1000000ULL
         + (uint64_t)((c.QuadPart % freq.QuadPart) * 1000000LL / freq.QuadPart);
}

/* Mit steuerbarer Uhr stellt der Leerlauf die Uhr weiter -- sonst stuende
   sie still, sobald alle Tasks warten, und ein leerer Kern liesse sich
   nicht testen. */
void irk_port_idle(void) { if (host_use_fake) host_fake_us += 100; else Sleep(0); }

#else  /* POSIX */

#include <ucontext.h>
#include <sys/time.h>

/* Je Kern eigen: jeder Kern ist hier ein Thread, und die Haupttask
   eines Kerns sichert beim ersten Wechsel ihren Kontext hierhin.
   Ein gemeinsames Objekt liessen die Kerne gegenseitig ihre
   Stackzeiger ueberschreiben. Tasks wechseln den Kern nie, der
   thread-lokale Zeiger bleibt also gueltig. */
static __thread ucontext_t main_uc;

void *irk_ctx_create(void *stack, size_t size, void (*entry)(void))
{
    ucontext_t *uc = (ucontext_t *)malloc(sizeof(ucontext_t));
    if (uc == NULL) return NULL;

    if (getcontext(uc) != 0) { free(uc); return NULL; }
    uc->uc_stack.ss_sp    = stack;
    uc->uc_stack.ss_size  = size;
    uc->uc_link           = NULL;
    makecontext(uc, entry, 0);
    return uc;
}

void irk_ctx_switch(void **old_sp, void *new_sp)
{
    ucontext_t *from = (ucontext_t *)*old_sp;
    if (from == NULL) { from = &main_uc; *old_sp = from; }
    swapcontext(from, (ucontext_t *)new_sp);
}

size_t irk_ctx_min_stack(void) { return 16384; }

static void irk_ctx_init(void) { }

static uint64_t host_real_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

void irk_port_idle(void) { if (host_use_fake) host_fake_us += 100; }

#endif


/*========================================================================*\
 *  Uebrige Portierungsschicht
\*========================================================================*/

void irk_port_init(void)
{
    irk_ctx_init();
}

irk_time_t irk_port_micros(void)
{
    /* Bei 32-Bit-Zeit schneidet der Cast die oberen Bits ab -- genau so,
       wie ein echter 32-Bit-Zaehler ueberlaeuft. Die Tests nutzen das, um
       den Ueberlauf gezielt mitten in einen Testlauf zu legen. */
    return (irk_time_t)(host_use_fake ? host_fake_us : host_real_us());
}

void irk_port_crit_enter(void) { }
void irk_port_crit_exit (void) { }

/* Fuer Tests: wie oft und womit zuletzt ein Kernelfehler gemeldet wurde. */
volatile int irk_test_error_count = 0;
volatile int irk_test_last_error  = 0;

void irk_port_error(int code, int detail)
{
    irk_test_error_count++;
    irk_test_last_error = code;
    fprintf(stderr, "[IRKernel] Fehler %d (Detail %d)\n", code, detail);
}


/*========================================================================*\
 *  Interrupts und Mehrkernbetrieb  --  auf dem PC nachgestellt
 *
 *  Ein "Kern" ist hier ein Betriebssystem-Thread mit eigenen Fibers. Jeder
 *  Thread meldet seine Kernnummer mit irk_test_set_core(); die Tests koennen
 *  so zwei Kerne echt parallel laufen lassen. Einen Interrupt stellt
 *  irk_test_set_isr() nach. Die kernuebergreifende Sperre ist ein Mutex
 *  mit einem Schachtelungszaehler je Thread.
\*========================================================================*/

static __thread int host_core   = 0;
static __thread int host_in_isr = 0;

void irk_test_set_core(int c) { host_core   = c;  }
void irk_test_set_isr(int on) { host_in_isr = on; }

int irk_port_in_isr(void)
{
    return host_in_isr;
}

#if IRK_MAX_CORES > 1

#include <pthread.h>

static pthread_mutex_t host_xlock  = PTHREAD_MUTEX_INITIALIZER;
static __thread int    host_xdepth = 0;

uint8_t irk_port_core_id(void)
{
    return (uint8_t)host_core;
}

void irk_port_xlock(void)
{
    if (host_xdepth++ == 0) pthread_mutex_lock(&host_xlock);
}

void irk_port_xunlock(void)
{
    if (host_xdepth > 0 && --host_xdepth == 0) pthread_mutex_unlock(&host_xlock);
}

#endif


/*========================================================================*\
 *  Steuerbare Uhr  --  nur fuer Tests
 *
 *  Erlaubt es, Zeit deterministisch verstreichen zu lassen, statt auf
 *  echte Millisekunden zu warten. Damit werden Scheduling-Tests
 *  reproduzierbar und laufen in Sekundenbruchteilen durch.
\*========================================================================*/

void irk_test_clock_enable(int on)       { host_use_fake = on; }
void irk_test_clock_set(uint32_t ms)     { host_fake_us  = (uint64_t)ms * 1000ULL; }
void irk_test_clock_set_us(uint64_t us)  { host_fake_us  = us; }
void irk_test_clock_add(uint32_t ms)     { host_fake_us += (uint64_t)ms * 1000ULL; }
void irk_test_clock_add_us(uint64_t us)  { host_fake_us += us; }

#endif /* Host-Plattform */
