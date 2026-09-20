# IRKernel

**Stackful cooperative multitasking with proportional-fair priority scheduling — for microcontrollers where an RTOS doesn't fit.**

The idea goes back to a kernel written in 1993 for a PC-based robot controller, developed by the author at the Institute of Robotics Research (IRF) as part of scientific research at the University of Dortmund. After more than 30 years, the author picked the idea up again and reimplemented it for microcontrollers in 2026.

```c
#include <IRKernel.h>

static uint8_t stack_a[200], stack_b[200];

void task_a(void) { for (;;) { /* work */ irk_yield(); } }
void task_b(void) { for (;;) { /* work */ irk_yield(); } }

void setup() {
    irk_init(1);
    irk_task_create(task_a, 1, stack_a, sizeof(stack_a), "a");
    irk_task_create(task_b, 3, stack_b, sizeof(stack_b), "b");
    //                     ^ three times the CPU share of task_a
}

void loop() { irk_delay(1000); }
```

---

## Why another scheduler?

The Arduino ecosystem already has plenty of cooperative multitasking libraries. IRKernel exists because of one combination none of them offer:

|                                          | Stackful | Priorities         | No starvation |
| ---------------------------------------- |:--------:|:------------------:|:-------------:|
| TaskScheduler, protothreads, AceRoutine  | ✗        | interval only      | —             |
| CoopTask, CoopThreads, Arduino Scheduler | ✓        | ✗ round-robin      | ✓             |
| FreeRTOS, ChibiOS                        | ✓        | ✓ preemptive       | ✗             |
| **IRKernel**                             | **✓**    | **✓ proportional** | **✓**         |

**Stackful** means every task owns a stack. A task can call `irk_yield()` from anywhere — five call levels deep, inside a loop, in the middle of a state machine — and resume later with all its local variables intact. Callback-based schedulers cannot do this; they force you to chop long operations into fragments and hand-roll a state machine to stitch them back together.

**Proportional priorities** mean a priority is a *share*, not a rank. A task with priority 3 receives three times the CPU time of a task with priority 1 — it does not preempt it. Nothing starves. Priorities are 16 bits wide (`irk_prio_t`, 1 … 65535), so a housekeeping task at priority 1 next to a control loop at priority 9999 gets exactly 0.01 %. This is close in spirit to what Linux later called CFS, and it turns out to be exactly what you want when a control loop, a user interface and a communication link have to coexist on one small chip.

**Cooperative** means nothing interrupts you between two `irk_yield()` calls. Shared variables need no mutexes, and there are no race conditions to hunt. That, not code size, is the real argument against a preemptive RTOS on a small part.

---

## When to use what

**IRKernel is multitasking for bare metal — next to interrupts, which stay where hard real time really matters.**

A system built on IRKernel has three layers, and each one has its own job:

| Layer           | What it is for                                                                  | Time scale                   | Measured on real hardware                                                                    |
| --------------- | ------------------------------------------------------------------------------- | ---------------------------- | -------------------------------------------------------------------------------------------- |
| **Interrupt**   | Hard real time: encoder edges, emergency stop, communication                    | µs, preempts everything      | Runs independently of the kernel; kernel calls *from* an ISR are rejected by design          |
| **Cyclic task** | Control loops with a fixed period                                               | ms                           | 10 ms period held to within 41–86 µs while the other core runs at full load (RP2040, RP2350) |
| **Fair share**  | Everything else, by share: user interface, communication, logging, housekeeping | as often as its share allows | 3.00 at priorities 768 : 256; a priority-1 task still gets its 962 ppm and never starves     |

The layers meet through a flag or a queue: the interrupt only sets a flag or puts a value into a queue, and a task picks it up. `RP2040_DualCore` shows this pattern.

That puts IRKernel between the two usual choices:

- **Just `loop()`** is enough as long as everything fits into one loop. Once several things need to wait, time out and run at different rates, you end up writing state machines by hand.
- **A full RTOS** brings preemption, and with it mutexes around every shared variable, priority inversion and stack sizing for tasks that can be interrupted anywhere. On a small part it often doesn't fit at all.
- **IRKernel** gives each activity its own stack and lets it be written as straight code with `irk_delay()` and blocking queues, without preemption between tasks on one core — while interrupts keep their hard real-time role.

---

## Footprint

Measured with `avr-gcc 7.3.0` on ATmega328P (Arduino Uno, 2 KB RAM, 32 KB flash):

