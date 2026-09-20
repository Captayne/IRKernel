/*========================================================================*\
 *
 *  irk_port_arduino.cpp  --  Plattformunabhaengiger Teil der
 *                            IRKernel-Portierungsschicht fuer Arduino
 *
 *  Der Kontextwechsel steckt architekturabhaengig in
 *      irk_port_avr.c        bzw.
 *      irk_port_cortexm.c
 *
 *  Hier stehen nur Zeit, Leerlauf und Diagnose -- alles ueber die
 *  Arduino-Kern-API, also auf jedem Board gleich.
 *
\*========================================================================*/

#if defined(ARDUINO)

#include <Arduino.h>
#if defined(ARDUINO_ARCH_RP2040)
#include <hardware/timer.h>      /* time_us_64() */
#include <hardware/sync.h>       /* Hardware-Spinlocks */
#include <pico/platform.h>       /* get_core_num() */
#endif

extern "C" {

#include "../irk_port.h"


void irk_port_init(void)
{
    /* Der Arduino-Kern hat zu diesem Zeitpunkt bereits alles aufgesetzt,
       was der Kernel braucht (insbesondere den millis()-Timer). */
}


irk_time_t irk_port_micros(void)
{
#if defined(ARDUINO_ARCH_RP2040)
    /* RP2040/RP2350: der Hardware-Timer zaehlt ohnehin 64 Bit breit. */
    return (irk_time_t)time_us_64();

#elif IRK_TIME_64
    /* Andere Boards liefern mit micros() nur 32 Bit, die alle 71 Minuten
       ueberlaufen. Die Ueberlaeufe werden hier mitgezaehlt. Das ist
       korrekt, solange diese Funktion oefter als alle 71 Minuten gerufen
       wird -- der Scheduler tut das bei jedem irk_yield(). Nur aus
       Task-Kontext aufrufen, nicht aus Interrupts. */
    static uint32_t zuletzt = 0;
    static uint64_t oben    = 0;
    uint32_t m = (uint32_t)micros();
    if (m < zuletzt) oben += 0x100000000ULL;
    zuletzt = m;
    return oben | m;

#else
    /* 32-Bit-Zeit: Ueberlauf ist erlaubt, der Kernel rechnet nur mit
       Differenzen. Auf AVR hat micros() eine Aufloesung von 4 us. */
    return (irk_time_t)micros();
#endif
}


#if defined(ARDUINO_ARCH_RP2040) && (IRK_MAX_CORES > 1)
/*------------------------------------------------------------------------*\
 *  Mehrkernbetrieb auf RP2040 / RP2350
 *
 *  Kernnummer: das SIO-Register CPUID, das jeder Kern an derselben Adresse
 *  mit seiner eigenen Nummer liest -- im SDK get_core_num().
 *
 *  Sperre: einer der Hardware-Spinlocks des Chips. Das Pico-SDK haelt die
 *  Nummern PICO_SPINLOCK_ID_OS1 und OS2 ausdruecklich fuer Betriebssystem-
 *  kerne frei; IRKernel nimmt OS2. Ein fest gewaehlter Spinlock braucht
 *  keine Anmeldung zur Laufzeit -- die waere selbst ein Wettlauf, wenn
 *  beide Kerne gleichzeitig starten. Der Zaehler je Kern macht die Sperre
 *  schachtelbar.
 *
 *  Die Interrupts bleiben dabei AN (spin_lock_unsafe_blocking statt
 *  spin_lock_blocking). Sie zu sperren schuetzt nur vor Interrupt-Handlern,
 *  die denselben Lock nehmen -- und aus einem Interrupt heraus wird
 *  IRKernel nie gerufen (irk_enter lehnt das ab). Unterbricht ein Interrupt
 *  einen Kern, der den Lock haelt, wartet der andere nur etwas laenger.
 *  Gesperrte Interrupts dagegen stoerten USB, Timer und alles andere auf
 *  dem Kern, der gerade auf den Lock wartet.
\*------------------------------------------------------------------------*/

static uint32_t irk_xl_tiefe[2] = { 0, 0 };

uint8_t irk_port_core_id(void)
{
    return (uint8_t)get_core_num();
}

void irk_port_xlock(void)
{
    uint32_t k = get_core_num();
    if (irk_xl_tiefe[k]++ == 0) {
        spin_lock_unsafe_blocking(spin_lock_instance(PICO_SPINLOCK_ID_OS2));
    }
}

void irk_port_xunlock(void)
{
    uint32_t k = get_core_num();
    if (irk_xl_tiefe[k] > 0 && --irk_xl_tiefe[k] == 0) {
        spin_unlock_unsafe(spin_lock_instance(PICO_SPINLOCK_ID_OS2));
    }
}
#endif

void irk_port_idle(void)
{
    /* Wird nur gerufen, wenn keine Task lauffaehig ist und lediglich
       eine Frist ablaufen muss. Hier liesse sich schlafen legen --
       bewusst nicht getan, weil ein Sleep auf vielen Boards den
       millis()-Timer mit anhaelt und der Kernel dann nicht mehr
       aufwacht. Wer das fuer sein Board besser weiss, ersetzt diese
       Funktion. */
}



/* Schwach gebunden: eine eigene Definition im Sketch ueberschreibt
   diese hier, ohne dass die Library angefasst werden muss. */
__attribute__((weak))
void irk_port_error(int code, int detail)
{
#if defined(IRK_DEBUG_SERIAL)
    Serial.print(F("[IRKernel] Fehler "));
    Serial.print(code);
    Serial.print(F(" ("));
    Serial.print(detail);
    Serial.println(F(")"));
#else
    (void)code; (void)detail;
#endif
}

} /* extern "C" */

#endif /* ARDUINO */
