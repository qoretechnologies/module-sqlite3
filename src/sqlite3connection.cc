/*
  sqlite3connection.cc

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

#include "sqlite3connection.h"


QoreSqlite3Connection::QoreSqlite3Connection(sqlite3* handler, const QoreEncoding* enc)
        : m_handler(handler), enc(enc) {
}

bool QoreSqlite3Connection::begin(ExceptionSink* xsink) {
    if (!sqlite3_get_autocommit(m_handler)) {
        return true;
    }
    char * zErrMsg = 0;
    int rc = sqlite3_exec(m_handler, "BEGIN;", NULL, 0, &zErrMsg);
    if (rc != SQLITE_OK) {
        xsink->raiseException("SQLITE3-BEGIN-ERROR", zErrMsg);
        sqlite3_free(zErrMsg);
        return false;
    }
    return true;
}

bool QoreSqlite3Connection::commit(ExceptionSink* xsink) {
    char * zErrMsg = 0;
    int rc = sqlite3_exec(m_handler, "COMMIT;", NULL, 0, &zErrMsg);
    if (rc != SQLITE_OK) {
        xsink->raiseException("SQLITE3-COMMIT-ERROR", zErrMsg);
        sqlite3_free(zErrMsg);
        return false;
    }
    return true;
}

bool QoreSqlite3Connection::rollback(ExceptionSink* xsink) {
    char * zErrMsg = 0;

    int rc = sqlite3_exec(m_handler, "ROLLBACK;", NULL, 0, &zErrMsg);
    if (rc != SQLITE_OK) {
        xsink->raiseException("SQLITE3-ROLLBACK-ERROR", zErrMsg);
        sqlite3_free(zErrMsg);
        return false;
    }
    return true;
}

bool QoreSqlite3Connection::close() {
    int rc = sqlite3_close(m_handler);
    return (rc == SQLITE_OK);
}

char * QoreSqlite3Connection::getServerVersion() {
    return (char*)sqlite3_libversion();
}
