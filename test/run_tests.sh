#!/bin/sh
# Baut die IRKernel-Tests gegen das Host-Backend und fuehrt sie aus.
#
# test_kernel.c -- Scheduling-Politik, ein Thread:
#   64-Bit-Zeit, Uhr startet bei 0
#   64-Bit-Zeit, Uhr startet 2 s vor 2^32 us  (findet versehentliches
#                                              Abschneiden auf 32 Bit)
#   32-Bit-Zeit, Uhr startet bei 0
#   32-Bit-Zeit, Uhr startet 2 s vor 2^32 us  (der Ueberlauf faellt mitten
#                                              in jeden einzelnen Test)
#   64-Bit-Zeit, uebersetzt fuer zwei Kerne   (alle Tests auf Kern 0 --
#                                              prueft Sperre und Kernpfade)
#   64-Bit-Zeit, Mindest-Zeitscheibe 1 ms     (der abschaltbare Pfad)
#
# test_multicore.c -- zwei Kerne echt parallel als zwei Threads:
#   64-Bit-Zeit und 32-Bit-Zeit
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}

WRAP_T0=4292967296ULL      # 2^32 - 2.000.000 us

fehler=0
lauf() {                   # $1 = Bezeichnung, $2 = Testquelle, $3 = Flags
    echo
    echo "############################################################"
    echo "  $1"
    echo "############################################################"
    $CC -std=c99 -Wall -Wextra -O1 -g $3 \
        -o test/test_lauf \
        "$2" src/IRKernel.c src/port/irk_port_host.c
    if ./test/test_lauf; then :; else fehler=$((fehler + 1)); fi
}

K=test/test_kernel.c
M=test/test_multicore.c

lauf "64-Bit-Zeit, Start bei 0"              $K "-DIRK_TIME_64=1"
lauf "64-Bit-Zeit, Start kurz vor 2^32"      $K "-DIRK_TIME_64=1 -DIRK_TEST_T0=$WRAP_T0"
lauf "32-Bit-Zeit, Start bei 0"              $K "-DIRK_TIME_64=0"
lauf "32-Bit-Zeit, Ueberlauf mitten im Test" $K "-DIRK_TIME_64=0 -DIRK_TEST_T0=$WRAP_T0"
lauf "64-Bit-Zeit, fuer zwei Kerne uebersetzt" $K "-DIRK_TIME_64=1 -DIRK_MAX_CORES=2 -pthread"
lauf "64-Bit-Zeit, Mindest-Zeitscheibe 1 ms"  $K "-DIRK_TIME_64=1 -DIRK_MIN_TIMESLICE_US=1000"
lauf "Zwei Kerne parallel, 64-Bit-Zeit"      $M "-DIRK_TIME_64=1 -DIRK_MAX_CORES=2 -pthread"
lauf "Zwei Kerne parallel, 32-Bit-Zeit"      $M "-DIRK_TIME_64=0 -DIRK_MAX_CORES=2 -pthread"

echo
echo "============================================================"
echo "  Varianten mit Fehlern: $fehler von 8"
echo "============================================================"
exit $fehler
