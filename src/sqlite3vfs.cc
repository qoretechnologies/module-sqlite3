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
#include <new>

/*  The database file of a connection that a sandbox restricts is pinned (qore#7605): the directory that holds it is
    held by the pinned path, and the database is opened with the procfs link of that directory and its name
    ("/proc/self/fd/<dirfd>/<name>"), so that the files that SQLite derives from the database name (the name and a
    suffix: "-journal", "-wal", super-journals) are in the directory checked, whatever its path reaches afterwards.
    The name is used as it is: resolving the procfs link would reach the directory by its path.  SQLite's unix VFS
    opens every file without following a link (O_NOFOLLOW); a derived file that is a link is also refused before it
    is opened, checked, or removed.

    The main database file of a pinned connection has no shared memory (WAL mode is not available): the WAL-index of
    the unix VFS belongs to the file (its inode), is shared by the connections of the process that have it open, and
    is named and removed by the name of the connection that created it, which could outlive the pinned directory of
    that connection; a pinned connection therefore neither creates nor removes one.
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
//! true if pinned_vfs is registered
bool pinned_vfs_registered = false;

//! the I/O methods of the main database files of pinned connections: those of the base VFS without shared memory
//! (version 1), by the methods of the base VFS; the elements of a map do not move, and are never removed, as open
//! files refer to them
std::map<const sqlite3_io_methods*, sqlite3_io_methods> no_shm_methods;

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

//! returns the I/O methods of \a methods without shared memory, or nullptr if they cannot be allocated
const sqlite3_io_methods* get_no_shm_methods(const sqlite3_io_methods* methods) {
    std::lock_guard<std::mutex> lock(pinned_lock);
    try {
        auto i = no_shm_methods.find(methods);
        if (i == no_shm_methods.end()) {
            i = no_shm_methods.insert({methods, *methods}).first;
            // version 1: no xShmMap(), xShmLock(), xShmBarrier(), xShmUnmap(), xFetch(), xUnfetch()
            i->second.iVersion = 1;
        }
        return &i->second;
    } catch (std::bad_alloc&) {
        return nullptr;
    }
}

int pinned_open(sqlite3_vfs* vfs, const char* zName, sqlite3_file* file, int flags, int* outFlags) {
    if (name_refused(zName)) {
        return SQLITE_CANTOPEN;
    }
    int rc = base_vfs->xOpen(base_vfs, zName, file, flags, outFlags);
    if (rc == SQLITE_OK && (flags & SQLITE_OPEN_MAIN_DB) && file->pMethods) {
        const sqlite3_io_methods* m = get_no_shm_methods(file->pMethods);
        if (!m) {
            file->pMethods->xClose(file);
            file->pMethods = nullptr;
            return SQLITE_NOMEM;
        }
        file->pMethods = m;
    }
    return rc;
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
    if (sqlite3_vfs_register(&pinned_vfs, 0) != SQLITE_OK) {
        return -1;
    }
    pinned_vfs_registered = true;
    return 0;
}

void qore_sqlite3_pinned_vfs_done() {
    if (pinned_vfs_registered) {
        sqlite3_vfs_unregister(&pinned_vfs);
        pinned_vfs_registered = false;
    }
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
    try {
        std::lock_guard<std::mutex> lock(pinned_lock);
        pinned_databases[name] = std::move(db);
    } catch (std::bad_alloc&) {
        name.clear();
        xsink->outOfMemory();
        return -1;
    }
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
