/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * app.db / appinfo.db access. */
#pragma once

#include <stdint.h>

#include "pkg.h"

/* Read only: the system databases are written by the system alone. */
int db_is_registered(const char *title_id);

/* Diagnostics: rows of addcont.db (any table, any text column) that contain needle.
 * tables gets the names of the tables they are in. -1 when the database cannot be read. */
int db_addcont_rows(const char *needle, char *tables, size_t n);
