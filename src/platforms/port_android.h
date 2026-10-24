/**
 * platforms/port_android.h
 *
 * Display preferences owned by the Android backend (android.c) and offered in
 * Settings. Both take effect immediately and are saved.
 */
#ifndef PLATFORMS_PORT_ANDROID_H
#define PLATFORMS_PORT_ANDROID_H

#include <stdbool.h>

/* Smooth (bilinear) or sharp (nearest-neighbour) scaling of the picture. */
bool port_get_smooth(void);
void port_set_smooth(bool smooth);

/* Size of the window buffer the picture is drawn into: 0 native (up to
 * 960 px high), 1 at most 720 px high, 2 at most 540 px high. Lower is
 * lighter on the CPU of weaker devices. */
int  port_get_resolution(void);
void port_set_resolution(int level);

#endif
