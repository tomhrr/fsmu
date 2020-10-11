#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <utime.h>
#include <syslog.h>
#include <limits.h>
#include <stddef.h>
#include <pwd.h>
#include "./uthash/uthash.h"

struct backing_path_st {
    char backing_path[PATH_MAX];
    int empty;
    UT_hash_handle hh;
};
struct link_mapping_st {
    char maildir_path[PATH_MAX];
    struct backing_path_st *backing_path_st;
    UT_hash_handle hh;
};
struct link_mapping_st *link_mappings = NULL;

static struct options {
    const char *backing_dir;
    const char *mu_home;
    const char *mu;
    int enable_indexing;
    int refresh_timeout;
    int delete_remove;
    int help;
} options;

#define OPTION(t, p) \
    { t, offsetof(struct options, p), 1 }
static const struct fuse_opt option_spec[] = {
    OPTION("--backing-dir=%s", backing_dir),
    OPTION("--muhome=%s", mu_home),
    OPTION("--mu=%s", mu),
    OPTION("--enable-indexing", enable_indexing),
    OPTION("--refresh-timeout", refresh_timeout),
    OPTION("--delete-remove", delete_remove),
    OPTION("--help", help),
    FUSE_OPT_END
};

static void verify_path(const char *path)
{
    if (strlen(path) > PATH_MAX) {
        syslog(LOG_ERR, "verify_path: '%s' is too long\n", path);
        abort();
    }
}

static int resolve_path_noexists(const char *path, char *buf)
{
    if (strchr(path + 1, '/') != NULL) {
        sprintf(buf, "%s/_%s", options.backing_dir, path + 1);
        return 0;
    } else {
        return -ENOENT;
    }
}

static int resolve_path(const char *path, char *buf)
{
    if (strchr(path + 1, '/') != NULL) {
        sprintf(buf, "%s/_%s", options.backing_dir, path + 1);
        struct stat stbuf;
        int res = stat(buf, &stbuf);
        if (res != 0) {
            return -ENOENT;
        }
        return 0;
    } else {
        return -ENOENT;
    }
}

static void *fsmu_init(struct fuse_conn_info *conn)
{
    return NULL;
}

static int fsmu_open(const char *path, struct fuse_file_info *info)
{
    return 0;
}

static int fsmu_release(const char *path, struct fuse_file_info *info)
{
    return 0;
}

static int fsmu_truncate(const char *path, off_t offset)
{
    return 0;
}

static int dirname(const char *path, char *buf)
{
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) {
        return -1;
    }
    int length = last_slash - path;
    strncpy(buf, path, length);
    buf[length] = 0;

    return 0;
}

static int basename(const char *path, char *buf)
{
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) {
        return -1;
    }
    int length = strlen(path) - (last_slash - path) - 1;
    strncpy(buf, last_slash + 1, length);
    buf[length] = 0;

    return 0;
}

static int make_backing_path_if_required(const char *backing_path)
{
    struct stat stbuf;
    memset(&stbuf, 0, sizeof(struct stat));
    int res = stat(backing_path, &stbuf);
    if (res != 0) {
        res = mkdir(backing_path, 0755);
        if (res != 0) {
            syslog(LOG_ERR, "make_backing_path_if_required: "
                            "cannot create '%s': %s\n",
                   backing_path, strerror(errno));
            return -1;
        }
    }

    char backing_path_cur[PATH_MAX];
    strcpy(backing_path_cur, backing_path);
    strcat(backing_path_cur, "/cur");
    memset(&stbuf, 0, sizeof(struct stat));
    res = stat(backing_path_cur, &stbuf);
    if (res != 0) {
        res = mkdir(backing_path_cur, 0755);
        if (res != 0) {
            syslog(LOG_ERR, "make_backing_path_if_required: "
                            "cannot create '%s': %s\n",
                   backing_path_cur, strerror(errno));
            return -1;
        }
    }

    char backing_path_new[PATH_MAX];
    strcpy(backing_path_new, backing_path);
    strcat(backing_path_new, "/new");
    memset(&stbuf, 0, sizeof(struct stat));
    res = stat(backing_path_new, &stbuf);
    if (res != 0) {
        res = mkdir(backing_path_new, 0755);
        if (res != 0) {
            syslog(LOG_ERR, "make_backing_path_if_required: "
                            "cannot create '%s': %s\n",
                   backing_path_new, strerror(errno));
            return -1;
        }
    }

    return 0;
}

