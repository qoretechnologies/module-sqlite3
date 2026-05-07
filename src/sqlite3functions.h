/*
  sqlite3functions.h

  Qore Programming Language

  Copyright 2003 - 2026 Qore Technologies, s.r.o <http://qore.org>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef SQLITE3FUNCTIONS_H
#define SQLITE3FUNCTIONS_H

#include <sqlite3.h>
#include <qore/Qore.h>

//! Registers Qore-backed regex UDFs on the given sqlite3 connection.
/**
    Registers:
    - regexp(pattern, str) -> 0/1 — backs the SQL `expr REGEXP pattern` operator
    - regexp_replace(str, pattern, repl) -> str — global replace
    - regexp_replace(str, pattern, repl, flags) -> str — with PCRE flags
                                                        (chars: i, m, s, x, g)

    @param db open sqlite3 connection
    @param xsink exception sink to receive registration failures

    @retval int 0 on success, -1 on error (xsink populated)
*/
int qore_sqlite3_register_functions(sqlite3* db, ExceptionSink* xsink);

#endif
