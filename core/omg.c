#include "omg.h"
#include "create_table.h"
#include <curl/curl.h>
#include <jansson.h>
#include <pcre2posix.h>
#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// libcurl related
const char *HEADER_ACCEPT = "Accept: application/vnd.github.v3.star+json";
const char *HEADER_UA = "User-Agent: omg-client/0.1.0";
const char *API_ROOT = "https://api.github.com";
const char *GET_METHOD = "GET";
const char *DELETE_METHOD = "DELETE";

typedef struct {
  char *memory;
  size_t size;
} response;

static void free_response(response *resp) {
  if (resp->memory) {
#ifdef VERBOSE
    printf("free response, body is %s\n", resp->memory);
#endif
    free(resp->memory);
  }
}

static void free_curl_slist(struct curl_slist **lst) {
  if (*lst) {
#ifdef VERBOSE
    printf("free curl list, body is %s\n", (*lst)->data);
#endif
    curl_slist_free_all(*lst);
  }
}

static void free_curl_handler(CURL **curl) {
  if (*curl) {
#ifdef VERBOSE
    printf("free curl handler\n");
#endif
    curl_easy_cleanup(*curl);
  }
}

#define auto_curl_slist                                                        \
  struct curl_slist __attribute__((cleanup(free_curl_slist)))
#define auto_response response __attribute__((cleanup(free_response)))
#define auto_curl CURL __attribute__((cleanup(free_curl_handler)))

static size_t mem_cb(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t realsize = size * nmemb;
  response *mem = (response *)userp;

  char *ptr = realloc(mem->memory, mem->size + realsize + 1);
  if (!ptr) {
    fprintf(stderr, "not enough memory (realloc returned NULL)\n");
    return 0;
  }

  mem->memory = ptr;
  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}

// utils
#ifdef OMG_TEST
const size_t PER_PAGE = 10;
#else
const size_t PER_PAGE = 100;
#endif

const size_t SQL_DEFAULT_LEN = 512;

// matched:
// 1. language
// 2. full_name
// 3. current stats
static const char *const RE =
    "<span itemprop=\"programmingLanguage\">(\\S+)</span>"
    ".*?<a href=\"/(\\S+/\\S+)/stargazers.*?(\\d+) stars this";

void omg_free_char(char **buf) {
  if (*buf) {
#ifdef VERBOSE
    printf("free char %s\n", *buf);
#endif
    free((void *)*buf);
  }
}

static char *strdup_when_not_null(const unsigned char *input) {
  if (input) {
    return strdup((const char *)input);
  }
  return NULL;
}

static void free_stmt(sqlite3_stmt **stmt) {
  if (*stmt) {
#ifdef VERBOSE
    printf("free stmt\n");
#endif
    sqlite3_finalize(*stmt);
  }
}

#define auto_sqlite3_stmt sqlite3_stmt *__attribute__((cleanup(free_stmt)))

// actual omg implementation
struct omg_context {
  sqlite3 *db;
  CURL *curl;
  struct curl_slist *headers;
  regex_t trending_re;
  /* int timeout; */
};

omg_error omg_new_error() {
  return (omg_error){.code = OMG_CODE_OK, .message = NULL};
}

void print_error(omg_error err) {
  printf("code:%d, msg:%s\n", err.code, err.message);
}

bool is_ok(omg_error err) { return err.code == OMG_CODE_OK; }

const omg_error NO_ERROR = {.code = OMG_CODE_OK, .message = NULL};

static bool empty_string(const char *s) {
  if (s) {
    return 0 == strlen(s);
  }
  return true;
}

void omg_free_context(omg_context *ctx) {
  if (*ctx) {
#ifdef VERBOSE
    printf("free omg_context\n");
#endif
    curl_slist_free_all((*ctx)->headers);
    curl_easy_cleanup((*ctx)->curl);
    curl_global_cleanup();
    sqlite3_close((*ctx)->db);
    pcre2_regfree(&(*ctx)->trending_re);
    free(*ctx);
  }
}

static omg_error init_db(const char *root, sqlite3 **out_db) {
  sqlite3 *db = NULL;
  int ret = sqlite3_open(root, &db);
  if (ret) {
    const char *msg = sqlite3_errmsg(db);
    sqlite3_close(db);
    return (omg_error){.code = OMG_CODE_DB, .message = msg};
  }

  char *err_msg = NULL;
  ret = sqlite3_exec(db, (const char *)core_create_table_sql, NULL, NULL,
                     &err_msg);
  if (ret) {
    fprintf(stderr, "exec create table sql failed:%s\n", err_msg);
    sqlite3_free(err_msg);
    return (omg_error){.code = OMG_CODE_DB, .message = "exec sql failed"};
  }

  *out_db = db;
  return NO_ERROR;
}