static int add_link_mapping(const char *maildir_path,
                            const char *backing_path)
{
    struct link_mapping_st *lmst = NULL;
    HASH_FIND_STR(link_mappings, maildir_path, lmst);
    if (!lmst) {
        struct backing_path_st *all = NULL;
        struct backing_path_st *bpst = malloc(sizeof(struct backing_path_st));
        if (!bpst) {
            syslog(LOG_ERR, "malloc failed: %s\n", strerror(errno));
            abort();
        }
        strcpy(bpst->backing_path, backing_path);
        bpst->empty = 1;
        HASH_ADD_STR(all, backing_path, bpst);

        lmst = malloc(sizeof(struct link_mapping_st));
        if (!lmst) {
            syslog(LOG_ERR, "malloc failed: %s\n", strerror(errno));
            abort();
        }
        strcpy(lmst->maildir_path, maildir_path);
        lmst->backing_path_st = all;
        HASH_ADD_STR(link_mappings, maildir_path, lmst);
    } else {
        struct backing_path_st *bpst = lmst->backing_path_st;
        struct backing_path_st *bpst_sub = NULL;
        HASH_FIND_STR(bpst, backing_path, bpst_sub);
        if (bpst_sub) {
            syslog(LOG_DEBUG, "add_link_mapping: %s already present "
                              "as mapping from %s",
                   backing_path, maildir_path);
            abort();
        }

        struct backing_path_st *new = malloc(sizeof(struct backing_path_st));
        if (!new) {
            syslog(LOG_ERR, "malloc failed: %s\n", strerror(errno));
            abort();
        }
        strcpy(new->backing_path, backing_path);
        new->empty = 1;
        HASH_ADD_STR(bpst, backing_path, new);
    }

    return 0;
}

static int remove_link_mapping(const char *maildir_path,
                               const char *backing_path)
{
    struct link_mapping_st *lmst = NULL;
    HASH_FIND_STR(link_mappings, maildir_path, lmst);
    if (!lmst) {
        syslog(LOG_DEBUG, "remove_link_mapping: maildir_path %s "
                          "not present",
               maildir_path);
        abort();
    }

    struct backing_path_st *bpst = lmst->backing_path_st;
    struct backing_path_st *bpst_sub = NULL;
    HASH_FIND_STR(bpst, backing_path, bpst_sub);
    if (!bpst_sub) {
        syslog(LOG_DEBUG, "remove_link_mapping: %s not present "
                          "as mapping from %s",
               backing_path, maildir_path);
        abort();
    }

    HASH_DEL(bpst, bpst_sub);
    free(bpst_sub);

    return 0;
}

