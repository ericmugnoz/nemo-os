// sound.h — Nemo OS
//
// Driver de audio virtio-sound (dispositivo 'virtio-sound-device' en
// la maquina 'virt' de QEMU). V1: un unico stream de salida (stream 0)
// a 44100Hz/16 bits/estereo fijo, reproduccion SINCRONA (bloqueante
// hasta que termina de sonar) -- ver la nota grande en sound.c sobre
// por que empezamos asi y que haria falta para polifonia real.
#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>
#include <stdbool.h>

// Busca el dispositivo virtio-sound, negocia features, configura las
// 4 colas (control/evento/tx/rx) y prepara+arranca el stream 0 de
// salida a 44100Hz/16 bits/estereo. Devuelve 'true' si todo salio
// bien -- si devuelve 'false' (dispositivo no encontrado, u otro
// fallo), el resto de funciones de este driver son no-ops seguros.
bool sound_init(void);

bool sound_available(void);

// Reproduce un buffer de muestras PCM de 16 bits, estereo,
// entrelazadas (L,R,L,R,...) a 44100Hz -- BLOQUEA hasta que termina
// de sonar (V1, ver nota en sound.c). 'sample_count' es el numero de
// FRAMES (pares L+R), no de bytes ni de muestras individuales.
void sound_play_blocking(const int16_t *samples, uint32_t frame_count);

#endif
