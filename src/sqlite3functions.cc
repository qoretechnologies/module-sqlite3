/*
  sqlite3functions.cc

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

#include "sqlite3functions.h"

#include <qore/QoreRegexInterface.h>

namespace {

// Translate a flags string ("imsxg") to QRE_* options. Returns true if 'g' was present.
bool parse_subst_flags(const unsigned char* flags, int nflags, int64& opts) {
    bool global = false;
    for (int i = 0; i < nflags; ++i) {
        switch (flags[i]) {
            case 'i': opts |= QRE_CASELESS;  break;
            case 'm': opts |= QRE_MULTILINE; break;
            case 's': opts |= QRE_DOTALL;    break;
            case 'x': opts |= QRE_EXTENDED;  break;
            case 'g': global = true;         break;
            default:                         break;
        }
    }
    return global;
}

extern "C" void regex_match_destroy(void* p) {
    delete static_cast<QoreRegexInterface*>(p);
}

extern "C" void regex_subst_destroy(void* p) {
    delete static_cast<QoreRegexSubstInterface*>(p);
}

// regexp(pattern, str) -> int (0/1) | NULL
// SQLite invokes this function for the `expr REGEXP pattern` operator
// (pattern is argv[0], expr is argv[1]).
extern "C" void regexp_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    assert(argc == 2);
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL
        || sqlite3_value_type(argv[1]) == SQLITE_NULL) {
        sqlite3_result_null(ctx);
        return;
    }

    QoreRegexInterface* re = static_cast<QoreRegexInterface*>(sqlite3_get_auxdata(ctx, 0));
    if (!re) {
        const char* p = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
        int plen = sqlite3_value_bytes(argv[0]);
        QoreString pat(p, static_cast<size_t>(plen), QCS_UTF8);
        ExceptionSink xsink;
        std::unique_ptr<QoreRegexInterface> tmp(new QoreRegexInterface(&xsink, pat));
        if (xsink) {
            xsink.clear();
            sqlite3_result_error(ctx, "regexp: invalid regular expression pattern", -1);
            return;
        }
        re = tmp.release();
        // SQLite takes ownership of `re` via the destructor below.
        sqlite3_set_auxdata(ctx, 0, re, regex_match_destroy);
    }

    const char* t = reinterpret_cast<const char*>(sqlite3_value_text(argv[1]));
    int tlen = sqlite3_value_bytes(argv[1]);
    QoreString target(t, static_cast<size_t>(tlen), QCS_UTF8);
    ExceptionSink xsink;
    bool m = re->match(target, &xsink);
    if (xsink) {
        xsink.clear();
        sqlite3_result_error(ctx, "regexp: error during pattern match", -1);
        return;
    }
    sqlite3_result_int(ctx, m ? 1 : 0);
}

// regexp_replace(str, pattern, repl [, flags]) -> str | NULL
extern "C" void regexp_replace_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    assert(argc == 3 || argc == 4);

    for (int i = 0; i < argc; ++i) {
        if (sqlite3_value_type(argv[i]) == SQLITE_NULL) {
            sqlite3_result_null(ctx);
            return;
        }
    }

    int64 opts = QRE_GLOBAL;
    if (argc == 4) {
        const unsigned char* flags = sqlite3_value_text(argv[3]);
        int flen = sqlite3_value_bytes(argv[3]);
        opts = 0;
        if (parse_subst_flags(flags, flen, opts)) {
            opts |= QRE_GLOBAL;
        }
    }

    // Cache only when flags arg is absent — otherwise the cached regex's options
    // could become stale if the flags column varies across rows.
    const bool can_cache = (argc == 3);
    QoreRegexSubstInterface* re = nullptr;
    std::unique_ptr<QoreRegexSubstInterface> owned;

    if (can_cache) {
        re = static_cast<QoreRegexSubstInterface*>(sqlite3_get_auxdata(ctx, 1));
    }
    if (!re) {
        const char* p = reinterpret_cast<const char*>(sqlite3_value_text(argv[1]));
        int plen = sqlite3_value_bytes(argv[1]);
        QoreString pat(p, static_cast<size_t>(plen), QCS_UTF8);
        ExceptionSink xsink;
        owned.reset(new QoreRegexSubstInterface(&xsink, pat, opts));
        if (xsink) {
            xsink.clear();
            sqlite3_result_error(ctx, "regexp_replace: invalid regular expression pattern", -1);
            return;
        }
        if (can_cache) {
            re = owned.release();
            sqlite3_set_auxdata(ctx, 1, re, regex_subst_destroy);
        } else {
            re = owned.get();
        }
    }

    const char* s = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
    int slen = sqlite3_value_bytes(argv[0]);
    QoreString src(s, static_cast<size_t>(slen), QCS_UTF8);

    const char* r = reinterpret_cast<const char*>(sqlite3_value_text(argv[2]));
    int rlen = sqlite3_value_bytes(argv[2]);
    QoreString repl(r, static_cast<size_t>(rlen), QCS_UTF8);

    ExceptionSink xsink;
    SimpleRefHolder<QoreStringNode> result(re->subst(src, repl, &xsink));
    if (xsink) {
        xsink.clear();
        sqlite3_result_error(ctx, "regexp_replace: error during substitution", -1);
        return;
    }
    if (!result) {
        sqlite3_result_null(ctx);
        return;
    }
    sqlite3_result_text(ctx, result->c_str(), static_cast<int>(result->size()), SQLITE_TRANSIENT);
}

} // anonymous namespace

int qore_sqlite3_register_functions(sqlite3* db, ExceptionSink* xsink) {
    int rc = sqlite3_create_function_v2(
        db, "regexp", 2,
        SQLITE_UTF8 | SQLITE_DETERMINISTIC,
        nullptr, regexp_func, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        xsink->raiseException("SQLITE3-REGISTER-ERROR",
            "failed to register regexp() UDF: %s", sqlite3_errmsg(db));
        return -1;
    }

    rc = sqlite3_create_function_v2(
        db, "regexp_replace", 3,
        SQLITE_UTF8 | SQLITE_DETERMINISTIC,
        nullptr, regexp_replace_func, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        xsink->raiseException("SQLITE3-REGISTER-ERROR",
            "failed to register regexp_replace/3 UDF: %s", sqlite3_errmsg(db));
        return -1;
    }

    rc = sqlite3_create_function_v2(
        db, "regexp_replace", 4,
        SQLITE_UTF8 | SQLITE_DETERMINISTIC,
        nullptr, regexp_replace_func, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        xsink->raiseException("SQLITE3-REGISTER-ERROR",
            "failed to register regexp_replace/4 UDF: %s", sqlite3_errmsg(db));
        return -1;
    }

    return 0;
}
