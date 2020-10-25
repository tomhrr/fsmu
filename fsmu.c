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

static struct options {
    const char *backing_dir;
    const char *mu_home;
    const char *mu;
    int enable_indexing;
    int refresh_timeout;
    int delete_remove;
    int help;
} options;

static char *backing_dir_reverse;

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
        syslog(LOG_ERR, "verify_path: '%s' is too long", path);
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
        syslog(LOG_ERR, "dirname: cannot get directory name "
                        "for '%s'", path);
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
        syslog(LOG_ERR, "basename: cannot get base name "
                        "for '%s'", path);
        return -1;
    }
    int length = strlen(path) - (last_slash - path) - 1;
    strncpy(buf, last_slash + 1, length);
    buf[length] = 0;

    return 0;
}

static int is_dot(const char *entry)
{
    return ((strcmp(entry, ".")  == 0)
         || (strcmp(entry, "..") == 0));
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
                            "cannot create '%s': %s",
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
                            "cannot create '%s': %s",
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
                            "cannot create '%s': %s",
                   backing_path_new, strerror(errno));
            return -1;
        }
    }

    return 0;
}

static int mkdirp(const char *path)
{
    struct stat st_buf;
    memset(&st_buf, 0, sizeof(struct stat));
    int res = stat(path, &st_buf);
    if (res != 0) {
        res = mkdir(path, 0755);
        if ((res != 0) && (errno == ENOENT)) {
            char dir[PATH_MAX];
            res = dirname(path, dir);
            if (res != 0) {
                return -1;
            }
            res = mkdirp(dir);
            if (res != 0) {
                return -1;
            }
            res = mkdir(path, 0755);
            if (res != 0) {
                syslog(LOG_ERR, "mkdirp: cannot make directory "
                                "'%s': %s",
                       path, strerror(errno));
                return -1;
            }
        }
    }

    return 0;
}

static int get_reverse_path(const char *maildir_path,
                            const char *backing_path,
                            char *buf)
{
    char filename[PATH_MAX];
    int res = basename(backing_path, filename);
    if (res != 0) {
        return -1;
    }

    char backing_dir[PATH_MAX];
    res = dirname(backing_path, backing_dir);
    if (res != 0) {
        return -1;
    }
    char backing_dir_single[PATH_MAX];
    res = basename(backing_dir, backing_dir_single);
    if (res != 0) {
        return -1;
    }

    char backing_dir2[PATH_MAX];
    res = dirname(backing_dir, backing_dir2);
    if (res != 0) {
        return -1;
    }
    char backing_dir_single2[PATH_MAX];
    res = basename(backing_dir2, backing_dir_single2);
    if (res != 0) {
        return -1;
    }

    strcpy(buf, backing_dir_reverse);
    strcat(buf, maildir_path);
    strcat(buf, "/");
    strcat(buf, backing_dir_single2);
    strcat(buf, "/");
    strcat(buf, backing_dir_single);
    strcat(buf, "/");
    strcat(buf, filename);

    return 0;
}

static int add_link_mapping(const char *maildir_path,
                            const char *backing_path)
{
    char reverse_path[PATH_MAX];
    int res = get_reverse_path(maildir_path, backing_path,
                               reverse_path);
    if (res != 0) {
        return -1;
    }

    char reverse_path_dir[PATH_MAX];
    res = dirname(reverse_path, reverse_path_dir);
    if (res != 0) {
        return -1;
    }
    res = mkdirp(reverse_path_dir);
    if (res != 0) {
        return -1;
    }

    res = symlink(backing_path, reverse_path);
    if (res != 0) {
        syslog(LOG_ERR, "add_link_mapping: failed for '%s' to '%s': %s",
               backing_path, reverse_path, strerror(errno));
        return -1;
    }

    return 0;
}

