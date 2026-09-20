/*========================================================================*\
 *
 *  avr_main.c  --  QEMU-Testrahmen fuer ATmega328P (arduino-uno)
 *
 *  Bare metal, ohne Arduino-Kern. Ausgabe direkt ueber USART0.
 *
 *  Die Zeit kommt aus einem Softwarezaehler statt aus einem Timer:
 *  QEMU emuliert die 8-Bit-Timer des ATmega nur unvollstaendig, und der
 *  Test soll ohnehin den Kontextwechsel pruefen, nicht die Zeitbasis.
 *
 *  Bauen und starten:  test/qemu/run_qemu.sh
 *
\*========================================================================*/

#include <avr/io.h>

#include "../../src/IRKernel.h"
#include "../../src/irk_port.h"

#ifndef F_CPU
#define F_CPU 16000000UL
#endif
#define BAUD_UBRR  8            /* 115200 bei 16 MHz mit U2X0 */


/*------------------------------------------------------------------------*\
 *  USART0
\*------------------------------------------------------------------------*/

static void uart_init(void)
{
    UBRR0H = (uint8_t)(BAUD_UBRR >> 8);
    UBRR0L = (uint8_t)(BAUD_UBRR);
    UCSR0A = (1 << U2X0);
    UCSR0B = (1 << TXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);   /* 8N1 */
}

static void uart_putc(char c)
{
    while (!(UCSR0A & (1 << UDRE0))) { }
    UDR0 = (uint8_t)c;
}

static void put(const char *s)
{
    while (*s) uart_putc(*s++);
}


/*------------------------------------------------------------------------*\
 *  Portierungsschicht, soweit nicht in irk_port_avr.c
 *
 *  irk_port_avr.c liefert irk_ctx_create/-switch/-min_stack sowie die
 *  kritischen Abschnitte. Der Rest steht hier.
\*------------------------------------------------------------------------*/

volatile uint32_t sw_ms = 0;

void     irk_port_init(void)             { }
irk_time_t irk_port_micros(void)       { return (irk_time_t)sw_ms * 1000u; }
void     irk_port_idle(void)             { sw_ms++; }

void irk_port_error(int code, int detail)
{
    put("  [Kernelfehler ");
    uart_putc((char)('0' + (code / 10) % 10));
    uart_putc((char)('0' + code % 10));
    put(" / ");
    uart_putc((char)('0' + (detail / 10) % 10));
    uart_putc((char)('0' + detail % 10));
    put("]\r\n");
}


/*------------------------------------------------------------------------*\
 *  Gemeinsamer Testkoerper
\*------------------------------------------------------------------------*/

#include "qemu_tests.c"


int main(void)
{
    uart_init();
#if defined(__AVR_ATmega2560__)
    put("\r\n[AVR ATmega2560]\r\n");
#elif defined(__AVR_ATmega328P__)
    put("\r\n[AVR ATmega328P]\r\n");
#else
    put("\r\n[AVR]\r\n");
#endif
#if defined(__AVR_3_BYTE_PC__)
    put("[3-Byte-Programmzaehler -- eigener Rahmenaufbau]\r\n");
#else
    put("[2-Byte-Programmzaehler]\r\n");
#endif

    tests_ausfuehren();

    /* QEMU kennt fuer AVR keinen sauberen Ausstieg -- der Testlaeufer
       erkennt das Ende an der Marke und beendet die Maschine. */
    put("QEMU-ENDE\r\n");
    for (;;) { }
    return 0;
}
