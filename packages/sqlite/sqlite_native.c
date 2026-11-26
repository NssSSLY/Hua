#include "hua/native.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define TAG_CONN 1u
#define TAG_STMT 2u

#define SQLITE_OK          0
#define SQLITE_ERROR       1
#define SQLITE_BUSY        5
#define SQLITE_LOCKED      6
#define SQLITE_READONLY    8
#define SQLITE_IOERR      10
#define SQLITE_CANTOPEN   14
#define SQLITE_CONSTRAINT 19
#define SQLITE_RANGE      25
#define SQLITE_ROW       100
#define SQLITE_DONE      101

#define SQLITE_OPEN_READWRITE 0x00000002
#define SQLITE_OPEN_CREATE    0x00000004

#define SQLITE_INTEGER 1
#define SQLITE_FLOAT   2
#define SQLITE_TEXT    3
#define SQLITE_BLOB    4
#define SQLITE_NULL    5

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef void (*sqlite3_destructor_type)(void *);
#define SQLITE_TRANSIENT ((sqlite3_destructor_type)-1)

typedef struct {
    int loaded;
    int (*initialize)(void);
    int (*open_v2)(const char *, sqlite3 **, int, const char *);
    int (*close_v2)(sqlite3 *);
    int (*exec)(sqlite3 *, const char *, int (*)(void *, int, char **, char **), void *, char **);
    void (*free_fn)(void *);
    const char *(*errmsg)(sqlite3 *);
    int (*errcode)(sqlite3 *);
    int (*extended_errcode)(sqlite3 *);
    int (*changes)(sqlite3 *);
    int64_t (*last_insert_rowid)(sqlite3 *);
    int (*prepare_v2)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
    int (*finalize)(sqlite3_stmt *);
    int (*reset)(sqlite3_stmt *);
    int (*step)(sqlite3_stmt *);
    int (*bind_null)(sqlite3_stmt *, int);
    int (*bind_int64)(sqlite3_stmt *, int, int64_t);
    int (*bind_double)(sqlite3_stmt *, int, double);
    int (*bind_text)(sqlite3_stmt *, int, const char *, int, sqlite3_destructor_type);
    int (*bind_blob)(sqlite3_stmt *, int, const void *, int, sqlite3_destructor_type);
    int (*column_count)(sqlite3_stmt *);
    const char *(*column_name)(sqlite3_stmt *, int);
    int (*column_type)(sqlite3_stmt *, int);
    int64_t (*column_int64)(sqlite3_stmt *, int);
    double (*column_double)(sqlite3_stmt *, int);
    const unsigned char *(*column_text)(sqlite3_stmt *, int);
    int (*column_bytes)(sqlite3_stmt *, int);
    const void *(*column_blob)(sqlite3_stmt *, int);
} SqliteApi;

static SqliteApi g_sql;

static int load_sqlite_dll(void) {
    if (g_sql.loaded) return 1;
#ifdef _WIN32
    HMODULE self = NULL;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&load_sqlite_dll, &self)) {
        return 0;
    }
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(self, path, 32768);
    if (n == 0 || n >= 32768) return 0;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) slash = wcsrchr(path, L'/');
    if (!slash) return 0;
    wcscpy(slash + 1, L"sqlite3.dll");
    HMODULE lib = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) return 0;
#define LOAD_SYM(field, sym) \
    do { g_sql.field = (void *)GetProcAddress(lib, sym); if (!g_sql.field) return 0; } while (0)
#else
    void *lib = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return 0;
#define LOAD_SYM(field, sym) \
    do { g_sql.field = dlsym(lib, sym); if (!g_sql.field) return 0; } while (0)