| Configuration                                              | Flash   | Static RAM |
| ---------------------------------------------------------- | -------:| ----------:|
| Default (6 tasks, 8 semaphores, queues)                    | 5380 B  | **374 B**  |
| Minimal (4 tasks, 4 semaphores, no cyclic)                 | 3644 B  | **190 B**  |
| `SelfTest` sketch on a real Uno, 3 tasks x 220 B stack `*` | 15028 B | 1452 B     |
| `SelfTest` sketch on a real Leonardo (ATmega32U4) `*`      | 17016 B | 1417 B     |

That is 43 bytes per task descriptor and 7 bytes per semaphore, plus whatever stacks you give your tasks. On a Uno, four tasks with 300-byte stacks leave room to spare.

`*` The `SelfTest` rows are what the Arduino IDE reports for the current version, built against the real Arduino AVR core 1.8.8: 70 % of the Uno's RAM and 55 % of the Leonardo's, leaving 596 B and 1143 B for the main stack. The two kernel-only rows above are from version 1.x — the current kernel is larger (see *Time base*), mainly because of microsecond deadlines, cyclic start offsets and the stack early warning.

*For comparison: the 1993 original needed roughly 13 KB of static RAM before a single task stack, because it was dimensioned for a PC — 64 tasks and 1023 semaphores, fixed at compile time. Every dimension is now configurable in `irk_config.h`.*

---

## Supported platforms

| Architecture                                     | Context switch                         | Verified                                                                        |
| ------------------------------------------------ | -------------------------------------- | ------------------------------------------------------------------------------- |
| AVR ATmega328P (Uno, 2-byte PC)                  | 18 registers + hardware return address | **QEMU `arduino-uno`** and **real hardware: Arduino Uno, `SelfTest` 13/13**     |
| AVR ATmega32U4 (Leonardo, 2-byte PC, native USB) | same as ATmega328P                     | **real hardware: Arduino Leonardo, `SelfTest` 13/13, all AVR examples**         |
| AVR ATmega2560 (Mega, 3-byte PC)                 | same, three-byte return address        | **QEMU `mega2560`**                                                             |
| ARM Cortex-M0 / M0+ (SAMD21, RP2040)             | ARMv6-M sequence                       | **QEMU `microbit`** and **real hardware: RP2040 (Waveshare), `SelfTest` 13/13** |
| ARM Cortex-M3 (STM32F1)                          | ARMv7-M sequence                       | **QEMU `mps2-an385`**                                                           |
| ARM Cortex-M4 / M7 (STM32F4, Teensy)             | ARMv7-M + FPU registers                | **QEMU `mps2-an386`**                                                           |
| ARM Cortex-M33 (RP2350 / Pico 2)                 | identical code to Cortex-M4            | **real hardware: Pico 2W, `SelfTest` 13/13, `RP2040_DualCore` 33/33**           |
| PC (Windows / POSIX)                             | Fibers / ucontext                      | host test suite                                                                 |

> **What has and has not been verified.** The context switchers have been *executed* under full instruction-level emulation (QEMU 9.2.3) on five targets — see `test/qemu/run_qemu.sh`. On every one of them the same eight checks pass, including the two that matter most: that callee-saved registers survive a switch, and that a deep call chain resumes intact.
> 
> Cortex-M33 is covered by equivalence rather than by its own run: `irk_ctx_switch` compiles to a byte-identical instruction sequence for Cortex-M4 and Cortex-M33 (both take the ARMv8-M/ARMv7-M Mainline branch), and the M4 run exercises it. QEMU's `mps2-an505` boots into TrustZone secure state and would need considerably more startup code for no additional coverage.
> 
> **Real silicon:** four boards so far. A Waveshare RP2040 passes all 13 `SelfTest` checks, including register preservation and the deep call chain, and runs the two-core `RP2040_DualCore` test. A Raspberry Pi Pico 2W (RP2350, Cortex-M33 cores) does the same: `SelfTest` 13/13 and `RP2040_DualCore` 33/33, with the same fair-share figures as the RP2040. An Arduino Uno (ATmega328P) passes all 13 `SelfTest` checks and runs the `MultiLoop` example -- the first run of the two-byte-return-address AVR path outside emulation. An Arduino Leonardo (ATmega32U4) passes all 13 `SelfTest` checks too; of each task's 220-byte stack, 133 bytes were never touched — with the USB interrupt, which runs on whatever task stack is current, included. `FairShare`, `CyclicControl` and `SemaphoreMutex` run there as well, and delays measured against `micros()` never expire early: about 100 µs late with an idle core, 250–300 µs next to two busy tasks. The Cortex-M3 and M4 paths are verified in emulation only. Emulation does not reproduce interrupt latency, timer behaviour, memory wait states or the surrounding Arduino core. `irk_port_micros()` is a software counter in the QEMU harness, so nothing about real timing has been tested. Flash `SelfTest` before trusting a board.

