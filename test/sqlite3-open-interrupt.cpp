// Copyright 2026 Qore Technologies, s.r.o.
// Deterministically interrupt only sandboxed native opens; no sleeps or races.
#include <qore/Qore.h>
#include <qore/QoreSandboxManager.h>
#include <sqlite3.h>
#include <dlfcn.h>
#include <cassert>

extern "C" int sqlite3_open(const char* name, sqlite3** db) {
    using Open = int (*)(const char*, sqlite3**);
    static Open open = reinterpret_cast<Open>(dlsym(RTLD_NEXT, "sqlite3_open"));
    assert(open);
    if (!open) {
        *db = nullptr;
        return SQLITE_MISUSE;
    }
    int result = open(name, db);
    QoreSandboxManagerHelper smh;
    if (smh) {
        smh->requestInterrupt();
    }
    return result;
}
