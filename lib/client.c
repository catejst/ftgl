/*
 * Copyright (c) 2017-2018 Snowflake Computing, Inc. All rights reserved.
 */

#include <assert.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <openssl/crypto.h>
#include <snowflake/client.h>
#include "constants.h"
#include "client_int.h"
#include "connection.h"
#include "memory.h"
#include "results.h"
#include "error.h"
#include "chunk_downloader.h"

#define curl_easier_escape(curl, string) curl_easy_escape(curl, string, 0)

// Define internal constants
sf_bool DISABLE_VERIFY_PEER;
char *CA_BUNDLE_FILE;
int32 SSL_VERSION;
sf_bool DEBUG;

static char *LOG_PATH = NULL;
static FILE *LOG_FP = NULL;

static SF_MUTEX_HANDLE log_lock;
static SF_MUTEX_HANDLE gmlocaltime_lock;

static SF_STATUS STDCALL
_snowflake_internal_query(SF_CONNECT *sf, const char *sql);

static SF_STATUS STDCALL
_set_parameters_session_info(SF_CONNECT *sf, cJSON *data);

static void STDCALL _set_current_objects(SF_STMT *sfstmt, cJSON *data);

static SF_STATUS STDCALL
_reset_connection_parameters(SF_CONNECT *sf, cJSON *parameters,
                             cJSON *session_info, sf_bool do_validate);

#define _SF_STMT_TYPE_DML 0x3000
#define _SF_STMT_TYPE_INSERT (_SF_STMT_TYPE_DML + 0x100)
#define _SF_STMT_TYPE_UPDATE (_SF_STMT_TYPE_DML + 0x200)
#define _SF_STMT_TYPE_DELETE (_SF_STMT_TYPE_DML + 0x300)
#define _SF_STMT_TYPE_MERGE (_SF_STMT_TYPE_DML + 0x400)
#define _SF_STMT_TYPE_MULTI_TABLE_INSERT (_SF_STMT_TYPE_DML + 0x500)

/**
 * Detects statement type is DML
 * @param stmt_type_id statement type id
 * @return SF_BOOLEAN_TRUE if the statement is DM or SF_BOOLEAN_FALSE
 */
static sf_bool detect_stmt_type(int64 stmt_type_id) {
    sf_bool ret;
    switch (stmt_type_id) {
        case _SF_STMT_TYPE_DML:
        case _SF_STMT_TYPE_INSERT:
        case _SF_STMT_TYPE_UPDATE:
        case _SF_STMT_TYPE_DELETE:
        case _SF_STMT_TYPE_MERGE:
        case _SF_STMT_TYPE_MULTI_TABLE_INSERT:
            ret = SF_BOOLEAN_TRUE;
            break;
        default:
            ret = SF_BOOLEAN_FALSE;
            break;
    }
    return ret;
}

#define _SF_STMT_SQL_BEGIN "begin"
#define _SF_STMT_SQL_COMMIT "commit"
#define _SF_STMT_SQL_ROLLBACK "rollback"

/**
 * precomputed 10^N for int64
 */
static const int64 pow10_int64[] = {
    1LL,
    10LL,
    100LL,
    1000LL,
    10000LL,
    100000LL,
    1000000LL,
    10000000LL,
    100000000LL,
    1000000000LL
};

/**
 * Maximum one-directional range of offset-based timezones (24 hours)
 */
#define TIMEZONE_OFFSET_RANGE  (int64)(24 * 60);

/**
 * Find string size, create buffer, copy over, and return.
 * @param var output buffer
 * @param str source string
 */
static void alloc_buffer_and_copy(char **var, const char *str) {
    size_t str_size;
    SF_FREE(*var);
    // If passed in string is null, then return since *var is already null from being freed
    if (str) {
        str_size = strlen(str) + 1; // For null terminator
        *var = (char *) SF_CALLOC(1, str_size);
        strncpy(*var, str, str_size);
    } else {
        *var = NULL;
    }
}

/**
 * Make a directory with the mode
 * @param file_path directory name
 * @param mode mode
 * @return 0 if success otherwise -1
 */