**Before trusting a new board, flash the `SelfTest` sketch.** It runs the whole feature set on the device and reports over serial at 115200 baud. Its first four checks are the ones the host suite cannot replace, because on a PC the context switch is done by fibers rather than by hand-written assembly:

1. a context switch happens at all
2. **callee-saved registers survive it** — eight values held live across a yield, so the compiler is forced to keep them in exactly the registers `irk_ctx_switch()` must preserve
3. a deep call chain survives it — recursion yields at its deepest point and verifies the checksum on the way back up
4. tasks really do have separate stacks

Each check announces itself *before* it runs and flushes the line, so if the board crashes or hangs, the last line printed names the culprit. Every wait loop is bounded: a broken scheduler produces a failure, not a hang.

ESP32 is deliberately **not** supported. The Arduino ESP32 core is built on ESP-IDF with FreeRTOS already running before `setup()` is called — `loop()` itself executes inside a FreeRTOS task. A second scheduler underneath it would be fighting the first one for no benefit. Use FreeRTOS there; that is what it is for.

---

## The API

```c
/* Kernel */
int         irk_init(irk_prio_t prio);       /* optional: first call starts it */
void        irk_yield(void);                 /* give up the CPU  */
uint8_t     irk_core_id(void);               /* core of the caller */
uint8_t     irk_task_core(irk_task_t t);     /* core a task lives on */

/* Tasks */
irk_task_t  irk_task_create(void (*fn)(void), irk_prio_t prio,   /* 1..65535 */
                            void *stack, size_t size, const char *name);
void        irk_delay(uint32_t ms);          /* cooperative delay() */
void        irk_delay_us(irk_time_t us);     /* same, in microseconds */
irk_time_t  irk_now_us(void);
int         irk_task_suspend(irk_task_t t);
int         irk_task_resume(irk_task_t t);
int         irk_task_kill(irk_task_t t);
int         irk_task_set_prio(irk_task_t t, irk_prio_t prio);

/* Cyclic tasks -- fixed tick, priority over everything else */
int         irk_task_set_cyclic(irk_task_t t, uint32_t period_ms);
int         irk_task_set_cyclic_us(irk_task_t t, irk_time_t period_us);
int         irk_task_set_cyclic_at(irk_task_t t, uint32_t period_ms,     /* first */
                                   uint32_t start_after_ms);               /* tick later */
int         irk_task_set_normal(irk_task_t t, irk_prio_t prio);

/* Counting semaphores -- no dynamic allocation whatsoever */
int         irk_sema_init(irk_sema_t s, int16_t count);
int         irk_sema_wait(irk_sema_t s);
int         irk_sema_signal(irk_sema_t s);
int         irk_sema_try_wait(irk_sema_t s);

/* Queues -- inter-task communication, caller supplies the storage */
int         irk_queue_init(irk_queue_t *q, void *storage,
                           uint8_t item_size, uint8_t capacity);
int         irk_queue_send(irk_queue_t *q, const void *item);   /* blocks if full  */
int         irk_queue_recv(irk_queue_t *q, void *item);         /* blocks if empty */
int         irk_queue_try_send(irk_queue_t *q, const void *item);
int         irk_queue_try_recv(irk_queue_t *q, void *item);

/* Diagnostics */
size_t      irk_stack_free(irk_task_t t);    /* smallest observed headroom */
int         irk_stack_watch(size_t min_free, /* early warning: calls back once */
                            irk_stack_alarm_fn cb);  /* per task below min_free */
uint32_t    irk_task_runtime(irk_task_t t);
irk_time_t  irk_task_runtime_us(irk_task_t t);
uint32_t    irk_task_calls(irk_task_t t);        /* runs so far -- for profiling */
irk_time_t  irk_task_vruntime(irk_task_t t);     /* fair-share account -- for profiling */
```

`IRKernel_classic.h` maps the original 1993 names (`Init_task`, `Done`, `RUNNING`, …) onto these, so sources written for the original kernel compile unchanged.