#endif
    LOAD_SYM(initialize, "sqlite3_initialize");
    LOAD_SYM(open_v2, "sqlite3_open_v2");
    LOAD_SYM(close_v2, "sqlite3_close_v2");
    LOAD_SYM(exec, "sqlite3_exec");
    LOAD_SYM(free_fn, "sqlite3_free");
    LOAD_SYM(errmsg, "sqlite3_errmsg");
    LOAD_SYM(errcode, "sqlite3_errcode");
    LOAD_SYM(extended_errcode, "sqlite3_extended_errcode");
    LOAD_SYM(changes, "sqlite3_changes");
    LOAD_SYM(last_insert_rowid, "sqlite3_last_insert_rowid");
    LOAD_SYM(prepare_v2, "sqlite3_prepare_v2");
    LOAD_SYM(finalize, "sqlite3_finalize");
    LOAD_SYM(reset, "sqlite3_reset");
    LOAD_SYM(step, "sqlite3_step");
    LOAD_SYM(bind_null, "sqlite3_bind_null");
    LOAD_SYM(bind_int64, "sqlite3_bind_int64");
    LOAD_SYM(bind_double, "sqlite3_bind_double");
    LOAD_SYM(bind_text, "sqlite3_bind_text");
    LOAD_SYM(bind_blob, "sqlite3_bind_blob");
    LOAD_SYM(column_count, "sqlite3_column_count");
    LOAD_SYM(column_name, "sqlite3_column_name");
    LOAD_SYM(column_type, "sqlite3_column_type");
    LOAD_SYM(column_int64, "sqlite3_column_int64");
    LOAD_SYM(column_double, "sqlite3_column_double");
    LOAD_SYM(column_text, "sqlite3_column_text");
    LOAD_SYM(column_bytes, "sqlite3_column_bytes");
    LOAD_SYM(column_blob, "sqlite3_column_blob");
#undef LOAD_SYM
    if (g_sql.initialize() != SQLITE_OK) return 0;
    g_sql.loaded = 1;
    return 1;
}

typedef struct {
    sqlite3 *db;
} ConnObj;

typedef struct {
    sqlite3 *db;
    sqlite3_stmt *stmt;
} StmtObj;

static const char *map_code(int rc) {
    int primary = rc & 0xFF;
    switch (primary) {
        case SQLITE_CONSTRAINT: return "constraint";
        case SQLITE_BUSY:
        case SQLITE_LOCKED:     return "busy";
        case SQLITE_READONLY:   return "readonly";
        case SQLITE_IOERR:      return "io";
        case SQLITE_CANTOPEN:   return "cantopen";
        case SQLITE_RANGE:      return "range";
        case SQLITE_ERROR:      return "syntax";
        default:                return "error";
    }
}

static hua_value sql_err(const hua_api_v2 *api, hua_env env, sqlite3 *db, int rc, const char *fallback) {
    int ext = db ? g_sql.extended_errcode(db) : rc;
    const char *msg = (db && g_sql.errmsg(db)) ? g_sql.errmsg(db) : (fallback ? fallback : "sqlite error");
    return api->err_value(env, "sqlite", map_code(rc), msg, "sqlite3", 1, (int64_t)ext);
}

static void conn_dtor(void *p) {
    ConnObj *c = (ConnObj *)p;
    if (!c) return;
    if (c->db && g_sql.loaded) g_sql.close_v2(c->db);
    free(c);
}

static int conn_closer(void *p, char *err_buf, size_t err_cap) {
    ConnObj *c = (ConnObj *)p;
    if (!c) return 0;
    int rc = SQLITE_OK;
    if (c->db && g_sql.loaded) {
        rc = g_sql.close_v2(c->db);
        if (rc != SQLITE_OK && err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "%s", g_sql.errmsg(c->db));
        }
    }
    free(c);
    return rc == SQLITE_OK ? 0 : rc;
}

static void stmt_dtor(void *p) {
    StmtObj *s = (StmtObj *)p;
    if (!s) return;
    if (s->stmt && g_sql.loaded) g_sql.finalize(s->stmt);
    free(s);
}

static int stmt_closer(void *p, char *err_buf, size_t err_cap) {
    StmtObj *s = (StmtObj *)p;
    if (!s) return 0;
    if (s->stmt && g_sql.loaded) {
        g_sql.finalize(s->stmt);
    }
    (void)err_buf;
    (void)err_cap;
    free(s);
    return 0;
}