static int mkpath(char *file_path) {
    assert(file_path && *file_path);
    char *p;
    for (p = strchr(file_path + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = '\0';
        if (sf_mkdir(file_path) == -1) {
            if (errno != EEXIST) {
                *p = '/';
                return -1;
            }
        }
        *p = '/';
    }
    return 0;
}

/**
 * Lock/Unlock log file for writing by mutex
 * @param udata user data (not used)
 * @param lock non-zere for lock or zero for unlock
 */
static void log_lock_func(void *udata, int lock) {
    if (lock) {
        _mutex_lock(&log_lock);
    } else {
        _mutex_unlock(&log_lock);
    }
}

/**
 * Extracts a Snowflake internal representation of timestamp into
 * seconds, nanoseconds and optionally Timezone offset.
 * @param sec pointer of seconds
 * @param nsec pointer of nanoseconds
 * @param tzoffset pointer of Timezone offset.
 * @param src source buffer including a Snowflake internal timestamp
 * @param timezone Timezone for TIMESTAMP_LTZ
 * @param scale Timestamp data type scale between 0 and 9
 * @return SF_BOOLEAN_TRUE if success otherwise SF_BOOLEAN_FALSE
 */
static sf_bool _extract_timestamp(
    SF_BIND_OUTPUT *result, SF_TYPE sftype,
    const char *src, const char *timezone, int64 scale) {
    time_t nsec = 0L;
    time_t sec = 0L;
    int64 tzoffset = 0;
    struct tm tm_obj;
    struct tm *tm_ptr = NULL;
    char tzname[64];
    char *tzptr = (char *) timezone;

    memset(&tm_obj, 0, sizeof(tm_obj));

    /* Search for a decimal point */
    char *ptr = strchr(src, (int) '.');
    if (ptr == NULL) {
        return SF_BOOLEAN_FALSE;
    }
    sec = strtoll(src, NULL, 10);

    /* Search for a space for TIMESTAMP_TZ */
    char *sptr = strchr(ptr + 1, (int) ' ');
    nsec = strtoll(ptr + 1, NULL, 10);
    if (sptr != NULL) {
        /* TIMESTAMP_TZ */
        nsec = strtoll(ptr + 1, NULL, 10);
        tzoffset = strtoll(sptr + 1, NULL, 10) - TIMEZONE_OFFSET_RANGE;
    }
    if (sec < 0 && nsec > 0) {
        nsec = pow10_int64[scale] - nsec;
        sec--;
    }
    log_info("sec: %lld, nsec: %lld", sec, nsec);

    if (sftype == SF_TYPE_TIMESTAMP_TZ) {
        /* make up Timezone name from the tzoffset */
        ldiv_t dm = ldiv((long) tzoffset, 60L);
        sprintf(tzname, "UTC%c%02ld:%02ld",
                dm.quot > 0 ? '+' : '-', labs(dm.quot), labs(dm.rem));
        tzptr = tzname;
    }

    /* replace a dot character with NULL */
    if (sftype == SF_TYPE_TIMESTAMP_NTZ ||
        sftype == SF_TYPE_TIME) {
        tm_ptr = sf_gmtime(&sec, &tm_obj);
    } else if (sftype == SF_TYPE_TIMESTAMP_LTZ ||
               sftype == SF_TYPE_TIMESTAMP_TZ) {
        /* set the environment variable TZ to the session timezone
         * so that localtime_tz honors it.
         */
        _mutex_lock(&gmlocaltime_lock);
        const char *prev_tz_ptr = sf_getenv("TZ");
        sf_setenv("TZ", tzptr);
        sf_tzset();
        sec += tzoffset * 60 * 2; /* adjust for TIMESTAMP_TZ */
        tm_ptr = sf_localtime(&sec, &tm_obj);
        if (prev_tz_ptr != NULL) {
            sf_setenv("TZ", prev_tz_ptr); /* cannot set to NULL */
        } else {
            sf_unsetenv("TZ");
        }
        sf_tzset();
        _mutex_unlock(&gmlocaltime_lock);
    }
    if (tm_ptr == NULL) {
        result->len = 0;
        return SF_BOOLEAN_FALSE;
    }
    const char *fmt0;
    if (sftype != SF_TYPE_TIME) {
        fmt0 = "%Y-%m-%d %H:%M:%S";
    } else {
        fmt0 = "%H:%M:%S";
    }
    /* adjust scale */
    char fmt[20];
    sprintf(fmt, ".%%0%lldld", scale);
    result->len = strftime(result->value,
                           result->max_length, fmt0, &tm_obj);
    if (scale > 0) {
        result->len += snprintf(
            &((char *) result->value)[result->len],
            result->max_length - result->len, fmt,
            nsec);
    }
    if (sftype == SF_TYPE_TIMESTAMP_TZ) {
        /* Timezone info */
        ldiv_t dm = ldiv((long) tzoffset, 60L);
        result->len += snprintf(
            &((char *) result->value)[result->len],
            result->max_length - result->len,
            " %c%02ld:%02ld",
            dm.quot > 0 ? '+' : '-', labs(dm.quot), labs(dm.rem));
    }
    return SF_BOOLEAN_TRUE;
}

/**
 * Reset the connection parameters with the returned parameteres
 * @param sf SF_CONNECT object
 * @param parameters the returned parameters
 */
static SF_STATUS STDCALL _reset_connection_parameters(
    SF_CONNECT *sf, cJSON *parameters, cJSON *session_info,
    sf_bool do_validate) {
    if (parameters != NULL) {
        int i, len;
        for (i = 0, len = cJSON_GetArraySize(parameters); i < len; ++i) {
            cJSON *p1 = cJSON_GetArrayItem(parameters, i);
            cJSON *name = cJSON_GetObjectItem(p1, "name");
            cJSON *value = cJSON_GetObjectItem(p1, "value");
            if (strcmp(name->valuestring, "TIMEZONE") == 0) {
                if (sf->timezone == NULL ||
                    strcmp(sf->timezone, value->valuestring) != 0) {
                    alloc_buffer_and_copy(&sf->timezone, value->valuestring);
                }
            }
        }
    }
    SF_STATUS ret = SF_STATUS_ERROR_GENERAL;
    if (session_info != NULL) {
        char msg[1024];
        // database
        cJSON *db = cJSON_GetObjectItem(session_info, "databaseName");
        if (do_validate && sf->database && sf->database[0] != (char) 0 &&
            db->valuestring == NULL) {
            sprintf(msg, "Specified database doesn't exists: [%s]",
                    sf->database);
            SET_SNOWFLAKE_ERROR(
                &sf->error,
                SF_STATUS_ERROR_APPLICATION_ERROR,
                msg,
                SF_SQLSTATE_UNABLE_TO_CONNECT
            );
            goto cleanup;
        }
        alloc_buffer_and_copy(&sf->database, db->valuestring);

        // schema
        cJSON *schema = cJSON_GetObjectItem(session_info, "schemaName");
        if (do_validate && sf->schema && sf->schema[0] != (char) 0 &&
            schema->valuestring == NULL) {
            sprintf(msg, "Specified schema doesn't exists: [%s]",
                    sf->schema);
            SET_SNOWFLAKE_ERROR(
                &sf->error,
                SF_STATUS_ERROR_APPLICATION_ERROR,
                msg,
                SF_SQLSTATE_UNABLE_TO_CONNECT
            );
            goto cleanup;

        }
        alloc_buffer_and_copy(&sf->schema, schema->valuestring);

        // warehouse
        cJSON *warehouse = cJSON_GetObjectItem(session_info, "warehouseName");
        if (do_validate && sf->warehouse && sf->warehouse[0] != (char) 0 &&
            warehouse->valuestring == NULL) {
            sprintf(msg, "Specified warehouse doesn't exists: [%s]",
                    sf->warehouse);
            SET_SNOWFLAKE_ERROR(
                &sf->error,
                SF_STATUS_ERROR_APPLICATION_ERROR,
                msg,
                SF_SQLSTATE_UNABLE_TO_CONNECT
            );
            goto cleanup;
        }
        alloc_buffer_and_copy(&sf->warehouse, warehouse->valuestring);

        // role
        cJSON *role = cJSON_GetObjectItem(session_info, "roleName");
        // No validation is required as already done by the server
        alloc_buffer_and_copy(&sf->role, role->valuestring);
    }
    ret = SF_STATUS_SUCCESS;
cleanup:
    return ret;
}

/*
 * Initializes logging file
 */
static sf_bool STDCALL log_init(const char *log_path, SF_LOG_LEVEL log_level) {
    _mutex_init(&log_lock);
    _mutex_init(&gmlocaltime_lock);

    sf_bool ret = SF_BOOLEAN_FALSE;
    time_t current_time;
    struct tm *time_info;
    char time_str[15];
    time(&current_time);
    time_info = localtime(&current_time);
    strftime(time_str, sizeof(time_str), "%Y%m%d%H%M%S", time_info);
    const char *sf_log_path;
    const char *sf_log_level_str;
    SF_LOG_LEVEL sf_log_level = log_level;

    size_t log_path_size = 1; //Start with 1 to include null terminator
    log_path_size += strlen(time_str);

    /* The environment variables takes precedence over the specified parameters */
    sf_log_path = sf_getenv("SNOWFLAKE_LOG_PATH");
    if (sf_log_path == NULL && log_path) {
        sf_log_path = log_path;
    }

    sf_log_level_str = sf_getenv("SNOWFLAKE_LOG_LEVEL");
    if (sf_log_level_str != NULL) {
        sf_log_level = log_from_str_to_level(sf_log_level_str);
    }

    // Set logging level
    if (DEBUG) {
        log_set_quiet(SF_BOOLEAN_FALSE);
    } else {
        log_set_quiet(SF_BOOLEAN_TRUE);
    }
    log_set_level(sf_log_level);
    log_set_lock(&log_lock_func);

    // If log path is specified, use absolute path. Otherwise set logging dir to be relative to current directory
    log_path_size += 30; // Size of static format characters
    if (sf_log_path) {
        log_path_size += strlen(sf_log_path);
        LOG_PATH = (char *) SF_CALLOC(1, log_path_size);
        snprintf(LOG_PATH, log_path_size, "%s/snowflake_%s.txt", sf_log_path,
                 (char *) time_str);
    } else {
        LOG_PATH = (char *) SF_CALLOC(1, log_path_size);
        snprintf(LOG_PATH, log_path_size, "logs/snowflake_%s.txt",
                 (char *) time_str);
    }
    if (LOG_PATH != NULL) {
        // Create log file path (if it already doesn't exist)
        if (mkpath(LOG_PATH) == -1) {
            fprintf(stderr, "Error creating log directory. Error code: %s\n",
                    strerror(errno));
            goto cleanup;
        }
        // Open log file
        LOG_FP = fopen(LOG_PATH, "w+");
        if (LOG_FP) {
            // Set log file
            log_set_fp(LOG_FP);
        } else {
            fprintf(stderr,
                    "Error opening file from file path: %s\nError code: %s\n",
                    LOG_PATH, strerror(errno));
            goto cleanup;
        }

    } else {
        fprintf(stderr,
                "Log path is NULL. Was there an error during path construction?\n");
        goto cleanup;
    }

    ret = SF_BOOLEAN_TRUE;

cleanup:
    return ret;
}

/**
 * Cleans up memory allocated for log init and closes log file.
 */
static void STDCALL log_term() {
    SF_FREE(LOG_PATH);
    if (LOG_FP) {
        fclose(LOG_FP);
        log_set_fp(NULL);
    }
    _mutex_term(&gmlocaltime_lock);
    _mutex_term(&log_lock);
}

/**
 * Process connection parameters
 * @param sf SF_CONNECT
 */
SF_STATUS STDCALL
_snowflake_check_connection_parameters(SF_CONNECT *sf) {
    if (is_string_empty(sf->account)) {
        // Invalid connection
        log_error(ERR_MSG_ACCOUNT_PARAMETER_IS_MISSING);
        SET_SNOWFLAKE_ERROR(
            &sf->error,
            SF_STATUS_ERROR_BAD_CONNECTION_PARAMS,
            ERR_MSG_ACCOUNT_PARAMETER_IS_MISSING,
            SF_SQLSTATE_UNABLE_TO_CONNECT);
        return SF_STATUS_ERROR_GENERAL;
    }

    if (is_string_empty(sf->user)) {
        // Invalid connection
        log_error(ERR_MSG_USER_PARAMETER_IS_MISSING);
        SET_SNOWFLAKE_ERROR(
            &sf->error,
            SF_STATUS_ERROR_BAD_CONNECTION_PARAMS,
            ERR_MSG_USER_PARAMETER_IS_MISSING,
            SF_SQLSTATE_UNABLE_TO_CONNECT);
        return SF_STATUS_ERROR_GENERAL;
    }

    if (is_string_empty(sf->password)) {
        // Invalid connection
        log_error(ERR_MSG_PASSWORD_PARAMETER_IS_MISSING);
        SET_SNOWFLAKE_ERROR(
            &sf->error,
            SF_STATUS_ERROR_BAD_CONNECTION_PARAMS,
            ERR_MSG_PASSWORD_PARAMETER_IS_MISSING,
            SF_SQLSTATE_UNABLE_TO_CONNECT);
        return SF_STATUS_ERROR_GENERAL;
    }

    if (!sf->host) {
        // construct a host parameter if not specified,
        char buf[1024];
        if (sf->region) {
            snprintf(buf, sizeof(buf), "%s.%s.snowflakecomputing.com",
                     sf->account, sf->region);
        } else {
            snprintf(buf, sizeof(buf), "%s.snowflakecomputing.com",
                     sf->account);
        }
        alloc_buffer_and_copy(&sf->host, buf);
    }
    // split account and region if connected by a dot.
    char *dot_ptr = strchr(sf->account, (int) '.');
    if (dot_ptr) {
        char *extracted_account = NULL;
        char *extracted_region = NULL;
        alloc_buffer_and_copy(&extracted_region, dot_ptr + 1);
        *dot_ptr = (char) 0;
        alloc_buffer_and_copy(&extracted_account, sf->account);
        SF_FREE(sf->account);
        SF_FREE(sf->region);
        sf->account = extracted_account;
        sf->region = extracted_region;
    }
    if (!sf->protocol) {
        alloc_buffer_and_copy(&sf->protocol, "https");
    }
    if (!sf->port) {
        alloc_buffer_and_copy(&sf->port, "443");
    }

    log_debug("application name: %s", sf->application_name);
    log_debug("application version: %s", sf->application_version);
    log_debug("authenticator: %s", sf->authenticator);
    log_debug("user: %s", sf->user);
    log_debug("password: %s", sf->password ? "****" : sf->password);
    log_debug("host: %s", sf->host);
    log_debug("port: %s", sf->port);
    log_debug("account: %s", sf->account);
    log_debug("region: %s", sf->region);
    log_debug("database: %s", sf->database);
    log_debug("schema: %s", sf->schema);
    log_debug("warehouse: %s", sf->warehouse);
    log_debug("role: %s", sf->role);
    log_debug("protocol: %s", sf->protocol);
    log_debug("autocommit: %s", sf->autocommit ? "true": "false");
    log_debug("insecure_mode: %s", sf->insecure_mode ? "true" : "false");
    log_debug("timezone: %s", sf->timezone);
    log_debug("login_timeout: %d", sf->login_timeout);
    log_debug("network_timeout: %d", sf->network_timeout);

    return SF_STATUS_SUCCESS;
}


SF_STATUS STDCALL snowflake_global_init(
    const char *log_path, SF_LOG_LEVEL log_level) {
    SF_STATUS ret = SF_STATUS_ERROR_GENERAL;

    // Initialize constants
    DISABLE_VERIFY_PEER = SF_BOOLEAN_FALSE;
    CA_BUNDLE_FILE = NULL;
    SSL_VERSION = CURL_SSLVERSION_TLSv1_2;
    DEBUG = SF_BOOLEAN_FALSE;

    sf_memory_init();
    sf_error_init();
    if (!log_init(log_path, log_level)) {
        // no way to log error because log_init failed.
        fprintf(stderr, "Error during log initialization");
        goto cleanup;
    }
    CURLcode curl_ret = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curl_ret != CURLE_OK) {
        log_fatal("curl_global_init() failed: %s",
                  curl_easy_strerror(curl_ret));
        goto cleanup;
    }

    ret = SF_STATUS_SUCCESS;

cleanup:
    return ret;
}