---

## Several `loop()` functions, as if the board had more cores

`IRKernel_loops.h` is a thin wrapper over the API above. Include it instead of `IRKernel.h` and just write more loops:

```cpp
#include <IRKernel_loops.h>

void setup1() { pinMode(3, OUTPUT); }
void loop1()  { digitalWrite(3, HIGH); delay(500); digitalWrite(3, LOW); delay(500); }

void setup2() { }
void loop2()  { Serial.println(analogRead(A0)); delay(200); }
```

That is the whole sketch: no `irk_init()`, no `irk_task_create()`, no stacks, no `irk_yield()`.

- `setup1`…`setup16` and `loop1`…`loop16` are declared **weak**, so the wrapper sees at run time which ones the sketch actually defines and creates a task only for those.
- Each loop gets its own stack and calls `setupN()` once, then `loopN()` forever, yielding after every pass.
- `delay()` is redirected to `irk_delay()` inside the sketch, so a waiting loop no longer blocks the others; `yield()` becomes `irk_yield()`. `delayMicroseconds()` is left alone — short, exact waits for bit-banged protocols should still busy-wait. Define `IRK_LOOPS_NO_DELAY_MACRO` to keep the originals.
- `setup()` and `loop()` are supplied by the wrapper if the sketch has none. Write your own and call `irk_loops_begin()` once in `setup()`.
- Count and stack size: `IRK_LOOP_MAX` and `IRK_LOOP_STACK` in `irk_config.h` (AVR: 4 × 256 B, otherwise 8 × 1024 B), up to a ceiling of 16. Every slot is reserved whether used or not, and a `#define` in the sketch does not reach the library. `IRK_MAX_TASKS` caps it too — the main task counts, so an AVR has room for five loops at most.
- On RP2040/RP2350 `setup1()`/`loop1()` already mean the *real* second core, so there the wrapper starts numbering at 2.

The cooperative rule still applies: a loop hands over at the end of a pass and at every `delay()`. A pass that computes for 200 ms straight keeps the others waiting for 200 ms; call `irk_yield()` or `delay(0)` in between if that matters.

The `MultiLoop` example runs four such loops — an SOS blink pattern, a sensor average, a serial console and a clock — on an Arduino Leonardo: 9588 B of flash, 1827 B of RAM, and 135 to 183 bytes of each 256-byte stack never touched. Without a kernel, those four would have to be rewritten as interlocking state machines driven by `millis()`.

---

## Time base (new in 2.0)

Internally the kernel counts **microseconds**. Every millisecond function from 1.x still exists and keeps its meaning — `irk_delay(500)` is still 500 ms — and each has a `_us` counterpart.

The type is `irk_time_t`: **64 bits** by default on ARM and PC, **32 bits** on AVR, where 64-bit arithmetic would cost noticeable flash and RAM. A 32-bit microsecond counter wraps every 71.6 minutes, so the kernel is built to not care:

- **Deadlines are stored as start + duration, never as a target time.** The check is `now - start >= duration` in unsigned arithmetic, which is exact across any wrap as long as a single deadline is shorter than half the counter range. On AVR that limit is 35 minutes per deadline; `irk_delay()` with longer times splits them internally.
- **Cyclic tasks keep a reference that advances by exactly one period per activation.** No drift, and no `activations × period` product — in 1.x that product would have overflowed every 71 minutes. If a cyclic task falls several periods behind, the missed ones are skipped rather than replayed in a burst; the phase is preserved.
- **Cyclic tasks can start with an offset.** `irk_task_set_cyclic_at(t, period, start_after)` makes the first activation happen `start_after` after the call, and every later one on the fixed grid *call + offset + k × period*. Two tasks with the same period and different offsets are therefore phase-locked, like the cams on a drum controller: one switches an output on, the other switches it off, and they cannot drift apart. A test runs exactly that for 10 simulated seconds and checks that every on-to-off interval is 500 ms and that the last one equals the first.
- **Fair share uses virtual runtime.** Each task has an account that grows by `runtime × 256 / priority`, and the task with the smallest account runs. The division remainder is carried per task — the same error term as in Bresenham's line algorithm, so nothing is lost even at priority 65535 with short slices. When the smallest account reaches a threshold, the same amount is **subtracted from every account**. Unlike the halving used in 1.x, this preserves every distance exactly, so a starved task keeps its full claim.
- A waking task gets the **larger** of its own account and the current minimum: it is not compensated for time spent waiting, and it cannot gain an edge by sleeping briefly.