omg_error omg_setup_context(const char *path, const char *github_token,
                            omg_context *out) {
  curl_global_init(CURL_GLOBAL_ALL);
  CURL *curl = curl_easy_init();
  if (!curl) {
    return (omg_error){.code = OMG_CODE_CURL, .message = "curl init"};
  }
#ifdef CURL_VERBOSE
  curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
#endif
  struct curl_slist *req_headers = NULL;
  char header_auth[128];
  sprintf(header_auth, "Authorization: token %s", github_token);
  req_headers = curl_slist_append(
      req_headers, "Content-Type: application/json; charset=utf-8");
  if (req_headers == NULL) {
    return (omg_error){.code = OMG_CODE_CURL, .message = "init curl header"};
  }
  req_headers = curl_slist_append(req_headers, HEADER_ACCEPT);
  if (req_headers == NULL) {
    curl_slist_free_all(req_headers);
    return (omg_error){.code = OMG_CODE_CURL, .message = "append2 header"};
  }
  req_headers = curl_slist_append(req_headers, header_auth);
  req_headers = curl_slist_append(req_headers, HEADER_UA);
  if (req_headers == NULL) {
    curl_slist_free_all(req_headers);
    return (omg_error){.code = OMG_CODE_CURL, .message = "append3 header"};
  }
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, req_headers);
  sqlite3 *db = NULL;
  omg_error err = init_db(path, &db);
  if (!is_ok(err)) {
    return err;
  }

  regex_t trending_re;
  if (pcre2_regcomp(&trending_re, RE, REG_DOTALL)) {
    return (omg_error){.code = OMG_CODE_INTERNAL,
                       .message = "init trending regexp"};
  }
  omg_context ctx = malloc(sizeof(struct omg_context));
  ctx->db = db;
  ctx->curl = curl;
  ctx->headers = req_headers;
  ctx->trending_re = trending_re;
  *out = ctx;

  return NO_ERROR;
}

static omg_error omg_request(omg_context ctx, const char *method,
                             const char *url, json_t *payload, json_t **out) {
  CURL *curl = ctx->curl;

  curl_easy_setopt(curl, CURLOPT_URL, url);
  if (payload) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_dumps(payload, 0));
  } else {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
  }
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);

  auto_response chunk = {.memory = malloc(1), .size = 0};
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    return (omg_error){.code = OMG_CODE_CURL,
                       .message = curl_easy_strerror(res)};
  }

  long response_code;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  static int no_content = 204;
  static int not_modified = 304;
  if (response_code == no_content || response_code == not_modified) {
    return NO_ERROR;
  }

  json_error_t error;
  json_t *resp = json_loads(chunk.memory, JSON_COMPACT, &error);
  if (!resp) {
    return (omg_error){.code = OMG_CODE_JSON, .message = error.text};
  }
  *out = resp;

  return NO_ERROR;
}

static size_t write_data(void *ptr, size_t size, size_t nmemb, void *stream) {
  size_t written = fwrite(ptr, size, nmemb, (FILE *)stream);
  return written;
}

omg_error omg_download(omg_context ctx, const char *url, const char *filename) {
  // create a new curl handler every time, since download maybe called in
  // multiple threads
  auto_curl *curl = curl_easy_init();
  if (!curl) {
    return (omg_error){.code = OMG_CODE_CURL, .message = "curl init"};
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, GET_METHOD);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  FILE *file = fopen(filename, "wb");
  if (!file) {
    return (omg_error){.code = OMG_CODE_INTERNAL,
                       .message = "open file failed"};
  }

  curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
  CURLcode res = curl_easy_perform(curl);
  fclose(file);
  if (res != CURLE_OK) {
    return (omg_error){.code = OMG_CODE_CURL,
                       .message = curl_easy_strerror(res)};
  }

  long response_code;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  if (response_code >= 400) {
    fprintf(stderr, "Download %s failed with %ld", filename, response_code);
    return (omg_error){.code = OMG_CODE_CURL,
                       .message = "download file failed"};
  }

  return NO_ERROR;
}

/****************/
/* GitHub repos */
/****************/
#define FREE_OBJ_FIELD(obj, field)                                             \
  ({                                                                           \
    if ((obj)->field) {                                                        \
      free((void *)(obj)->field);                                              \
    }                                                                          \
  })

omg_repo omg_new_repo() {
  return (omg_repo){
      .full_name = NULL,
      .description = NULL,
      .created_at = NULL,
      .license = NULL,
      .pushed_at = NULL,
      .lang = NULL,
      .homepage = NULL,
  };
}

void omg_free_repo(omg_repo *repo) {
  if (repo) {
#ifdef VERBOSE
    printf("free omg_repo, name is %s\n", repo->full_name);
#endif
    FREE_OBJ_FIELD(repo, full_name);
    FREE_OBJ_FIELD(repo, description);
    FREE_OBJ_FIELD(repo, created_at);
    FREE_OBJ_FIELD(repo, license);
    FREE_OBJ_FIELD(repo, pushed_at);
    FREE_OBJ_FIELD(repo, lang);
    FREE_OBJ_FIELD(repo, homepage);
  }
}