static int update_backing_path(const char *backing_path,
                               const char *temp_path)
{
    struct dirent *dent;
    struct stat stbuf;

    DIR *backing_dir_handle = opendir(backing_path);
    if (!backing_dir_handle) {
        syslog(LOG_ERR, "refresh_dir: cannot open '%s': %s\n",
               backing_path, strerror(errno));
        return -1;
    }
    while ((dent = readdir(backing_dir_handle)) != NULL) {
        if (strcmp(dent->d_name, ".") == 0) {
            continue;
        }
        if (strcmp(dent->d_name, "..") == 0) {
            continue;
        }
        char temp_path_ent[PATH_MAX];
        strcpy(temp_path_ent, temp_path);
        strcat(temp_path_ent, dent->d_name);
        memset(&stbuf, 0, sizeof(struct stat));
        int res = stat(temp_path_ent, &stbuf);
        if (res == 0) {
            res = unlink(temp_path_ent);
            if (res != 0) {
                syslog(LOG_ERR, "refresh_dir: 1unable to remove link "
                                "that already exists (%s): %s\n",
                       dent->d_name, strerror(errno));
                return -1;
            }
        } else {
            char backing_path_ent[PATH_MAX];
            strcpy(backing_path_ent, backing_path);
            strcat(backing_path_ent, dent->d_name);

            char maildir_path[PATH_MAX];
            ssize_t len = readlink(backing_path_ent, maildir_path, PATH_MAX);
            if (len == PATH_MAX) {
                syslog(LOG_ERR, "refresh_dir: too much path data for '%s'\n",
                    backing_path_ent);
                return -1;
            }
            if (len == -1) {
                syslog(LOG_ERR, "refresh_dir: unable to read link for '%s': %s\n",
                    backing_path_ent, strerror(errno));
                return -1 * errno;
            }
            maildir_path[len] = 0;

            res = remove_link_mapping(maildir_path, backing_path);
            if (res != 0) {
                syslog(LOG_ERR, "refresh_dir: unable to remove link mapping");
                return -1;
            }

            res = unlink(backing_path_ent);
            if (res != 0) {
                syslog(LOG_ERR, "refresh_dir: 2unable to remove link "
                                "that no longer exists (%s): %s\n",
                       dent->d_name, strerror(errno));
                return -1;
            }
        }
    }

    DIR *temp_dir_handle = opendir(temp_path);
    if (!temp_dir_handle) {
        syslog(LOG_ERR, "refresh_dir: cannot open '%s': %s\n",
               temp_path, strerror(errno));
        return -1;
    }
    while ((dent = readdir(temp_dir_handle)) != NULL) {
        if (strcmp(dent->d_name, ".") == 0) {
            continue;
        }
        if (strcmp(dent->d_name, "..") == 0) {
            continue;
        }
        char backing_path_ent[PATH_MAX];
        strcpy(backing_path_ent, backing_path);
        strcat(backing_path_ent, dent->d_name);

        char temp_path_ent[PATH_MAX];
        strcpy(temp_path_ent, temp_path);
        strcat(temp_path_ent, dent->d_name);

        int res = rename(temp_path_ent, backing_path_ent);
        if (res != 0) {
            syslog(LOG_ERR, "refresh_dir: unable to rename link (%s -> %s): %s\n",
                   temp_path_ent, backing_path_ent, strerror(errno));
            return -1;
        }

        char maildir_path[PATH_MAX];
        ssize_t len = readlink(backing_path_ent, maildir_path, PATH_MAX);
        if (len == PATH_MAX) {
            syslog(LOG_ERR, "refresh_dir: too much path data for '%s'\n",
                backing_path_ent);
            return -1;
        }
        if (len == -1) {
            syslog(LOG_ERR, "refresh_dir: unable to read link for '%s': %s\n",
                backing_path_ent, strerror(errno));
            return -1 * errno;
        }
        maildir_path[len] = 0;

        add_link_mapping(maildir_path, backing_path_ent);
    }

    return 0;
}

