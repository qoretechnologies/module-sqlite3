/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
  sqlite3vfs.h

  Qore Programming Language

  Copyright 2026 Qore Technologies, s.r.o <http://qore.org>

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

#ifndef SQLITE3VFS_H
#define SQLITE3VFS_H

#include <sqlite3.h>
#include <qore/Qore.h>
#include <qore/QoreSandboxManager.h>

#include <string>

#if defined(QORE_HAS_DBI_PIN_CONNECT_PATH) && defined(__linux__)
//! defined if a database of a connection that a sandbox restricts is opened through its pinned path (qore#7605)
#define QORE_SQLITE3_PINNED_VFS 1

//! the name of the VFS that opens the files of a pinned database
#define QORE_SQLITE3_PINNED_VFS_NAME "qore-pinned"

//! registers the VFS that opens the files of pinned databases; returns 0 for OK
DLLLOCAL int qore_sqlite3_pinned_vfs_init();

//! unregisters the VFS that opens the files of pinned databases, when the module is deleted
DLLLOCAL void qore_sqlite3_pinned_vfs_done();

//! pins the database file of a connection that a sandbox restricts (qore#7605)
/** The file is checked against the sandbox of the code that chose the connection values and pinned
    (qore_dbi_pin_connect_path()); the database is then opened with the name returned in \a name (the procfs path
    of the directory held and the name of the file) and the VFS QORE_SQLITE3_PINNED_VFS_NAME, so that the database
    and the files that SQLite derives from its name (journal, super-journal) are opened in the directory checked; the
    database has no shared memory, so WAL mode is not available;
    \a name is empty if the path is not pinned (no sandbox restricts the connection): the database is then opened by
    the path given

    @return 0 for OK, -1 if an exception has been raised
*/
DLLLOCAL int qore_sqlite3_pin_database(Datasource* ds, const char* path, std::string& name, ExceptionSink* xsink);

//! releases a database pinned with qore_sqlite3_pin_database(), after the connection is closed
DLLLOCAL void qore_sqlite3_unpin_database(const std::string& name);
#endif

#endif
