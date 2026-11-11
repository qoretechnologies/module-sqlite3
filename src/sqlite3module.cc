/*
    sqlite3module.cc

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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>
#include <memory>
#include <string>

#include <qore/Qore.h>
#include <qore/QoreSandboxManager.h>

#include "sqlite3module.h"
#include "sqlite3connection.h"
#include "sqlite3executor.h"

#if defined(QDBI_METHOD_STMT_FETCH_COLUMNAR) || defined(QDBI_METHOD_SELECT_COLUMNAR)
#include <qore/QoreColumnarResult.h>
#endif
#include "sqlite3functions.h"
#include "config.h"

//! the exception code and the reason why the authorizer refused a statement in this thread, for
//! qore_sqlite3_take_auth_error()
static thread_local std::string qore_sqlite3_auth_err, qore_sqlite3_auth_denial;

//! Refuses a statement in the authorizer, recording the exception code and the reason
static int qore_sqlite3_auth_deny(const char* reason, const char* err = "FILESYSTEM-ACCESS-DENIED") {
    try {
        qore_sqlite3_auth_err = err;
        qore_sqlite3_auth_denial = reason;
    } catch (...) {
        // the statement is refused in any case
    }
    return SQLITE_DENY;
}

//! Checks the files that SQL opens against the sandbox of the code that compiles the statement
/** ATTACH opens the file named by its argument, VACUUM INTO its target (when it runs, SQLite attaches the evaluated
    name, so the authorizer sees it as a literal then), and PRAGMA temp_store_directory and data_store_directory let
    SQLite create files in any directory; without a sandbox, all are allowed.  In a sandbox, an ATTACH argument that
    is not a literal (which SQLite passes as NULL when it compiles the statement) or a URI cannot be checked and is
    refused, as is setting a directory, and a file name is checked against the filesystem policy as for the database
    itself.  The authorizer runs in the thread that compiles the statement (also when sqlite3_step() compiles it again)
    or, for VACUUM INTO, runs it.

    No C++ exception may leave this function, which SQLite calls.
*/
static int qore_sqlite3_authorizer_intern(int code, const char* arg1, const char* arg2) {
    if (code == SQLITE_PRAGMA) {
        // only setting a directory opens files elsewhere
        if (!arg1 || !arg2 || (strcasecmp(arg1, "temp_store_directory") && strcasecmp(arg1, "data_store_directory"))) {
            return SQLITE_OK;
        }
    } else if (code != SQLITE_ATTACH) {
        return SQLITE_OK;
    }
    QoreSandboxManagerHelper smh(QoreSandboxManagerHelper::Policy);
    if (!smh) {
        return SQLITE_OK;
    }
    if (code == SQLITE_PRAGMA) {
        return qore_sqlite3_auth_deny("setting the SQLite temporary or data directory cannot be checked against the "
            "sandbox");
    }
    if (!arg1) {
        return qore_sqlite3_auth_deny("an SQLite ATTACH or VACUUM INTO file name that is not a literal cannot be "
            "checked against the sandbox");
    }
    // an empty name is a private temporary database
    if (!*arg1 || !strcmp(arg1, ":memory:")) {
        return SQLITE_OK;
    }
    if (!strncasecmp(arg1, "file:", 5)) {
        return qore_sqlite3_auth_deny("an SQLite ATTACH or VACUUM INTO file name that is a URI cannot be checked "
            "against the sandbox");
    }
    ExceptionSink xsink;
    if (!smh->checkFilesystemAccess(arg1, QSEC_READ | QSEC_WRITE | QSEC_CREATE, &xsink)) {
        // the exception of the check (FILESYSTEM-ACCESS-DENIED, or an error resolving the path)
        QoreValue err = xsink.getExceptionErr();
        QoreValue desc = xsink.getExceptionDesc();
        int rc = qore_sqlite3_auth_deny(desc.getType() == NT_STRING ? desc.get<const QoreStringNode>()->c_str()
            : "access to the file is denied by the sandbox",
            err.getType() == NT_STRING ? err.get<const QoreStringNode>()->c_str() : "FILESYSTEM-ACCESS-DENIED");
        xsink.clear();
        return rc;
    }
    return SQLITE_OK;
}