static int refresh_dir(const char *path, int force)
{
    syslog(LOG_DEBUG, "refresh_dir: '%s'\n", path);
    verify_path(path);

    if ((strcmp(path, "/") == 0)
            || (strlen(path) <= 1)
            || (path[1] == '_')) {
        syslog(LOG_DEBUG, "refresh_dir: '%s' cannot be refreshed\n", path);
        return 0;
    }

    char root_dirname[PATH_MAX];
    strcpy(root_dirname, path);
    char *separator = strchr(root_dirname + 1, '/');
    if (separator != NULL) {
        *separator = 0;
    }

    char search_path[PATH_MAX];
    sprintf(search_path, "%s%s", options.backing_dir, root_dirname);
    struct stat stbuf;
    int res = stat(search_path, &stbuf);
    if (res != 0) {
        syslog(LOG_ERR, "refresh_dir: '%s' cannot be refreshed\n", path);
        return -1 * errno;
    }

    char last_update_path[PATH_MAX];
    sprintf(last_update_path, "%s.last-update", search_path);
    res = stat(last_update_path, &stbuf);
    if (res == 0) {
        int threshold = time(NULL) - options.refresh_timeout;
        if (!force && (stbuf.st_mtim.tv_sec > threshold)) {
            syslog(LOG_DEBUG, "refresh_dir: '%s' refreshed "
                              "less than %ds ago\n", path,
                              options.refresh_timeout);
            return 0;
        }
    } else if (errno == ENOENT) {
        FILE *last_update_file = fopen(last_update_path, "w");
        if (!last_update_file) {
            syslog(LOG_ERR, "refresh_dir: cannot write "
                            "last-update for '%s': %s\n",
                            path, strerror(errno));
            return -1;
        }
        res = fclose(last_update_file);
        if (res != 0) {
            syslog(LOG_ERR, "refresh_dir: cannot close "
                            "last-update for '%s': %s\n",
                            path, strerror(errno));
            return -1;
        }
    }
    res = utime(last_update_path, NULL);
    if (res != 0) {
        syslog(LOG_ERR, "refresh_dir: cannot update "
                        "last-update for '%s': %s\n",
                        path, strerror(errno));
        return -1;
    }

    char cmd[4 * PATH_MAX];
    if (options.enable_indexing) {
        sprintf(cmd, "%s index %s%s",
                options.mu,
                (options.mu_home ? "--muhome=" : ""),
                (options.mu_home ? options.mu_home : ""));
        syslog(LOG_INFO, "refresh_dir: running mu index\n");
        res = system(cmd);
        if (res != 0) {
            syslog(LOG_ERR, "refresh_dir: mu index failed\n");
            return -1;
        }
    }

    char query[PATH_MAX];
    strcpy(query, root_dirname + 1);
    char backing_path[PATH_MAX];
    sprintf(backing_path, "%s/_%s", options.backing_dir, query);

    int len = strlen(query);
    for (int i = 0; i < len; i++) {
        if (query[i] == '+') {
            query[i] = '/';
        }
    }

    char template[PATH_MAX];
    strcpy(template, options.backing_dir);
    strcat(template, "/tempdir.XXXXXX");
    char *temp_dirname = mkdtemp(template);
    if (!temp_dirname) {
        syslog(LOG_ERR, "refresh_dir: unable to make temporary "
                        "directory (%s): %s\n",
               template, strerror(errno));
        return -1;
    }

    sprintf(cmd, "%s find %s%s --clearlinks --format=links "
                 "--linksdir='%s' '%s'",
            options.mu,
            (options.mu_home ? "--muhome=" : ""),
            (options.mu_home ? options.mu_home : ""),
            temp_dirname, query);
    syslog(LOG_INFO, "refresh_dir: running mu find: '%s'\n", cmd);
    res = system(cmd);
    if (res != 0) {
        syslog(LOG_ERR, "refresh_dir: mu find failed\n");
        return -1;
    }

    res = make_backing_path_if_required(backing_path);
    if (res != 0) {
        return -1;
    }

    char backing_path_cur[PATH_MAX];
    strcpy(backing_path_cur, backing_path);
    strcat(backing_path_cur, "/cur/");

    char temp_path_cur[PATH_MAX];
    strcpy(temp_path_cur, temp_dirname);
    strcat(temp_path_cur, "/cur/");

    res = update_backing_path(backing_path_cur, temp_path_cur);
    if (res != 0) {
        return -1;
    }

    char backing_path_new[PATH_MAX];
    strcpy(backing_path_new, backing_path);
    strcat(backing_path_new, "/new/");

    char temp_path_new[PATH_MAX];
    strcpy(temp_path_new, temp_dirname);
    strcat(temp_path_new, "/new/");

    res = update_backing_path(backing_path_new, temp_path_new);
    if (res != 0) {
        return -1;
    }

    return 0;
}

static int fsmu_readdir(const char *path, void *buf,
                        fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *info)
{
    syslog(LOG_DEBUG, "readdir: '%s'\n", path);
    verify_path(path);

    if (strcmp(path, "/") == 0) {
        DIR *backing_dir_handle = opendir(options.backing_dir);
        if (!backing_dir_handle) {
            syslog(LOG_ERR, "readdir: cannot open '%s': %s\n",
                   path, strerror(errno));
            return -1;
        }
        struct dirent *dent;
        while ((dent = readdir(backing_dir_handle)) != NULL) {
            if (dent->d_name[0] == '_') {
                continue;
            }
            filler(buf, dent->d_name, 0, 0);
        }
        int res = closedir(backing_dir_handle);
        if (res != 0) {
            syslog(LOG_ERR, "readdir: cannot close '%s': %s\n",
                   options.backing_dir, strerror(errno));
            return -1;
        }

        syslog(LOG_DEBUG, "readdir: '%s' completed\n", path);
        return 0;
    }

    char backing_path[PATH_MAX];
    int res = resolve_path(path, backing_path);
    if (res != 0) {
        if (path[1] == '_') {
            return -ENOENT;
        }
        refresh_dir(path, 0);
        sprintf(backing_path, "%s/_%s", options.backing_dir, path + 1);
    }

    DIR *dir_handle = opendir(backing_path);
    if (!dir_handle) {
        syslog(LOG_ERR, "readdir: cannot open '%s': %s\n",
               path, strerror(errno));
        return -1;
    }
    struct dirent *dent;
    while ((dent = readdir(dir_handle)) != NULL) {
        filler(buf, dent->d_name, 0, 0);
    }
    res = closedir(dir_handle);
    if (res != 0) {
        syslog(LOG_ERR, "readdir: cannot close '%s': %s\n",
               backing_path, strerror(errno));
        return -1;
    }

    syslog(LOG_DEBUG, "readdir: '%s' completed\n", path);
    return 0;
}

