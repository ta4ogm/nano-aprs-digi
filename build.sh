#!/bin/sh
# Yazilimi derler ve ayar aracinin sayfasina gomer.
# Gerekli: avr-gcc, avr-libc, binutils-avr, python3
set -e
cd "$(dirname "$0")"
mkdir -p build
cd firmware
avr-gcc -mmcu=atmega328p -Os -I. -std=gnu99 \
  main.c hardware/Serial.c hardware/AFSK.c util/CRC-CCIT.c protocol/AX25.c protocol/Digipeater.c \
  -o ../build/digi.elf -lm
cd ..
avr-objcopy -O ihex -R .eeprom build/digi.elf build/digi_base.hex
avr-size build/digi.elf
python3 - <<'PY'
t = open('site/index.template.html', encoding='utf-8').read()
h = open('build/digi_base.hex').read()
open('index.html', 'w', encoding='utf-8').write(t.replace('__BASE_HEX__', h))
PY
cp build/digi_base.hex firmware/digi_base.hex
echo "Tamam: index.html ve firmware/digi_base.hex guncellendi."
