/*
    sqlite3executor.cc

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

#include "sqlite3executor.h"

#if defined(QDBI_METHOD_SELECT_COLUMNAR) || defined(QDBI_METHOD_STMT_FETCH_COLUMNAR)
#include <qore/QoreBufferNode.h>
#include <qore/QoreColumnarResult.h>

#include <memory>
#include <string>
#include <vector>
#endif

#if defined(QDBI_METHOD_SELECT_COLUMNAR) || defined(QDBI_METHOD_STMT_FETCH_COLUMNAR)
namespace {
struct SqliteColumnarStorage {
    std::vector<int64> int_values;
    std::vector<double> float_values;
    std::vector<uint8_t> validity;
};

static size_t sqlite_columnar_bitmap_size(size_t size) {
    return (size + 7) / 8;
}

static void sqlite_columnar_set_validity_bit(std::vector<uint8_t>& validity, size_t index, bool valid) {
    size_t byte = index / 8;
    if (byte >= validity.size()) {
        validity.resize(byte + 1, 0);
    }

    uint8_t mask = uint8_t(1) << (index % 8);
    if (valid) {
        validity[byte] |= mask;
    } else {
        validity[byte] &= ~mask;
    }
}

static bool sqlite_columnar_is_valid(const std::vector<uint8_t>& validity, size_t index) {
    if (validity.empty()) {
        return true;
    }
    size_t byte = index / 8;
    return byte < validity.size() && (validity[byte] & (uint8_t(1) << (index % 8)));
}

enum class SqliteColumnarKind {
    Empty,
    Int64,
    Float64,
    List,
};

class SqliteColumnarBuilder {
public:
    SqliteColumnarBuilder(const char* n_name, ExceptionSink* xsink) : name(n_name), list(xsink) {
    }

    const char* getName() const {
        return name.c_str();
    }

    int append(sqlite3_stmt* stmt, int column_index, ExceptionSink* xsink) {
        int type = sqlite3_column_type(stmt, column_index);
        if (kind == SqliteColumnarKind::List) {
            return appendList(stmt, column_index, xsink);
        }

        if (type == SQLITE_NULL) {
            appendNull();
            return 0;
        }

        switch (type) {
            case SQLITE_INTEGER:
                if (kind == SqliteColumnarKind::Empty) {
                    initDense(SqliteColumnarKind::Int64);
                } else if (kind != SqliteColumnarKind::Int64) {
                    if (fallbackToList(xsink)) {
                        return -1;
                    }
                    return appendList(stmt, column_index, xsink);
                }
                storage->int_values.push_back(sqlite3_column_int64(stmt, column_index));
                appendValid();
                return 0;

            case SQLITE_FLOAT:
                if (kind == SqliteColumnarKind::Empty) {
                    initDense(SqliteColumnarKind::Float64);
                } else if (kind != SqliteColumnarKind::Float64) {
                    if (fallbackToList(xsink)) {
                        return -1;
                    }
                    return appendList(stmt, column_index, xsink);
                }
                storage->float_values.push_back(sqlite3_column_double(stmt, column_index));
                appendValid();
                return 0;

            default:
                if (fallbackToList(xsink)) {
                    return -1;
                }
                return appendList(stmt, column_index, xsink);
        }
    }

    QoreValue finish(ExceptionSink* xsink) {
        if (kind == SqliteColumnarKind::List) {
            return list.release();
        }
        if (kind == SqliteColumnarKind::Empty) {
            ReferenceHolder<QoreListNode> rv(new QoreListNode(autoTypeInfo), xsink);
            for (size_t i = 0; i < row_count; ++i) {
                if (i && !(i % 100) && qore_check_cancel(xsink)) {
                    return QoreValue();
                }
                rv->push(null(), xsink);
                if (*xsink) {
                    return QoreValue();
                }
            }
            return rv.release();
        }

        assert(storage);
        QoreBufferElementType element_type = kind == SqliteColumnarKind::Float64
            ? QoreBufferElementType::Float64
            : QoreBufferElementType::Int64;
        const void* data = element_type == QoreBufferElementType::Float64
            ? static_cast<const void*>(storage->float_values.empty() ? nullptr : storage->float_values.data())
            : static_cast<const void*>(storage->int_values.empty() ? nullptr : storage->int_values.data());
        bool nullable = null_count > 0;
        const uint8_t* validity = nullable && !storage->validity.empty() ? storage->validity.data() : nullptr;
        return QoreBufferNode::wrapExternalStorage(element_type, nullable, row_count, data, validity, storage,
            null_count, xsink);
    }

private:
    void initDense(SqliteColumnarKind n_kind) {
        assert(kind == SqliteColumnarKind::Empty);
        kind = n_kind;
        storage.reset(new SqliteColumnarStorage);
        if (!row_count) {
            return;
        }

        storage->validity.resize(sqlite_columnar_bitmap_size(row_count), 0);
        if (kind == SqliteColumnarKind::Float64) {
            storage->float_values.resize(row_count, 0.0);
        } else {
            storage->int_values.resize(row_count, 0);
        }
    }

    void ensureValidity() {
        assert(storage);
        if (!storage->validity.empty()) {
            storage->validity.resize(sqlite_columnar_bitmap_size(row_count + 1), 0);
            return;
        }

        storage->validity.resize(sqlite_columnar_bitmap_size(row_count + 1), 0xff);
    }

    void appendNull() {
        if (kind == SqliteColumnarKind::Empty) {
            ++null_count;
            ++row_count;
            return;
        }

        ensureValidity();
        sqlite_columnar_set_validity_bit(storage->validity, row_count, false);
        if (kind == SqliteColumnarKind::Float64) {
            storage->float_values.push_back(0.0);
        } else {
            storage->int_values.push_back(0);
        }
        ++null_count;
        ++row_count;
    }

    void appendValid() {
        if (storage && !storage->validity.empty()) {
            sqlite_columnar_set_validity_bit(storage->validity, row_count, true);
        }
        ++row_count;
    }

    int fallbackToList(ExceptionSink* xsink) {
        if (kind == SqliteColumnarKind::List) {
            return 0;
        }

        list = new QoreListNode(autoTypeInfo);
        for (size_t i = 0; i < row_count; ++i) {
            if (i && !(i % 100) && qore_check_cancel(xsink)) {
                return -1;
            }

            if (kind == SqliteColumnarKind::Empty || !sqlite_columnar_is_valid(storage->validity, i)) {
                list->push(null(), xsink);
            } else if (kind == SqliteColumnarKind::Float64) {
                list->push(storage->float_values[i], xsink);
            } else {
                list->push(storage->int_values[i], xsink);
            }
            if (*xsink) {
                return -1;
            }
        }

        kind = SqliteColumnarKind::List;
        storage.reset();
        null_count = 0;
        return 0;
    }

    int appendList(sqlite3_stmt* stmt, int column_index, ExceptionSink* xsink) {
        ValueHolder value(QoreSqlite3ExecBase::columnValue(stmt, column_index), xsink);
        if (*xsink) {
            return -1;
        }

        list->push(value.release(), xsink);
        if (*xsink) {
            return -1;
        }
        ++row_count;
        return 0;
    }

    std::string name;
    SqliteColumnarKind kind = SqliteColumnarKind::Empty;
    std::shared_ptr<SqliteColumnarStorage> storage;
    ReferenceHolder<QoreListNode> list;
    size_t row_count = 0;
    int64_t null_count = 0;
};
}
#endif

int QoreSqlite3ExecBase::parseForBind(QoreString& str, const QoreListNode* args, ExceptionSink* xsink) {
    char quote = 0;
    const char *p = str.c_str();
    QoreString tmp;
    int index = 0;
    int nParams = 0;

    while (*p) {
        // Track quoted strings to avoid processing % inside them
        if ((*p) == '\'' || (*p) == '\"') {
            if (!quote) {
                // Starting a quoted string
                quote = *p;
            } else if (quote == *p) {
                // Inside a quoted string and saw the same quote character
                // Handle SQL-style escaped quotes by doubling (e.g., 'don''t')
                if (p[1] == quote) {
                    // Escaped quote: skip both and stay inside the quoted string
                    p += 2;
                    continue;
                }
                // Closing quote: leave quoted string
                quote = '\0';
            }
            ++p;
            continue;
        }

        // Skip non-% characters and % inside quotes
        if (quote || (*p) != '%') {
            ++p;
            continue;
        }

        // found value marker outside quotes
        int offset = p - str.c_str();

        p++;
        const QoreValue v = args ? args->retrieveEntry(index++) : QoreValue();

        if ((*p) == 'd') {
            DBI_concat_numeric(&tmp, v);
            str.replace(offset, 2, tmp.c_str());
            p = str.c_str() + offset + tmp.strlen();
            tmp.clear();
            continue;
        }
        if ((*p) == 's') {
            if (DBI_concat_string(&tmp, v, xsink))
                return -1;
            str.replace(offset, 2, tmp.c_str());
            p = str.c_str() + offset + tmp.strlen();
            tmp.clear();
            continue;
        }
        if ((*p) != 'v') {
            xsink->raiseException("SQLITE3-PARSE-EXCEPTION",
                                  "invalid value specification (expecting '%%v', '%%d', or '%%s', got %%%c)", *p);
            return -1;
        }
        ++p;
        if (isalpha(*p)) {
            xsink->raiseException("SQLITE3-PARSE-EXCEPTION",
                                  "invalid value specification (expecting '%%v', '%%d', or '%%s', got %%v%c*)", *p);
            return -1;
        }

        // replace value marker with "?<num>" - sqlite3 binding
        // find byte offset in case string buffer is reallocated with replace()
        ++nParams;
        tmp.sprintf("?%d", nParams);
        str.replace(offset, 2, tmp.c_str());
        p = str.c_str() + offset + tmp.strlen();
        tmp.clear();

        if (!m_realArgs) {
            m_realArgs = new QoreListNode(autoTypeInfo);
        }
        m_realArgs->push(v.refSelf(), xsink);
    }

    return 0;
}

int QoreSqlite3ExecBase::bindParameters(sqlite3_stmt* stmt, ExceptionSink* xsink) {
    qore_type_t argType;
    QoreValue arg;

    for (int i = 0; i < sqlite3_bind_parameter_count(stmt); ++i)     {
        arg = m_realArgs->retrieveEntry(i);
        argType = arg.getType();

        switch (argType) {
            case NT_NOTHING:
            case NT_NULL:
                if (SQLITE_OK != sqlite3_bind_null(stmt, i+1)) {
                    xsink->raiseException("SQLITE3-BIND-EXCEPTION", "Failed to bind NULL");
                    return -1;
                }
                break;
            case NT_INT:
                if (SQLITE_OK != sqlite3_bind_int64(stmt, i+1, arg.getAsBigInt())) {
                    xsink->raiseException("SQLITE3-BIND-EXCEPTION", "Failed to bind integer");
                    return -1;
                }
                break;
            case NT_FLOAT:
                if (SQLITE_OK != sqlite3_bind_double(stmt, i+1, arg.getAsFloat())) {
                    xsink->raiseException("SQLITE3-BIND-EXCEPTION", "Failed to bind double/float");
                    return -1;
                }
                break;
            case NT_STRING: {
                QoreStringValueHelper s(arg);
                if (SQLITE_OK != sqlite3_bind_text(stmt, i+1, s->c_str(), s->strlen(), SQLITE_TRANSIENT)) {
                    xsink->raiseException("SQLITE3-BIND-EXCEPTION", "Failed to bind string");
                    return -1;
                }
                break;
            }
            case NT_BOOLEAN:
                if (SQLITE_OK != sqlite3_bind_int64(stmt, i+1, arg.getAsBool())) {
                    xsink->raiseException("SQLITE3-BIND-EXCEPTION", "Failed to bind bool");
                    return -1;
                }
                break;
            case NT_DATE: {
                const DateTimeNode* d = arg.get<const DateTimeNode>();
                QoreString str;
                d->format(str, "IF");
                if (SQLITE_OK != sqlite3_bind_text(stmt, i+1, str.c_str(), str.strlen(), SQLITE_TRANSIENT)) {
                    xsink->raiseException("SQLITE3-BIND-EXCEPTION", "Failed to bind date '%s' as string",
                        str.c_str());
                    return -1;
                }
                break;
            }
            case NT_NUMBER: {
                const QoreNumberNode* n = arg.get<const QoreNumberNode>();
                QoreString str;
                n->toString(str);
                if (SQLITE_OK != sqlite3_bind_text(stmt, i+1, str.c_str(), str.strlen(), SQLITE_TRANSIENT)) {
                    xsink->raiseException("SQLITE3-BIND-EXCEPTION", "Failed to bind number '%s' as string",
                        str.c_str());
                    return -1;
                }
                break;
            }
            case NT_BINARY: {
                const BinaryNode* b = arg.get<const BinaryNode>();
                if (SQLITE_OK != sqlite3_bind_blob(stmt, i+1, b->getPtr(), b->size(), SQLITE_TRANSIENT)) {
                    xsink->raiseException("SQLITE3-BIND-EXCEPTION", "Failed to bind BLOB");
                    return -1;
                }
                break;
            }
            default:
                xsink->raiseException("SQLITE3-BIND-EXCEPTION", "Cannot bind unsupported type '%s'",
                    arg.getTypeName());
                return -1;
        } // switch
    } // for
    return 0;
}

QoreValue QoreSqlite3ExecBase::columnValue(sqlite3_stmt * stmt, int index) {
    int columnType = sqlite3_column_type(stmt, index);

    switch (columnType) {
        case SQLITE_INTEGER:
            return sqlite3_column_int64(stmt, index);

        case SQLITE_FLOAT:
            return sqlite3_column_double(stmt, index);

        case SQLITE_BLOB: {
            int nBlob = sqlite3_column_bytes(stmt, index);
            unsigned char *zBlob = (unsigned char *)malloc(nBlob);
            memcpy(zBlob, sqlite3_column_blob(stmt, index), nBlob);
            return new BinaryNode(zBlob, nBlob);
        }

        case SQLITE_NULL:
            return null();

        case SQLITE_TEXT:
        default:
            break;
    };

    return new QoreStringNode((const char*)sqlite3_column_text(stmt, index));
}

QoreSqlite3Executor::QoreSqlite3Executor(sqlite3* handler, const QoreEncoding* enc, ExceptionSink* xsink)
        : QoreSqlite3ExecBase(enc, new QoreListNode(autoTypeInfo)), m_handler(handler) {
}

QoreSqlite3Executor::~QoreSqlite3Executor() {
}

QoreValue QoreSqlite3Executor::exec(
        Datasource *ds,
        const QoreString *qstr,
        const QoreListNode *args,
        ExceptionSink* xsink) {
    ReferenceHolder<QoreHashNode> hash(reinterpret_cast<QoreHashNode*>(select_internal(ds, qstr, args, true,
        "SQLITE3-EXEC", xsink)), xsink);
    if (*xsink) {
        return QoreValue();
    }

    if (hash->size() > 0) {
        return hash.release();
    }

    return sqlite3_changes(m_handler);
}

QoreValue QoreSqlite3Executor::execRaw(
    Datasource *ds,
    const QoreString *qstr,
    ExceptionSink* xsink) {
    ReferenceHolder<QoreHashNode> hash(reinterpret_cast<QoreHashNode*>(select_internal(ds, qstr, 0, false,
        "SQLITE3-EXECRAW", xsink)), xsink);
    if (*xsink) {
        return QoreValue();
    }

    if (hash->size() > 0) {
        return hash.release();
    }

    return sqlite3_changes(m_handler);
}

QoreValue QoreSqlite3Executor::select(
    Datasource *ds,
    const QoreString *qstr,
    const QoreListNode *args,
    ExceptionSink* xsink) {
    ReferenceHolder<QoreHashNode> hash(reinterpret_cast<QoreHashNode*>(select_internal(ds, qstr, args, true,
            "SQLITE3-SELECT", xsink)), xsink);
    if (*xsink) {
        return QoreValue();
    }

    if (hash->size() > 0) {
        return hash.release();
    }

    return sqlite3_changes(m_handler);
}

QoreListNode* QoreSqlite3Executor::select_rows(
    Datasource *ds,
    const QoreString *qstr,
    const QoreListNode *args,
    ExceptionSink* xsink) {
    TempEncodingHelper qstr0(qstr, enc, xsink);
    if (*xsink) {
        return nullptr;
    }
    size_t len = qstr0->strlen();
    QoreString statement(qstr0.giveBuffer(), len, len + 1, enc);
    if (parseForBind(statement, args, xsink)) {
        xsink->raiseException("SQLITE3-SELECT-ROWS", "failed to parse bind variables");
        return nullptr;
    }

    // Check for interrupt before statement preparation
    if (qore_check_cancel(xsink)) {
        return nullptr;
    }

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(m_handler, statement.c_str(), -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        xsink->raiseException("SQLITE3-SELECT-ROWS", "sqlite3 error: %s", sqlite3_errmsg(m_handler));
        return nullptr;
    }
    ON_BLOCK_EXIT(sqlite3_finalize, stmt);

    if (bindParameters(stmt, xsink)) {
        xsink->raiseException("SQLITE3-SELECT-ROWS", "failed to bind variables");
        return nullptr;
    }

    ReferenceHolder<QoreListNode> res(new QoreListNode(autoTypeInfo), xsink);
    int row_count = 0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        // Check for interrupt periodically during fetch (every 100 rows)
        if ((row_count % 100) == 0 && qore_check_cancel(xsink)) {
            return nullptr;
        }
        ++row_count;

        QoreHashNode* head = new QoreHashNode(autoTypeInfo);

        for (int i = 0; i < sqlite3_column_count(stmt); ++i) {
            head->setKeyValue(sqlite3_column_name(stmt, i), columnValue(stmt, i), xsink);
        }
        res->push(head, xsink);
    }

    if (rc != SQLITE_DONE) {
        xsink->raiseException("SQLITE3-SELECT-ROWS", "sqlite3 error: %s", sqlite3_errmsg(m_handler));
        return nullptr;
    }

    return res.release();
}

QoreHashNode* QoreSqlite3Executor::select_internal(
            Datasource *ds,
            const QoreString *qstr,
            const QoreListNode *args,
            bool binding,
            const char * calltype,
            ExceptionSink* xsink) {
    TempEncodingHelper qstr0(qstr, enc, xsink);
    if (*xsink) {
        return nullptr;
    }
    size_t len = qstr0->strlen();
    QoreString statement(qstr0.giveBuffer(), len, len + 1, enc);
    if (binding) {
        if (parseForBind(statement, args, xsink)) {
            xsink->raiseException(calltype, "failed to parse bind variables");
            return nullptr;
        }
    }

    // Check for interrupt before statement preparation
    if (qore_check_cancel(xsink)) {
        return nullptr;
    }

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(m_handler, statement.c_str(), -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        xsink->raiseException(calltype, "sqlite3 error: %s", sqlite3_errmsg(m_handler));
        return nullptr;
    }
    ON_BLOCK_EXIT(sqlite3_finalize, stmt);

    if (binding && bindParameters(stmt, xsink)) {
        xsink->raiseException(calltype, "failed to bind variables");
        return nullptr;
    }

    // columns as keys
    ReferenceHolder<QoreHashNode> hash(new QoreHashNode(autoTypeInfo), xsink);

    for (int i = 0; i < sqlite3_column_count(stmt); ++i) {
        hash->setKeyValue(sqlite3_column_name(stmt, i), new QoreListNode(autoTypeInfo), xsink);
    }

    // fetch the results
    int row_count = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        // Check for interrupt periodically during fetch (every 100 rows)
        if ((row_count % 100) == 0 && qore_check_cancel(xsink)) {
            return nullptr;
        }
        ++row_count;

        for (int i = 0; i < sqlite3_column_count(stmt); ++i) {
            QoreListNode* node = hash->getKeyValue(sqlite3_column_name(stmt, i)).get<QoreListNode>();
            node->push(columnValue(stmt, i), xsink);
        }
    }

    if (rc != SQLITE_DONE) {
        xsink->raiseException(calltype, "sqlite3 error: %s", sqlite3_errmsg(m_handler));
        return nullptr;
    }

    return hash.release();
}

int QoreSqlite3PreparedStatement::prepare(const QoreString& sql, const QoreListNode* args, bool parse,
        ExceptionSink* xsink) {
    assert(!this->sql);
    // create copy of string and convert encoding if necessary
    this->sql = sql.convertEncoding(enc, xsink);
    if (*xsink) {
        return -1;
    }

    assert(!m_realArgs);
    if (args) {
        assert(parse);
    }

    if (parse && parseForBind(*this->sql, args, xsink)) {
        xsink->raiseException("SQLITE3-PREPARE-ERROR", "failed to parse bind variables");
        return -1;
    }

    // Check for interrupt before statement preparation
    if (qore_check_cancel(xsink)) {
        return -1;
    }

    assert(!stmt);
    int rc = sqlite3_prepare_v2(conn->handler(), this->sql->c_str(), -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        xsink->raiseException("SQLITE3-PREPARE-ERROR", "%s", sqlite3_errmsg(conn->handler()));
        return -1;
    }

    if (sqlite3_bind_parameter_count(stmt) && bindParameters(stmt, xsink)) {
        xsink->raiseException("SQLITE3-PREPARE-ERROR", "failed to bind variables");
        return -1;
    }

    return 0;
}

int QoreSqlite3PreparedStatement::bind(const QoreListNode& l, ExceptionSink* xsink) {
    assert(stmt);

    int rc = sqlite3_reset(stmt);
    if (rc != SQLITE_OK) {
        xsink->raiseException("SQLITE3-STATEMENT-BIND-ERROR", "failed to reset statement before binding: %s",
            sqlite3_errmsg(conn->handler()));
        return -1;
    }

    // parseForBind() records the arguments supplied to prepare(); replace them with the values supplied to bind().
    // Without this assignment, prepare(sql) followed by bind(values) keeps the NOTHING placeholders captured during
    // prepare and silently binds NULL instead of the caller's values.
    m_realArgs = l.copy();
    if (sqlite3_bind_parameter_count(stmt) && bindParameters(stmt, xsink)) {
        xsink->raiseException("SQLITE3-STATEMENT-BIND-ERROR", "failed to bind variables");
        return -1;
    }
    return 0;
}

int QoreSqlite3PreparedStatement::exec(ExceptionSink* xsink) {
    assert(sql);
    assert(stmt);
    assert(!sql_active);
    if (!conn->begin(xsink)) {
        return -1;
    }
    int rc = sqlite3_reset(stmt);
    if (rc != SQLITE_OK) {
        xsink->raiseException("SQLITE3-STATEMENT-EXEC-ERROR", "failed to reset statement before execution: %s",
            sqlite3_errmsg(conn->handler()));
        return -1;
    }
    row_count = -1;

    // DML and DDL statements without results must be stepped by exec(); result-producing statements are stepped lazily
    // by next() and the fetch APIs. When count_changes is enabled, ordinary DML exposes one synthetic result column;
    // consume that result here while preserving actual RETURNING clauses and result-producing pragmas.
    int column_count = sqlite3_column_count(stmt);
    bool count_changes_result = false;
    if (!sqlite3_stmt_readonly(stmt) && column_count == 1) {
        const char* name = sqlite3_column_name(stmt, 0);
        count_changes_result = name && (!strcmp(name, "rows inserted") || !strcmp(name, "rows updated")
            || !strcmp(name, "rows deleted"));
    }
    if (!column_count || count_changes_result) {
        do {
            if (qore_check_cancel(xsink, "executing SQLite statement")) {
                return -1;
            }
            rc = sqlite3_step(stmt);
        } while (rc == SQLITE_ROW);
        if (rc != SQLITE_DONE) {
            xsink->raiseException("SQLITE3-STATEMENT-EXEC-ERROR", "sqlite3 error: %s",
                sqlite3_errmsg(conn->handler()));
            return -1;
        }
        return 0;
    }
    sql_active = true;
    return 0;
}

bool QoreSqlite3PreparedStatement::next(ExceptionSink* xsink) {
    if (!sql_active) {
        return false;
    }
    assert(sql_active);

    // Check for interrupt before fetching next row
    if (xsink && qore_check_cancel(xsink)) {
        sql_active = false;
        return false;
    }

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sql_active = false;
        return false;
    }
    if (row_count == -1) {
        row_count = 1;
    } else {
        ++row_count;
    }
    return true;
}

QoreHashNode* QoreSqlite3PreparedStatement::getOutputHash(ExceptionSink* xsink, int maxrows) {
    assert(sql);
    assert(stmt);
    assert(sql_active);
    assert(row_count != -1);

    int end = maxrows > 0 ? row_count + maxrows : -1;

    ReferenceHolder<QoreHashNode> rv(new QoreHashNode(autoTypeInfo), xsink);
    if (maxrows < 0) {
        for (int i = 0; i < sqlite3_column_count(stmt); ++i) {
            if (i && !(i % 100) && qore_check_cancel(xsink, "initializing SQLite columnar result")) {
                return nullptr;
            }
            rv->setKeyValue(sqlite3_column_name(stmt, i), new QoreListNode(autoTypeInfo), xsink);
            if (*xsink) {
                return nullptr;
            }
        }
    }

    // fetch the results
    while (next(xsink)) {
        if (*xsink) {
            return nullptr;
        }

        if (rv->empty()) {
            for (int i = 0; i < sqlite3_column_count(stmt); ++i) {
                rv->setKeyValue(sqlite3_column_name(stmt, i), new QoreListNode(autoTypeInfo), xsink);
                if (*xsink) {
                    return nullptr;
                }
            }
        }
        for (int i = 0; i < sqlite3_column_count(stmt); ++i) {
            QoreListNode* node = rv->getKeyValue(sqlite3_column_name(stmt, i)).get<QoreListNode>();
            node->push(columnValue(stmt, i), xsink);
        }

        if (maxrows > 0 && row_count == end) {
            break;
        }
    }

    return rv.release();
}

QoreListNode* QoreSqlite3PreparedStatement::getOutputList(ExceptionSink* xsink, int maxrows) {
    assert(sql);
    assert(stmt);
    assert(sql_active);
    assert(row_count != -1);

    int end = maxrows > 0 ? row_count + maxrows : -1;

    ReferenceHolder<QoreListNode> rv(new QoreListNode(autoHashTypeInfo), xsink);
    while (next(xsink)) {
        if (*xsink) {
            return nullptr;
        }

        ReferenceHolder<QoreHashNode> head(new QoreHashNode(autoTypeInfo), xsink);

        for (int i = 0; i < sqlite3_column_count(stmt); ++i) {
            head->setKeyValue(sqlite3_column_name(stmt, i), columnValue(stmt, i), xsink);
        }
        rv->push(head.release(), xsink);

        if (maxrows > 0 && row_count == end) {
            break;
        }
    }

    return rv.release();
}

QoreHashNode* QoreSqlite3PreparedStatement::fetchRow(ExceptionSink* xsink) {
    assert(sql);
    assert(stmt);

    if (!sql_active || row_count == -1) {
        xsink->raiseException("SQLITE3-FETCH-ROW-ERROR", "SQLStatement::next() must be called and return True before "
            " calling SQLStatement::fetchRow()");
        return nullptr;
    }

    ReferenceHolder<QoreHashNode> rv(new QoreHashNode(autoTypeInfo), xsink);

    for (int i = 0, e = sqlite3_column_count(stmt); i < e; ++i) {
        rv->setKeyValue(sqlite3_column_name(stmt, i), columnValue(stmt, i), xsink);
    }

    return rv.release();
}

QoreListNode* QoreSqlite3PreparedStatement::fetchRows(int rows, ExceptionSink* xsink) {
    assert(sql);
    assert(stmt);

    if (!sql_active) {
        xsink->raiseException("SQLITE3-FETCH-ROWS-ERROR", "SQL statement is inactive or has reached the end of the "
            "result set");
        return nullptr;
    }

    if (row_count == -1) {
        row_count = 0;
    }

    return getOutputList(xsink, rows);
}

QoreHashNode* QoreSqlite3PreparedStatement::fetchColumns(int rows, ExceptionSink* xsink) {
    assert(sql);
    assert(stmt);

    if (!sql_active) {
        xsink->raiseException("SQLITE3-FETCH-COLUMNS-ERROR", "SQL statement is inactive or has reached the end of "
            "the result set");
        return nullptr;
    }

    if (row_count == -1) {
        row_count = 0;
    }

    return getOutputHash(xsink, rows);
}

#if defined(QDBI_METHOD_SELECT_COLUMNAR) || defined(QDBI_METHOD_STMT_FETCH_COLUMNAR)
QoreColumnarResult* QoreSqlite3PreparedStatement::fetchColumnar(int rows, ExceptionSink* xsink) {
    assert(sql);
    assert(stmt);

    if (!sql_active) {
        xsink->raiseException("SQLITE3-FETCH-COLUMNAR-ERROR", "SQL statement is inactive or has reached the end of "
            "the result set");
        return nullptr;
    }

    if (row_count == -1) {
        row_count = 0;
    }

    int end = rows > 0 ? row_count + rows : -1;
    int column_count = sqlite3_column_count(stmt);

    std::vector<std::unique_ptr<SqliteColumnarBuilder>> builders;
    builders.reserve(column_count);
    for (int i = 0; i < column_count; ++i) {
        if (i && !(i % 100) && qore_check_cancel(xsink, "initializing SQLite columnar result")) {
            return nullptr;
        }
        builders.emplace_back(new SqliteColumnarBuilder(sqlite3_column_name(stmt, i), xsink));
    }

    while (next(xsink)) {
        if (*xsink) {
            return nullptr;
        }

        for (int i = 0; i < column_count; ++i) {
            if (i && !(i % 100) && qore_check_cancel(xsink, "fetching SQLite columnar row")) {
                return nullptr;
            }
            if (builders[i]->append(stmt, i, xsink)) {
                return nullptr;
            }
        }

        if (rows > 0 && row_count == end) {
            break;
        }
    }

    ReferenceHolder<QoreHashNode> columns(new QoreHashNode(autoTypeInfo), xsink);
    for (int i = 0; i < column_count; ++i) {
        if (i && !(i % 100) && qore_check_cancel(xsink, "finalizing SQLite columnar result")) {
            return nullptr;
        }
        columns->setKeyValue(builders[i]->getName(), builders[i]->finish(xsink), xsink);
        if (*xsink) {
            return nullptr;
        }
    }

#ifdef SQLITE_DESCRIBE
    ReferenceHolder<QoreHashNode> desc(describe(xsink), xsink);
    if (*xsink) {
        return nullptr;
    }
    return QoreColumnarResult::fromColumnHash(*columns, *desc, xsink);
#else
    return QoreColumnarResult::fromColumnHash(*columns, nullptr, xsink);
#endif
}
#endif

int QoreSqlite3PreparedStatement::rowsAffected() {
    return sqlite3_changes(conn->handler());
}

QoreHashNode* QoreSqlite3PreparedStatement::describe(ExceptionSink* xsink) {
#ifdef SQLITE_DESCRIBE
    // set up hash for row
    ReferenceHolder<QoreHashNode> h(new QoreHashNode(autoTypeInfo), xsink);
    QoreString namestr("name");
    QoreString maxsizestr("maxsize");
    QoreString typestr("type");
    QoreString dbtypestr("native_type");
    QoreString internalstr("internal_id");

    for (int i = 0, e = sqlite3_column_count(stmt); i < e; ++i) {
        const char* column_name = sqlite3_column_name(stmt, i);
        int ctype = sqlite3_column_type(stmt, i);

        ReferenceHolder<QoreHashNode> col(new QoreHashNode(autoTypeInfo), xsink);
        col->setKeyValue(namestr, new QoreStringNode(column_name), xsink);
        col->setKeyValue(internalstr, ctype, xsink);

        const char* stype;
        qore_type_t qtype;
        switch (ctype) {
            case SQLITE_INTEGER:
                stype = "int";
                qtype = NT_INT;
                break;

            case SQLITE_FLOAT:
                stype = "float";
                qtype = NT_FLOAT;
                break;

            case SQLITE_BLOB:
                stype = "blob";
                qtype = NT_BINARY;
                break;

            case SQLITE_TEXT:
                stype = "text";
                qtype = NT_STRING;
                break;

            default:
                stype = "unknown";
                qtype = -1;
                break;
        };

        col->setKeyValue(typestr, qtype, xsink);
        col->setKeyValue(dbtypestr, new QoreStringNode(stype), xsink);
        col->setKeyValue(maxsizestr, -1, xsink);

        h->setKeyValue(column_name, col.release(), xsink);
    }

    return h.release();
#else
    xsink->raiseException("SQLITE3-DESCRIBE-ERROR", "SQLStatement::describe() is not supported");
    return nullptr;
#endif
}

void QoreSqlite3PreparedStatement::reset(ExceptionSink* xsink) {
    if (stmt) {
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }

    if (sql) {
        delete sql;
        sql = nullptr;
    }

    // ReferenceHolder handles deref automatically when assigned nullptr
    m_realArgs = nullptr;

    sql_active = false;
    row_count = -1;
}