static int fsmu_getattr(const char *path, struct stat *stbuf)
{
    syslog(LOG_DEBUG, "getattr: '%s'\n", path);
    verify_path(path);

    memset(stbuf, 0, sizeof(struct stat));
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        syslog(LOG_DEBUG, "getattr: '%s' completed\n", path);
        return 0;
    }

    char backing_path[PATH_MAX];
    int res = resolve_path_noexists(path, backing_path);
    if (res != 0) {
        sprintf(backing_path, "%s%s", options.backing_dir, path);
    }

    int len = strlen(backing_path);
    if (len >= 4) {
        const char *tail = backing_path + len - 3;
        if (   (strcmp(tail, "cur") == 0)
            || (strcmp(tail, "new") == 0)) {
            syslog(LOG_INFO, "getattr: refreshing cur/new path\n", path);
            refresh_dir(path, 0);
        }
    }

    res = stat(backing_path, stbuf);
    if (res != 0) {
        syslog(LOG_ERR, "getattr: unable to stat '%s': %s\n",
               path, strerror(errno));
        return -1 * errno;
    }

    syslog(LOG_DEBUG, "getattr: '%s' completed\n", path);
    return res;
}

static int fsmu_rename(const char *from, const char *to)
{
    syslog(LOG_DEBUG, "rename: '%s' to '%s'\n", from, to);
    verify_path(from);
    verify_path(to);

    if (from == to) {
        syslog(LOG_DEBUG, "rename: '%s' is the same as '%s'\n", from, to);
        return 0;
    }

    char from_dir[PATH_MAX];
    int res = dirname(from, from_dir);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to get directory for '%s'\n", from);
        return -1;
    }
    char to_dir[PATH_MAX];
    res = dirname(to, to_dir);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to get directory for '%s'\n", to);
        return -1;
    }

    char from_dir_next[PATH_MAX];
    res = dirname(from_dir, from_dir_next);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to get directory for '%s'\n",
               from_dir);
        return -1;
    }
    char to_dir_next[PATH_MAX];
    res = dirname(to_dir, to_dir_next);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to get directory for '%s'\n",
               to_dir);
        return -1;
    }

    res = strcmp(from_dir_next, to_dir_next);
    if (res != 0) {
        syslog(LOG_ERR, "rename: directories do not match: "
                        "'%s' and '%s'\n", from_dir_next,
                        to_dir_next);
        return -1;
    }

    char to_dir_next_single[PATH_MAX];
    res = basename(to_dir, to_dir_next_single);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to get basename from '%s'\n",
               to_dir);
        return -1;
    }

    char to_basename[PATH_MAX];
    res = basename(to, to_basename);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to get basename from '%s'\n",
               to);
        return -1;
    }

    char from_backing_path[PATH_MAX];
    res = resolve_path(from, from_backing_path);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to resolve '%s'\n", from);
        return -1;
    }

    char from_maildir_path[PATH_MAX];
    ssize_t len = readlink(from_backing_path, from_maildir_path, PATH_MAX);
    if (len == PATH_MAX) {
        syslog(LOG_ERR, "rename: too much path data for '%s'\n",
               from_backing_path);
        return -1;
    }
    if (len == -1) {
        syslog(LOG_ERR, "rename: unable to read link for '%s': %s\n",
               from_backing_path, strerror(errno));
        return -1 * errno;
    }
    from_maildir_path[len] = 0;

    char from_maildir_dir[PATH_MAX];
    char to_maildir_path[PATH_MAX];
    res = dirname(from_maildir_path, from_maildir_dir);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to get directory for '%s'\n",
               from_maildir_path);
        return -1;
    }
    res = dirname(from_maildir_dir, to_maildir_path);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to get directory for '%s'\n",
               from_maildir_dir);
        return -1;
    }

    strcat(to_maildir_path, "/");
    strncat(to_maildir_path, to_dir_next_single, PATH_MAX);
    strcat(to_maildir_path, "/");
    strncat(to_maildir_path, to_basename, PATH_MAX);

    res = rename(from_maildir_path, to_maildir_path);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to rename '%s' to '%s': %s\n",
               from_maildir_path, to_maildir_path,
               strerror(errno));
        return -1;
    }

    char to_backing_path[PATH_MAX];
    res = resolve_path_noexists(to, to_backing_path);
    if (res != 0) {
        syslog(LOG_ERR, "rename: cannot resolve path '%s'\n", to);
        return -1;
    }
    res = symlink(to_maildir_path, to_backing_path);
    if (res != 0) {
        syslog(LOG_ERR, "rename: cannot make symlink for '%s': %s\n",
               to_backing_path, strerror(errno));
        return -1;
    }
    res = unlink(from_backing_path);
    if (res != 0) {
        syslog(LOG_ERR, "rename: cannot remove old symlink '%s': %s\n",
               from_backing_path, strerror(errno));
        return -1;
    }

    syslog(LOG_DEBUG, "rename: '%s' to '%s' completed\n", from, to);
    return 0;
}