static int qore_sqlite3_authorizer(void*, int code, const char* arg1, const char* arg2, const char*, const char*) {
    if (code != SQLITE_ATTACH && code != SQLITE_PRAGMA) {
        return SQLITE_OK;
    }
    try {
        return qore_sqlite3_authorizer_intern(code, arg1, arg2);
    } catch (...) {
        // out of memory, for example: the statement is refused
        return qore_sqlite3_auth_deny("the SQLite statement could not be checked against the sandbox");
    }
}

int qore_sqlite3_take_auth_error(sqlite3* db, ExceptionSink* xsink) {
    if (qore_sqlite3_auth_denial.empty()) {
        return 0;
    }
    std::string err, reason;
    err.swap(qore_sqlite3_auth_err);
    reason.swap(qore_sqlite3_auth_denial);
    if (!db || sqlite3_errcode(db) != SQLITE_AUTH) {
        return 0;
    }
    xsink->raiseException(err.empty() ? "FILESYSTEM-ACCESS-DENIED" : err.c_str(), "%s", reason.c_str());
    return -1;
}

static void qore_sqlite3_module_init(QoreModuleInitContext& ctx, ExceptionSink& xsink);
static void qore_sqlite3_module_ns_init(QoreNamespace* rns, QoreNamespace* qns, ExceptionSink& xsink);
static void qore_sqlite3_module_delete();

extern "C" DLLEXPORT void sqlite3_qore_module_desc(QoreModuleInfo& mod_info) {
    mod_info.name = "sqlite3";
    mod_info.version = PACKAGE_VERSION;
    mod_info.desc = "Sqlite3 database driver";
    mod_info.author = "Petr Vanek";
    mod_info.url = "http://www.qore.org";
    mod_info.api_major = QORE_MODULE_API_MAJOR;
    mod_info.api_minor = QORE_MODULE_API_MINOR;
    mod_info.init = qore_sqlite3_module_init;
    mod_info.ns_init = qore_sqlite3_module_ns_init;
    mod_info.del = qore_sqlite3_module_delete;
    mod_info.license = QL_MIT;
    mod_info.license_str = "MIT";
}

DBIDriver* DBID_SQLITE3 = nullptr;

using Sqlite3Handle = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;

// this is the thread key that will tell us if the current thread has been initialized for sqlite threading
static pthread_key_t ptk_sqlite3;

static inline void checkInit() {
    if (!pthread_getspecific(ptk_sqlite3)) {
        pthread_setspecific(ptk_sqlite3, (void*)1);
    }
}

static void sqlite3_thread_cleanup(void *unused) {
//     if (pthread_getspecific(ptk_sqlite3))
}

static sqlite3* qore_sqlite3_init(Datasource* ds, ExceptionSink* xsink) {
    if (!ds->getDBName()) {
        xsink->raiseException("DATASOURCE-MISSING-DBNAME", "Datasource has an empty dbname parameter");
        return nullptr;
    }

    // TODO/FIXME: better encoding handling (but sqlite is utf8 mainly)
    ds->setDBEncoding("utf8");
    ds->setQoreEncoding("utf8");

    // Check for interrupt before database open
    if (qore_check_cancel(xsink)) {
        return nullptr;
    }

    // Check filesystem sandbox access (skip for an in-memory database)
    const char* dbname = ds->getDBName();
#ifdef QORE_HAS_DBI_CHECK_CONNECT_PATH
    // the file is checked against the policy of the code that chose it (captured on the Datasource), not of the code
    // that opens the connection (qore#5747); a URI cannot be checked
    if (!strncasecmp(dbname, "file:", 5) && qore_dbi_connect_restricted(ds)) {
        xsink->raiseException("FILESYSTEM-ACCESS-DENIED", "the SQLite database '%s' is a URI, which cannot be checked "
            "against the sandbox", dbname);
        return nullptr;
    }
    if (strcmp(dbname, ":memory:")
        && qore_dbi_check_connect_path(ds, dbname, QSEC_READ | QSEC_WRITE | QSEC_CREATE, xsink)) {
        return nullptr;
    }
#else
    {
        QoreSandboxManagerHelper smh(QoreSandboxManagerHelper::Policy);
        if (smh) {
            // a URI (with URI support, which the library can have by default) names a file, options and a VFS that
            // the path check cannot see
            if (!strncasecmp(dbname, "file:", 5)) {
                xsink->raiseException("FILESYSTEM-ACCESS-DENIED", "the SQLite database '%s' is a URI, which cannot "
                    "be checked against the sandbox", dbname);
                return nullptr;
            }
            // SQLite databases need read/write access (or create for new files)
            if (strcmp(dbname, ":memory:")
                && !smh->checkFilesystemAccess(dbname, QSEC_READ | QSEC_WRITE | QSEC_CREATE, xsink)) {
                return nullptr;
            }
        }
    }
#endif

    sqlite3* db = nullptr;
    int ret = sqlite3_open(ds->getDBName(), &db);
    // SQLite can return a handle on failure; every returned handle must be closed.
    Sqlite3Handle holder(db, sqlite3_close);
    if (qore_check_cancel(xsink, "opening SQLite database")) {
        return nullptr;
    }
    if (ret != SQLITE_OK) {
        xsink->raiseException("SQLITE3-CONNECT-ERROR", "cannot open %s", ds->getDBName());
        return nullptr;
    }

    if (!db) {
        xsink->outOfMemory();
        return nullptr;
    }

    // checks the files that SQL opens (ATTACH, VACUUM INTO) against the sandbox of the code that runs it
    if (sqlite3_set_authorizer(db, qore_sqlite3_authorizer, nullptr) != SQLITE_OK) {
        xsink->raiseException("SQLITE3-CONNECT-ERROR", "cannot set the authorizer of %s: %s", ds->getDBName(),
            sqlite3_errmsg(db));
        return nullptr;
    }

    if (qore_sqlite3_register_functions(db, xsink)) {
        return nullptr;
    }
    if (qore_check_cancel(xsink, "initializing SQLite database")) {
        return nullptr;
    }

    return holder.release();
}