static int remove_link_mapping(const char *maildir_path,
                               const char *backing_path)
{
    char reverse_path[PATH_MAX];
    int res = get_reverse_path(maildir_path, backing_path,
                               reverse_path);
    if (res != 0) {
        return -1;
    }

    res = unlink(reverse_path);
    if (res != 0) {
        return -1;
    }
    char *last_slash = strrchr(reverse_path, '/');
    *last_slash = 0;
    res = rmdir(reverse_path);
    if (res != 0) {
        return -1;
    }
    last_slash = strrchr(reverse_path, '/');
    *last_slash = 0;
    res = rmdir(reverse_path);
    if (res != 0) {
        return -1;
    }

    for (;;) {
        last_slash = strrchr(reverse_path, '/');
        *last_slash = 0;
        int len = strlen(reverse_path);
        if (len >= 9) {
            const char *tail = reverse_path + len - 9;
            if (strcmp(tail, "/_reverse") == 0) {
                return 0;
            }
        }

        DIR *reverse_handle = opendir(reverse_path);
        if (!reverse_handle) {
            syslog(LOG_ERR, "remove_link_mapping: unable "
                            "to open directory '%s'",
                   reverse_path);
            return -1;
        }
        struct dirent *dent;
        int count = 0;
        while ((dent = readdir(reverse_handle)) != NULL) {
            if (is_dot(dent->d_name)) {
                continue;
            }
            count++;
        }
        closedir(reverse_handle);
        if (count != 0) {
            break;
        }
        res = rmdir(reverse_path);
        if (res != 0) {
            return -1;
        }
    }

    return 0;
}

static int update_backing_path(const char *backing_path,
                               const char *temp_path)
{
    struct dirent *dent;
    struct stat stbuf;

    DIR *backing_dir_handle = opendir(backing_path);
    if (!backing_dir_handle) {
        syslog(LOG_ERR, "update_backing_path: cannot open '%s': %s",
               backing_path, strerror(errno));
        return -1;
    }
    while ((dent = readdir(backing_dir_handle)) != NULL) {
        if (is_dot(dent->d_name)) {
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
                syslog(LOG_ERR, "update_backing_path: unable to remove link "
                                "'%s' that already exists: %s",
                       dent->d_name, strerror(errno));
                closedir(backing_dir_handle);
                return -1;
            }
        } else {
            char backing_path_ent[PATH_MAX];
            strcpy(backing_path_ent, backing_path);
            strcat(backing_path_ent, dent->d_name);

            char maildir_path[PATH_MAX];
            ssize_t len = readlink(backing_path_ent, maildir_path, PATH_MAX);
            if (len == PATH_MAX) {
                syslog(LOG_ERR, "update_backing_path: too much path "
                                "data for '%s",
                       backing_path_ent);
                closedir(backing_dir_handle);
                return -1;
            }
            if (len == -1) {
                syslog(LOG_ERR, "update_backing_path: unable to read "
                                "link for '%s': %s",
                       backing_path_ent, strerror(errno));
                closedir(backing_dir_handle);
                return -1;
            }
            maildir_path[len] = 0;

            res = remove_link_mapping(maildir_path, backing_path);
            if (res != 0) {
                syslog(LOG_ERR, "update_backing_path: unable "
                                "to remove link mapping");
                closedir(backing_dir_handle);
                return -1;
            }
            res = unlink(backing_path_ent);
            if (res != 0) {
                syslog(LOG_ERR, "update_backing_path: unable "
                                "to remove previous backing path "
                                "'%s': %s",
                       backing_path_ent, strerror(errno));
                closedir(backing_dir_handle);
                return -1;
            }
        }
    }
    closedir(backing_dir_handle);

    DIR *temp_dir_handle = opendir(temp_path);
    if (!temp_dir_handle) {
        syslog(LOG_ERR, "update_backing_path: cannot open '%s': %s",
               temp_path, strerror(errno));
        return -1;
    }
    while ((dent = readdir(temp_dir_handle)) != NULL) {
        if (is_dot(dent->d_name)) {
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
            syslog(LOG_ERR, "update_backing_path: unable to "
                            "rename link ('%s' -> '%s'): %s",
                   temp_path_ent, backing_path_ent, strerror(errno));
            closedir(temp_dir_handle);
            return -1;
        }

        char maildir_path[PATH_MAX];
        ssize_t len = readlink(backing_path_ent, maildir_path, PATH_MAX);
        if (len == PATH_MAX) {
            syslog(LOG_ERR, "update_backing_path: too much path "
                            "data for '%s'",
                   backing_path_ent);
            closedir(temp_dir_handle);
            return -1;
        }
        if (len == -1) {
            syslog(LOG_ERR, "update_backing_path: unable to read "
                            "link for '%s': %s",
                   backing_path_ent, strerror(errno));
            closedir(temp_dir_handle);
            return -1;
        }
        maildir_path[len] = 0;

        add_link_mapping(maildir_path, backing_path_ent);
    }
    closedir(temp_dir_handle);

    return 0;
}