static int get_conn(const hua_api_v2 *api, hua_env env, hua_value h, ConnObj **out, hua_value *err_out) {
    void *raw = NULL;
    int st = api->handle_get(env, h, TAG_CONN, &raw);
    if (st == HUA_HANDLE_CLOSED) {
        *err_out = api->err_value(env, "sqlite", "closed", "database connection is closed", NULL, 0, 0);
        return 0;
    }
    if (st == HUA_HANDLE_POISONED) {
        *err_out = api->err_value(env, "sqlite", "poisoned", "database connection is poisoned", NULL, 0, 0);
        return 0;
    }
    if (st != HUA_HANDLE_OK) {
        *err_out = NULL;
        return 0;
    }
    *out = (ConnObj *)raw;
    return 1;
}

static int get_stmt(const hua_api_v2 *api, hua_env env, hua_value h, StmtObj **out, hua_value *err_out) {
    void *raw = NULL;
    int st = api->handle_get(env, h, TAG_STMT, &raw);
    if (st == HUA_HANDLE_CLOSED) {
        *err_out = api->err_value(env, "sqlite", "closed", "statement is finalized", NULL, 0, 0);
        return 0;
    }
    if (st == HUA_HANDLE_POISONED) {
        *err_out = api->err_value(env, "sqlite", "poisoned", "statement is poisoned", NULL, 0, 0);
        return 0;
    }
    if (st != HUA_HANDLE_OK) {
        *err_out = NULL;
        return 0;
    }
    *out = (StmtObj *)raw;
    return 1;
}