static int fsmu_read(const char *path, char *buf, size_t size,
                     off_t offset, struct fuse_file_info *info)
{
    syslog(LOG_DEBUG, "read: '%s'\n", path);
    verify_path(path);

    char backing_path[PATH_MAX];
    int res = resolve_path(path, backing_path);
    if (res != 0) {
        syslog(LOG_ERR, "read: unable to resolve '%s'\n", path);
        return -1;
    }

    FILE *backing_file = fopen(backing_path, "r");
    if (!backing_file) {
        syslog(LOG_ERR, "read: unable to open '%s': %s\n", path,
               strerror(errno));
        return -1 * errno;
    }

    res = fseek(backing_file, offset, SEEK_SET);
    if (res != 0) {
        syslog(LOG_ERR, "read: '%s': failed to seek: %s\n",
               path, strerror(errno));
        return res;
    }

    size_t bytes = fread(buf, 1, size, backing_file);
    res = fclose(backing_file);
    if (res != 0) {
        syslog(LOG_ERR, "read: '%s': failed to close: %s\n",
               path, strerror(errno));
        return res;
    }

    syslog(LOG_DEBUG, "read: '%s' completed\n", path);
    return bytes;
}

static int fsmu_mkdir(const char *path, mode_t mode)
{
    syslog(LOG_DEBUG, "mkdir: '%s'\n", path);
    verify_path(path);

    char backing_path[PATH_MAX];
    int res = resolve_path_noexists(path, backing_path);
    if (res != 0) {
        sprintf(backing_path, "%s%s", options.backing_dir, path);
    }
    res = mkdir(backing_path, mode);
    if (res != 0) {
        syslog(LOG_ERR, "mkdir: '%s': failed: %s\n",
               path, strerror(errno));
        return -1 * errno;
    }

    syslog(LOG_DEBUG, "mkdir: '%s' completed\n", path);
    return 0;
}

static int fsmu_rmdir(const char *path)
{
    syslog(LOG_DEBUG, "rmdir: '%s'\n", path);
    verify_path(path);

    if (strchr(path + 1, '/') != NULL) {
        syslog(LOG_ERR, "rmdir: cannot remove nested directory '%s'\n",
               path);
        return -1;
    }

    char real_path[PATH_MAX];
    sprintf(real_path, "%s%s", options.backing_dir, path);
    int res = rmdir(real_path);
    if (res != 0) {
        syslog(LOG_ERR, "rmdir: '%s': failed: %s\n",
               path, strerror(errno));
        return -1 * errno;
    }

    strcat(real_path, ".last-update");
    res = unlink(real_path);
    if (res != 0) {
        syslog(LOG_INFO, "rmdir: '%s': unable to remove "
                         "last-update file: %s\n",
               path, strerror(errno));
    }

    syslog(LOG_DEBUG, "rmdir: '%s' completed\n", path);
    return 0;
}