static int refresh_dir(const char *path, int force)
{
    syslog(LOG_DEBUG, "refresh_dir: '%s'", path);
    verify_path(path);

    if ((strcmp(path, "/") == 0)
            || (strlen(path) <= 1)
            || (path[1] == '_')) {
        syslog(LOG_DEBUG, "refresh_dir: '%s' cannot be refreshed", path);
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
        syslog(LOG_ERR, "refresh_dir: '%s' cannot be refreshed", path);
        return -1;
    }

    char last_update_path[PATH_MAX];
    sprintf(last_update_path, "%s.last-update", search_path);
    res = stat(last_update_path, &stbuf);
    if (res == 0) {
        int threshold = time(NULL) - options.refresh_timeout;
        if (!force && (stbuf.st_mtim.tv_sec > threshold)) {
            syslog(LOG_DEBUG, "refresh_dir: '%s' refreshed "
                              "less than %ds ago", path,
                              options.refresh_timeout);
            return 0;
        }
    } else if (errno == ENOENT) {
        FILE *last_update_file = fopen(last_update_path, "w");
        if (!last_update_file) {
            syslog(LOG_ERR, "refresh_dir: cannot write "
                            "last-update for '%s': %s",
                            path, strerror(errno));
            return -1;
        }
        res = fclose(last_update_file);
        if (res != 0) {
            syslog(LOG_ERR, "refresh_dir: cannot close "
                            "last-update for '%s': %s",
                            path, strerror(errno));
            return -1;
        }
    }
    res = utime(last_update_path, NULL);
    if (res != 0) {
        syslog(LOG_ERR, "refresh_dir: cannot update "
                        "last-update for '%s': %s",
                        path, strerror(errno));
        return -1;
    }

    char cmd[4 * PATH_MAX];
    if (options.enable_indexing) {
        sprintf(cmd, "%s index %s%s",
                options.mu,
                (options.mu_home ? "--muhome=" : ""),
                (options.mu_home ? options.mu_home : ""));
        syslog(LOG_INFO, "refresh_dir: running mu index");
        res = system(cmd);
        if (res != 0) {
            syslog(LOG_ERR, "refresh_dir: mu index failed");
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
    strcat(template, "/_tempdir.XXXXXX");
    char *temp_dirname = mkdtemp(template);
    if (!temp_dirname) {
        syslog(LOG_ERR, "refresh_dir: unable to make temporary "
                        "directory (%s): %s",
               template, strerror(errno));
        return -1;
    }

    sprintf(cmd, "%s find %s%s --clearlinks --format=links "
                 "--linksdir='%s' '%s'",
            options.mu,
            (options.mu_home ? "--muhome=" : ""),
            (options.mu_home ? options.mu_home : ""),
            temp_dirname, query);
    syslog(LOG_INFO, "refresh_dir: running mu find: '%s'", cmd);
    res = system(cmd);
    /* 2 is the documented return code for "no results found".  1024
     * is the return code seen in practice. */
    if ((res != 0) && (res != 2) && (res != 1024)) {
        syslog(LOG_ERR, "refresh_dir: mu find failed");
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

    char tempdir_part[PATH_MAX];
    strcpy(tempdir_part, temp_dirname);
    strcat(tempdir_part, "/new");
    res = rmdir(tempdir_part);
    if (res != 0) {
        syslog(LOG_ERR, "refresh_dir: cannot remove temp/new: %s",
               strerror(errno));
        return -1;
    }
    strcpy(tempdir_part, temp_dirname);
    strcat(tempdir_part, "/cur");
    res = rmdir(tempdir_part);
    if (res != 0) {
        syslog(LOG_ERR, "refresh_dir: cannot remove temp/cur: %s",
               strerror(errno));
        return -1;
    }
    strcpy(tempdir_part, temp_dirname);
    strcat(tempdir_part, "/tmp");
    res = rmdir(tempdir_part);
    if (res != 0) {
        syslog(LOG_ERR, "refresh_dir: cannot remove temp/tmp: %s",
               strerror(errno));
        return -1;
    }

    DIR *temp_dir_handle = opendir(temp_dirname);
    if (!temp_dir_handle) {
        syslog(LOG_ERR, "refresh_dir: cannot open '%s': %s",
               temp_dirname, strerror(errno));
        return -1;
    }
    struct dirent *dent;
    while ((dent = readdir(temp_dir_handle)) != NULL) {
        if (is_dot(dent->d_name)) {
            continue;
        }
        char path[PATH_MAX];
        strcpy(path, temp_dirname);
        strcat(path, "/");
        strcat(path, dent->d_name);
        res = unlink(path);
        if (res != 0) {
            syslog(LOG_ERR, "refresh_dir: cannot unlink '%s': %s",
                   path, strerror(errno));
            closedir(temp_dir_handle);
            return -1;
        }
    }
    closedir(temp_dir_handle);
    res = rmdir(temp_dirname);
    if (res != 0) {
        syslog(LOG_ERR, "refresh_dir: cannot remove temp: %s",
               strerror(errno));
        return -1;
    }

    return 0;
}

static int fsmu_readdir(const char *path, void *buf,
                        fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *info)
{
    syslog(LOG_DEBUG, "readdir: '%s'", path);
    verify_path(path);

    if (strcmp(path, "/") == 0) {
        DIR *backing_dir_handle = opendir(options.backing_dir);
        if (!backing_dir_handle) {
            syslog(LOG_ERR, "readdir: cannot open '%s': %s",
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
        closedir(backing_dir_handle);

        syslog(LOG_DEBUG, "readdir: '%s' completed", path);
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
        syslog(LOG_ERR, "readdir: cannot open '%s': %s",
               path, strerror(errno));
        return -1;
    }
    struct dirent *dent;
    while ((dent = readdir(dir_handle)) != NULL) {
        filler(buf, dent->d_name, 0, 0);
    }
    closedir(dir_handle);

    syslog(LOG_DEBUG, "readdir: '%s' completed", path);
    return 0;
}

static int fsmu_getattr(const char *path, struct stat *stbuf)
{
    syslog(LOG_DEBUG, "getattr: '%s'", path);
    verify_path(path);

    memset(stbuf, 0, sizeof(struct stat));
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        syslog(LOG_DEBUG, "getattr: '%s' completed", path);
        return 0;
    }

    char backing_path[PATH_MAX];
    int res = resolve_path_noexists(path, backing_path);
    if (res != 0) {
        sprintf(backing_path, "%s%s", options.backing_dir, path);
    }

    int len = strlen(backing_path);
    if (len >= 4) {
        const char *tail = backing_path + len - 4;
        if (   (strcmp(tail, "/cur") == 0)
            || (strcmp(tail, "/new") == 0)) {
            syslog(LOG_INFO, "getattr: refreshing cur/new path");
            refresh_dir(path, 0);
        }
    }
    if (len >= 9) {
        const char *tail = backing_path + len - 9;
        if (strcmp(tail, "/.refresh") == 0) {
            stbuf->st_mode = S_IFREG;
            stbuf->st_size = 0;
            return 0;
        }
    }

    res = stat(backing_path, stbuf);
    if (res != 0) {
        syslog(LOG_ERR, "getattr: unable to stat '%s': %s",
               path, strerror(errno));
        return -1 * errno;
    }

    syslog(LOG_DEBUG, "getattr: '%s' completed", path);
    return res;
}

static int update_link_mapping(const char *maildir_path,
                               const char *new_maildir_path,
                               const char *dirname_new,
                               const char *basename_new,
                               const char *flags)
{
    char reverse_path[PATH_MAX];
    strcpy(reverse_path, backing_dir_reverse);
    strcat(reverse_path, "/");
    strcat(reverse_path, maildir_path);

    struct dirent *dent;
    struct dirent *dent_search;
    struct dirent *dent_type;

    DIR *reverse_handle = opendir(reverse_path);
    if (!reverse_handle) {
        syslog(LOG_ERR, "update_link_mapping: cannot open '%s': %s",
               reverse_path, strerror(errno));
        return -1;
    }
    while ((dent = readdir(reverse_handle)) != NULL) {
        if (is_dot(dent->d_name)) {
            continue;
        }
        char search_path[PATH_MAX];
        strcpy(search_path, reverse_path);
        strcat(search_path, "/");
        strcat(search_path, dent->d_name);
        DIR *search_dir_handle = opendir(search_path);
        if (!search_dir_handle) {
            syslog(LOG_ERR, "update_link_mapping: cannot open search path '%s': %s",
                   search_path, strerror(errno));
            closedir(reverse_handle);
            return -1;
        }
        while ((dent_search = readdir(search_dir_handle)) != NULL) {
            if (is_dot(dent_search->d_name)) {
                continue;
            }
            char type_path[PATH_MAX];
            strcpy(type_path, search_path);
            strcat(type_path, "/");
            strcat(type_path, dent_search->d_name);
            DIR *type_dir_handle = opendir(type_path);
            if (!type_dir_handle) {
                syslog(LOG_ERR, "update_link_mapping: cannot open type path '%s': %s",
                       type_path, strerror(errno));
                closedir(search_dir_handle);
                closedir(reverse_handle);
                return -1;
            }
            while ((dent_type = readdir(type_dir_handle)) != NULL) {
                if (is_dot(dent_type->d_name)) {
                    continue;
                }
                char reverse_path_full[PATH_MAX];
                strcpy(reverse_path_full, type_path);
                strcat(reverse_path_full, "/");
                strcat(reverse_path_full, dent_type->d_name);
                char backing_path[PATH_MAX];

                ssize_t len = readlink(reverse_path_full, backing_path, PATH_MAX);
                if (len == PATH_MAX) {
                    syslog(LOG_ERR, "update_link_mapping: too much path data for '%s'",
                           reverse_path_full);
                    closedir(type_dir_handle);
                    closedir(search_dir_handle);
                    closedir(reverse_handle);
                    return -1;
                }
                if (len == -1) {
                    syslog(LOG_ERR, "update_link_mapping: unable to read link for '%s': %s",
                           reverse_path_full, strerror(errno));
                    closedir(type_dir_handle);
                    closedir(search_dir_handle);
                    closedir(reverse_handle);
                    return -1;
                }
                backing_path[len] = 0;

                int res = remove_link_mapping(maildir_path, backing_path);
                if (res != 0) {
                    syslog(LOG_ERR, "update_link_mapping: cannot remove old link mapping");
                    closedir(type_dir_handle);
                    closedir(search_dir_handle);
                    closedir(reverse_handle);
                    return -1;
                }
                res = unlink(backing_path);
                if (res != 0) {
                    syslog(LOG_ERR, "update_link_mapping: cannot remove old backing path");
                    closedir(type_dir_handle);
                    closedir(search_dir_handle);
                    closedir(reverse_handle);
                    return -1;
                }

                char backing_path_new[PATH_MAX];
                char backing_path_dir[PATH_MAX];
                res = dirname(backing_path, backing_path_dir);
                if (res != 0) {
                    closedir(type_dir_handle);
                    closedir(search_dir_handle);
                    closedir(reverse_handle);
                    return -1;
                }
                char backing_path_dir2[PATH_MAX];
                res = dirname(backing_path_dir, backing_path_dir2);
                if (res != 0) {
                    closedir(type_dir_handle);
                    closedir(search_dir_handle);
                    closedir(reverse_handle);
                    return -1;
                }
                char new_maildir_path_dir[PATH_MAX];
                res = dirname(new_maildir_path, new_maildir_path_dir);
                if (res != 0) {
                    closedir(type_dir_handle);
                    closedir(search_dir_handle);
                    closedir(reverse_handle);
                    return -1;
                }
                char new_maildir_path_dir_single[PATH_MAX];
                res = basename(new_maildir_path_dir,
                               new_maildir_path_dir_single);
                if (res != 0) {
                    closedir(type_dir_handle);
                    closedir(search_dir_handle);
                    closedir(reverse_handle);
                    return -1;
                }

                strcpy(backing_path_new, backing_path_dir2);
                strcat(backing_path_new, "/");
                strcat(backing_path_new, new_maildir_path_dir_single);
                strcat(backing_path_new, "/");

                if (!flags) {
                    strcat(backing_path_new, basename_new);
                } else {
                    char filename[PATH_MAX];
                    res = basename(backing_path, filename);
                    if (res != 0) {
                        closedir(type_dir_handle);
                        closedir(search_dir_handle);
                        closedir(reverse_handle);
                        return -1;
                    }
                    char *to_flags = strrchr(filename, ':');
                    if (!to_flags) {
                        strcat(backing_path_new, filename);
                        strcat(backing_path_new, flags);
                    } else {
                        strcpy(to_flags, flags);
                        strcat(backing_path_new, filename);
                    }
                }

                res = add_link_mapping(new_maildir_path, backing_path_new);
                if (res != 0) {
                    closedir(type_dir_handle);
                    closedir(search_dir_handle);
                    closedir(reverse_handle);
                    return -1;
                }

                res = symlink(new_maildir_path, backing_path_new);
                if (res != 0) {
                    syslog(LOG_ERR, "update_link_mapping: unable to "
                                    "relink backing path '%s': %s",
                           backing_path_new, strerror(errno));
                    closedir(type_dir_handle);
                    closedir(search_dir_handle);
                    closedir(reverse_handle);
                    return -1;
                }
            }
            closedir(type_dir_handle);
        }
        closedir(search_dir_handle);
    }
    closedir(reverse_handle);

    return 0;
}

static int equal_to_flags(const char *path1, const char *path2)
{
    const char *basename1 = strrchr(path1, '/');
    const char *basename2 = strrchr(path2, '/');
    if (!basename1 || !basename2) {
        return -1;
    }

    const char *colon1 = strrchr(basename1, ':');
    const char *colon2 = strrchr(basename2, ':');
    if (!colon1 && !colon2) {
        return 0;
    }
    if ((colon1 != 0) != (colon2 != 0)) {
        return 0;
    }

    int len1 = colon1 - path1;
    int len2 = colon2 - path2;
    if (len1 != len2) {
        return -1;
    }

    int res = strncmp(path1, path2, len1);
    if (res == 0) {
        return 0;
    } else {
        return -1;
    }
}

static int fsmu_rename(const char *from, const char *to)
{
    syslog(LOG_DEBUG, "rename: '%s' to '%s'", from, to);
    verify_path(from);
    verify_path(to);

    const char *flags = NULL;
    if (equal_to_flags(from, to) == 0) {
        const char *basename = strrchr(to, '/');
        flags = strchr(basename, ':');
        if (flags && (strlen(flags) <= 1)) {
            flags = NULL;
        }
    }

    if (from == to) {
        syslog(LOG_DEBUG, "rename: '%s' is the same as '%s'", from, to);
        return 0;
    }

    char from_dir[PATH_MAX];
    int res = dirname(from, from_dir);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to get directory for '%s'", from);
        return -1;
    }
    char to_dir[PATH_MAX];
    res = dirname(to, to_dir);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to get directory for '%s'", to);
        return -1;
    }

    char from_dir_next[PATH_MAX];
    res = dirname(from_dir, from_dir_next);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to get directory for '%s'",
               from_dir);
        return -1;
    }
    char to_dir_next[PATH_MAX];
    res = dirname(to_dir, to_dir_next);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to get directory for '%s'",
               to_dir);
        return -1;
    }

    res = strcmp(from_dir_next, to_dir_next);
    if (res != 0) {
        syslog(LOG_ERR, "rename: directories do not match: "
                        "'%s' and '%s'", from_dir_next,
                        to_dir_next);
        return -1;
    }

    char to_dir_next_single[PATH_MAX];
    res = basename(to_dir, to_dir_next_single);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to get basename from '%s'",
               to_dir);
        return -1;
    }

    char to_basename[PATH_MAX];
    res = basename(to, to_basename);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to get basename from '%s'",
               to);
        return -1;
    }

    char from_backing_path[PATH_MAX];
    res = resolve_path(from, from_backing_path);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to resolve '%s'", from);
        return -1;
    }

    char from_maildir_path[PATH_MAX];
    ssize_t len = readlink(from_backing_path, from_maildir_path, PATH_MAX);
    if (len == PATH_MAX) {
        syslog(LOG_ERR, "rename: too much path data for '%s'",
               from_backing_path);
        return -1;
    }
    if (len == -1) {
        syslog(LOG_ERR, "rename: unable to read link for '%s': %s",
               from_backing_path, strerror(errno));
        return -1;
    }
    from_maildir_path[len] = 0;

    char maildir_basename[PATH_MAX];
    res = basename(from_maildir_path, maildir_basename);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to get basename for '%s'",
               from_maildir_path);
        return -1;
    }

    char from_maildir_dir[PATH_MAX];
    char to_maildir_path[PATH_MAX];
    res = dirname(from_maildir_path, from_maildir_dir);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to get directory for '%s'",
               from_maildir_path);
        return -1;
    }
    res = dirname(from_maildir_dir, to_maildir_path);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to get directory for '%s'",
               from_maildir_dir);
        return -1;
    }

    strcat(to_maildir_path, "/");
    strcat(to_maildir_path, to_dir_next_single);
    strcat(to_maildir_path, "/");
    if (!flags) {
        strcat(to_maildir_path, to_basename);
    } else {
        char *to_flags = strrchr(maildir_basename, ':');
        if (!to_flags) {
            strcat(to_maildir_path, maildir_basename);
            strcat(to_maildir_path, flags);
        } else {
            strcpy(to_flags, flags);
            strcat(to_maildir_path, maildir_basename);
        }
    }
    syslog(LOG_DEBUG, "rename: maildir paths: (%s) -> (%s)",
        from_maildir_path, to_maildir_path);

    res = rename(from_maildir_path, to_maildir_path);
    if (res != 0) {
        syslog(LOG_ERR, "rename: unable to rename '%s' to '%s': %s",
               from_maildir_path, to_maildir_path,
               strerror(errno));
        return -1;
    }

    res = update_link_mapping(from_maildir_path, to_maildir_path,
                              to_dir_next_single, to_basename, flags);
    if (res != 0) {
        syslog(LOG_ERR, "rename: ulm failed: %s",
               strerror(errno));
        return -1;
    }

    syslog(LOG_DEBUG, "rename: '%s' to '%s' completed", from, to);
    return 0;
}

