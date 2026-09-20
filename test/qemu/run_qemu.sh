#!/bin/sh
#=========================================================================
#
#  run_qemu.sh  --  Fuehrt die IRKernel-Testreihe in QEMU aus
#
#  Prueft den handgeschriebenen Kontextwechsel auf echter
#  Instruktionsemulation der Zielarchitekturen -- das kann der
#  Host-Test nicht, weil dort Fibers an seine Stelle treten.
#
#  Vorausgesetzt werden:
#      qemu-system-avr, qemu-system-arm      (MSYS2: mingw-w64-*-qemu)
#      avr-gcc            aus dem Arduino-AVR-Kern
#      arm-none-eabi-gcc  aus dem arduino-pico-Kern
#
#  Pfade lassen sich per Umgebungsvariable ueberschreiben:
#      AVR_BIN=... ARM_BIN=... QEMU_BIN=... ./run_qemu.sh
#
#=========================================================================
set -e
cd "$(dirname "$0")/../.."

ARDUINO15="${ARDUINO15:-$HOME/AppData/Local/Arduino15/packages}"
# Toolchains selbst suchen: die Arduino-IDE aktualisiert die Kerne und legt
# neue Versionen in neue Ordner. Genommen wird die neueste vorhandene.
neueste() { ls -d $1 2>/dev/null | sort -V | tail -1; }
AVR_BIN="${AVR_BIN:-$(neueste "$ARDUINO15/arduino/tools/avr-gcc/*")/bin}"
ARM_BIN="${ARM_BIN:-$(neueste "$ARDUINO15/rp2040/tools/pqt-gcc/*")/bin}"
echo "AVR-Toolchain: $AVR_BIN"
echo "ARM-Toolchain: $ARM_BIN"
QEMU_BIN="${QEMU_BIN:-/c/msys64/ucrt64/bin}"

B=test/qemu/build
mkdir -p $B

CFLAGS="-std=gnu99 -Wall -Os -Isrc"
ARMFLAGS="$CFLAGS -ffreestanding -nostdlib -DIRK_ENABLE_MALLOC_STACKS=0"

gesamt=0
schlecht=0

pruefe_lauf() {           # $1 = Bezeichnung, $2 = Ausgabedatei
    gesamt=$((gesamt + 1))
    if grep -q "FERTIG-OHNE-FEHLER" "$2" 2>/dev/null; then
        echo "  BESTANDEN   $1"
    else
        schlecht=$((schlecht + 1))
        echo "  DURCHGEFALLEN  $1"
        sed -n '/IRKernel/,$p' "$2" | tr -d '\r' | sed 's/^/      /'
    fi
}

#-------------------------------------------------------------------------
#  AVR
#-------------------------------------------------------------------------
bau_avr() {               # $1 = mcu, $2 = Name
    $AVR_BIN/avr-gcc.exe -mmcu=$1 -DF_CPU=16000000UL $CFLAGS \
        -c -o $B/$2_main.o test/qemu/avr_main.c
    $AVR_BIN/avr-gcc.exe -mmcu=$1 $CFLAGS -c -o $B/$2_k.o src/IRKernel.c
    $AVR_BIN/avr-gcc.exe -mmcu=$1 $CFLAGS -c -o $B/$2_p.o src/port/irk_port_avr.c
    $AVR_BIN/avr-gcc.exe -mmcu=$1 -o $B/$2.elf $B/$2_main.o $B/$2_k.o $B/$2_p.o
}

lauf_avr() {              # $1 = Maschine, $2 = Name, $3 = Bezeichnung
    # Das Programm laeuft nach dem Test endlos weiter; die Marke im Text
    # sagt uns, wann wir abbrechen duerfen. Ausgabe in eine Datei, nicht
    # in eine Pipe -- QEMU puffert sonst.
    timeout 120 $QEMU_BIN/qemu-system-avr.exe -M $1 -bios $B/$2.elf \
        -nographic -serial stdio > $B/$2_out.txt 2>&1 || true
    pruefe_lauf "$3" "$B/$2_out.txt"
}

#-------------------------------------------------------------------------
#  ARM
#-------------------------------------------------------------------------
bau_arm() {               # $1 = cpu-flags, $2 = Name
    for f in arm_startup arm_main; do
        $ARM_BIN/arm-none-eabi-gcc.exe $1 -mthumb $ARMFLAGS \
            -c -o $B/$2_$f.o test/qemu/$f.c
    done
    $ARM_BIN/arm-none-eabi-gcc.exe $1 -mthumb $ARMFLAGS -c -o $B/$2_k.o src/IRKernel.c
    $ARM_BIN/arm-none-eabi-gcc.exe $1 -mthumb $ARMFLAGS -c -o $B/$2_p.o src/port/irk_port_cortexm.c
    $ARM_BIN/arm-none-eabi-gcc.exe $1 -mthumb -nostdlib -T test/qemu/arm.ld \
        -o $B/$2.elf $B/$2_arm_startup.o $B/$2_arm_main.o $B/$2_k.o $B/$2_p.o -lgcc
}

lauf_arm() {              # $1 = Maschine, $2 = Name, $3 = Bezeichnung
    timeout 120 $QEMU_BIN/qemu-system-arm.exe -M $1 \
        -semihosting-config enable=on,target=native \
        -nographic -kernel $B/$2.elf > $B/$2_out.txt 2>&1 || true
    pruefe_lauf "$3" "$B/$2_out.txt"
}


echo "============================================================"
echo " IRKernel  --  Kontextwechsel unter QEMU"
echo "============================================================"

echo
echo "AVR:"
bau_avr atmega328p  uno
lauf_avr uno        uno       "ATmega328P   (2-Byte-PC)"
bau_avr atmega2560  mega
lauf_avr mega2560   mega      "ATmega2560   (3-Byte-PC)"

echo
echo "ARM Cortex-M:"
bau_arm "-mcpu=cortex-m0" m0
lauf_arm microbit    m0       "Cortex-M0    (ARMv6-M)"
bau_arm "-mcpu=cortex-m3" m3
lauf_arm mps2-an385  m3       "Cortex-M3    (ARMv7-M)"
# softfp statt hard: die Pico-Toolchain hat keine libgcc fuer die Hard-Float-
# Aufrufkonvention des M4, und seit 2.0 wird libgcc fuer 64-Bit-Division
# gebraucht. Die FPU bleibt aktiv (__ARM_FP gesetzt), der Kontextwechsel
# sichert also weiterhin s16..s31 -- nur Gleitkomma-Argumente werden ueber
# normale Register uebergeben.
bau_arm "-mcpu=cortex-m4 -mfloat-abi=softfp -mfpu=fpv4-sp-d16" m4
lauf_arm mps2-an386  m4       "Cortex-M4    (ARMv7E-M, mit FPU)"

echo
echo "============================================================"
echo " $gesamt Laeufe, $schlecht durchgefallen"
echo "============================================================"

# Hinweis: Cortex-M33 wird hier nicht gefahren. QEMUs mps2-an505
# startet im Secure-Zustand mit TrustZone, was einen deutlich
# groesseren Startcode braeuchte. Der erzeugte Maschinencode von
# irk_ctx_switch ist fuer M33 und M4 identisch (nachpruefbar mit
# objdump), der M4-Lauf deckt den Pfad also ab.

exit $schlecht