static hua_value fn_open(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    if (!load_sqlite_dll()) return api->abort_error(env, "failed to load companion sqlite3.dll");
    const char *p = NULL;
    size_t len = 0;
    if (argc != 1 || !api->as_string(env, args[0], &p, &len)) return NULL;
    char *path_z = (char *)malloc(len + 1);
    if (!path_z) return api->abort_error(env, "out of memory");
    if (len > 0) memcpy(path_z, p, len);
    path_z[len] = '\0';
    sqlite3 *db = NULL;
    int rc = g_sql.open_v2(path_z, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    free(path_z);
    if (rc != SQLITE_OK) {
        hua_value err = sql_err(api, env, db, rc, "cannot open sqlite database");
        if (db) g_sql.close_v2(db);
        return err;
    }
    ConnObj *c = (ConnObj *)calloc(1, sizeof(ConnObj));
    c->db = db;
    hua_value h = api->handle_new(env, TAG_CONN, c, conn_dtor);
    if (!h) {
        g_sql.close_v2(db);
        free(c);
        return NULL;
    }
    return api->ok_value(env, h);
}

static hua_value fn_close(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    char err_buf[256];
    if (argc != 1) return NULL;
    int st = api->handle_close(env, args[0], TAG_CONN, conn_closer, err_buf, sizeof(err_buf));
    if (st == HUA_HANDLE_POISONED) {
        return api->err_value(env, "sqlite", "poisoned", err_buf[0] ? err_buf : "close failed", "sqlite3", 0, 0);
    }
    if (st != HUA_HANDLE_OK) return NULL;
    return api->ok_void(env);
}

static hua_value fn_exec(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    ConnObj *c = NULL;
    hua_value err = NULL;
    const char *sql = NULL;
    size_t len = 0;
    if (argc != 2 || !api->as_string(env, args[1], &sql, &len)) return NULL;
    if (!get_conn(api, env, args[0], &c, &err)) return err;
    char *sql_z = (char *)malloc(len + 1);
    if (!sql_z) return api->abort_error(env, "out of memory");
    if (len > 0) memcpy(sql_z, sql, len);
    sql_z[len] = '\0';
    char *errmsg = NULL;
    int rc = g_sql.exec(c->db, sql_z, NULL, NULL, &errmsg);
    free(sql_z);
    if (rc != SQLITE_OK) {
        hua_value out = sql_err(api, env, c->db, rc, errmsg);
        if (errmsg) g_sql.free_fn(errmsg);
        return out;
    }
    return api->ok_void(env);
}

static hua_value fn_changes(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    ConnObj *c = NULL;
    hua_value err = NULL;
    if (argc != 1) return NULL;
    if (!get_conn(api, env, args[0], &c, &err)) return err;
    return api->ok_value(env, api->integer(env, (int64_t)g_sql.changes(c->db)));
}

static hua_value fn_last_insert_rowid(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    ConnObj *c = NULL;
    hua_value err = NULL;
    if (argc != 1) return NULL;
    if (!get_conn(api, env, args[0], &c, &err)) return err;
    return api->ok_value(env, api->integer(env, g_sql.last_insert_rowid(c->db)));
}

static hua_value fn_prepare(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    ConnObj *c = NULL;
    hua_value err = NULL;
    const char *sql = NULL;
    size_t len = 0;
    if (argc != 2 || !api->as_string(env, args[1], &sql, &len)) return NULL;
    if (!get_conn(api, env, args[0], &c, &err)) return err;
    sqlite3_stmt *stmt = NULL;
    int rc = g_sql.prepare_v2(c->db, sql, (int)len, &stmt, NULL);
    if (rc != SQLITE_OK || !stmt) {
        return sql_err(api, env, c->db, rc == SQLITE_OK ? SQLITE_ERROR : rc, "prepare failed");
    }
    StmtObj *s = (StmtObj *)calloc(1, sizeof(StmtObj));
    s->db = c->db;
    s->stmt = stmt;
    hua_value h = api->handle_new(env, TAG_STMT, s, stmt_dtor);
    if (!h) {
        g_sql.finalize(stmt);
        free(s);
        return NULL;
    }
    return api->ok_value(env, h);
}

static hua_value fn_finalize(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    char err_buf[128];
    if (argc != 1) return NULL;
    int st = api->handle_close(env, args[0], TAG_STMT, stmt_closer, err_buf, sizeof(err_buf));
    if (st == HUA_HANDLE_POISONED) return api->err_value(env, "sqlite", "poisoned", err_buf, "sqlite3", 0, 0);
    if (st != HUA_HANDLE_OK) return NULL;
    return api->ok_void(env);
}

static hua_value fn_reset(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    StmtObj *s = NULL;
    hua_value err = NULL;
    if (argc != 1) return NULL;
    if (!get_stmt(api, env, args[0], &s, &err)) return err;
    int rc = g_sql.reset(s->stmt);
    if (rc != SQLITE_OK) return sql_err(api, env, s->db, rc, "reset failed");
    return api->ok_void(env);
}

static hua_value fn_step(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    StmtObj *s = NULL;
    hua_value err = NULL;
    if (argc != 1) return NULL;
    if (!get_stmt(api, env, args[0], &s, &err)) return err;
    int rc = g_sql.step(s->stmt);
    if (rc == SQLITE_ROW) return api->ok_value(env, api->boolean(env, 1));
    if (rc == SQLITE_DONE) return api->ok_value(env, api->boolean(env, 0));
    return sql_err(api, env, s->db, rc, "step failed");
}

static hua_value fn_bind_null(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    StmtObj *s = NULL;
    hua_value err = NULL;
    int64_t idx = 0;
    if (argc != 2 || !api->as_int(env, args[1], &idx)) return NULL;
    if (!get_stmt(api, env, args[0], &s, &err)) return err;
    int rc = g_sql.bind_null(s->stmt, (int)idx);
    if (rc != SQLITE_OK) return sql_err(api, env, s->db, rc, "bind_null failed");
    return api->ok_void(env);
}

static hua_value fn_bind_int(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    StmtObj *s = NULL;
    hua_value err = NULL;
    int64_t idx = 0, val = 0;
    if (argc != 3 || !api->as_int(env, args[1], &idx) || !api->as_int(env, args[2], &val)) return NULL;
    if (!get_stmt(api, env, args[0], &s, &err)) return err;
    int rc = g_sql.bind_int64(s->stmt, (int)idx, val);
    if (rc != SQLITE_OK) return sql_err(api, env, s->db, rc, "bind_int failed");
    return api->ok_void(env);
}

static hua_value fn_bind_float(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    StmtObj *s = NULL;
    hua_value err = NULL;
    int64_t idx = 0;
    double val = 0.0;
    if (argc != 3 || !api->as_int(env, args[1], &idx) || !api->as_float(env, args[2], &val)) return NULL;
    if (!get_stmt(api, env, args[0], &s, &err)) return err;
    int rc = g_sql.bind_double(s->stmt, (int)idx, val);
    if (rc != SQLITE_OK) return sql_err(api, env, s->db, rc, "bind_float failed");
    return api->ok_void(env);
}

static hua_value fn_bind_text(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    StmtObj *s = NULL;
    hua_value err = NULL;
    int64_t idx = 0;
    const char *txt = NULL;
    size_t len = 0;
    if (argc != 3 || !api->as_int(env, args[1], &idx) || !api->as_string(env, args[2], &txt, &len)) return NULL;
    if (!get_stmt(api, env, args[0], &s, &err)) return err;
    int rc = g_sql.bind_text(s->stmt, (int)idx, txt ? txt : "", (int)len, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) return sql_err(api, env, s->db, rc, "bind_text failed");
    return api->ok_void(env);
}

static hua_value fn_bind_blob(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    StmtObj *s = NULL;
    hua_value err = NULL;
    int64_t idx = 0;
    const uint8_t *ptr = NULL;
    size_t len = 0;
    if (argc != 3 || !api->as_int(env, args[1], &idx) || !api->as_bytes(env, args[2], &ptr, &len)) return NULL;
    if (!get_stmt(api, env, args[0], &s, &err)) return err;
    int rc = g_sql.bind_blob(s->stmt, (int)idx, len > 0 ? (const void *)ptr : (const void *)"", (int)len, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) return sql_err(api, env, s->db, rc, "bind_blob failed");
    return api->ok_void(env);
}

static hua_value fn_column_count(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    StmtObj *s = NULL;
    hua_value err = NULL;
    if (argc != 1) return NULL;
    if (!get_stmt(api, env, args[0], &s, &err)) return err;
    return api->ok_value(env, api->integer(env, (int64_t)g_sql.column_count(s->stmt)));
}

static hua_value fn_column_type(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    StmtObj *s = NULL;
    hua_value err = NULL;
    int64_t col = 0;
    if (argc != 2 || !api->as_int(env, args[1], &col)) return NULL;
    if (!get_stmt(api, env, args[0], &s, &err)) return err;
    int t = g_sql.column_type(s->stmt, (int)col);
    const char *name = "null";
    if (t == SQLITE_INTEGER) name = "int";
    else if (t == SQLITE_FLOAT) name = "float";
    else if (t == SQLITE_TEXT) name = "text";
    else if (t == SQLITE_BLOB) name = "blob";
    return api->ok_value(env, api->string(env, name, strlen(name)));
}

static hua_value fn_column_int(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    StmtObj *s = NULL;
    hua_value err = NULL;
    int64_t col = 0;
    if (argc != 2 || !api->as_int(env, args[1], &col)) return NULL;
    if (!get_stmt(api, env, args[0], &s, &err)) return err;
    return api->ok_value(env, api->integer(env, g_sql.column_int64(s->stmt, (int)col)));
}

static hua_value fn_column_float(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    StmtObj *s = NULL;
    hua_value err = NULL;
    int64_t col = 0;
    if (argc != 2 || !api->as_int(env, args[1], &col)) return NULL;
    if (!get_stmt(api, env, args[0], &s, &err)) return err;
    return api->ok_value(env, api->floating(env, g_sql.column_double(s->stmt, (int)col)));
}

static hua_value fn_column_text(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    StmtObj *s = NULL;
    hua_value err = NULL;
    int64_t col = 0;
    if (argc != 2 || !api->as_int(env, args[1], &col)) return NULL;
    if (!get_stmt(api, env, args[0], &s, &err)) return err;
    const unsigned char *txt = g_sql.column_text(s->stmt, (int)col);
    int n = g_sql.column_bytes(s->stmt, (int)col);
    return api->ok_value(env, api->string(env, txt ? (const char *)txt : "", n > 0 ? (size_t)n : 0));
}

static hua_value fn_column_blob(const hua_api_v2 *api, hua_env env, const hua_value *args, uint32_t argc) {
    StmtObj *s = NULL;
    hua_value err = NULL;
    int64_t col = 0;
    if (argc != 2 || !api->as_int(env, args[1], &col)) return NULL;
    if (!get_stmt(api, env, args[0], &s, &err)) return err;
    const void *blob = g_sql.column_blob(s->stmt, (int)col);
    int n = g_sql.column_bytes(s->stmt, (int)col);
    return api->ok_value(env, api->bytes(env, blob ? (const uint8_t *)blob : NULL, n > 0 ? (size_t)n : 0));
}

static const char *const P_STR[] = { "string" };
static const char *const P_H[] = { "NativeHandle" };
static const char *const P_H_STR[] = { "NativeHandle", "string" };
static const char *const P_H_INT[] = { "NativeHandle", "int" };
static const char *const P_H_INT_INT[] = { "NativeHandle", "int", "int" };
static const char *const P_H_INT_FLT[] = { "NativeHandle", "int", "float" };
static const char *const P_H_INT_STR[] = { "NativeHandle", "int", "string" };
static const char *const P_H_INT_BYTES[] = { "NativeHandle", "int", "Bytes" };

static const hua_export_v2 EXPORTS[] = {
    { "open", 1, P_STR, "Result<NativeHandle,std.error.Value>", fn_open },
    { "close", 1, P_H, "Result<void,std.error.Value>", fn_close },
    { "exec", 2, P_H_STR, "Result<void,std.error.Value>", fn_exec },
    { "changes", 1, P_H, "Result<int,std.error.Value>", fn_changes },
    { "last_insert_rowid", 1, P_H, "Result<int,std.error.Value>", fn_last_insert_rowid },
    { "prepare", 2, P_H_STR, "Result<NativeHandle,std.error.Value>", fn_prepare },
    { "finalize", 1, P_H, "Result<void,std.error.Value>", fn_finalize },
    { "reset", 1, P_H, "Result<void,std.error.Value>", fn_reset },
    { "step", 1, P_H, "Result<bool,std.error.Value>", fn_step },
    { "bind_null", 2, P_H_INT, "Result<void,std.error.Value>", fn_bind_null },
    { "bind_int", 3, P_H_INT_INT, "Result<void,std.error.Value>", fn_bind_int },
    { "bind_float", 3, P_H_INT_FLT, "Result<void,std.error.Value>", fn_bind_float },
    { "bind_text", 3, P_H_INT_STR, "Result<void,std.error.Value>", fn_bind_text },
    { "bind_blob", 3, P_H_INT_BYTES, "Result<void,std.error.Value>", fn_bind_blob },
    { "column_count", 1, P_H, "Result<int,std.error.Value>", fn_column_count },
    { "column_type", 2, P_H_INT, "Result<string,std.error.Value>", fn_column_type },
    { "column_int", 2, P_H_INT, "Result<int,std.error.Value>", fn_column_int },
    { "column_float", 2, P_H_INT, "Result<float,std.error.Value>", fn_column_float },
    { "column_text", 2, P_H_INT, "Result<string,std.error.Value>", fn_column_text },
    { "column_blob", 2, P_H_INT, "Result<Bytes,std.error.Value>", fn_column_blob },
};

static const hua_module_v2 MODULE = {
    HUA_NATIVE_ABI_V2,
    (uint32_t)sizeof(hua_module_v2),
    (uint32_t)(sizeof(EXPORTS) / sizeof(EXPORTS[0])),
    EXPORTS
};

HUA_MODULE_EXPORT const hua_module_v2 *hua_module_entry_v2(void) {
    return &MODULE;
}