static int fsmu_read(const char *path, char *buf, size_t size,
                     off_t offset, struct fuse_file_info *info)
{
    syslog(LOG_DEBUG, "read: '%s'", path);
    verify_path(path);

    int len = strlen(path);
    if (len >= 9) {
        const char *tail = path + len - 9;
        if (strcmp(tail, "/.refresh") == 0) {
            syslog(LOG_INFO, "getattr: forcibly refreshing path");
            refresh_dir(path, 1);
            return 0;
        }
    }

    char backing_path[PATH_MAX];
    int res = resolve_path(path, backing_path);
    if (res != 0) {
        syslog(LOG_ERR, "read: unable to resolve '%s'", path);
        return -1;
    }

    FILE *backing_file = fopen(backing_path, "r");
    if (!backing_file) {
        syslog(LOG_ERR, "read: unable to open '%s': %s", path,
               strerror(errno));
        return -1;
    }

    res = fseek(backing_file, offset, SEEK_SET);
    if (res != 0) {
        syslog(LOG_ERR, "read: '%s': failed to seek: %s",
               path, strerror(errno));
        return res;
    }

    size_t bytes = fread(buf, 1, size, backing_file);
    res = fclose(backing_file);
    if (res != 0) {
        syslog(LOG_ERR, "read: '%s': failed to close: %s",
               path, strerror(errno));
        return res;
    }

    syslog(LOG_DEBUG, "read: '%s' completed", path);
    return bytes;
}