static int qore_sqlite3_commit(Datasource* ds, ExceptionSink* xsink) {
    checkInit();
    QoreSqlite3Connection* d =(QoreSqlite3Connection*)ds->getPrivateData();
    return d->commit(xsink) ? 0 : -1;
}

static int qore_sqlite3_rollback(Datasource* ds, ExceptionSink* xsink) {
    checkInit();
    QoreSqlite3Connection* d =(QoreSqlite3Connection*)ds->getPrivateData();
    return d->rollback(xsink) ? 0 : -1;
}

static QoreValue qore_sqlite3_select_rows(Datasource* ds, const QoreString* qstr, const QoreListNode *args,
        ExceptionSink* xsink) {
    checkInit();
    QoreSqlite3Connection* d = (QoreSqlite3Connection*)ds->getPrivateData();
    QoreSqlite3Executor exec(d->handler(), d->getEncoding(), xsink);
    return exec.select_rows(ds, qstr, args, xsink);
}

static QoreValue qore_sqlite3_select(Datasource* ds, const QoreString* qstr, const QoreListNode* args,
    ExceptionSink* xsink) {
    checkInit();
    QoreSqlite3Connection* d = (QoreSqlite3Connection*)ds->getPrivateData();
    QoreSqlite3Executor exec(d->handler(), d->getEncoding(), xsink);
    return exec.select(ds, qstr, args, xsink);
}

#ifdef QDBI_METHOD_SELECT_COLUMNAR
class QoreSqlite3StatementResetHelper {
public:
    DLLLOCAL QoreSqlite3StatementResetHelper(QoreSqlite3PreparedStatement& n_stmt, ExceptionSink* n_xsink)
            : stmt(n_stmt), xsink(n_xsink) {
    }

    DLLLOCAL ~QoreSqlite3StatementResetHelper() {
        stmt.reset(xsink);
    }

private:
    QoreSqlite3PreparedStatement& stmt;
    ExceptionSink* xsink;
};

static QoreColumnarResult* qore_sqlite3_select_columnar(Datasource* ds, const QoreString* qstr,
        const QoreListNode* args, ExceptionSink* xsink) {
    checkInit();

    QoreSqlite3PreparedStatement stmt(ds);
    QoreSqlite3StatementResetHelper reset(stmt, xsink);
    if (stmt.prepare(*qstr, args, true, xsink)) {
        return nullptr;
    }
    if (args && stmt.bind(*args, xsink)) {
        return nullptr;
    }
    if (stmt.exec(xsink)) {
        return nullptr;
    }

    return stmt.fetchColumnar(-1, xsink);
}
#endif

static QoreValue qore_sqlite3_exec(Datasource* ds, const QoreString* qstr, const QoreListNode *args,
        ExceptionSink* xsink) {
    checkInit();
    QoreSqlite3Connection* d = (QoreSqlite3Connection*)ds->getPrivateData();
    QoreSqlite3Executor exec(d->handler(), d->getEncoding(), xsink);
    return exec.exec(ds, qstr, args, xsink);
}

