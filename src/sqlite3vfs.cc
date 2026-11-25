/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
  sqlite3vfs.cc

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

#include "sqlite3vfs.h"

#ifdef QORE_SQLITE3_PINNED_VFS

#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

/*  The database file of a connection that a sandbox restricts is pinned (qore#7605): the directory that holds it is
    held by the pinned path, and the database is opened with the procfs link of that directory and its name
    ("/proc/self/fd/<dirfd>/<name>"), so that every file that SQLite derives from the database name (the name and a
    suffix: "-journal", "-wal", "-shm") is in the directory checked, whatever its path reaches afterwards.  SQLite
    opens the database file without following a link; the files that it derives from the name are checked not to be
    links before they are opened.  The name is used as it is: resolving the procfs link would reach the directory by
    its path.
*/

namespace {
struct PinnedDatabase {
    //! the pinned path of the database file
    QoreDbiPinnedPath* pin = nullptr;

    ~PinnedDatabase() {
        qore_dbi_pinned_path_free(pin);
    }
};

//! the pinned databases, by the name that they are opened with
std::map<std::string, std::unique_ptr<PinnedDatabase>> pinned_databases;
std::mutex pinned_lock;

//! the VFS that the pinned VFS delegates to (the default VFS)
sqlite3_vfs* base_vfs = nullptr;
sqlite3_vfs pinned_vfs;

//! returns true if a name given to the pinned VFS is refused: a file derived from the name of a pinned database that
//! is a link, or that is not in its directory
bool name_refused(const char* zName) {
    if (!zName || strncmp(zName, "/proc/self/fd/", 14)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(pinned_lock);
    for (auto& i : pinned_databases) {
        const std::string& key = i.first;
        if (strncmp(zName, key.c_str(), key.size())) {
            continue;
        }
        const char* suffix = zName + key.size();
        if (!*suffix) {
            // the database file itself, which SQLite opens without following a link
            return false;
        }
        // another database whose name starts with this one's
        if (*suffix != '-') {
            continue;
        }
        // a file derived from the database name: a suffix without a directory, and not a link
        if (strchr(suffix, '/')) {
            return true;
        }
        return qore_dbi_pinned_path_access(i.second->pin, suffix, F_OK) && errno == ELOOP;
    }
    return false;
}

int pinned_open(sqlite3_vfs* vfs, const char* zName, sqlite3_file* file, int flags, int* outFlags) {
    if (name_refused(zName)) {
        return SQLITE_CANTOPEN;
    }
    return base_vfs->xOpen(base_vfs, zName, file, flags, outFlags);
}

int pinned_delete(sqlite3_vfs* vfs, const char* zName, int syncDir) {
    if (name_refused(zName)) {
        return SQLITE_IOERR_DELETE;
    }
    return base_vfs->xDelete(base_vfs, zName, syncDir);
}

int pinned_access(sqlite3_vfs* vfs, const char* zName, int flags, int* pResOut) {
    if (name_refused(zName)) {
        *pResOut = 0;
        return SQLITE_OK;
    }
    return base_vfs->xAccess(base_vfs, zName, flags, pResOut);
}

int pinned_full_pathname(sqlite3_vfs* vfs, const char* zName, int nOut, char* zOut) {
    // the name of a pinned database is used as it is: resolving the procfs link would reach the file by its name
    if (zName && !strncmp(zName, "/proc/self/fd/", 14)) {
        size_t len = strlen(zName);
        if ((int)len >= nOut) {
            return SQLITE_CANTOPEN;
        }
        memcpy(zOut, zName, len + 1);
        return SQLITE_OK;
    }
    return base_vfs->xFullPathname(base_vfs, zName, nOut, zOut);
}
}

int qore_sqlite3_pinned_vfs_init() {
    base_vfs = sqlite3_vfs_find(nullptr);
    if (!base_vfs) {
        return -1;
    }
    pinned_vfs = *base_vfs;
    pinned_vfs.zName = QORE_SQLITE3_PINNED_VFS_NAME;
    pinned_vfs.pNext = nullptr;
    pinned_vfs.pAppData = nullptr;
    pinned_vfs.xOpen = pinned_open;
    pinned_vfs.xDelete = pinned_delete;
    pinned_vfs.xAccess = pinned_access;
    pinned_vfs.xFullPathname = pinned_full_pathname;
    return sqlite3_vfs_register(&pinned_vfs, 0) == SQLITE_OK ? 0 : -1;
}

int qore_sqlite3_pin_database(Datasource* ds, const char* path, std::string& name, ExceptionSink* xsink) {
    std::unique_ptr<PinnedDatabase> db(new PinnedDatabase);
    db->pin = qore_dbi_pin_connect_path(ds, path, QSEC_READ | QSEC_WRITE | QSEC_CREATE, xsink);
    if (!db->pin) {
        return -1;
    }
    // without a sandbox that restricts the connection, or where the path cannot be pinned, the name is not one of
    // the procfs link of the directory held
    name = qore_dbi_pinned_path_get(db->pin);
    if (strncmp(name.c_str(), "/proc/self/fd/", 14)) {
        name.clear();
        return 0;
    }
    std::lock_guard<std::mutex> lock(pinned_lock);
    pinned_databases[name] = std::move(db);
    return 0;
}

void qore_sqlite3_unpin_database(const std::string& name) {
    std::unique_ptr<PinnedDatabase> db;
    {
        std::lock_guard<std::mutex> lock(pinned_lock);
        auto i = pinned_databases.find(name);
        if (i == pinned_databases.end()) {
            return;
        }
        db = std::move(i->second);
        pinned_databases.erase(i);
    }
}

#endif