SF_STATUS STDCALL snowflake_global_term() {
    curl_global_cleanup();

    // Cleanup Constants
    SF_FREE(CA_BUNDLE_FILE);

    log_term();
    sf_alloc_map_to_log(SF_BOOLEAN_TRUE);
    sf_error_term();
    sf_memory_term();
    return SF_STATUS_SUCCESS;
}

SF_STATUS STDCALL
snowflake_global_set_attribute(SF_GLOBAL_ATTRIBUTE type, const void *value) {
    switch (type) {
        case SF_GLOBAL_DISABLE_VERIFY_PEER:
            DISABLE_VERIFY_PEER = *(sf_bool *) value;
            break;
        case SF_GLOBAL_CA_BUNDLE_FILE:
            alloc_buffer_and_copy(&CA_BUNDLE_FILE, value);
            break;
        case SF_GLOBAL_SSL_VERSION:
            SSL_VERSION = *(int32 *) value;
            break;
        case SF_GLOBAL_DEBUG:
            DEBUG = *(sf_bool *) value;
            if (DEBUG) {
                log_set_quiet(SF_BOOLEAN_FALSE);
            } else {
                log_set_quiet(SF_BOOLEAN_TRUE);
            }
            break;
        default:
            break;
    }
    return SF_STATUS_SUCCESS;
}

SF_STATUS STDCALL
snowflake_global_get_attribute(SF_GLOBAL_ATTRIBUTE type, void *value) {
    switch (type) {
        case SF_GLOBAL_DISABLE_VERIFY_PEER:
            *((sf_bool *) value) = DISABLE_VERIFY_PEER;
            break;
        case SF_GLOBAL_CA_BUNDLE_FILE:
            if (CA_BUNDLE_FILE) {
                strncpy(value, CA_BUNDLE_FILE, strlen(CA_BUNDLE_FILE) + 1);
            }
            break;
        default:
            break;
    }
    return SF_STATUS_SUCCESS;
}

SF_CONNECT *STDCALL snowflake_init() {
    SF_CONNECT *sf = (SF_CONNECT *) SF_CALLOC(1, sizeof(SF_CONNECT));

    // Make sure memory was actually allocated
    if (sf) {
        // Initialize object with default values
        sf->host = NULL;
        sf->port = NULL;
        sf->user = NULL;
        sf->password = NULL;
        sf->database = NULL;
        sf->account = NULL;
        sf->region = NULL;
        sf->role = NULL;
        sf->warehouse = NULL;
        sf->schema = NULL;
        alloc_buffer_and_copy(&sf->protocol, "https");
        sf->passcode = NULL;
        sf->passcode_in_password = SF_BOOLEAN_FALSE;
        sf->insecure_mode = SF_BOOLEAN_FALSE;
        sf->autocommit = SF_BOOLEAN_TRUE;
        sf->timezone = NULL;
        alloc_buffer_and_copy(&sf->authenticator, SF_AUTHENTICATOR_DEFAULT);
        alloc_buffer_and_copy(&sf->application_name, SF_API_NAME);
        alloc_buffer_and_copy(&sf->application_version, SF_API_VERSION);

        _mutex_init(&sf->mutex_parameters);

        sf->token = NULL;
        sf->master_token = NULL;
        sf->login_timeout = SF_LOGIN_TIMEOUT;
        sf->network_timeout = 0;
        sf->sequence_counter = 0;
        _mutex_init(&sf->mutex_sequence_counter);
        sf->request_id[0] = '\0';
        clear_snowflake_error(&sf->error);
    }

    return sf;
}

SF_STATUS STDCALL snowflake_term(SF_CONNECT *sf) {
    // Ensure object is not null
    if (!sf) {
        return SF_STATUS_ERROR_CONNECTION_NOT_EXIST;
    }
    cJSON *resp = NULL;
    char *s_resp = NULL;
    clear_snowflake_error(&sf->error);

    if (sf->token && sf->master_token) {
        /* delete the session */
        URL_KEY_VALUE url_params[] = {
            {.key="delete=", .value="true", .formatted_key=NULL, .formatted_value=NULL, .key_size=0, .value_size=0}
        };
        if (request(sf, &resp, DELETE_SESSION_URL, url_params,
                    sizeof(url_params) / sizeof(URL_KEY_VALUE), NULL, NULL,
                    POST_REQUEST_TYPE, &sf->error, SF_BOOLEAN_FALSE)) {
            s_resp = cJSON_Print(resp);
            log_trace("JSON response:\n%s", s_resp);
            /* Even if the session deletion fails, it will be cleaned after 7 days.
             * Catching error here won't help
             */
        }
        cJSON_Delete(resp);
        SF_FREE(s_resp);
    }

    _mutex_term(&sf->mutex_sequence_counter);
    _mutex_term(&sf->mutex_parameters);
    SF_FREE(sf->host);
    SF_FREE(sf->port);
    SF_FREE(sf->user);
    SF_FREE(sf->password);
    SF_FREE(sf->database);
    SF_FREE(sf->account);
    SF_FREE(sf->region);
    SF_FREE(sf->role);
    SF_FREE(sf->warehouse);
    SF_FREE(sf->schema);
    SF_FREE(sf->protocol);
    SF_FREE(sf->passcode);
    SF_FREE(sf->authenticator);
    SF_FREE(sf->application_name);
    SF_FREE(sf->application_version);
    SF_FREE(sf->timezone);
    SF_FREE(sf->master_token);
    SF_FREE(sf->token);
    SF_FREE(sf);

    return SF_STATUS_SUCCESS;
}

