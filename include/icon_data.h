/*
 * icon_data.h - declarations for the embedded tray icon.
 *
 * The bytes themselves live in src/icon_data.c so that the optimizer
 * in main.c cannot see them as compile-time constants and strip them
 * as dead data. main.c reads them through these externs, which makes
 * the compiler treat the contents as runtime-unknown and keep the
 * array intact.
 */
#ifndef KVMAPPER_ICON_DATA_H
#define KVMAPPER_ICON_DATA_H

#include <stddef.h>

extern const unsigned char kv_ico_data[];
extern const size_t kv_ico_size;

#endif /* KVMAPPER_ICON_DATA_H */
