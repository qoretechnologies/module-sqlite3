/*
  sqlite3connection.h

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

#ifndef SQLITE3CONNECTION_H
#define SQLITE3CONNECTION_H

#include <sqlite3.h>
#include <qore/Qore.h>

/*! \brief A Qore ready wrapper for Sqlite3 API.
    There is only one instance of this class in this module.
    All select/exec depending stuff is located in QoreSqlite3Executor,
    the class with created instances on demand for each statement.

    \author Petr Vanek <petr.vanek@qoretechnologies.com>
*/
class QoreSqlite3Connection
{
public:
    /*! \brief Cosntruct a Sqlite3 driver for Qore module.

        \param sqlite3 * a reference to already inicialized sqlite3 handler.
    */
    DLLLOCAL QoreSqlite3Connection(sqlite3* handler, const QoreEncoding* enc);

    DLLLOCAL ~QoreSqlite3Connection() {};

    /*! \brief Public access to the DB conection handler.

        \retval sqlite3* sqlite3 handler.
    */
    DLLLOCAL sqlite3* handler() { return m_handler; };

    /*! \brief Start a transaction.
        Error checking is left for sqlite3 engine.

        \retval bool true for success; false for any error.
    */
    DLLLOCAL bool begin(ExceptionSink* xsink);

    /*! \brief Preforms a COMMIT statement without any transaction checking.
        Error checking is left for sqlite3 engine.

        \retval bool true for success; false for any error.
    */
    DLLLOCAL bool commit(ExceptionSink* xsink);

    /*! \brief Preforms a ROLLBACK statement without any transaction checking.
        Error checking is left for sqlite3 engine.

        \retval bool true for success; false for any error.
    */
    DLLLOCAL bool rollback(ExceptionSink* xsink);

    /*! \brief Close active sqlite3 handler.

        \retval bool true on success; false on error.
    */
    DLLLOCAL bool close();

    /*! \brief Get sqlite3 backend version.

        Sqlite DB engine is "serverless" so there is only this method
        as we don't need any "client version". Qore DB API is using this
        method for both get*Version() methods.

        \retval char* A string with Sqlite3 library in use (format: X.Y.Z)
    */
    DLLLOCAL char* getServerVersion();

    //! Returns the Qore encoding to use for string data
    DLLLOCAL const QoreEncoding* getEncoding() {
        return enc;
    }

private:
    //! The current sqlite3 connection.
    sqlite3* m_handler;

    //! The character encoding touse for string data
    const QoreEncoding* enc;
};

#endif