static QoreValue qore_sqlite3_exec_raw(Datasource* ds, const QoreString* qstr, ExceptionSink* xsink) {
    checkInit();
    QoreSqlite3Connection* d = (QoreSqlite3Connection*)ds->getPrivateData();
    QoreSqlite3Executor exec(d->handler(), d->getEncoding(), xsink);
    return exec.execRaw(ds, qstr, xsink);
}

static int qore_sqlite3_open_datasource(Datasource* ds, ExceptionSink* xsink) {
// 	printf("open datasource\n");
    checkInit();

    Sqlite3Handle db(qore_sqlite3_init(ds, xsink), sqlite3_close);
    if (!db) {
        return -1;
    }

    QoreSqlite3Connection* d_sqlite3 = new QoreSqlite3Connection(db.get(), QEM.findCreate(ds->getOSEncoding()));
    db.release();
    ds->setPrivateData(d_sqlite3);

    return 0;
}

static int qore_sqlite3_close_datasource(Datasource* ds) {
    QORE_TRACE("qore_sqlite3_close_datasource()");

    checkInit();
    QoreSqlite3Connection* d = (QoreSqlite3Connection*)ds->getPrivateData();

    if (!d->close())
        return -1;

    delete d;
    ds->setPrivateData(NULL);

    return 0;
}

static QoreValue qore_sqlite3_get_server_version(Datasource* ds, ExceptionSink* xsink) {
    checkInit();
    QoreSqlite3Connection* d = (QoreSqlite3Connection*)ds->getPrivateData();
    return new QoreStringNode(d->getServerVersion());
}

static QoreValue qore_sqlite3_get_client_version(const Datasource* ds, ExceptionSink* xsink) {
    checkInit();
    QoreSqlite3Connection* d = (QoreSqlite3Connection*)ds->getPrivateData();
    return new QoreStringNode(d->getServerVersion());
}

static int qore_sqlite3_begin_transaction(Datasource* ds, ExceptionSink* xsink) {
    checkInit();
    QoreSqlite3Connection* d =(QoreSqlite3Connection*)ds->getPrivateData();
    return d->begin(xsink) ? 0 : -1;
}

static int qore_sqlite3_stmt_prepare(SQLStatement* stmt, const QoreString& str, const QoreListNode* args,
        ExceptionSink* xsink) {
    assert(!stmt->getPrivateData());
    QoreSqlite3PreparedStatement* bg = new QoreSqlite3PreparedStatement(stmt->getDatasource());
    stmt->setPrivateData(bg);
    return bg->prepare(str, args, true, xsink);
}

static int qore_sqlite3_stmt_prepare_raw(SQLStatement* stmt, const QoreString& str, ExceptionSink* xsink) {
    assert(!stmt->getPrivateData());
    QoreSqlite3PreparedStatement* bg = new QoreSqlite3PreparedStatement(stmt->getDatasource());
    stmt->setPrivateData(bg);
    return bg->prepare(str, nullptr, true, xsink);
}

static int qore_sqlite3_stmt_bind(SQLStatement* stmt, const QoreListNode& l, ExceptionSink* xsink) {
   QoreSqlite3PreparedStatement* bg = (QoreSqlite3PreparedStatement*)stmt->getPrivateData();
   assert(bg);
   return bg->bind(l, xsink);
}

static int qore_sqlite3_stmt_bind_placeholders(SQLStatement* stmt, const QoreListNode& l, ExceptionSink* xsink) {
    xsink->raiseException("SQLITE3-BIND-PLACEHOLDERS-ERROR", "binding placeholders is not necessary or supported "
        "with the sqlite3 driver");
    return -1;
}

static int qore_sqlite3_stmt_bind_values(SQLStatement* stmt, const QoreListNode& l, ExceptionSink* xsink) {
    QoreSqlite3PreparedStatement* bg = (QoreSqlite3PreparedStatement*)stmt->getPrivateData();
    assert(bg);
    return bg->bind(l, xsink);
}

static int qore_sqlite3_stmt_exec(SQLStatement* stmt, ExceptionSink* xsink) {
    QoreSqlite3PreparedStatement* bg = (QoreSqlite3PreparedStatement*)stmt->getPrivateData();
    assert(bg);
    return bg->exec(xsink);
}