static int fsmu_unlink(const char *path)
{
    syslog(LOG_DEBUG, "unlink: '%s'\n", path);
    verify_path(path);

    if (!options.delete_remove) {
        syslog(LOG_DEBUG, "unlink: --delete-remove not set, returning\n");
        return 0;
    }

    char backing_path[PATH_MAX];
    int res = resolve_path(path, backing_path);
    if (res != 0) {
        syslog(LOG_ERR, "unlink: unable to resolve '%s'\n",
               path);
        return -1 * errno;
    }

    char maildir_path[PATH_MAX];
    ssize_t len = readlink(backing_path, maildir_path, PATH_MAX);
    if (len == PATH_MAX) {
        syslog(LOG_ERR, "unlink: too much path data for '%s'\n",
               backing_path);
        return -1;
    }
    if (len == -1) {
        syslog(LOG_ERR, "unlink: unable to read link for '%s': %s\n",
               backing_path, strerror(errno));
        return -1 * errno;
    }
    maildir_path[len] = 0;

    res = unlink(maildir_path);
    if (res != 0) {
        syslog(LOG_ERR, "unlink: '%s': unable to remove: %s\n",
               maildir_path, strerror(errno));
        return -1 * errno;
    }
    res = unlink(backing_path);
    if (res != 0) {
        syslog(LOG_ERR, "unlink: '%s': unable to remove: %s\n",
               backing_path, strerror(errno));
        return -1 * errno;
    }

    syslog(LOG_DEBUG, "unlink: '%s' completed\n", path);
    return 0;
}

static const struct fuse_operations operations = {
    .init     = fsmu_init,
    .readdir  = fsmu_readdir,
    .open     = fsmu_open,
    .getattr  = fsmu_getattr,
    .read     = fsmu_read,
    .rename   = fsmu_rename,
    .release  = fsmu_release,
    .truncate = fsmu_truncate,
    .mkdir    = fsmu_mkdir,
    .rmdir    = fsmu_rmdir,
    .unlink   = fsmu_unlink,
};

static void usage(const char *progname)
{
    printf("usage: %s [options] <mountpoint>\n\n", progname);
    printf("File-system specific options:\n"
           "    --backing-dir=<s>       Backing directory path\n"
           "    --refresh-timeout=<d>   Do not perform search again if\n"
           "                            requested within <d> seconds\n"
           "                            (default: 30)\n"
           "    --enable-indexing       Whether to index before searching\n"
           "                            (default: false)\n"
           "    --delete-remove         Whether deletions should take\n"
           "                            effect (default: false)\n"
           "    --mu=<s>                Path to mu executable\n"
           "    --muhome=<s>            --muhome option for mu calls\n"
           "\n");
}

int expand_tilde(const char *path, char *buf)
{
    const char *homedir = getenv("HOME");
    if (!homedir) {
        struct passwd *pw = getpwuid(getuid());
        homedir = pw->pw_dir;
    }

    int path_len = strlen(path);
    int homedir_len = strlen(homedir);
    int j = 0;
    for (int i = 0; i < path_len; i++) {
        if (path[i] == '~') {
            strncpy(buf + j, homedir, homedir_len);
            j += homedir_len;
        } else {
            buf[j++] = path[i];
        }
    }
    buf[j] = 0;

    return 0;
}

int main(int argc, char **argv)
{
    options.refresh_timeout = 30;
    options.mu = strdup("mu");
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    if (fuse_opt_parse(&args, &options, option_spec, NULL) == -1) {
        return 1;
    }

    if (options.help) {
        usage(argv[0]);
        int res = fuse_opt_add_arg(&args, "--help");
        if (res != 0) {
            return 1;
        }
        args.argv[0][0] = '\0';
    }
    if (!options.backing_dir) {
        printf("backing_dir must be set.\n");
        usage(argv[0]);
        return 1;
    }

    char backing_dir_final[PATH_MAX];
    expand_tilde(options.backing_dir, backing_dir_final);
    options.backing_dir = backing_dir_final;

    char mu_final[PATH_MAX];
    expand_tilde(options.mu, mu_final);
    options.mu = mu_final;

    if (options.mu_home) {
        char mu_home_final[PATH_MAX];
        expand_tilde(options.mu_home, mu_home_final);
        options.mu_home = mu_home_final;
    }

    fuse_main(args.argc, args.argv, &operations, NULL);
    fuse_opt_free_args(&args);

    return 0;
}