The host test suite runs in four variants — 64-bit and 32-bit time, each once starting at zero and once starting **two seconds before 2³² µs**, so the wrap falls into the middle of every single test. An additional test starts half a second before the wrap of `irk_time_t` itself and checks that fair share (436 : 871 ms), a 700 ms deadline (exactly 700 000 µs) and a 10 ms cycle (201 ticks in 2 s) run straight through it.

The optional minimum time slice is `IRK_MIN_TIMESLICE_US`, **off by default** — see *`irk_yield()` means "done"* below. Projects that set `IRK_MIN_TIMESLICE_MS` keep working.

**Cost on AVR:** the ATmega328P kernel grows from 5380 to 6024 bytes of flash and from 374 to 450 bytes of RAM, mostly because deadlines are now stored as two values each. With cyclic start offsets, the stack early warning and the core/ISR checks added since, it is now 7426 bytes of flash and 502 bytes of RAM.

---

## Things worth knowing

**`irk_yield()` means "done".** In 1993 it was called `Done()`, and that is the contract: a task yields when it has finished a piece of work, not in the middle of a computation whose result it still needs. So `irk_yield()` hands over immediately. That makes a pattern possible that needs no interrupt at all: a task that only reads a level sensor and perhaps switches an LED, then yields, runs *very often* while using only its share. How often follows from

> frequency ≈ share ÷ duration of one pass

At priority 1 next to a busy task at priority 255 (share ≈ 0.39 %), a 20 µs pass runs about every 5 ms. Scheduler overhead does not break fairness: each run is charged the scheduling pass that preceded it, so a task that yields constantly pays for that out of its own account.

Code that calls `irk_yield()` after every few instructions — an interpreter loop, for instance — can set `IRK_MIN_TIMESLICE_US` > 0. `irk_yield()` then returns at once until the task has run that long. Blocking calls (`irk_delay()`, semaphores, queues) always switch.

**Waiting belongs in `irk_delay()`, not in a yield loop.** Consider two tasks at the same priority: A does nothing but `while (1) irk_yield();`, B computes for two seconds before it yields.

1. B runs for 2 s. Its account grows by 2 s × 256.
2. B yields. A is now 2 s behind and gets the CPU.
3. A yields after a few microseconds, is charged those, is still almost 2 s behind — and is chosen again.
4. This repeats until A, too, has used 2 s. **B waits two seconds while A spins.**

That is exactly what equal priority promises: equal CPU time. A claims its two seconds even though it has nothing to do with them. Nothing breaks — a single run is charged at most 2²² µs ≈ 4.2 s (`IRK_SLICE_CHARGE_MAX`), so accounts stay below 2³¹ even with 32-bit time, and normalisation preserves every distance. But B's throughput is halved, and A burns the time in scheduling passes. The same happens with a minimum time slice switched on; the cause is fair share, not the slice.

The difference lies between *runnable* and *waiting*. A task that spins in a yield loop is runnable and accumulates a claim. A task that waits — `irk_delay()`, a semaphore, a queue — is lifted to the current minimum account when it wakes (`irk_align()`); it is not compensated for the time it slept. So write A as

```c
for (;;) { check_level(); irk_delay_us(500); }
```

or let it block on a semaphore if it waits for something specific. A short task that does real work and then yields — the level sensor above — is fine; one that yields without doing anything is not.

Keep in mind, too, that while B computes, nobody else runs, cyclic tasks included. Missed cycles are skipped, not replayed.

**Stacks are yours to size.** There is no growth detection at runtime; `IRK_ENABLE_STACKCHECK` paints new stacks with a pattern and `irk_stack_free()` reports the smallest headroom ever observed. Watch it during development and leave margin. On AVR, pass a static array so the linker accounts for it.

**The kernel starts itself.** Any IRKernel call made from a core that has no scheduler yet starts one for that core, with the calling code as its main task at priority 1. `irk_init(prio)` is still there if the main task should get a different priority.

**Never call the kernel from an interrupt.** Interrupts themselves are fine and stay enabled; only a kernel call *from* an ISR is rejected with `IRK_ERR_IN_ISR` (detected via IPSR on Cortex-M). `irk_now_us()` is the one exception. Set a flag in the ISR and let a task pick it up.