SF_STATUS STDCALL snowflake_connect(SF_CONNECT *sf) {
    sf_bool success = SF_BOOLEAN_FALSE;
    SF_JSON_ERROR json_error;
    if (!sf) {
        // connection object doesn't exists
        return SF_STATUS_ERROR_CONNECTION_NOT_EXIST;
    }
    if (sf->token || sf->master_token) {
        // safe guard not to override the existing connection
        SET_SNOWFLAKE_ERROR(
            &sf->error,
            SF_STATUS_ERROR_APPLICATION_ERROR,
            ERR_MSG_CONNECTION_ALREADY_EXISTS,
            SF_SQLSTATE_CONNECTION_ALREADY_EXIST);
        return SF_STATUS_ERROR_GENERAL;
    }
    // Reset error context
    clear_snowflake_error(&sf->error);

    char os_version[128];
    sf_os_version(os_version);

    log_info("Snowflake C/C++ API: %s, OS: %s, OS Version: %s",
             SF_API_VERSION,
             sf_os_name(),
             os_version);
    cJSON *body = NULL;
    cJSON *data = NULL;
    cJSON *resp = NULL;
    char *s_body = NULL;
    char *s_resp = NULL;
    // Encoded URL to use with libcurl
    URL_KEY_VALUE url_params[] = {
        {.key = "request_id=", .value=sf->request_id, .formatted_key=NULL, .formatted_value=NULL, .key_size=0, .value_size=0},
        {.key = "&databaseName=", .value=sf->database, .formatted_key=NULL, .formatted_value=NULL, .key_size=0, .value_size=0},
        {.key = "&schemaName=", .value=sf->schema, .formatted_key=NULL, .formatted_value=NULL, .key_size=0, .value_size=0},
        {.key = "&warehouse=", .value=sf->warehouse, .formatted_key=NULL, .formatted_value=NULL, .key_size=0, .value_size=0},
        {.key = "&roleName=", .value=sf->role, .formatted_key=NULL, .formatted_value=NULL, .key_size=0, .value_size=0},
    };
    SF_STATUS ret = _snowflake_check_connection_parameters(sf);
    if (ret != SF_STATUS_SUCCESS) {
        goto cleanup;
    }
    ret = SF_STATUS_ERROR_GENERAL; // reset to the error

    uuid4_generate(sf->request_id);// request id

    // Create body
    body = create_auth_json_body(
        sf,
        sf->application_name,
        sf->application_name,
        sf->application_version,
        sf->timezone,
        sf->autocommit);
    log_trace("Created body");
    s_body = cJSON_Print(body);
    // TODO delete password before printing
    if (DEBUG) {
        log_debug("body:\n%s", s_body);
    }

    // Send request and get data
    if (request(sf, &resp, SESSION_URL, url_params,
                sizeof(url_params) / sizeof(URL_KEY_VALUE), s_body, NULL,
                POST_REQUEST_TYPE, &sf->error, SF_BOOLEAN_FALSE)) {
        s_resp = cJSON_Print(resp);
        log_trace("Here is JSON response:\n%s", s_resp);
        if ((json_error = json_copy_bool(&success, resp, "success")) !=
            SF_JSON_ERROR_NONE) {
            log_error("JSON error: %d", json_error);
            SET_SNOWFLAKE_ERROR(&sf->error, SF_STATUS_ERROR_BAD_JSON,
                                "No valid JSON response",
                                SF_SQLSTATE_UNABLE_TO_CONNECT);
            goto cleanup;
        }
        if (!success) {
            cJSON *messageJson = cJSON_GetObjectItem(resp, "message");
            char *message = NULL;
            cJSON *codeJson = NULL;
            int64 code = -1;
            if (messageJson) {
                message = messageJson->valuestring;
            }
            codeJson = cJSON_GetObjectItem(resp, "code");
            if (codeJson) {
                code = strtol(codeJson->valuestring, NULL, 10);
            } else {
                log_debug("no code element.");
            }

            SET_SNOWFLAKE_ERROR(&sf->error, (SF_STATUS) code,
                                message ? message : "Query was not successful",
                                SF_SQLSTATE_UNABLE_TO_CONNECT);
            goto cleanup;
        }

        data = cJSON_GetObjectItem(resp, "data");
        if (!set_tokens(sf, data, "token", "masterToken", &sf->error)) {
            goto cleanup;
        }

        _mutex_lock(&sf->mutex_parameters);
        ret = _set_parameters_session_info(sf, data);
        _mutex_unlock(&sf->mutex_parameters);
        if (ret > 0) {
            goto cleanup;
        }
    } else {
        log_error("No response");
        if (sf->error.error_code == SF_STATUS_SUCCESS) {
            SET_SNOWFLAKE_ERROR(&sf->error, SF_STATUS_ERROR_BAD_JSON,
                                "No valid JSON response",
                                SF_SQLSTATE_UNABLE_TO_CONNECT);
        }
        goto cleanup;
    }

    /* we are done... */
    ret = SF_STATUS_SUCCESS;

cleanup:
    // Delete password and passcode for security's sake
    if (sf->password) {
        // Write over password in memory including null terminator
        memset(sf->password, 0, strlen(sf->password) + 1);
        SF_FREE(sf->password);
    }
    if (sf->passcode) {
        // Write over passcode in memory including null terminator
        memset(sf->passcode, 0, strlen(sf->passcode) + 1);
        SF_FREE(sf->passcode);
    }
    cJSON_Delete(body);
    cJSON_Delete(resp);
    SF_FREE(s_body);
    SF_FREE(s_resp);

    return ret;
}

static SF_STATUS STDCALL _set_parameters_session_info(
    SF_CONNECT *sf, cJSON *data) {
    SF_STATUS ret = _reset_connection_parameters(
        sf,
        cJSON_GetObjectItem(data, "parameters"),
        cJSON_GetObjectItem(data, "sessionInfo"), SF_BOOLEAN_TRUE);
    return ret;
}

static void STDCALL _set_current_objects(SF_STMT *sfstmt, cJSON *data) {
    if (json_copy_string(&sfstmt->connection->database, data,
                         "finalDatabaseName")) {
        log_warn("No valid database found in response");
    }
    if (json_copy_string(&sfstmt->connection->schema, data,
                         "finalSchemaName")) {
        log_warn("No valid schema found in response");
    }
    if (json_copy_string(&sfstmt->connection->warehouse, data,
                         "finalWarehouseName")) {
        log_warn("No valid warehouse found in response");
    }
    if (json_copy_string(&sfstmt->connection->role, data,
                         "finalRoleName")) {
        log_warn("No valid role found in response");
    }
}

SF_STATUS STDCALL snowflake_set_attribute(
    SF_CONNECT *sf, SF_ATTRIBUTE type, const void *value) {
    if (!sf) {
        return SF_STATUS_ERROR_CONNECTION_NOT_EXIST;
    }
    clear_snowflake_error(&sf->error);
    switch (type) {
        case SF_CON_ACCOUNT:
            alloc_buffer_and_copy(&sf->account, value);
            break;
        case SF_CON_REGION:
            alloc_buffer_and_copy(&sf->region, value);
            break;
        case SF_CON_USER:
            alloc_buffer_and_copy(&sf->user, value);
            break;
        case SF_CON_PASSWORD:
            alloc_buffer_and_copy(&sf->password, value);
            break;
        case SF_CON_DATABASE:
            alloc_buffer_and_copy(&sf->database, value);
            break;
        case SF_CON_SCHEMA:
            alloc_buffer_and_copy(&sf->schema, value);
            break;
        case SF_CON_WAREHOUSE:
            alloc_buffer_and_copy(&sf->warehouse, value);
            break;
        case SF_CON_ROLE:
            alloc_buffer_and_copy(&sf->role, value);
            break;
        case SF_CON_HOST:
            alloc_buffer_and_copy(&sf->host, value);
            break;
        case SF_CON_PORT:
            alloc_buffer_and_copy(&sf->port, value);
            break;
        case SF_CON_PROTOCOL:
            alloc_buffer_and_copy(&sf->protocol, value);
            break;
        case SF_CON_PASSCODE:
            alloc_buffer_and_copy(&sf->passcode, value);
            break;
        case SF_CON_PASSCODE_IN_PASSWORD:
            sf->passcode_in_password = *((sf_bool *) value);
            break;
        case SF_CON_APPLICATION_NAME:
            alloc_buffer_and_copy(&sf->application_name, value);
            break;
        case SF_CON_APPLICATION_VERSION:
            alloc_buffer_and_copy(&sf->application_version, value);
            break;
        case SF_CON_AUTHENTICATOR:
            alloc_buffer_and_copy(&sf->authenticator, value);
            break;
        case SF_CON_INSECURE_MODE:
            sf->insecure_mode = value ? *((sf_bool *) value) : SF_BOOLEAN_FALSE;
            break;
        case SF_CON_LOGIN_TIMEOUT:
            sf->login_timeout = value ? *((int64 *) value) : SF_LOGIN_TIMEOUT;
            break;
        case SF_CON_NETWORK_TIMEOUT:
            sf->network_timeout = value ? *((int64 *) value) : SF_LOGIN_TIMEOUT;
            break;
        case SF_CON_AUTOCOMMIT:
            sf->autocommit = value ? *((sf_bool *) value) : SF_BOOLEAN_TRUE;
            break;
        case SF_CON_TIMEZONE:
            alloc_buffer_and_copy(&sf->timezone, value);
            break;
        default:
            SET_SNOWFLAKE_ERROR(&sf->error, SF_STATUS_ERROR_BAD_ATTRIBUTE_TYPE,
                                "Invalid attribute type",
                                SF_SQLSTATE_UNABLE_TO_CONNECT);
            return SF_STATUS_ERROR_APPLICATION_ERROR;
    }

    return SF_STATUS_SUCCESS;
}

SF_STATUS STDCALL snowflake_get_attribute(
    SF_CONNECT *sf, SF_ATTRIBUTE type, void **value) {
    if (!sf) {
        return SF_STATUS_ERROR_CONNECTION_NOT_EXIST;
    }
    clear_snowflake_error(&sf->error);
    //TODO Implement this
    return SF_STATUS_SUCCESS;
}

/**
 * Resets SF_COLUMN_DESC in SF_STMT
 * @param sfstmt
 */
static void STDCALL _snowflake_stmt_desc_reset(SF_STMT *sfstmt) {
    int64 i = 0;
    if (sfstmt->desc) {
        /* column metadata */
        for (i = 0; i < sfstmt->total_fieldcount; i++) {
            SF_FREE(sfstmt->desc[i].name);
        }
        SF_FREE(sfstmt->desc);
    }
    sfstmt->desc = NULL;
}

/**
 * Resets SNOWFLAKE_STMT parameters.
 *
 * @param sfstmt
 */