static int fsmu_mkdir(const char *path, mode_t mode)
{
    syslog(LOG_DEBUG, "mkdir: '%s'", path);
    verify_path(path);

    char backing_path[PATH_MAX];
    int res = resolve_path_noexists(path, backing_path);
    if (res != 0) {
        sprintf(backing_path, "%s%s", options.backing_dir, path);
    }
    res = mkdir(backing_path, mode);
    if (res != 0) {
        syslog(LOG_ERR, "mkdir: '%s': failed: %s",
               path, strerror(errno));
        return -1 * errno;
    }

    syslog(LOG_DEBUG, "mkdir: '%s' completed", path);
    return 0;
}

static int fsmu_rmdir(const char *path)
{
    syslog(LOG_DEBUG, "rmdir: '%s'", path);
    verify_path(path);

    if (strchr(path + 1, '/') != NULL) {
        syslog(LOG_ERR, "rmdir: cannot remove nested directory '%s'",
               path);
        return -1;
    }

    char real_path[PATH_MAX];
    sprintf(real_path, "%s%s", options.backing_dir, path);
    int res = rmdir(real_path);
    if (res != 0) {
        syslog(LOG_ERR, "rmdir: '%s': failed: %s",
               path, strerror(errno));
        return -1 * errno;
    }

    strcat(real_path, ".last-update");
    res = unlink(real_path);
    if (res != 0) {
        syslog(LOG_INFO, "rmdir: '%s': unable to remove "
                         "last-update file: %s",
               path, strerror(errno));
    }

    path = path + 1;
    char backing_path[PATH_MAX];
    sprintf(backing_path, "%s/_%s/cur", options.backing_dir, path);
    DIR *backing_handle = opendir(backing_path);
    if (!backing_handle) {
        syslog(LOG_ERR, "rmdir: cannot open '%s': %s",
               backing_path, strerror(errno));
        return -1;
    }
    struct dirent *dent;
    while ((dent = readdir(backing_handle)) != NULL) {
        if (is_dot(dent->d_name)) {
            continue;
        }
        char backing_file[PATH_MAX];
        strcpy(backing_file, backing_path);
        strcat(backing_file, "/");
        strcat(backing_file, dent->d_name);

        char maildir_path[PATH_MAX];
        ssize_t len = readlink(backing_file, maildir_path, PATH_MAX);
        if (len == PATH_MAX) {
            syslog(LOG_ERR, "rmdir: too much path data for '%s'",
                backing_file);
            closedir(backing_handle);
            return -1;
        }
        if (len == -1) {
            syslog(LOG_ERR, "rmdir: unable to read link for '%s': %s",
                backing_file, strerror(errno));
            closedir(backing_handle);
            return -1;
        }
        maildir_path[len] = 0;

        res = unlink(backing_file);
        if (res != 0) {
            syslog(LOG_ERR, "rmdir: cannot remove file '%s': %s",
                   backing_file, strerror(errno));
            closedir(backing_handle);
            return -1;
        }
        res = remove_link_mapping(maildir_path, backing_file);
        if (res != 0) {
            closedir(backing_handle);
            return -1;
        }
    }
    closedir(backing_handle);
    res = rmdir(backing_path);
    if (res != 0) {
        syslog(LOG_ERR, "rmdir: cannot remove '%s': %s",
                backing_path, strerror(errno));
        return -1;
    }

    sprintf(backing_path, "%s/_%s/new", options.backing_dir, path);
    backing_handle = opendir(backing_path);
    if (!backing_handle) {
        syslog(LOG_ERR, "rmdir: cannot open '%s': %s",
               backing_path, strerror(errno));
        return -1;
    }
    while ((dent = readdir(backing_handle)) != NULL) {
        if (is_dot(dent->d_name)) {
            continue;
        }
        char backing_file[PATH_MAX];
        strcpy(backing_file, backing_path);
        strcat(backing_file, "/");
        strcat(backing_file, dent->d_name);

        char maildir_path[PATH_MAX];
        ssize_t len = readlink(backing_file, maildir_path, PATH_MAX);
        if (len == PATH_MAX) {
            syslog(LOG_ERR, "rmdir: too much path data for '%s'",
                   backing_file);
            closedir(backing_handle);
            return -1;
        }
        if (len == -1) {
            syslog(LOG_ERR, "rmdir: unable to read link for '%s': %s",
                   backing_file, strerror(errno));
            closedir(backing_handle);
            return -1;
        }
        maildir_path[len] = 0;

        res = unlink(backing_file);
        if (res != 0) {
            syslog(LOG_ERR, "rmdir: cannot remove file '%s': %s",
                   backing_file, strerror(errno));
            closedir(backing_handle);
            return -1;
        }
        res = remove_link_mapping(maildir_path, backing_file);
        if (res != 0) {
            closedir(backing_handle);
            return -1;
        }
    }
    closedir(backing_handle);
    res = rmdir(backing_path);
    if (res != 0) {
        syslog(LOG_ERR, "rmdir: cannot remove '%s': %s",
                backing_path, strerror(errno));
        return -1;
    }
    sprintf(backing_path, "%s/_%s", options.backing_dir, path);
    res = rmdir(backing_path);
    if (res != 0) {
        syslog(LOG_ERR, "rmdir: cannot remove '%s': %s",
                backing_path, strerror(errno));
        return -1;
    }

    syslog(LOG_DEBUG, "rmdir: '%s' completed", path);
    return 0;
}