**The kernel prints nothing.** It reports errors (`IRK_ERR_…`) in exactly one place, `irk_port_error(code, detail)`, and on Arduino that report is silent by default. To see it, either define `IRK_DEBUG_SERIAL` in `irk_config.h` — the port then prints `[IRKernel] Fehler <code> (<detail>)` — or write your own `void irk_port_error(int code, int detail)` in the sketch; the port's version is weak. Your version may be called from an interrupt, so it should only set counters or flags. Note that a `#define IRK_DEBUG_SERIAL` in the sketch is not enough: the Arduino IDE compiles the library separately. Diagnostics that cost memory can be switched off in `irk_config.h`: `IRK_ENABLE_STATS` (run counters, `irk_task_calls()` then returns 0), `IRK_ENABLE_STACKCHECK` (stack painting, `irk_stack_free()`, `irk_stack_watch()`), `IRK_ENABLE_NAMES` and `IRK_ENABLE_WATCH`. Query functions such as `irk_task_runtime_us()` or `irk_task_vruntime()` are removed by the linker if nothing calls them.

---

## Multi-core (RP2040 / RP2350)

On boards with `ARDUINO_ARCH_RP2040`, `IRK_MAX_CORES` is 2; everywhere else it is 1 and none of the following costs anything.

- **One scheduler per core.** Tasks are created on the core that calls `irk_task_create()` and never migrate. Fair share, cyclic tasks and time slices are evaluated per core — so code in `setup()`/`loop()` and code in `setup1()`/`loop1()` each get their own scheduler, started automatically on first use.
- **One set of kernel objects for both.** Task handles, semaphores and queues are global. A task on core 1 can wait on a semaphore signalled from core 0, read a queue filled by core 0, or suspend, resume and kill a task on core 0. The wake-up itself is only a state change; the owning core's scheduler does the switch at its next yield.
- **Main task handles are fixed:** core *c* has `IRK_MAIN_TASK_OF(c)`, i.e. 1 and 2. User tasks therefore start at handle 3 — never hard-code handles, keep the value `irk_task_create()` returns.
- **Protection** is one hardware spinlock (`PICO_SPINLOCK_ID_OS2`, reserved by the Pico SDK for exactly this) held only for the few instructions that touch shared state — never across a context switch, and never while a user callback runs.
- **Deadlock detection knows about the other core.** A core whose tasks all wait on semaphores, queues or `resume` is not reported as deadlocked, because the other core may still release them.
- A call from any core number ≥ `IRK_MAX_CORES` is rejected with `IRK_ERR_WRONG_CORE`.

Inside one core, the cooperative guarantee holds as before. **Between cores it does not:** a variable touched by tasks on both cores needs a semaphore or a queue, just like with any other two threads.

---

## Test suite

The scheduling policy is verified on the host against a simulated clock, so runs are deterministic and finish in a fraction of a second:

```
$ test/run_tests.sh

== Fair Share -- Prioritaet bestimmt den Rechenzeitanteil
  Laufzeiten:  prio1=4051  prio2=8102  prio3=12153  (ms)
  Durchlaeufe: prio1=4001  prio2=8002  prio3=12003

== Zyklische Task -- periodische Aktivierung mit Vorrang
  Aktivierungen in 1000 ms bei 10 ms Periode: 101 (erwartet ~100)

== Semaphore -- gegenseitiger Ausschluss, ohne jede Allokation
  Hoechste Zahl gleichzeitig im kritischen Abschnitt: 1

 24 Pruefungen, 0 Fehler
```

The fair-share ratios come out at exactly 1 : 2 : 3.

`run_tests.sh` runs eight variants: the scheduling suite (89 checks) with 64- and 32-bit time, each starting at zero and just before the wrap, once more compiled for two cores, and once with the optional minimum time slice switched on; one of its tests confirms that a 20 µs task at priority 1 next to a priority-255 load runs 196 times per second at exactly 0.39 % CPU; and `test_multicore.c` (30 checks) with both time widths. That one runs **two cores truly in parallel** as two OS threads on the real clock: autostart and fixed main handles, mutual exclusion of four tasks across both cores on one semaphore, 2000 values through a queue from core 0 to core 1 in FIFO order while it runs full, a cross-core wake-up without a false deadlock report, suspend/resume/kill of a core-0 task from core 1, 400 short-lived tasks created simultaneously on each core, and rejection of calls from an ISR and from an unknown core.

### On emulated targets