static void STDCALL _snowflake_stmt_reset(SF_STMT *sfstmt) {
    clear_snowflake_error(&sfstmt->error);

    strncpy(sfstmt->sfqid, "", SF_UUID4_LEN);
    sfstmt->request_id[0] = '\0';

    if (sfstmt->sql_text) {
        SF_FREE(sfstmt->sql_text); /* SQL */
    }
    sfstmt->sql_text = NULL;

    if (sfstmt->raw_results) {
        cJSON_Delete(sfstmt->raw_results);
        sfstmt->raw_results = NULL;
    }
    sfstmt->raw_results = NULL;

    if (sfstmt->params) {
        sf_array_list_deallocate(sfstmt->params); /* binding parameters */
    }
    sfstmt->params = NULL;

    if (sfstmt->results) {
        sf_array_list_deallocate(sfstmt->results); /* binding columns */
    }
    sfstmt->results = NULL;

    _snowflake_stmt_desc_reset(sfstmt);

    if (sfstmt->stmt_attrs) {
        sf_array_list_deallocate(sfstmt->stmt_attrs);
    }
    sfstmt->stmt_attrs = NULL;

    /* clear error handle */
    clear_snowflake_error(&sfstmt->error);

    sfstmt->chunk_rowcount = -1;
    sfstmt->total_rowcount = -1;
    sfstmt->total_fieldcount = -1;
    sfstmt->total_row_index = -1;

    // Destroy chunk downloader
    chunk_downloader_term(sfstmt->chunk_downloader);
    sfstmt->chunk_downloader = NULL;

    if (sfstmt->put_get_response) {
        // clean up put get response data
        sf_put_get_response_deallocate(sfstmt->put_get_response);
        sfstmt->put_get_response = NULL;
    }
}

SF_PUT_GET_RESPONSE *STDCALL sf_put_get_response_allocate() {
    SF_PUT_GET_RESPONSE *sf_put_get_response = (SF_PUT_GET_RESPONSE *)
        SF_CALLOC(1, sizeof(SF_PUT_GET_RESPONSE));

    SF_STAGE_CRED *sf_stage_cred = (SF_STAGE_CRED *) SF_CALLOC(1,
                                                               sizeof(SF_STAGE_CRED));

    SF_STAGE_INFO *sf_stage_info = (SF_STAGE_INFO *) SF_CALLOC(1,
                                                               sizeof(SF_STAGE_INFO));

    SF_ENC_MAT *sf_enc_mat = (SF_ENC_MAT *) SF_CALLOC(1,
                                                      sizeof(SF_ENC_MAT));

    sf_stage_info->stage_cred = sf_stage_cred;
    sf_put_get_response->stage_info = sf_stage_info;
    sf_put_get_response->enc_mat = sf_enc_mat;

    return sf_put_get_response;
}

void STDCALL
sf_put_get_response_deallocate(SF_PUT_GET_RESPONSE *put_get_response) {
    SF_FREE(put_get_response->stage_info->stage_cred->aws_key_id);
    SF_FREE(put_get_response->stage_info->stage_cred->aws_secret_key);
    SF_FREE(put_get_response->stage_info->stage_cred->aws_token);
    SF_FREE(put_get_response->stage_info->stage_cred);
    SF_FREE(put_get_response->stage_info->location_type);
    SF_FREE(put_get_response->stage_info->location);
    SF_FREE(put_get_response->stage_info->path);
    SF_FREE(put_get_response->stage_info->region);
    SF_FREE(put_get_response->stage_info);
    SF_FREE(put_get_response->enc_mat->query_stage_master_key);
    SF_FREE(put_get_response->enc_mat);

    cJSON_Delete((cJSON *) put_get_response->src_list);

    SF_FREE(put_get_response);
}


SF_STMT *STDCALL snowflake_stmt(SF_CONNECT *sf) {
    if (!sf) {
        return NULL;
    }

    SF_STMT *sfstmt = (SF_STMT *) SF_CALLOC(1, sizeof(SF_STMT));
    if (sfstmt) {
        _snowflake_stmt_reset(sfstmt);
        sfstmt->connection = sf;
    }
    return sfstmt;
}

void STDCALL snowflake_stmt_term(SF_STMT *sfstmt) {
    if (sfstmt) {
        _snowflake_stmt_reset(sfstmt);
        SF_FREE(sfstmt);
    }
}

SF_STATUS STDCALL snowflake_bind_param(
    SF_STMT *sfstmt, SF_BIND_INPUT *sfbind) {
    if (!sfstmt) {
        return SF_STATUS_ERROR_STATEMENT_NOT_EXIST;
    }
    clear_snowflake_error(&sfstmt->error);
    if (sfstmt->params == NULL) {
        sfstmt->params = sf_array_list_init();
    }
    sf_array_list_set(sfstmt->params, sfbind, sfbind->idx);
    return SF_STATUS_SUCCESS;
}

SF_STATUS snowflake_bind_param_array(
    SF_STMT *sfstmt, SF_BIND_INPUT *sfbind_array, size_t size) {
    size_t i;
    if (!sfstmt) {
        return SF_STATUS_ERROR_STATEMENT_NOT_EXIST;
    }
    clear_snowflake_error(&sfstmt->error);
    if (sfstmt->params == NULL) {
        sfstmt->params = sf_array_list_init();
    }
    for (i = 0; i < size; i++) {
        sf_array_list_set(sfstmt->params, &sfbind_array[i],
                          sfbind_array[i].idx);
    }
    return SF_STATUS_SUCCESS;
}

SF_STATUS STDCALL snowflake_bind_result(
    SF_STMT *sfstmt, SF_BIND_OUTPUT *sfbind) {
    if (!sfstmt) {
        return SF_STATUS_ERROR_STATEMENT_NOT_EXIST;
    }
    clear_snowflake_error(&sfstmt->error);
    if (sfstmt->results == NULL) {
        sfstmt->results = sf_array_list_init();
    }
    sf_array_list_set(sfstmt->results, sfbind, sfbind->idx);
    return SF_STATUS_SUCCESS;
}

SF_STATUS snowflake_bind_result_array(
    SF_STMT *sfstmt, SF_BIND_OUTPUT *sfbind_array, size_t size) {
    size_t i;
    if (!sfstmt) {
        return SF_STATUS_ERROR_STATEMENT_NOT_EXIST;
    }
    clear_snowflake_error(&sfstmt->error);
    if (sfstmt->results == NULL) {
        sfstmt->results = sf_array_list_init();
    }
    for (i = 0; i < size; i++) {
        sf_array_list_set(sfstmt->results, &sfbind_array[i],
                          sfbind_array[i].idx);
    }
    return SF_STATUS_SUCCESS;
}

SF_STATUS STDCALL snowflake_query(
    SF_STMT *sfstmt, const char *command, size_t command_size) {
    if (!sfstmt) {
        return SF_STATUS_ERROR_STATEMENT_NOT_EXIST;
    }
    clear_snowflake_error(&sfstmt->error);
    SF_STATUS ret = snowflake_prepare(sfstmt, command, command_size);
    if (ret != SF_STATUS_SUCCESS) {
        return ret;
    }
    ret = snowflake_execute(sfstmt);
    if (ret != SF_STATUS_SUCCESS) {
        return ret;
    }
    return SF_STATUS_SUCCESS;
}

