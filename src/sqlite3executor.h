/*
  sqlite3executor.h

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

#ifndef SQLITE3EXECUTOR_H
#define SQLITE3EXECUTOR_H

#include <sqlite3.h>
#include <qore/Qore.h>
#include <qore/QoreSandboxManager.h>

#include "sqlite3connection.h"

//! Base SQL operation class
class QoreSqlite3ExecBase {
public:
    DLLLOCAL QoreSqlite3ExecBase(const QoreEncoding* enc, QoreListNode* m_realArgs = nullptr)
            : enc(enc), m_realArgs(m_realArgs, nullptr) {
    }

    /*! \brief Prepare a string (SQL statement) for variables binding.
        It performs some Qore DB bind syntax validity checks too.

        \param str a mutable Qore string with SQL statement
        \param args a list with bindable parameters
        \param xsink exceptions handler

        \retval bool 0 on success, -1 on error
    */
    DLLLOCAL int parseForBind(QoreString& str, const QoreListNode* args, ExceptionSink* xsink);

    /*! \brief Bind arguments to the SQL statement.

        \param stmt a prepared sqlite3 statement.
        \param xsink exception handler

        \retval bool 0 on success, -1 on error
    */
    DLLLOCAL int bindParameters(sqlite3_stmt* stmt, ExceptionSink* xsink);

    /*! \brief Universal Sqlite3 to Qore nodes conversion.
        \param stmt a reference for sqlite3 SQL statement. It has to be
                    prepared and fetched already.
        \param index index of column to retrieve.
        \retval Qore node type by its sqlite afinity.
                    Possible types: int, float, binary, string, and NULL
    */
    DLLLOCAL static QoreValue columnValue(sqlite3_stmt* stmt, int index);

protected:
    //! Encoding to use
    const QoreEncoding* enc;

    //! Really used parameters list for statement (%s etc. filtered out)
    ReferenceHolder<QoreListNode> m_realArgs;
};

/*! \brief Data operations class.
    It will be created for every request from sqlite3module functions
    to keep statement related data clean and separated from QoreSqlite3Connection.
    \author Petr Vanek <petr.vanek@qoretechnologies.com>
*/
class QoreSqlite3Executor : public QoreSqlite3ExecBase {
public:
    DLLLOCAL QoreSqlite3Executor(sqlite3* handler, const QoreEncoding* enc, ExceptionSink* xsink);

    DLLLOCAL ~QoreSqlite3Executor();

    /*! \brief Implementation for Qore DB API exec().
        It's primarily used for DDL/INSERT/UPDATE/DELETE statemets, but
        it can handle all stuff as it's calling select() method.
        \param ds a Datasource reference from Qore API.
        \param qstr a SQL statement from Qore API.
        \param args a list with bindable parameters from Qore API.
        \param xsink exception handler.
        \retval hash for SELECTs, count of the affected
                rows for CRUD stuff.
    */
    DLLLOCAL QoreValue exec(
        Datasource *ds,
        const QoreString *qstr,
        const QoreListNode *args,
        ExceptionSink* xsink);

    /*! \brief Implementation for Qore DB API execRaw().
        It's primarily used for DDL statemets, but
        it can handle all stuff. But there is no variable binding.
        \param ds a Datasource reference from Qore API.
        \param qstr a SQL statement from Qore API.
        \param xsink exception handler.
        \retval hash for SELECTs, count of the affected
                rows for CRUD stuff.
    */
    DLLLOCAL QoreValue execRaw(
        Datasource *ds,
        const QoreString *qstr,
        ExceptionSink* xsink);

    /*! \brief Implementation for Qore DB API select().
        \param ds a Datasource reference from Qore API.
        \param qstr a SQL statement from Qore API.
        \param args a list with bindable parameters from Qore API.
        \param xsink exception handler.
        \retval QoreHashNode with coluns as keys, data as lists.
    */
    DLLLOCAL QoreValue select(
        Datasource *ds,
        const QoreString *qstr,
        const QoreListNode *args,
        ExceptionSink* xsink);

    /*! \brief Implementation for Qore DB API selectRows().
        \param ds a Datasource reference from Qore API.
        \param qstr a SQL statement from Qore API.
        \param args a list with bindable parameters from Qore API.
        \param xsink exception handler.
        \retval QoreListNode with the list of hashes.
    */
    DLLLOCAL QoreListNode * select_rows(
        Datasource *ds,
        const QoreString *qstr,
        const QoreListNode *args,
        ExceptionSink* xsink);

protected:
    //! Current sqlite3 connection.
    sqlite3* m_handler;

    /*! \brief Internal implementation of select() DB API.
        \param ds a Datasource reference from Qore API.
        \param qstr a SQL statement from Qore API.
        \param args a list with bindable parameters from Qore API.
        \param binding flag if it should allow variable binding (true) or not (false)
        \param xsink exception handler.
        \retval QoreHashNode with columns as keys, data as lists.
    */
    DLLLOCAL QoreHashNode * select_internal(
        Datasource *ds,
        const QoreString *qstr,
        const QoreListNode *args,
        bool binding,
        const char * calltype,
        ExceptionSink* xsink);
};

class QoreSqlite3PreparedStatement : public QoreSqlite3ExecBase {
public:
    DLLLOCAL QoreSqlite3PreparedStatement(Datasource* ds)
        : QoreSqlite3ExecBase(QEM.findCreate(ds->getOSEncoding())), conn((QoreSqlite3Connection*)ds->getPrivateData()) {
    }

    DLLLOCAL ~QoreSqlite3PreparedStatement() {
        assert(!sql);
    }

    // returns 0 for OK, -1 for error
    DLLLOCAL int prepare(const QoreString& sql, const QoreListNode* args, bool n_parse, ExceptionSink* xsink);
    DLLLOCAL int bind(const QoreListNode& l, ExceptionSink* xsink);
    DLLLOCAL int exec(ExceptionSink* xsink);
    DLLLOCAL QoreHashNode* fetchRow(ExceptionSink* xsink);
    DLLLOCAL QoreListNode* fetchRows(int rows, ExceptionSink* xsink);
    DLLLOCAL QoreHashNode* fetchColumns(int rows, ExceptionSink* xsink);
    DLLLOCAL QoreHashNode* describe(ExceptionSink* xsink);
    DLLLOCAL bool next(ExceptionSink* xsink = nullptr);

    DLLLOCAL QoreHashNode* getOutputHash(ExceptionSink* xsink, int maxrows = -1);
    DLLLOCAL QoreListNode* getOutputList(ExceptionSink* xsink, int maxrows = -1);

    DLLLOCAL int rowsAffected();

    DLLLOCAL void reset(ExceptionSink* xsink);

protected:
    QoreSqlite3Connection* conn = nullptr;
    QoreString* sql = nullptr;
    sqlite3_stmt* stmt = nullptr;

    // SQL active flag
    bool sql_active = false;

    // row count
    int row_count = -1;

    DLLLOCAL int prepareIntern(const QoreListNode* args, ExceptionSink* xsink);
};

#endif