omg_repo_list omg_new_repo_list() {
  return (omg_repo_list){.repo_array = NULL, .length = 0};
}

void omg_free_repo_list(omg_repo_list *repo_lst) {
  if (repo_lst) {
#ifdef VERBOSE
    printf("free omg_repo_list, length is %zu\n", repo_lst->length);
#endif

    for (size_t i = 0; i < repo_lst->length; i++) {
      omg_free_repo(&(repo_lst->repo_array[i]));
    }
    free(repo_lst->repo_array);
  }
}

static char *dup_json_string(json_t *root, const char *key) {
  json_t *v = json_object_get(root, key);
  if (json_is_null(v)) {
    return NULL;
  } else {
    return strdup(json_string_value(v));
  }
}

static omg_repo repo_from_json(json_t *root) {
  json_t *license = json_object_get(root, "license");
  char *license_key = NULL;
  if (!json_is_null(license)) {
    license_key = dup_json_string(license, "key");
  }
  return (omg_repo){
      .id = json_integer_value(json_object_get(root, "id")),
      .full_name = dup_json_string(root, "full_name"),
      .description = dup_json_string(root, "description"),
      .private = json_boolean_value(json_object_get(root, "private")),
      .created_at = dup_json_string(root, "created_at"),
      .license = license_key,
      .pushed_at = dup_json_string(root, "pushed_at"),
      .stargazers_count =
          json_integer_value(json_object_get(root, "stargazers_count")),
      .watchers_count =
          json_integer_value(json_object_get(root, "watchers_count")),
      .forks_count = json_integer_value(json_object_get(root, "forks_count")),
      .lang = dup_json_string(root, "language"),
      .homepage = dup_json_string(root, "homepage"),
      .size = json_integer_value(json_object_get(root, "size")),
  };
}