SF_STATUS STDCALL snowflake_fetch(SF_STMT *sfstmt) {
    if (!sfstmt) {
        return SF_STATUS_ERROR_STATEMENT_NOT_EXIST;
    }
    clear_snowflake_error(&sfstmt->error);
    SF_STATUS ret = SF_STATUS_ERROR_GENERAL;
    sf_bool get_chunk_success = SF_BOOLEAN_TRUE;
    int64 i;
    uint64 index;
    cJSON *row = NULL;
    cJSON *raw_result;
    SF_BIND_OUTPUT *result;

    // Check for chunk_downloader error
    if (sfstmt->chunk_downloader && get_error(sfstmt->chunk_downloader)) {
        goto cleanup;
    }

    // If no more results, set return to SF_STATUS_EOF
    if (sfstmt->chunk_rowcount == 0) {
        if (sfstmt->chunk_downloader) {
            log_debug("Fetching next chunk from chunk downloader.");
            _critical_section_lock(&sfstmt->chunk_downloader->queue_lock);
            do {
                if (sfstmt->chunk_downloader->consumer_head >=
                    sfstmt->chunk_downloader->queue_size) {
                    // No more chunks, set EOL and break
                    log_debug("Out of chunks, setting EOL.");
                    cJSON_Delete(sfstmt->raw_results);
                    sfstmt->raw_results = NULL;
                    ret = SF_STATUS_EOF;
                    break;
                } else {
                    // Get index and increment
                    index = sfstmt->chunk_downloader->consumer_head;
                    while (
                        sfstmt->chunk_downloader->queue[index].chunk == NULL &&
                        !get_shutdown_or_error(
                            sfstmt->chunk_downloader)) {
                        _cond_wait(
                            &sfstmt->chunk_downloader->consumer_cond,
                            &sfstmt->chunk_downloader->queue_lock);
                    }

                    if (get_error(sfstmt->chunk_downloader)) {
                        get_chunk_success = SF_BOOLEAN_FALSE;
                        break;
                    } else if (get_shutdown(sfstmt->chunk_downloader)) {
                        get_chunk_success = SF_BOOLEAN_FALSE;
                        break;
                    }

                    sfstmt->chunk_downloader->consumer_head++;

                    // Delete old cJSON results struct
                    cJSON_Delete((cJSON *) sfstmt->raw_results);
                    // Set new chunk and remove chunk reference from locked array
                    sfstmt->raw_results = sfstmt->chunk_downloader->queue[index].chunk;
                    sfstmt->chunk_downloader->queue[index].chunk = NULL;
                    sfstmt->chunk_rowcount = sfstmt->chunk_downloader->queue[index].row_count;
                    log_debug("Acquired chunk %llu from chunk downloader",
                              index);
                    if (_cond_signal(
                        &sfstmt->chunk_downloader->producer_cond)) {
                        SET_SNOWFLAKE_ERROR(&sfstmt->error,
                                            SF_STATUS_ERROR_PTHREAD,
                                            "Unable to send signal using produce_cond",
                                            "");
                        get_chunk_success = SF_BOOLEAN_FALSE;
                        break;
                    }
                }
            }
            while (0);
            _critical_section_unlock(&sfstmt->chunk_downloader->queue_lock);
        } else {
            // If there is no chunk downloader set, then we've truly reached the end of the results and should set EOL
            log_debug("No chunk downloader set, end of results.");
            ret = SF_STATUS_EOF;
        }

        // If we've reached the end, or we have an error getting the next chunk, goto cleanup and return status
        if (ret == SF_STATUS_EOF || !get_chunk_success) {
            goto cleanup;
        }
    }

    // Check that we can write to the provided result bindings
    for (i = 0; i < sfstmt->total_fieldcount; i++) {
        result = sf_array_list_get(sfstmt->results, (size_t) i + 1);
        if (result == NULL) {
            continue;
        } else {
            if (result->c_type != sfstmt->desc[i].c_type &&
                result->c_type != SF_C_TYPE_STRING) {
                // TODO add error msg
                goto cleanup;
            }
        }
    }

    // Get next result row
    row = cJSON_DetachItemFromArray(sfstmt->raw_results, 0);
    sfstmt->chunk_rowcount--;

    // Write to results
    for (i = 0; i < sfstmt->total_fieldcount; i++) {
        result = sf_array_list_get(sfstmt->results, (size_t) i + 1);
        log_info("snowflake type: %s, C type: %s",
                 snowflake_type_to_string(sfstmt->desc[i].type),
                 result != NULL ?
                 snowflake_c_type_to_string(result->c_type) : NULL);
        if (result == NULL) {
            // not bound. skipping
            continue;
        }
        time_t sec = 0L;
        struct tm tm_obj;
        struct tm *tm_ptr;
        size_t slen = (size_t)0;
        memset(&tm_obj, 0, sizeof(tm_obj));

        raw_result = cJSON_GetArrayItem(row, (int) i);
        if (raw_result->valuestring == NULL) {
            log_info("setting value and len = NULL");
            ((char *) result->value)[0] = '\0'; // empty string
            result->len = (size_t) 0;
            result->is_null = SF_BOOLEAN_TRUE;
            continue;
        }
        result->is_null = SF_BOOLEAN_FALSE;
        switch (result->c_type) {
            case SF_C_TYPE_INT8:
                switch (sfstmt->desc[i].type) {
                    case SF_TYPE_BOOLEAN:
                        *(int8 *) result->value = cJSON_IsTrue(raw_result)
                                                  ? SF_BOOLEAN_TRUE
                                                  : SF_BOOLEAN_FALSE;
                        break;
                    default:
                        // field is a char?
                        *(int8 *) result->value = (int8) raw_result->valuestring[0];
                        break;
                }
                result->len = sizeof(int8);
                break;
            case SF_C_TYPE_UINT8:
                *(uint8 *) result->value = (uint8) raw_result->valuestring[0];
                result->len = sizeof(uint8);
                break;
            case SF_C_TYPE_INT64:
                *(int64 *) result->value = (int64) strtoll(
                    raw_result->valuestring, NULL, 10);
                result->len = sizeof(int64);
                break;
            case SF_C_TYPE_UINT64:
                *(uint64 *) result->value = (uint64) strtoull(
                    raw_result->valuestring, NULL, 10);
                result->len = sizeof(uint64);
                break;
            case SF_C_TYPE_FLOAT64:
                *(float64 *) result->value = (float64) strtod(
                    raw_result->valuestring, NULL);
                result->len = sizeof(float64);
                break;
            case SF_C_TYPE_STRING:
                switch (sfstmt->desc[i].type) {
                    case SF_TYPE_BOOLEAN:
                        log_info("value: %p, max_length: %lld, len: %lld",
                                 result->value, result->max_length,
                                 result->len);
                        if (strcmp(raw_result->valuestring, "0") == 0) {
                            /* False */
                            strncpy(result->value, SF_BOOLEAN_FALSE_STR,
                                    result->max_length);
                            result->len = sizeof(SF_BOOLEAN_FALSE_STR) - 1;
                        } else {
                            /* True */
                            strncpy(result->value, SF_BOOLEAN_TRUE_STR,
                                    result->max_length);
                            result->len = sizeof(SF_BOOLEAN_TRUE_STR) - 1;
                        }
                        break;
                    case SF_TYPE_DATE:
                        sec =
                            (time_t) strtol(raw_result->valuestring, NULL, 10) *
                            86400L;
                        _mutex_lock(&gmlocaltime_lock);
                        tm_ptr = sf_gmtime(&sec, &tm_obj);
                        _mutex_unlock(&gmlocaltime_lock);
                        if (tm_ptr == NULL) {
                            SET_SNOWFLAKE_ERROR(&sfstmt->error,
                                                SF_STATUS_ERROR_CONVERSION_FAILURE,
                                                "Failed to convert a date value to a string.",
                                                SF_SQLSTATE_GENERAL_ERROR);
                            result->len = 0;
                            goto cleanup;
                        }
                        result->len = strftime(
                            result->value, result->max_length,
                            "%Y-%m-%d", &tm_obj);
                        break;
                    case SF_TYPE_TIME:
                    case SF_TYPE_TIMESTAMP_NTZ:
                    case SF_TYPE_TIMESTAMP_LTZ:
                    case SF_TYPE_TIMESTAMP_TZ:
                        if (!_extract_timestamp(
                            result,
                            sfstmt->desc[i].type,
                            raw_result->valuestring,
                            sfstmt->connection->timezone,
                            sfstmt->desc[i].scale)) {
                            SET_SNOWFLAKE_ERROR(&sfstmt->error,
                                                SF_STATUS_ERROR_CONVERSION_FAILURE,
                                                "Failed to convert a timestamp value to a string.",
                                                SF_SQLSTATE_GENERAL_ERROR);
                            result->len = 0;
                            goto cleanup;
                        }
                        break;
                    default:
                        slen = strlen(raw_result->valuestring);
                        log_debug("slen: %llu, buflen: %llu:%llu, realloc: %p",
                                     (unsigned long)slen,
                                     result->max_length,
                                     result->len,
                                     (void*)sfstmt->user_realloc_func);
                        result->len = slen;
                        if (sfstmt->user_realloc_func != NULL &&
                            slen > result->max_length) {
                            // reallocate the result buffer if the output
                            // buffer size is not large enough.
                            result->value = sfstmt->user_realloc_func(
                                result->value, slen + 1);
                            result->max_length = slen + 1;
                        }
                        strncpy(result->value, raw_result->valuestring,
                                result->max_length);
                        break;
                }
                break;
            case SF_C_TYPE_BOOLEAN:
                *(sf_bool *) result->value = cJSON_IsTrue(raw_result)
                                             ? SF_BOOLEAN_TRUE
                                             : SF_BOOLEAN_FALSE;
                result->len = sizeof(sf_bool);
                break;
            case SF_C_TYPE_TIMESTAMP:
                /* TODO: may need Snowflake TIMESTAMP struct like super set of strust tm */
                break;
            default:
                break;
        }
    }

    ret = SF_STATUS_SUCCESS;

cleanup:
    cJSON_Delete(row);
    return ret;
}

static SF_STATUS STDCALL
_snowflake_internal_query(SF_CONNECT *sf, const char *sql) {
    if (!sf) {
        return SF_STATUS_ERROR_CONNECTION_NOT_EXIST;
    }
    SF_STATUS ret;
    SF_STMT *sfstmt = snowflake_stmt(sf);
    if (sfstmt == NULL) {
        ret = SF_STATUS_ERROR_OUT_OF_MEMORY;
        SET_SNOWFLAKE_ERROR(
            &sf->error,
            SF_STATUS_ERROR_OUT_OF_MEMORY,
            "Out of memory in creating SF_STMT. ",
            SF_SQLSTATE_UNABLE_TO_CONNECT);
        goto error;
    }
    ret = snowflake_query(sfstmt, sql, 0);
    if (ret != SF_STATUS_SUCCESS) {
        snowflake_propagate_error(sf, sfstmt);
        goto error;
    }

error:
    snowflake_stmt_term(sfstmt);
    return ret;
}

SF_STATUS STDCALL snowflake_trans_begin(SF_CONNECT *sf) {
    return _snowflake_internal_query(sf, _SF_STMT_SQL_BEGIN);
}

SF_STATUS STDCALL snowflake_trans_commit(SF_CONNECT *sf) {
    return _snowflake_internal_query(sf, _SF_STMT_SQL_COMMIT);
}