static int fsmu_unlink(const char *path)
{
    syslog(LOG_DEBUG, "unlink: '%s'", path);
    verify_path(path);

    if (!options.delete_remove) {
        syslog(LOG_DEBUG, "unlink: --delete-remove not set, returning");
        return 0;
    }

    char backing_path[PATH_MAX];
    int res = resolve_path(path, backing_path);
    if (res != 0) {
        syslog(LOG_ERR, "unlink: unable to resolve '%s'",
               path);
        return -1;
    }

    char maildir_path[PATH_MAX];
    ssize_t len = readlink(backing_path, maildir_path, PATH_MAX);
    if (len == PATH_MAX) {
        syslog(LOG_ERR, "unlink: too much path data for '%s'",
               backing_path);
        return -1;
    }
    if (len == -1) {
        syslog(LOG_ERR, "unlink: unable to read link for '%s': %s",
               backing_path, strerror(errno));
        return -1;
    }
    maildir_path[len] = 0;

    res = unlink(maildir_path);
    if (res != 0) {
        syslog(LOG_ERR, "unlink: '%s': unable to remove: %s",
               maildir_path, strerror(errno));
        return -1;
    }
    res = unlink(backing_path);
    if (res != 0) {
        syslog(LOG_ERR, "unlink: '%s': unable to remove: %s",
               backing_path, strerror(errno));
        return -1;
    }

    syslog(LOG_DEBUG, "unlink: '%s' completed", path);
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

    backing_dir_reverse = malloc(PATH_MAX);
    strcpy(backing_dir_reverse, options.backing_dir);
    strcat(backing_dir_reverse, "/_reverse");

    fuse_main(args.argc, args.argv, &operations, NULL);
    fuse_opt_free_args(&args);

    return 0;
}