static int qore_sqlite3_stmt_define(SQLStatement* stmt, ExceptionSink* xsink) {
    // this is a noop in this driver
    return 0;
}

static int qore_sqlite3_stmt_affected_rows(SQLStatement* stmt, ExceptionSink* xsink) {
    QoreSqlite3PreparedStatement* bg = (QoreSqlite3PreparedStatement*)stmt->getPrivateData();
    assert(bg);
    return bg->rowsAffected();
}

static QoreHashNode* qore_sqlite3_stmt_get_output(SQLStatement* stmt, ExceptionSink* xsink) {
    xsink->raiseException("SQLITE3-GET-OUTPUT-ERROR", "sqlite3 does not support SQLStatement::getOutput() as it does "
        "not support stored procedures");
    return nullptr;
}

static QoreHashNode* qore_sqlite3_stmt_get_output_rows(SQLStatement* stmt, ExceptionSink* xsink) {
    xsink->raiseException("SQLITE3-GET-OUTPUT-ROWS-ERROR", "sqlite3 does not support SQLStatement::getOutput() as it "
        "does not support stored procedures");
    return nullptr;
}

static QoreHashNode* qore_sqlite3_stmt_fetch_row(SQLStatement* stmt, ExceptionSink* xsink) {
    QoreSqlite3PreparedStatement* bg = (QoreSqlite3PreparedStatement*)stmt->getPrivateData();
    assert(bg);
    return bg->fetchRow(xsink);
}

static QoreListNode* qore_sqlite3_stmt_fetch_rows(SQLStatement* stmt, int rows, ExceptionSink* xsink) {
    QoreSqlite3PreparedStatement* bg = (QoreSqlite3PreparedStatement*)stmt->getPrivateData();
    assert(bg);
    return bg->fetchRows(rows, xsink);
}

static QoreHashNode* qore_sqlite3_stmt_fetch_columns(SQLStatement* stmt, int rows, ExceptionSink* xsink) {
    QoreSqlite3PreparedStatement* bg = (QoreSqlite3PreparedStatement*)stmt->getPrivateData();
    assert(bg);
    return bg->fetchColumns(rows, xsink);
}

#ifdef QDBI_METHOD_STMT_FETCH_COLUMNAR
static QoreColumnarResult* qore_sqlite3_stmt_fetch_columnar(SQLStatement* stmt, int rows, ExceptionSink* xsink) {
    QoreSqlite3PreparedStatement* bg = (QoreSqlite3PreparedStatement*)stmt->getPrivateData();
    assert(bg);

    return bg->fetchColumnar(rows, xsink);
}
#endif

static QoreHashNode* qore_sqlite3_stmt_describe(SQLStatement* stmt, ExceptionSink* xsink) {
    QoreSqlite3PreparedStatement* bg = (QoreSqlite3PreparedStatement*)stmt->getPrivateData();
    assert(bg);
    return bg->describe(xsink);
}

static bool qore_sqlite3_stmt_next(SQLStatement* stmt, ExceptionSink* xsink) {
    QoreSqlite3PreparedStatement* bg = (QoreSqlite3PreparedStatement*)stmt->getPrivateData();
    assert(bg);
    return bg->next(xsink);
}

static int qore_sqlite3_stmt_close(SQLStatement* stmt, ExceptionSink* xsink) {
    QoreSqlite3PreparedStatement* bg = (QoreSqlite3PreparedStatement*)stmt->getPrivateData();
    assert(bg);
    bg->reset(xsink);
    delete bg;
    stmt->setPrivateData(nullptr);
    return *xsink ? -1 : 0;
}

