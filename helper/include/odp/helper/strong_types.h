/* Copyright (c) 2015, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

/**
 * @file
 *
 * ODP Strong Types. Common macros for implementing strong typing
 * for ODP abstract data types
 */

#ifndef ODPH_STRONG_TYPES_H_
#define ODPH_STRONG_TYPES_H_

/** @addtogroup odph_strong_types ODPH STRONG TYPES
 *  @{
 */

/** Use strong typing for ODP types */
#ifdef __cplusplus
/** @internal C++ helper macro for strong typing  @param type @return */
#define ODPH_HANDLE_T(type) struct _##type { uint8_t unused_dummy_var; } *type
#else
#define odph_handle_t struct { uint8_t unused_dummy_var; } *
/** @internal C helper macro for strong typing @param type @return */
#define ODPH_HANDLE_T(type) odph_handle_t type
#endif

/** Internal macro to get value of an ODP handle */
#define _odph_typeval(handle) ((uintptr_t)(handle))

/** Internal macro to get printable value of an ODP handle */
#define _odph_pri(handle) ((uint64_t)(uintptr_t)(handle))

/** Internal macro to convert a scalar to a typed handle */
#define _odph_cast_scalar(type, val) ((type)(uintptr_t)(val))

/**
 * @}
 */

#endif