`test/qemu/run_qemu.sh` builds a small bare-metal harness — no Arduino core, no timers, time from a software counter — and runs it under QEMU on every supported architecture. This is what actually exercises the hand-written assembly:

```
AVR:
  BESTANDEN   ATmega328P   (2-Byte-PC)
  BESTANDEN   ATmega2560   (3-Byte-PC)

ARM Cortex-M:
  BESTANDEN   Cortex-M0    (ARMv6-M)
  BESTANDEN   Cortex-M3    (ARMv7-M)
  BESTANDEN   Cortex-M4    (ARMv7E-M, mit FPU)

 5 Laeufe, 0 durchgefallen
```

Each run performs the same eight checks. Two of them are the reason the harness exists:

- **callee-saved registers survive a switch** — eight values are held live across `irk_yield()`, which forces the compiler to keep them in exactly the registers the switcher must preserve
- **a deep call chain survives** — recursion yields at its deepest point and verifies a checksum on the way back up

The ARM harness installs fault handlers that report over semihosting, so a broken switch (a cleared Thumb bit, a bad stack pointer) produces a diagnostic instead of a silent hang.

Notably, all five targets produce byte-identical scheduling results — `2045 / 4090 / 6135` runtime for priorities 1/2/3, and a producer running exactly one queue capacity ahead of its consumer. The scheduler is deterministic across architectures.

---

## Examples

| Sketch                | Shows                                                                                                                                                                                                                                                                                                    |
| --------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `FairShare`           | Three tasks at priorities 1/2/3, printing the ratio they actually achieve                                                                                                                                                                                                                                |
| `CyclicControl`       | A 10 ms control tick holding steady under a deliberately greedy background load                                                                                                                                                                                                                          |
| `SemaphoreMutex`      | When a semaphore is actually needed — and when cooperative scheduling means it is not                                                                                                                                                                                                                    |
| `SelfTest`            | Full on-device test suite — run this first on new hardware                                                                                                                                                                                                                                               |
| `MultiLoop`           | Four `loop()` functions on one core — blink pattern, sensor, serial console and clock, each written straight ahead with `delay()`                                                                                                                                                                        |
| `RP2040_DemoSelftest` | Every feature in one commented sketch, plus an RGB LED driven by phase-locked cyclic tasks                                                                                                                                                                                                               |
| `RP2040_DualCore`     | A scheduler on each core working together through shared queues and a semaphore. Runs in 10-second blocks — queue, cross-core wake-up, semaphore, fair share, a priority-1 task under full load, ISR rejection, then everything at once with per-task CPU time — each with its own LED colour and checks |

---

## Not yet ported

- **Task-local memory** (`Tmalloc` and friends) — a second heap is a poor idea on a part with kilobytes of RAM.
- **The IRDATA layer** — the robot programming language interpreter that sat on top of the original kernel. A much larger undertaking, and a separate project.

---

## Origin

The design goes back to a kernel Andreas Keibel wrote in October 1993 as
the multitasking foundation of a PC-based robot controller at the Institute
of Robotics Research, University of Dortmund.  It ran under DOS with Turbo C,
Watcom C and EMX, and later under Windows, next to a commercial simulation
system.  Context switching back then used `setjmp`/`longjmp` with a
hand-patched stack pointer inside the `jmp_buf` -- a technique that needed a
different magic offset for every compiler, and the one part that did not
survive: this implementation switches contexts in assembler, per
architecture.

What did survive is the design: the proportional-fair scheduler, the cyclic
tasks, the deadlock detection and the semaphores are the 1993 concept, worked
out again from scratch in 2026 for microcontrollers.

## License

**Free for private, educational and research use. Selling devices that contain it requires a paid licence.** See [LICENSE.md](LICENSE.md) for the full terms; the short version:

- **Free** for hobby projects, teaching, academic research, evaluation, and internal tools that are not shipped to third parties. You may pass the library on as long as the licence file travels with it.
- **Commercial use** — selling, renting or leasing devices that contain IRKernel — costs **0.30 EUR per device shipped**, with the **first 100 devices free**. The fee is per device, no matter how many processors or cores in it run the kernel. Report shipped quantities once a year; an invoice follows.
- **Source code** may be passed to contract manufacturers and development partners for your project, but not published or distributed separately.
- **No warranty**, and **no safety certification**: medical devices, vehicles, aviation and similar applications need a separate written agreement.

Licensing and questions: Andreas Keibel — dr.andreas.keibel@googlemail.com