static void qore_sqlite3_module_init(QoreModuleInitContext& ctx, ExceptionSink& xsink) {
    pthread_key_create(&ptk_sqlite3, NULL);
    tclist.push(sqlite3_thread_cleanup, NULL);

    // populate the method list structure with the method pointers
    qore_dbi_method_list methods;
    methods.add(QDBI_METHOD_OPEN,                   qore_sqlite3_open_datasource);
    methods.add(QDBI_METHOD_CLOSE,                  qore_sqlite3_close_datasource);
    methods.add(QDBI_METHOD_SELECT,                 qore_sqlite3_select);
    methods.add(QDBI_METHOD_SELECT_ROWS,            qore_sqlite3_select_rows);
#ifdef QDBI_METHOD_SELECT_COLUMNAR
    methods.add(QDBI_METHOD_SELECT_COLUMNAR,        qore_sqlite3_select_columnar);
#endif
    methods.add(QDBI_METHOD_EXEC,                   qore_sqlite3_exec);
#ifdef _QORE_HAS_DBI_EXECRAW
    methods.add(QDBI_METHOD_EXECRAW,                qore_sqlite3_exec_raw);
#endif
    methods.add(QDBI_METHOD_COMMIT,                 qore_sqlite3_commit);
    methods.add(QDBI_METHOD_ROLLBACK,               qore_sqlite3_rollback);
    methods.add(QDBI_METHOD_GET_SERVER_VERSION,     qore_sqlite3_get_server_version);
    methods.add(QDBI_METHOD_GET_CLIENT_VERSION,     qore_sqlite3_get_client_version);
    methods.add(QDBI_METHOD_BEGIN_TRANSACTION,      qore_sqlite3_begin_transaction);

    methods.add(QDBI_METHOD_STMT_PREPARE,           qore_sqlite3_stmt_prepare);
    methods.add(QDBI_METHOD_STMT_PREPARE_RAW,       qore_sqlite3_stmt_prepare_raw);
    methods.add(QDBI_METHOD_STMT_BIND,              qore_sqlite3_stmt_bind);
    methods.add(QDBI_METHOD_STMT_BIND_PLACEHOLDERS, qore_sqlite3_stmt_bind_placeholders);
    methods.add(QDBI_METHOD_STMT_BIND_VALUES,       qore_sqlite3_stmt_bind_values);
    methods.add(QDBI_METHOD_STMT_EXEC,              qore_sqlite3_stmt_exec);
    methods.add(QDBI_METHOD_STMT_DEFINE,            qore_sqlite3_stmt_define);
    methods.add(QDBI_METHOD_STMT_FETCH_ROW,         qore_sqlite3_stmt_fetch_row);
    methods.add(QDBI_METHOD_STMT_FETCH_ROWS,        qore_sqlite3_stmt_fetch_rows);
    methods.add(QDBI_METHOD_STMT_FETCH_COLUMNS,     qore_sqlite3_stmt_fetch_columns);
#ifdef QDBI_METHOD_STMT_FETCH_COLUMNAR
    methods.add(QDBI_METHOD_STMT_FETCH_COLUMNAR,    qore_sqlite3_stmt_fetch_columnar);
#endif
    methods.add(QDBI_METHOD_STMT_DESCRIBE,          qore_sqlite3_stmt_describe);
    methods.add(QDBI_METHOD_STMT_NEXT,              qore_sqlite3_stmt_next);
    methods.add(QDBI_METHOD_STMT_CLOSE,             qore_sqlite3_stmt_close);
    methods.add(QDBI_METHOD_STMT_AFFECTED_ROWS,     qore_sqlite3_stmt_affected_rows);
    methods.add(QDBI_METHOD_STMT_GET_OUTPUT,        qore_sqlite3_stmt_get_output);
    methods.add(QDBI_METHOD_STMT_GET_OUTPUT_ROWS,   qore_sqlite3_stmt_get_output_rows);

    // register database functions with DBI subsystem
    DBID_SQLITE3 = DBI.registerDriver("sqlite3", methods,
        DBI_CAP_LOB_SUPPORT
        | DBI_CAP_TRANSACTION_MANAGEMENT
        | DBI_CAP_BIND_BY_VALUE
        | DBI_CAP_HAS_EXECRAW
        | DBI_CAP_CHARSET_SUPPORT
        | DBI_CAP_HAS_NUMBER_SUPPORT
        | DBI_CAP_HAS_STATEMENT
#ifdef DBI_CAP_LOCAL_ONLY
        // a database is a local file, which qore_sqlite3_init() checks against the filesystem policy; the sandbox
        // does not ask the driver for network destinations (qore#5747)
        | DBI_CAP_LOCAL_ONLY
#endif
    );

}

static void qore_sqlite3_module_ns_init(QoreNamespace* rns, QoreNamespace* qns, ExceptionSink& xsink) {
    QORE_TRACE("qore_sqlite3_module_ns_init()");
    // nothing to do at the moment
}

static void qore_sqlite3_module_delete() {
    QORE_TRACE("qore_sqlite3_module_delete()");

    // cleanup any thread data
    tclist.pop(1);
    // delete thread key
    pthread_key_delete(ptk_sqlite3);
}