static omg_error save_repos(omg_context ctx, omg_repo_list repo_lst) {
  const char *sql =
      "INSERT INTO omg_repo (id, full_name, description, private, "
      "created_at, license, pushed_at, stargazers_count, watchers_count, "
      "forks_count, lang, homepage, `size`) "
      "   VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)    "
      "ON CONFLICT (id)                                                      "
      "   DO UPDATE SET "
      "   full_name=?2, description=?3, private =?4,             "
      "   created_at=?5, license=?6, pushed_at=?7, "
      "   stargazers_count=?8, watchers_count=?9,forks_count=?10, "
      "   lang=?11, homepage=?12, `size`=?13 ";

  auto_sqlite3_stmt stmt = NULL;
  int rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
  if (rc) {
    return (omg_error){.code = OMG_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }
  for (int i = 0; i < repo_lst.length; i++) {
    omg_repo repo = repo_lst.repo_array[i];
    int column = 1;
    sqlite3_bind_int(stmt, column++, repo.id);
    sqlite3_bind_text(stmt, column++, repo.full_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, column++, repo.description, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, column++, repo.private);
    sqlite3_bind_text(stmt, column++, repo.created_at, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, column++, repo.license, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, column++, repo.pushed_at, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, column++, repo.stargazers_count);
    sqlite3_bind_int(stmt, column++, repo.watchers_count);
    sqlite3_bind_int(stmt, column++, repo.forks_count);
    sqlite3_bind_text(stmt, column++, repo.lang, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, column++, repo.homepage, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, column++, repo.size);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
      fprintf(stderr, "insert %s failed. code:%d, msg:%s\n", repo.full_name, rc,
              sqlite3_errmsg(ctx->db));
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
  }

  return NO_ERROR;
}

static omg_error fetch_repos_by_page(omg_context ctx, size_t page_num,
                                     omg_repo_list *out) {
  char url[128];
  sprintf(url, "%s/user/repos?type=all&per_page=%zu&page=%zu&sort=created",
          API_ROOT, PER_PAGE, page_num);
  json_auto_t *resp = NULL;
  omg_error err = omg_request(ctx, GET_METHOD, url, NULL, &resp);
  if (!is_ok(err)) {
    return err;
  }

  size_t resp_len = json_array_size(resp);
  omg_repo *repos = malloc(sizeof(omg_repo) * resp_len);
  for (size_t i = 0; i < resp_len; i++) {
    repos[i] = repo_from_json(json_array_get(resp, i));
  }
  *out = (omg_repo_list){.repo_array = repos, .length = resp_len};
  return NO_ERROR;
}

static omg_error save_my_repos(omg_context ctx, omg_repo_list repo_lst) {
  omg_error err = save_repos(ctx, repo_lst);
  if (!is_ok(err)) {
    return err;
  }

  auto_sqlite3_stmt stmt = NULL;
  const char *sql = "insert or ignore into omg_my_repo(repo_id) values (?1)";
  int rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
  if (rc) {
    return (omg_error){.code = OMG_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }

  for (int i = 0; i < repo_lst.length; i++) {
    omg_repo repo = repo_lst.repo_array[i];
    sqlite3_bind_int(stmt, 1, repo.id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
      fprintf(stderr, "insert %s failed. code:%d, msg:%s\n", repo.full_name, rc,
              sqlite3_errmsg(ctx->db));
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
  }

  return NO_ERROR;
}

omg_error omg_sync_repos(omg_context ctx) {
  size_t page_num = 1;
  while (true) {
#ifdef OMG_TEST
    if (page_num > 2) {
      break;
    }
#endif
    omg_auto_repo_list repo_lst = omg_new_repo_list();
    omg_error err = fetch_repos_by_page(ctx, page_num++, &repo_lst);
    if (!is_ok(err)) {
      return err;
    }
    err = save_my_repos(ctx, repo_lst);
    if (!is_ok(err)) {
      return err;
    }

    if (repo_lst.length < PER_PAGE) {
      break;
    }
  };

  return NO_ERROR;
}

static omg_error prepare_query_sql(omg_context ctx, bool is_star,
                                   const char *keyword, const char *language,
                                   sqlite3_stmt **out) {
  omg_auto_char sql = malloc(SQL_DEFAULT_LEN);
  const char *first_column =
      is_star ? "datetime(starred_at, 'localtime') as starred_at"
              : "1"; // placehold
  const char *table_name = is_star ? "omg_my_star_view" : "omg_my_repo_view";
  sprintf(sql,
          "select %s,"
          "id,full_name,description,private,"
          "datetime(created_at, 'localtime'),"
          "license,"
          "datetime(pushed_at, 'localtime'),"
          "stargazers_count,watchers_count,forks_count,lang,homepage,`size` "
          "from %s where 1",
          first_column, table_name);

  if (!empty_string(keyword)) {
    int current_len = sprintf(sql,
                              "%s and (full_name like '%%%s%%' COLLATE NOCASE "
                              "or description like '%%%s%%' COLLATE NOCASE)",
                              sql, keyword, keyword);
    if (current_len > SQL_DEFAULT_LEN) {
      fprintf(stderr, "sql:%s, keyword:%s\n", sql, keyword);
      return (omg_error){.code = OMG_CODE_INTERNAL,
                         .message = "buffer not enough when append keyword"};
    }
  }

  if (!empty_string(language)) {
    int current_len =
        sprintf(sql, "%s and lang='%s' COLLATE NOCASE", sql, language);
    if (current_len > SQL_DEFAULT_LEN) {
      fprintf(stderr, "sql:%s, lang:%s\n", sql, language);
      return (omg_error){.code = OMG_CODE_INTERNAL,
                         .message = "buffer not enough when append language"};
    }
  }

  const char *sort_column = is_star ? "starred_at" : "created_at";
  int current_len = sprintf(sql, "%s order by %s desc", sql, sort_column);
  if (current_len > SQL_DEFAULT_LEN) {
    fprintf(stderr, "sql:%s, current:%d, len:%zu\n", sql, current_len,
            strlen(sql));
    return (omg_error){.code = OMG_CODE_INTERNAL,
                       .message = "buffer not enough when append order_by"};
  }
#ifdef VERBOSE
  printf("query sql:%s\n", sql);
#endif

  int rc = sqlite3_prepare_v2(ctx->db, sql, strlen(sql), out, NULL);
  if (rc) {
    return (omg_error){.code = OMG_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }

  return NO_ERROR;
}

static omg_repo repo_from_db(sqlite3_stmt *stmt) {
  omg_repo repo = omg_new_repo();
  int column = 1;
  repo.id = sqlite3_column_int(stmt, column++);
  repo.full_name = strdup_when_not_null(sqlite3_column_text(stmt, column++));
  repo.description = strdup_when_not_null(sqlite3_column_text(stmt, column++));
  repo.private = sqlite3_column_int(stmt, column++);
  repo.created_at = strdup_when_not_null(sqlite3_column_text(stmt, column++));
  repo.license = strdup_when_not_null(sqlite3_column_text(stmt, column++));
  repo.pushed_at = strdup_when_not_null(sqlite3_column_text(stmt, column++));
  repo.stargazers_count = sqlite3_column_int(stmt, column++);
  repo.watchers_count = sqlite3_column_int(stmt, column++);
  repo.forks_count = sqlite3_column_int(stmt, column++);
  repo.lang = strdup_when_not_null(sqlite3_column_text(stmt, column++));
  repo.homepage = strdup_when_not_null(sqlite3_column_text(stmt, column++));
  repo.size = sqlite3_column_int(stmt, column++);
  return repo;
}

omg_error omg_query_repos(omg_context ctx, const char *keyword,
                          const char *language, omg_repo_list *out) {
  auto_sqlite3_stmt stmt = NULL;
  omg_error err = prepare_query_sql(ctx, false, keyword, language, &stmt);
  if (!is_ok(err)) {
    return err;
  }

  size_t rows_count = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    rows_count++;
  }

  omg_repo *repo_arr = malloc(sizeof(omg_repo) * rows_count);
  sqlite3_reset(stmt);
  size_t row = 0;
  int rc = 0;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    repo_arr[row++] = repo_from_db(stmt);
  }
  if (rc != SQLITE_DONE) {
    return (omg_error){.code = OMG_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }

  *out = (omg_repo_list){.repo_array = repo_arr, .length = rows_count};
  return NO_ERROR;
}

/****************/
/* GitHub stars */
/****************/

omg_star omg_new_star() {
  return (omg_star){.starred_at = NULL, .repo = omg_new_repo()};
}

void omg_free_star(omg_star *star) {
  if (star) {
#ifdef VERBOSE
    printf("free omg_star, starred_at is %s\n", star->starred_at);
#endif
    FREE_OBJ_FIELD(star, starred_at);
    omg_free_repo(&(star->repo));
  }
}

omg_star_list omg_new_star_list() {
  return (omg_star_list){.star_array = NULL, .length = 0};
}

void omg_free_star_list(omg_star_list *star_list) {
  if (star_list) {
#ifdef VERBOSE
    printf("free omg_star_list, length is %zu\n", star_list->length);
#endif

    for (size_t i = 0; i < star_list->length; i++) {
      omg_free_star(&(star_list->star_array[i]));
    }
    free(star_list->star_array);
  }
}

static omg_error fetch_stars_by_page(omg_context ctx, size_t page_num,
                                     omg_star_list *out) {
  char url[128];
  sprintf(url, "%s/user/starred?type=all&per_page=%zu&page=%zu", API_ROOT,
          PER_PAGE, page_num);
  json_auto_t *resp = NULL;
  omg_error err = omg_request(ctx, GET_METHOD, url, NULL, &resp);
  if (!is_ok(err)) {
    return err;
  }

  size_t resp_len = json_array_size(resp);
  omg_star *stars = malloc(sizeof(omg_star) * resp_len);
  for (size_t i = 0; i < resp_len; i++) {
    json_t *one_star = json_array_get(resp, i);
    stars[i] =
        (omg_star){.repo = repo_from_json(json_object_get(one_star, "repo")),
                   .starred_at = dup_json_string(one_star, "starred_at")};
  }
  *out = (omg_star_list){.star_array = stars, .length = resp_len};

  return NO_ERROR;
}

omg_error omg_query_stars(omg_context ctx, const char *keyword,
                          const char *language, omg_star_list *out_lst) {
  auto_sqlite3_stmt stmt = NULL;
  omg_error err = prepare_query_sql(ctx, true, keyword, language, &stmt);
  if (!is_ok(err)) {
    return err;
  }

  size_t rows_count = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    rows_count++;
  }

  omg_star *star_arr = malloc(sizeof(omg_star) * rows_count);
  sqlite3_reset(stmt);
  size_t row = 0;
  int rc = 0;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    omg_repo repo = omg_new_repo();
    {
      int column = 1;
      repo.id = sqlite3_column_int(stmt, column++);
      repo.full_name =
          strdup_when_not_null(sqlite3_column_text(stmt, column++));
      repo.description =
          strdup_when_not_null(sqlite3_column_text(stmt, column++));
      repo.private = sqlite3_column_int(stmt, column++);
      repo.created_at =
          strdup_when_not_null(sqlite3_column_text(stmt, column++));
      repo.license = strdup_when_not_null(sqlite3_column_text(stmt, column++));
      repo.pushed_at =
          strdup_when_not_null(sqlite3_column_text(stmt, column++));
      repo.stargazers_count = sqlite3_column_int(stmt, column++);
      repo.watchers_count = sqlite3_column_int(stmt, column++);
      repo.forks_count = sqlite3_column_int(stmt, column++);
      repo.lang = strdup_when_not_null(sqlite3_column_text(stmt, column++));
      repo.homepage = strdup_when_not_null(sqlite3_column_text(stmt, column++));
      repo.size = sqlite3_column_int(stmt, column++);
    };

    star_arr[row++] = (omg_star){
        .starred_at = strdup_when_not_null(sqlite3_column_text(stmt, 0)),
        .repo = repo,
    };
  }

  if (rc != SQLITE_DONE) {
    return (omg_error){.code = OMG_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }

  *out_lst = (omg_star_list){.star_array = star_arr, .length = rows_count};
  return NO_ERROR;
}

static omg_error save_my_stars(omg_context ctx, omg_star_list star_lst) {
  omg_repo *repo_arr = malloc(sizeof(omg_repo) * star_lst.length);
  for (int i = 0; i < star_lst.length; i++) {
    repo_arr[i] = star_lst.star_array[i].repo;
  }
  omg_repo_list repo_lst = {.repo_array = repo_arr, .length = star_lst.length};
  omg_error err = save_repos(ctx, repo_lst);
  free(repo_arr);
  if (!is_ok(err)) {
    return err;
  }

  auto_sqlite3_stmt stmt = NULL;
  const char *sql =
      "insert into omg_my_star(starred_at, repo_id) values (?1, ?2)"
      "on conflict(repo_id)"
      "do update set starred_at = ?1";
  int rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
  if (rc) {
    return (omg_error){.code = OMG_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }

  for (int i = 0; i < star_lst.length; i++) {
    omg_star star = star_lst.star_array[i];
    sqlite3_bind_text(stmt, 1, star.starred_at, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, star.repo.id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
      fprintf(stderr, "insert %s failed. code:%d, msg:%s\n",
              star.repo.full_name, rc, sqlite3_errmsg(ctx->db));
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
  }

  return NO_ERROR;
}

omg_error omg_sync_stars(omg_context ctx) {
  size_t page_num = 1;
  while (true) {
#ifdef OMG_TEST
    if (page_num > 2) {
      break;
    }
#endif
    omg_auto_star_list star_lst = omg_new_star_list();
    omg_error err = fetch_stars_by_page(ctx, page_num++, &star_lst);
    if (!is_ok(err)) {
      return err;
    }
    err = save_my_stars(ctx, star_lst);
    if (!is_ok(err)) {
      return err;
    }

    if (star_lst.length < PER_PAGE) {
      break;
    }
  };

  return NO_ERROR;
}

omg_error omg_unstar(omg_context ctx, size_t repo_id) {
  const char *sql = "select full_name from omg_repo where id = ?";
  auto_sqlite3_stmt stmt = NULL;
  int rc = sqlite3_prepare_v2(ctx->db, sql, strlen(sql), &stmt, NULL);
  if (rc) {
    return (omg_error){.code = OMG_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }
  sqlite3_bind_int64(stmt, 1, repo_id);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    return (omg_error){.code = OMG_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }
  omg_auto_char full_name = strdup_when_not_null(sqlite3_column_text(stmt, 0));
  printf("delete repo %s\n", full_name);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    return (omg_error){.code = OMG_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }

  sql = "delete from omg_my_star where repo_id = ?";
  if (sqlite3_prepare_v2(ctx->db, sql, strlen(sql), &stmt, NULL)) {
    return (omg_error){.code = OMG_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }
  sqlite3_bind_int64(stmt, 1, repo_id);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    return (omg_error){.code = OMG_CODE_DB, .message = sqlite3_errmsg(ctx->db)};
  }

  char url[128];
  sprintf(url, "%s/user/starred/%s", API_ROOT, full_name);
  return omg_request(ctx, DELETE_METHOD, url, NULL, NULL);
}

omg_user omg_new_user() {
  return (omg_user){
      .login = NULL,
      .name = NULL,
      .company = NULL,
      .blog = NULL,
      .location = NULL,
      .email = NULL,
      .bio = NULL,
      .twitter_username = NULL,
      .created_at = NULL,
  };
}

void omg_free_user(omg_user *user) {
  if (user) {
#ifdef VERBOSE
    printf("free omg_user\n");
#endif
    FREE_OBJ_FIELD(user, login);
    FREE_OBJ_FIELD(user, name);
    FREE_OBJ_FIELD(user, company);
    FREE_OBJ_FIELD(user, blog);
    FREE_OBJ_FIELD(user, location);
    FREE_OBJ_FIELD(user, email);
    FREE_OBJ_FIELD(user, bio);
    FREE_OBJ_FIELD(user, twitter_username);
    FREE_OBJ_FIELD(user, created_at);
  }
}

static int integer_or_default(json_t *obj, const char *key) {
  json_t *tmp = json_object_get(obj, key);
  if (tmp) {
    return json_integer_value(tmp);
  }
  return -1;
}

omg_error omg_whoami(omg_context ctx, const char *username, omg_user *out) {
  char url[128];
  if (empty_string(username)) {
    sprintf(url, "%s/user", API_ROOT);
  } else {
    sprintf(url, "%s/users/%s", API_ROOT, username);
  }

  json_auto_t *resp = NULL;
  omg_error err = omg_request(ctx, GET_METHOD, url, NULL, &resp);
  if (!is_ok(err)) {
    return err;
  }

  json_t *msg = json_object_get(resp, "message");
  if (msg) {
    fprintf(stderr, "get whoami(%s) failed. %s", username, json_dumps(resp, 0));
    if (0 == strcmp(json_string_value(msg), "Not Found")) {
      return (omg_error){.code = OMG_CODE_GITHUB, .message = "User Not Found"};
    }

    return (omg_error){.code = OMG_CODE_GITHUB,
                       .message = "GitHub PAT authentication failed"};
  }

  int private_repos = integer_or_default(resp, "total_private_repos");
  int private_gists = integer_or_default(resp, "private_gists");
  int disk_usage = integer_or_default(resp, "disk_usage");
  *out = (omg_user){
      .login = dup_json_string(resp, "login"),
      .id = json_integer_value(json_object_get(resp, "id")),
      .name = dup_json_string(resp, "name"),
      .company = dup_json_string(resp, "company"),
      .blog = dup_json_string(resp, "blog"),
      .location = dup_json_string(resp, "location"),
      .email = dup_json_string(resp, "email"),
      .hireable = json_boolean_value(json_object_get(resp, "hireable")),
      .bio = dup_json_string(resp, "bio"),
      .twitter_username = dup_json_string(resp, "twitter_username"),
      .public_repos = json_integer_value(json_object_get(resp, "public_repos")),
      .public_gists = json_integer_value(json_object_get(resp, "public_gists")),
      .private_repos = private_repos,
      .private_gists = private_gists,
      .followers = json_integer_value(json_object_get(resp, "followers")),
      .following = json_integer_value(json_object_get(resp, "following")),
      .created_at = dup_json_string(resp, "created_at"),
      .disk_usage = disk_usage,
  };

  return NO_ERROR;
}

omg_commit omg_new_commit() {
  return (omg_commit){.sha = NULL, .message = NULL};
}

void omg_free_commit(omg_commit *commit) {
  if (commit) {
    FREE_OBJ_FIELD(commit, sha);
    FREE_OBJ_FIELD(commit, message);
  }
}

omg_commit_list omg_new_commit_list() {
  return (omg_commit_list){.commit_array = NULL, .length = 0};
}

void omg_free_commit_list(omg_commit_list *commit_lst) {
  if (commit_lst) {
    for (int i = 0; i < commit_lst->length; i++) {
      omg_free_commit(&commit_lst->commit_array[i]);
    }

    free(commit_lst->commit_array);
  }
}

omg_error omg_query_commits(omg_context ctx, const char *full_name, int limit,
                            omg_commit_list *out) {
  char url[128];
  sprintf(url, "%s/repos/%s/commits?per_page=%d", API_ROOT, full_name, limit);
  json_auto_t *resp = NULL;
  omg_error err = omg_request(ctx, GET_METHOD, url, NULL, &resp);
  if (!is_ok(err)) {
    return err;
  }

  size_t resp_len = json_array_size(resp);
  omg_commit *commit_array = malloc(sizeof(omg_commit) * resp_len);
  for (int i = 0; i < resp_len; i++) {
    json_t *one_commit = json_array_get(resp, i);
    json_t *commit_info = json_object_get(one_commit, "commit");
    json_t *author_info = json_object_get(commit_info, "author");
    commit_array[i] = (omg_commit){
        .sha = dup_json_string(one_commit, "sha"),
        .message = dup_json_string(commit_info, "message"),
        .author = dup_json_string(author_info, "name"),
        .email = dup_json_string(author_info, "email"),
        .date = dup_json_string(author_info, "date"),
    };
  }

  *out = (omg_commit_list){.commit_array = commit_array, .length = resp_len};

  return NO_ERROR;
}

// releases

omg_release_asset omg_new_release_asset() {
  return (omg_release_asset){.name = NULL, .download_url = NULL};
}

void omg_free_release_asset(omg_release_asset *release_asset) {
  if (release_asset) {
    FREE_OBJ_FIELD(release_asset, name);
    FREE_OBJ_FIELD(release_asset, download_url);
  }
}

omg_release omg_new_release() {
  return (omg_release){
      .name = NULL,
      .login = NULL,
      .tag_name = NULL,
      .body = NULL,
      .published_at = NULL,
      .asset_array = NULL,
      .asset_length = 0,
  };
}

void omg_free_release(omg_release *release) {
  if (release) {
    FREE_OBJ_FIELD(release, name);
    FREE_OBJ_FIELD(release, login);
    FREE_OBJ_FIELD(release, tag_name);
    FREE_OBJ_FIELD(release, body);
    FREE_OBJ_FIELD(release, published_at);
    if (release->asset_array) {
      for (int i = 0; i < release->asset_length; i++) {
        omg_free_release_asset(&release->asset_array[i]);
      }
      free(release->asset_array);
    }
  }
}

omg_release_list omg_new_release_list() {
  return (omg_release_list){.release_array = NULL, .length = 0};
}

void omg_free_release_list(omg_release_list *release_lst) {
  if (release_lst) {
    for (int i = 0; i < release_lst->length; i++) {
      omg_free_release(&release_lst->release_array[i]);
    }

    free(release_lst->release_array);
  }
}

omg_error omg_query_releases(omg_context ctx, const char *full_name, int limit,
                             omg_release_list *out) {
  char url[128];
  sprintf(url, "%s/repos/%s/releases?per_page=%d", API_ROOT, full_name, limit);
  json_auto_t *resp = NULL;
  omg_error err = omg_request(ctx, GET_METHOD, url, NULL, &resp);
  if (!is_ok(err)) {
    return err;
  }

  size_t resp_len = json_array_size(resp);
  omg_release *release_array = malloc(sizeof(omg_release) * resp_len);
  for (int i = 0; i < resp_len; i++) {
    json_t *one_release = json_array_get(resp, i);
    json_t *asset_info = json_object_get(one_release, "assets");
    json_t *author_info = json_object_get(one_release, "author");

    size_t asset_length = json_array_size(asset_info);
    omg_release_asset *asset_array =
        malloc(sizeof(omg_release_asset) * asset_length);
    for (int j = 0; j < asset_length; j++) {
      json_t *one_asset = json_array_get(asset_info, j);
      asset_array[j] = (omg_release_asset){
          .id = json_integer_value(json_object_get(one_asset, "id")),
          .name = dup_json_string(one_asset, "name"),
          .size = json_integer_value(json_object_get(one_asset, "size")),
          .download_count =
              json_integer_value(json_object_get(one_asset, "download_count")),
          .download_url = dup_json_string(one_asset, "browser_download_url"),
      };
    }

    release_array[i] = (omg_release){
        .id = json_integer_value(json_object_get(one_release, "id")),
        .login = dup_json_string(author_info, "login"),
        .name = dup_json_string(one_release, "name"),
        .tag_name = dup_json_string(one_release, "tag_name"),
        .body = dup_json_string(one_release, "body"),
        .draft = json_boolean_value(json_object_get(one_release, "draft")),
        .prerelease =
            json_boolean_value(json_object_get(one_release, "prerelease")),
        .published_at = dup_json_string(one_release, "published_at"),
        .asset_array = asset_array,
        .asset_length = asset_length,
    };
  }

  *out = (omg_release_list){.release_array = release_array, .length = resp_len};

  return NO_ERROR;
}

// trending

static const size_t TRENDING_LIST_LENGTH = 25;
static const size_t TRENDING_TUPLE_LENGTH = 4;

static omg_error omg_parse_trending(omg_context ctx, const char *html,
                                    omg_repo_list *out) {
  omg_repo *repo_array = malloc(sizeof(omg_repo) * TRENDING_LIST_LENGTH);
  regmatch_t pmatch[TRENDING_TUPLE_LENGTH];

  const char *head = html;
  size_t arr_len = 0;
  for (int i = 0; i < TRENDING_LIST_LENGTH; i++) {
    if (pcre2_regexec(&ctx->trending_re, head, TRENDING_TUPLE_LENGTH, pmatch,
                      0)) {
      break;
    }
    arr_len++;
    char *matched[TRENDING_TUPLE_LENGTH];
    for (int j = 1; j < TRENDING_TUPLE_LENGTH; j++) {
      regoff_t len = pmatch[j].rm_eo - pmatch[j].rm_so;

      char *buf = malloc(len + 1);
      memcpy(buf, head + pmatch[j].rm_so, len);
      buf[len] = '\0';
      matched[j] = buf;
    }

    omg_repo repo = omg_new_repo();
    repo.lang = matched[1];
    repo.full_name = matched[2];
    repo.stargazers_count = atoi(matched[3]);
    repo_array[i] = repo;

    head += pmatch[3].rm_eo;
  }

  *out = (omg_repo_list){.repo_array = repo_array, .length = arr_len};
  return NO_ERROR;
}

omg_error omg_query_trending(omg_context ctx, const char *lang,
                             const char *since, omg_repo_list *out) {

  char url[128];
  sprintf(url, "https://github.com/trending/%s?since=%s", lang, since);
  auto_curl *curl = curl_easy_init();
  if (!curl) {
    return (omg_error){.code = OMG_CODE_CURL, .message = "curl init"};
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, GET_METHOD);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
#ifdef VERBOSE
  curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
#endif

  auto_curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "x-pjax: true");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  auto_response chunk = {.memory = malloc(1), .size = 0};
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    return (omg_error){.code = OMG_CODE_CURL,
                       .message = curl_easy_strerror(res)};
  }

  long response_code;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  if (response_code != 200) {
    fprintf(stderr, "visit trending resp code:%ld", response_code);
    return (omg_error){.code = OMG_CODE_CURL,
                       .message = "get trending url not OK"};
  }

  omg_error err = omg_parse_trending(ctx, chunk.memory, out);
  if (!is_ok(err)) {
    return err;
  }

  return NO_ERROR;
}