SF_STATUS STDCALL snowflake_trans_rollback(SF_CONNECT *sf) {
    return _snowflake_internal_query(sf, _SF_STMT_SQL_ROLLBACK);
}

int64 STDCALL snowflake_affected_rows(SF_STMT *sfstmt) {
    size_t i;
    int64 ret = -1;
    cJSON *row = NULL;
    cJSON *raw_row_result;
    clear_snowflake_error(&sfstmt->error);
    if (!sfstmt) {
        /* no way to set the error other than return value */
        return ret;
    }
    if (cJSON_GetArraySize(sfstmt->raw_results) == 0) {
        /* no affected rows is determined. The potential cause is
         * the query is not DML or no stmt was executed at all . */
        SET_SNOWFLAKE_STMT_ERROR(
            &sfstmt->error,
            SF_STATUS_ERROR_APPLICATION_ERROR, "No results found.",
            SF_SQLSTATE_NO_DATA, sfstmt->sfqid);
        sfstmt->error.error_code = SF_STATUS_ERROR_APPLICATION_ERROR;
        return ret;
    }

    if (sfstmt->is_dml) {
        row = cJSON_DetachItemFromArray(sfstmt->raw_results, 0);
        ret = 0;
        for (i = 0; i < (size_t) sfstmt->total_fieldcount; ++i) {
            raw_row_result = cJSON_GetArrayItem(row, (int) i);
            ret += (int64) strtoll(raw_row_result->valuestring, NULL, 10);
        }
        cJSON_Delete(row);
    } else {
        ret = sfstmt->total_rowcount;
    }
    return ret;
}

SF_STATUS STDCALL
snowflake_prepare(SF_STMT *sfstmt, const char *command, size_t command_size) {
    if (!sfstmt) {
        return SF_STATUS_ERROR_STATEMENT_NOT_EXIST;
    }
    clear_snowflake_error(&sfstmt->error);
    SF_STATUS ret = SF_STATUS_ERROR_GENERAL;
    size_t sql_text_size = 1; // Don't forget about null terminator
    if (!command) {
        goto cleanup;
    }
    _snowflake_stmt_reset(sfstmt);
    // Set sql_text to command
    if (command_size == 0) {
        log_debug("Command size is 0, using to strlen to find query length.");
        sql_text_size += strlen(command);
    } else {
        log_debug("Command size non-zero, setting as sql text size.");
        sql_text_size += command_size;
    }
    sfstmt->sql_text = (char *) SF_CALLOC(1, sql_text_size);
    memcpy(sfstmt->sql_text, command, sql_text_size - 1);
    // Null terminate
    sfstmt->sql_text[sql_text_size - 1] = '\0';

    ret = SF_STATUS_SUCCESS;

cleanup:
    return ret;
}

SF_STATUS STDCALL snowflake_execute(SF_STMT *sfstmt) {
    return _snowflake_execute_ex(sfstmt, _is_put_get_command(sfstmt->sql_text));
}

SF_STATUS STDCALL _snowflake_execute_ex(SF_STMT *sfstmt,
                                        sf_bool is_put_get_command) {
    if (!sfstmt) {
        return SF_STATUS_ERROR_STATEMENT_NOT_EXIST;
    }
    clear_snowflake_error(&sfstmt->error);
    SF_STATUS ret = SF_STATUS_ERROR_GENERAL;
    SF_JSON_ERROR json_error;
    const char *error_msg;
    cJSON *body = NULL;
    cJSON *data = NULL;
    cJSON *rowtype = NULL;
    cJSON *resp = NULL;
    cJSON *chunks = NULL;
    cJSON *chunk_headers = NULL;
    char *qrmk = NULL;
    char *s_body = NULL;
    char *s_resp = NULL;
    sf_bool success = SF_BOOLEAN_FALSE;
    uuid4_generate(sfstmt->request_id);
    URL_KEY_VALUE url_params[] = {
        {.key="requestId=", .value=sfstmt->request_id, .formatted_key=NULL, .formatted_value=NULL, .key_size=0, .value_size=0}
    };
    size_t i;
    cJSON *bindings = NULL;
    SF_BIND_INPUT *input;
    const char *type;
    char *value;

    _mutex_lock(&sfstmt->connection->mutex_sequence_counter);
    sfstmt->sequence_counter = ++sfstmt->connection->sequence_counter;
    _mutex_unlock(&sfstmt->connection->mutex_sequence_counter);

    // TODO Do error handing and checking and stuff
    ARRAY_LIST *p = (ARRAY_LIST *) sfstmt->params;
    if (p && p->used > 0) {
        bindings = cJSON_CreateObject();
        for (i = 0; i < p->used; i++) {
            cJSON *binding;
            input = (SF_BIND_INPUT *) sf_array_list_get(p, i + 1);
            if (input == NULL) {
                continue;
            }
            // TODO check if input is null and either set error or write msg to log
            type = snowflake_type_to_string(
                c_type_to_snowflake(input->c_type, SF_TYPE_TIMESTAMP_NTZ));
            value = value_to_string(input->value, input->len, input->c_type);
            binding = cJSON_CreateObject();
            char idxbuf[20];
            sprintf(idxbuf, "%lu", (unsigned long) (i + 1));
            cJSON_AddStringToObject(binding, "type", type);
            cJSON_AddStringToObject(binding, "value", value);
            cJSON_AddItemToObject(bindings, idxbuf, binding);
            if (value) {
                SF_FREE(value);
            }
        }
    }

    if (is_string_empty(sfstmt->connection->master_token) ||
        is_string_empty(sfstmt->connection->token)) {
        log_error(
            "Missing session token or Master token. Are you sure that snowflake_connect was successful?");
        SET_SNOWFLAKE_ERROR(&sfstmt->error,
                            SF_STATUS_ERROR_BAD_CONNECTION_PARAMS,
                            "Missing session or master token. Try running snowflake_connect.",
                            SF_SQLSTATE_UNABLE_TO_CONNECT);
        goto cleanup;
    }

    // Create Body
    body = create_query_json_body(sfstmt->sql_text, sfstmt->sequence_counter);
    if (bindings != NULL) {
        /* binding parameters if exists */
        cJSON_AddItemToObject(body, "bindings", bindings);
    }
    s_body = cJSON_Print(body);
    log_debug("Created body");
    log_trace("Here is constructed body:\n%s", s_body);

    if (request(sfstmt->connection, &resp, QUERY_URL, url_params,
                sizeof(url_params) / sizeof(URL_KEY_VALUE), s_body, NULL,
                POST_REQUEST_TYPE, &sfstmt->error, is_put_get_command)) {
        s_resp = cJSON_Print(resp);
        log_trace("Here is JSON response:\n%s", s_resp);
        data = cJSON_GetObjectItem(resp, "data");
        if (json_copy_string_no_alloc(sfstmt->sfqid, data, "queryId",
                                      SF_UUID4_LEN) && !is_put_get_command) {
            log_debug("No valid sfqid found in response");
        }
        if ((json_error = json_copy_bool(&success, resp, "success")) ==
            SF_JSON_ERROR_NONE && success) {
            if (is_put_get_command) {
                sfstmt->put_get_response = sf_put_get_response_allocate();

                json_detach_array_from_object(
                    (cJSON **) (&sfstmt->put_get_response->src_list),
                    data, "src_locations");
                json_copy_string_no_alloc(sfstmt->put_get_response->command,
                                          data, "command", SF_COMMAND_LEN);
                json_copy_int(&sfstmt->put_get_response->parallel, data,
                              "parallel");
                json_copy_bool(&sfstmt->put_get_response->auto_compress, data,
                               "autoCompress");
                json_copy_bool(&sfstmt->put_get_response->overwrite, data,
                               "overwrite");
                json_copy_string_no_alloc(
                    sfstmt->put_get_response->source_compression,
                    data, "sourceCompression",
                    SF_SOURCE_COMPRESSION_TYPE_LEN);
                json_copy_bool(
                    &sfstmt->put_get_response->client_show_encryption_param,
                    data, "clientShowEncryptionParameter");

                cJSON *enc_mat = cJSON_GetObjectItem(data,
                                                     "encryptionMaterial");
                json_copy_string(
                    &sfstmt->put_get_response->enc_mat->query_stage_master_key,
                    enc_mat, "queryStageMasterKey");
                json_copy_string_no_alloc(
                    sfstmt->put_get_response->enc_mat->query_id,
                    enc_mat, "queryId", SF_UUID4_LEN);
                json_copy_int(&sfstmt->put_get_response->enc_mat->smk_id,
                              enc_mat, "smkId");

                cJSON *stage_info = cJSON_GetObjectItem(data, "stageInfo");
                cJSON *stage_cred = cJSON_GetObjectItem(stage_info, "creds");

                json_copy_string(
                    &sfstmt->put_get_response->stage_info->location_type,
                    stage_info, "locationType");
                json_copy_string(
                    &sfstmt->put_get_response->stage_info->location,
                    stage_info, "location");
                json_copy_string(&sfstmt->put_get_response->stage_info->path,
                                 stage_info, "path");
                json_copy_string(&sfstmt->put_get_response->stage_info->region,
                                 stage_info, "region");
                json_copy_string(
                    &sfstmt->put_get_response->stage_info->stage_cred->aws_secret_key,
                    stage_cred, "AWS_SECRET_KEY");
                json_copy_string(
                    &sfstmt->put_get_response->stage_info->stage_cred->aws_key_id,
                    stage_cred, "AWS_KEY_ID");
                json_copy_string(
                    &sfstmt->put_get_response->stage_info->stage_cred->aws_token,
                    stage_cred, "AWS_TOKEN");
            } else {
                // Set Database info
                _mutex_lock(&sfstmt->connection->mutex_parameters);
                /* Set other parameters. Ignore the status */
                _set_current_objects(sfstmt, data);
                _set_parameters_session_info(sfstmt->connection, data);
                _mutex_unlock(&sfstmt->connection->mutex_parameters);
                int64 stmt_type_id;
                if (json_copy_int(&stmt_type_id, data, "statementTypeId")) {
                    /* failed to get statement type id */
                    sfstmt->is_dml = SF_BOOLEAN_FALSE;
                } else {
                    sfstmt->is_dml = detect_stmt_type(stmt_type_id);
                }
                rowtype = cJSON_GetObjectItem(data, "rowtype");
                if (cJSON_IsArray(rowtype)) {
                    sfstmt->total_fieldcount = cJSON_GetArraySize(rowtype);
                    _snowflake_stmt_desc_reset(sfstmt);
                    sfstmt->desc = set_description(rowtype);
                }
                // Set results array
                if (json_detach_array_from_object(
                    (cJSON **) (&sfstmt->raw_results),
                    data, "rowset")) {
                    log_error("No valid rowset found in response");
                    SET_SNOWFLAKE_STMT_ERROR(&sfstmt->error,
                                             SF_STATUS_ERROR_BAD_JSON,
                                             "Missing rowset from response. No results found.",
                                             SF_SQLSTATE_APP_REJECT_CONNECTION,
                                             sfstmt->sfqid);
                    goto cleanup;
                }
                if (json_copy_int(&sfstmt->total_rowcount, data, "total")) {
                    log_warn(
                        "No total count found in response. Reverting to using array size of results");
                    sfstmt->total_rowcount = cJSON_GetArraySize(
                        sfstmt->raw_results);
                }
                // Get number of rows in this chunk
                sfstmt->chunk_rowcount = cJSON_GetArraySize(
                    sfstmt->raw_results);

                // Set large result set if one exists
                if ((chunks = cJSON_GetObjectItem(data, "chunks")) != NULL) {
                    // We don't care if there is no qrmk, so ignore return code
                    json_copy_string(&qrmk, data, "qrmk");
                    chunk_headers = cJSON_GetObjectItem(data, "chunkHeaders");
                    sfstmt->chunk_downloader = chunk_downloader_init(
                        qrmk,
                        chunk_headers,
                        chunks,
                        2, // thread count
                        4, // fetch slot
                        &sfstmt->error);
                    if (!sfstmt->chunk_downloader) {
                        // Unable to create chunk downloader. Error is set in chunk_downloader_init function.
                        goto cleanup;
                    }
                }
            }
        } else if (json_error != SF_JSON_ERROR_NONE) {
            JSON_ERROR_MSG(json_error, error_msg, "Success code");
            SET_SNOWFLAKE_STMT_ERROR(
                &sfstmt->error, SF_STATUS_ERROR_BAD_JSON,
                error_msg, SF_SQLSTATE_APP_REJECT_CONNECTION, sfstmt->sfqid);
            goto cleanup;
        } else if (!success) {
            cJSON *messageJson = NULL;
            char *message = NULL;
            cJSON *codeJson = NULL;
            int64 code = -1;
            if (json_copy_string_no_alloc(sfstmt->error.sqlstate, data,
                                          "sqlState", SF_SQLSTATE_LEN)) {
                log_debug("No valid sqlstate found in response");
            }
            messageJson = cJSON_GetObjectItem(resp, "message");
            if (messageJson) {
                message = messageJson->valuestring;
            }
            codeJson = cJSON_GetObjectItem(resp, "code");
            if (codeJson) {
                code = (int64) atol(codeJson->valuestring);
            } else {
                log_debug("no code element.");
            }
            SET_SNOWFLAKE_STMT_ERROR(&sfstmt->error, code,
                                     message ? message
                                             : "Query was not successful",
                                     NULL, sfstmt->sfqid);
            goto cleanup;
        }
    } else {
        log_trace("Connection failed");
    }

    // Everything went well if we got to this point
    ret = SF_STATUS_SUCCESS;

cleanup:
    cJSON_Delete(body);
    cJSON_Delete(resp);
    SF_FREE(s_body);
    SF_FREE(s_resp);
    SF_FREE(qrmk);

    return ret;
}

SF_ERROR_STRUCT *STDCALL snowflake_error(SF_CONNECT *sf) {
    if (!sf) {
        return NULL;
    }
    return &sf->error;
}

SF_ERROR_STRUCT *STDCALL snowflake_stmt_error(SF_STMT *sfstmt) {
    if (!sfstmt) {
        return NULL;
    }
    return &sfstmt->error;
}

uint64 STDCALL snowflake_num_rows(SF_STMT *sfstmt) {
    // TODO fix int vs uint stuff
    if (!sfstmt) {
        // TODO change to -1?
        return 0;
    }
    return (uint64) sfstmt->total_rowcount;
}

uint64 STDCALL snowflake_num_fields(SF_STMT *sfstmt) {
    // TODO fix int vs uint stuff
    if (!sfstmt) {
        // TODO change to -1?
        return 0;
    }
    return (uint64) sfstmt->total_fieldcount;
}

uint64 STDCALL snowflake_num_params(SF_STMT *sfstmt) {
    if (!sfstmt) {
        // TODO change to -1?
        return 0;
    }
    ARRAY_LIST *p = (ARRAY_LIST *) sfstmt->params;
    return p->used;
}

const char *STDCALL snowflake_sfqid(SF_STMT *sfstmt) {
    if (!sfstmt) {
        return NULL;
    }
    return sfstmt->sfqid;
}

const char *STDCALL snowflake_sqlstate(SF_STMT *sfstmt) {
    if (!sfstmt) {
        return NULL;
    }
    return sfstmt->error.sqlstate;
}

SF_COLUMN_DESC *STDCALL snowflake_desc(SF_STMT *sfstmt) {
    if (!sfstmt) {
        return NULL;
    }
    return sfstmt->desc;
}

SF_STATUS STDCALL snowflake_stmt_get_attr(
    SF_STMT *sfstmt, SF_STMT_ATTRIBUTE type, void **value) {
    if (!sfstmt) {
        return SF_STATUS_ERROR_STATEMENT_NOT_EXIST;
    }
    clear_snowflake_error(&sfstmt->error);
    switch(type) {
        case SF_STMT_USER_REALLOC_FUNC:
            *value = sfstmt->user_realloc_func;
            break;
        default:
            SET_SNOWFLAKE_ERROR(
                &sfstmt->error, SF_STATUS_ERROR_BAD_ATTRIBUTE_TYPE,
                "Invalid attribute type",
                SF_SQLSTATE_UNABLE_TO_CONNECT);
            return SF_STATUS_ERROR_APPLICATION_ERROR;
    }
    return SF_STATUS_SUCCESS;
}

SF_STATUS STDCALL snowflake_stmt_set_attr(
    SF_STMT *sfstmt, SF_STMT_ATTRIBUTE type, const void *value) {
    if (!sfstmt) {
        return SF_STATUS_ERROR_STATEMENT_NOT_EXIST;
    }
    clear_snowflake_error(&sfstmt->error);
    switch(type) {
        case SF_STMT_USER_REALLOC_FUNC:
            sfstmt->user_realloc_func = value;
            break;
        default:
            SET_SNOWFLAKE_ERROR(
                &sfstmt->error, SF_STATUS_ERROR_BAD_ATTRIBUTE_TYPE,
                "Invalid attribute type",
                SF_SQLSTATE_UNABLE_TO_CONNECT);
            return SF_STATUS_ERROR_APPLICATION_ERROR;
    }
    return SF_STATUS_SUCCESS;
}

SF_STATUS STDCALL snowflake_propagate_error(SF_CONNECT *sf, SF_STMT *sfstmt) {
    if (!sf) {
        return SF_STATUS_ERROR_CONNECTION_NOT_EXIST;
    }
    if (!sfstmt) {
        return SF_STATUS_ERROR_STATEMENT_NOT_EXIST;
    }
    if (sf->error.error_code) {
        /* if already error is set */
        SF_FREE(sf->error.msg);
    }
    memcpy(&sf->error, &sfstmt->error, sizeof(SF_ERROR_STRUCT));
    if (sfstmt->error.error_code) {
        /* any error */
        size_t len = strlen(sfstmt->error.msg);
        sf->error.msg = SF_CALLOC(len + 1, sizeof(char));
        if (sf->error.msg == NULL) {
            SET_SNOWFLAKE_ERROR(
                &sf->error,
                SF_STATUS_ERROR_OUT_OF_MEMORY,
                "Out of memory in creating a buffer for the error message.",
                SF_SQLSTATE_APP_REJECT_CONNECTION);
        }
        strncpy(sf->error.msg, sfstmt->error.msg, len);
    }
    return SF_STATUS_SUCCESS;
}
