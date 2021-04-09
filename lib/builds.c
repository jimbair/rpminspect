/*
 * Copyright © 2019 Red Hat, Inc.
 * Author(s): David Cantrell <dcantrell@redhat.com>
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program.  If not, see
 * <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

/**
 * @file builds.c
 * @author David Cantrell &lt;dcantrell@redhat.com&gt;
 * @date 2019-2020
 * @brief Collect builds from either a local or remote (Koji) source.
 * @copyright LGPL-3.0-or-later
 */

#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>
#include <assert.h>
#include <ftw.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>
#include <rpm/rpmlib.h>
#include <curl/curl.h>
#include <yaml.h>
#include "rpminspect.h"

/* Local global variables */
static struct rpminspect *workri = NULL;
static int whichbuild = BEFORE_BUILD;
static bool fetch_only = false;
static int mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;

/* This array holds strings that map to the whichbuild index value. */
static char *build_desc[] = { "before", "after" };

/* Local prototypes */
static void set_worksubdir(struct rpminspect *, workdir_t, const struct koji_build *, const struct koji_task *);
static void get_rpm_info(const char *);
static void prune_local(const int);
static int copytree(const char *, const struct stat *, int, struct FTW *);
static int download_build(const struct rpminspect *, const struct koji_build *);
static int download_task(const struct rpminspect *, const struct koji_task *);
static void curl_helper(const bool, const char *, const char *);

/*
 * Set the working subdirectory for this particular run based on whether
 * this is a remote build or a local build.
 */
static void set_worksubdir(struct rpminspect *ri, workdir_t wd, const struct koji_build *build, const struct koji_task *task)
{
    assert(ri != NULL);
    assert(wd != NULL_WORKDIR);

    if (ri->worksubdir != NULL) {
        return;
    }

    if (fetch_only) {
        if (build != NULL) {
            xasprintf(&ri->worksubdir, "%s/%s", ri->workdir, build->nvr);
        } else if (task != NULL) {
            xasprintf(&ri->worksubdir, "%s/scratch-%d", ri->workdir, task->id);
        } else {
            errx(RI_PROGRAM_ERROR, _("no koji build or task specified"));
        }

        if (mkdirp(ri->worksubdir, mode)) {
            err(RI_PROGRAM_ERROR, _("unable to create download directory %s"), ri->worksubdir);
        }
    } else {
        if (wd == LOCAL_WORKDIR) {
            xasprintf(&ri->worksubdir, "%s/local.XXXXXX", ri->workdir);
        } else if (wd == TASK_WORKDIR) {
            assert(task != NULL);
            xasprintf(&ri->worksubdir, "%s/scratch-%d.XXXXXX", ri->workdir, task->id);
        } else if (wd == BUILD_WORKDIR) {
            assert(build != NULL);
            xasprintf(&ri->worksubdir, "%s/%s-%s.XXXXXX", ri->workdir, build->name, build->version);
        } else {
            errx(RI_PROGRAM_ERROR, _("unknown workdir type %d"), wd);
        }

        if (mkdtemp(ri->worksubdir) == NULL) {
            err(RI_PROGRAM_ERROR, "mkdtemp()");
        }
    }

    return;
}

/*
 * Collect package peer information.
 */
static void get_rpm_info(const char *pkg)
{
    Header h;

    assert(pkg != NULL);
    h = get_rpm_header(workri, pkg);

    if (h == NULL) {
        return;
    }

    add_peer(&workri->peers, whichbuild, fetch_only, pkg, h);
    return;
}

/*
 * Walk a local build tree and prune empty arch subdirectories.
 */
static void prune_local(const int whichbuild) {
    char *lpath = NULL;
    char *apath = NULL;
    DIR *d = NULL;
    struct dirent *de = NULL;

    if (fetch_only) {
        lpath = strdup(workri->worksubdir);
    } else {
        xasprintf(&lpath, "%s/%s", workri->worksubdir, build_desc[whichbuild]);
    }

    if ((d = opendir(lpath)) == NULL) {
        free(lpath);
        return;
    }

    while ((de = readdir(d)) != NULL) {
        xasprintf(&apath, "%s/%s", lpath, de->d_name);
        (void) rmdir(apath);
        free(apath);
    }

    if (closedir(d) == -1) {
        warn("closedir()");
        return;
    }

    free(lpath);

    return;
}

/*
 * Used to recursively copy a build tree over to the working directory.
 */
static int copytree(const char *fpath, const struct stat *sb, int tflag, struct FTW *ftwbuf)
{
    static int toptrim = 0;
    char *workfpath = NULL;
    char *bufpath = NULL;
    char *copysrc = NULL;
    Header h;
    const char *arch = NULL;
    int ret = 0;

    /*
     * On our first call, take the length of the main directory that
     * we will work relative to for this rescursive copy.
     */
    if (ftwbuf->level == 0 && S_ISDIR(sb->st_mode)) {
        toptrim = strlen(fpath) + 1;
        return 0;
    }

    /* Ignore unreadable things */
    if (tflag == FTW_DNR || tflag == FTW_NS || tflag == FTW_SLN) {
        warnx(_("unable to read %s, skipping"), fpath);
        return 0;
    }

    /* Copy file or create a directory */
    workfpath = ((char *) fpath) + toptrim;
    xasprintf(&bufpath, "%s/%s/%s", workri->worksubdir, build_desc[whichbuild], workfpath);
    copysrc = realpath(fpath, NULL);

    if (S_ISDIR(sb->st_mode)) {
        if (mkdirp(bufpath, mode)) {
            warn("mkdirp()");
            ret = -1;
        }
    } else if (S_ISREG(sb->st_mode) || S_ISLNK(sb->st_mode)) {
        h = get_rpm_header(workri, fpath);

        if (h == NULL) {
            return ret;
        }

        arch = get_rpm_header_arch(h);

        if (!allowed_arch(workri, arch)) {
            return 0;
        }

        if (copyfile(copysrc, bufpath, true, false)) {
            warn("copyfile()");
            ret = -1;
        }

        /* Gather the RPM header for packages */
        get_rpm_info(bufpath);
    } else {
        warnx(_("unknown directory member encountered: %s"), fpath);
        ret = -1;
    }

    fflush(stderr);
    free(bufpath);
    free(copysrc);

    return ret;
}

/*
 * Download helper for libcurl
 */
static void curl_helper(const bool verbose, const char *src, const char *dst) {
    FILE *fp = NULL;
    CURL *c = NULL;
    int r;
    CURLcode cc;

    /* ignore unusued variable warnings if assert is disabled */
    (void) r;
    (void) cc;

    assert(src != NULL);
    assert(dst != NULL);

    DEBUG_PRINT("src=|%s|\ndst=|%s|\n", src, dst);

    /* initialize curl */
    if (!(c = curl_easy_init())) {
        warn("curl_easy_init()");
        return;
    }

    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, NULL);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);

    if (verbose) {
        printf(_("Downloading %s...\n"), src);
    }

    /* perform the download */
    fp = fopen(dst, "wb");

    if (fp == NULL) {
        err(RI_PROGRAM_ERROR, "fopen()");
    }

    curl_easy_setopt(c, CURLOPT_URL, src);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, true);
#ifdef CURLOPT_TCP_FASTOPEN /* not available on all versions of libcurl (e.g., <= 7.29) */
    curl_easy_setopt(c, CURLOPT_TCP_FASTOPEN, 1);
#endif
    cc = curl_easy_perform(c);

    if (fclose(fp) != 0) {
        err(RI_PROGRAM_ERROR, "fclose()");
    }

    /* remove output file if there was a download error (e.g., 404) */
    if (cc != CURLE_OK) {
        if (unlink(dst)) {
            warn("unlink()");
        }
    }

    curl_easy_cleanup(c);

    return;
}

/*
 * Given a remote artifact specification in a Koji build, download it
 * to our working directory.
 */
static int download_build(const struct rpminspect *ri, const struct koji_build *build)
{
    koji_buildlist_entry_t *buildentry = NULL;
    koji_rpmlist_entry_t *rpm = NULL;
    char *src = NULL;
    char *srcfmt = NULL;
    char *dst = NULL;
    char *pkg = NULL;
    FILE *fp = NULL;
    yaml_parser_t parser;
    yaml_token_t token;
    int in_filter = 0;
    string_list_t *filter = NULL;
    string_entry_t *filtered_rpm = NULL;
    bool filtered = false;

    assert(build != NULL);
    assert(build->builds != NULL);

    /* Iterate over list of builds, each with a list of packages */
    TAILQ_FOREACH(buildentry, build->builds, builditems) {
        if (TAILQ_EMPTY(buildentry->rpms)) {
            /*
             * not sure how you could have an empty package list,
             * but let's not underestimate what jobs people can
             * submit to koji
             */
            continue;
        }

        /* Download module metadata at the top level */
        if (ri->buildtype == KOJI_BUILD_MODULE) {
            /* Create destination directory */
            if (fetch_only) {
                xasprintf(&dst, "%s/files", workri->worksubdir);
            } else {
                xasprintf(&dst, "%s/%s/files", workri->worksubdir, build_desc[whichbuild]);
            }

            if (mkdirp(dst, mode)) {
                warn("mkdirp()");
                return -1;
            }

            /* Get the main metadata file */
            dst = strappend(dst, "/modulemd.txt", NULL);
            xasprintf(&src, "%s/packages/%s/%s/%s/files/module/modulemd.txt", workri->kojimbs, build->package_name, build->version, build->release);
            curl_helper(workri->verbose, src, dst);
            free(src);

            /* Get the list of artifacts to filter if we don't have it */
            if (filter == NULL) {
                /* prepare a YAML parser */
                if (!yaml_parser_initialize(&parser)) {
                    warn("yaml_parser_initialize()");
                }

                /* open the modulemd file */
                if ((fp = fopen(dst, "r")) == NULL) {
                    err(RI_PROGRAM_ERROR, "fopen()");
                }

                /* initialize a string list for the loop */
                filter = calloc(1, sizeof(*filter));
                assert(filter != NULL);
                TAILQ_INIT(filter);

                /* tell the YAML parser to read the modulemd file */
                yaml_parser_set_input_file(&parser, fp);

                /*
                 * Loop over the YAML file looking for the filter:
                 * block and pull in the 'rpms' listed there.  Store
                 * them as a string_list_t which we will turn in to
                 * a hash table at the end.
                 */
                do {
                    yaml_parser_scan(&parser, &token);

                    switch (token.type) {
                        case YAML_SCALAR_TOKEN:
                            if ((in_filter == 0) && !strcmp((char *) token.data.scalar.value, "filter")) {
                                in_filter++;
                            } else if ((in_filter == 1) && !strcmp((char *) token.data.scalar.value, "rpms")) {
                                in_filter++;
                            } else if (in_filter == 2) {
                                filtered_rpm = calloc(1, sizeof(*filtered_rpm));
                                assert(filtered_rpm != NULL);
                                filtered_rpm->data = strdup((char *) token.data.scalar.value);
                                TAILQ_INSERT_TAIL(filter, filtered_rpm, items);
                            }

                            break;
                        case YAML_BLOCK_END_TOKEN:
                            in_filter = 0;
                            break;
                        default:
                            break;
                    }

                    if (token.type != YAML_STREAM_END_TOKEN) {
                        yaml_token_delete(&token);
                    }
                } while (token.type != YAML_STREAM_END_TOKEN);

                /* destroy the YAML parser, close the input file */
                yaml_parser_delete(&parser);

                if (fclose(fp) != 0) {
                    err(RI_PROGRAM_ERROR, _("fclose()"));
                }
            }

            /* Need to use this in the next loop */
            free(dst);
        }

        /* Iterate over the list of packages for this build */
        TAILQ_FOREACH(rpm, buildentry->rpms, items) {
            /* skip arches the user wishes to exclude */
            if (!allowed_arch(ri, rpm->arch)) {
                continue;
            }

            /* create the destination directory */
            if (fetch_only) {
                xasprintf(&dst, "%s/%s", workri->worksubdir, rpm->arch);
            } else {
                xasprintf(&dst, "%s/%s/%s", workri->worksubdir, build_desc[whichbuild], rpm->arch);
            }

            if (mkdirp(dst, mode)) {
                warn("mkdirp()");
                return -1;
            }

            free(dst);

            /* for modules, get the per-arch module metadata */
            if (workri->buildtype == KOJI_BUILD_MODULE) {
                if (fetch_only) {
                    xasprintf(&dst, "%s/%s/modulemd.%s.txt", workri->worksubdir, rpm->arch, rpm->arch);
                } else {
                    xasprintf(&dst, "%s/%s/%s/modulemd.%s.txt", workri->worksubdir, build_desc[whichbuild], rpm->arch, rpm->arch);
                }

                /* only download this file if we have not already gotten it */
                if (access(dst, F_OK|R_OK)) {
                    xasprintf(&src, "%s/packages/%s/%s/%s/files/module/modulemd.%s.txt", workri->kojimbs, build->package_name, build->version, build->release, rpm->arch);
                    curl_helper(workri->verbose, src, dst);
                    free(src);
                }

                free(dst);
            }

            /* for module builds, filter out packages */
            if (workri->buildtype == KOJI_BUILD_MODULE && filter != NULL) {
                filtered = false;

                TAILQ_FOREACH(filtered_rpm, filter, items) {
                    if (!strcmp(filtered_rpm->data, rpm->name)) {
                        filtered = true;
                        break;
                    }
                }

                if (filtered) {
                    continue;
                }
            }

            /* build path strings */
            xasprintf(&pkg, "%s-%s-%s.%s.rpm", rpm->name, rpm->version, rpm->release, rpm->arch);

            if (fetch_only) {
                xasprintf(&dst, "%s/%s/%s", workri->worksubdir, rpm->arch, pkg);
            } else {
                xasprintf(&dst, "%s/%s/%s/%s", workri->worksubdir, build_desc[whichbuild], rpm->arch, pkg);
            }

            if (build->volume_name == NULL || !strcmp(build->volume_name, "DEFAULT")) {
                srcfmt = "%s/%s/%s/%s/%s/%s/%s";
            } else {
                srcfmt = "%s/vol/%s/packages/%s/%s/%s/%s/%s";
            }

            /* construct the source download url */
            xasprintf(&src, srcfmt,
                      (workri->buildtype == KOJI_BUILD_MODULE) ? workri->kojimbs : workri->kojiursine,
                      (build->volume_name == NULL || !strcmp(build->volume_name, "DEFAULT")) ? "packages" : build->volume_name,
                      (buildentry->package_name == NULL) ? build->name : buildentry->package_name,
                      (buildentry->version == NULL) ? build->version : buildentry->version,
                      (buildentry->release == NULL) ? build->release : buildentry->release,
                      rpm->arch,
                      pkg);

            /* download the package */
            curl_helper(workri->verbose, src, dst);

            /* gather the RPM header */
            get_rpm_info(dst);

            /* start over */
            free(src);
            free(dst);
            free(pkg);
        }

        list_free(filter, free);
        filter = NULL;
    }

    return 0;
}

/*
 * Given a remote artifact specification in a Koji task, download it
 * to our working directory.
 */
static int download_task(const struct rpminspect *ri, const struct koji_task *task)
{
    size_t len;
    char *pkg = NULL;
    char *src = NULL;
    char *dst = NULL;
    char *tail = NULL;
    koji_task_entry_t *descendent = NULL;
    string_entry_t *entry = NULL;

    assert(ri != NULL);
    assert(task != NULL);
    assert(task->descendents != NULL);

    TAILQ_FOREACH(descendent, task->descendents, items) {
        /* skip if we have nothing */
        if (TAILQ_EMPTY(descendent->srpms) && TAILQ_EMPTY(descendent->rpms)) {
            continue;
        }

        /* create the destination directory */
        if (fetch_only) {
            xasprintf(&dst, "%s/%s", workri->worksubdir, descendent->task->arch);
        } else {
            xasprintf(&dst, "%s/%s/%s", workri->worksubdir, build_desc[whichbuild], descendent->task->arch);
        }

        if (mkdirp(dst, mode)) {
            warn("mkdirp()");
            return -1;
        }

        free(dst);

        /* download SRPMs */
        if (allowed_arch(ri, "src")) {
            TAILQ_FOREACH(entry, descendent->srpms, items) {
                pkg = basename(entry->data);

                if (fetch_only) {
                    xasprintf(&dst, "%s/src", workri->worksubdir);
                } else {
                    xasprintf(&dst, "%s/%s/src", workri->worksubdir, build_desc[whichbuild]);
                }

                if (mkdirp(dst, mode)) {
                    warn("mkdirp()");
                    return -1;
                }

                len = strlen(dst);
                dst = realloc(dst, len + strlen(pkg) + 2);
                tail = dst + len;
                tail = stpcpy(tail, "/");
                tail = stpcpy(tail, pkg);
                assert(dst != NULL);

                xasprintf(&src, "%s/work/%s", workri->kojiursine, entry->data);
                curl_helper(workri->verbose, src, dst);

                /* gather the RPM header */
                get_rpm_info(dst);

                free(dst);
                free(src);
            }
        }

        TAILQ_FOREACH(entry, descendent->rpms, items) {
            /* skip arches the user wishes to exclude */
            if (!allowed_arch(ri, descendent->task->arch)) {
                continue;
            }

            pkg = basename(entry->data);

            if (fetch_only) {
                xasprintf(&dst, "%s/%s/%s", workri->worksubdir, descendent->task->arch, pkg);
            } else {
                xasprintf(&dst, "%s/%s/%s/%s", workri->worksubdir, build_desc[whichbuild], descendent->task->arch, pkg);
            }

            xasprintf(&src, "%s/work/%s", workri->kojiursine, entry->data);
            curl_helper(workri->verbose, src, dst);

            /* gather the RPM header */
            get_rpm_info(dst);

            free(dst);
            free(src);
        }
    }

    return 0;
}

/*
 * Given a remote RPM, download it to our working directory.
 */
static int download_rpm(const char *rpm)
{
    char *pkg = NULL;
    char *dstdir = NULL;
    char *dst = NULL;

    assert(rpm != NULL);

    /* the RPM filename */
    pkg = basename(rpm);

    /* create the destination directory */
    if (fetch_only) {
        dstdir = strdup(workri->worksubdir);
    } else {
        xasprintf(&dstdir, "%s/%s", workri->worksubdir, build_desc[whichbuild]);
    }

    if (mkdirp(dstdir, mode)) {
        warn("mkdirp()");
        return -1;
    }

    /* download the package */
    xasprintf(&dst, "%s/%s", dstdir, pkg);
    curl_helper(workri->verbose, rpm, dst);

    /* gather the RPM header */
    get_rpm_info(dst);

    /* clean up */
    free(dst);
    free(dstdir);

    return 0;
}

/* Returns true if the string specifies a task ID, which is just an int */
static bool is_task_id(const char *id)
{
    int i = 0;

    assert(id != NULL);

    while (id[i] != '\0') {
        if (!isdigit(id[i])) {
            return false;
        }

        i++;
    }

    return true;
}

/*
 * Returns true if a string contains a valid URL to an RPM, false
 * otherwise.  The check isn't perfect because it just looks at the
 * ending of the URL for ".rpm"
 */
static bool is_remote_rpm(const char *url)
{
    CURL *c = NULL;
    CURLcode r = -1;

    assert(url != NULL);

    if (!strsuffix(url, RPM_FILENAME_EXTENSION)) {
        return false;
    }

    c = curl_easy_init();

    if (c) {
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_NOBODY, 1);
        r = curl_easy_perform(c);
        curl_easy_cleanup(c);
    }

    if (r == CURLE_OK) {
        return true;
    } else {
        return false;
    }
}

/**
 * @brief Collects specified builds in to the working directory.
 *
 * For each build that is not NULL, determine if it is local or remote
 * and collect it in the appropriate manner.  For local builds, that
 * is just copying files.  For remote builds, **libcurl** is called to
 * download files.  Files are downloaded to each build's subdirectory
 * in the program working directory.  This function gathers both
 * before and after builds if specified at run time.
 *
 * @param ri The main program data structure; contains the before and
 *        after build specifications from the command line.
 * @param fo True if '-f' (fetch only) specified, false otherwise.
 * @return 0 on success, non-zero on failure.
 */
int gather_builds(struct rpminspect *ri, bool fo) {
    struct koji_build *build = NULL;
    struct koji_task *task = NULL;

    assert(ri != NULL);
    assert(ri->after != NULL);

    workri = ri;
    fetch_only = fo;

    /* process after first so the temp directory gets the NV of that pkg */
    if (ri->after != NULL) {
        whichbuild = AFTER_BUILD;

        if (is_local_build(ri->after) || is_local_rpm(ri, ri->after)) {
            if (fetch_only) {
                warnx(_("`%s' already exists"), ri->after);
                return -1;
            }

            set_worksubdir(ri, LOCAL_WORKDIR, NULL, NULL);

            /* copy after tree */
            if (nftw(ri->after, copytree, FOPEN_MAX, FTW_PHYS) == -1) {
                warn("nftw()");
                return -1;
            }

            /* clean up */
            prune_local(whichbuild);
        } else if (is_remote_rpm(ri->after)) {
            set_worksubdir(ri, LOCAL_WORKDIR, NULL, NULL);

            if (download_rpm(ri->after)) {
                warn("download_rpm()");
                return -1;
            }
        } else if (is_task_id(ri->after) && (task = get_koji_task(ri, ri->after)) != NULL) {
            set_worksubdir(ri, TASK_WORKDIR, NULL, task);

            if (download_task(ri, task)) {
                warn("download_task()");
                free_koji_task(task);
                return -1;
            }

            free_koji_task(task);
        } else if ((build = get_koji_build(ri, ri->after)) != NULL) {
            set_worksubdir(ri, BUILD_WORKDIR, build, NULL);

            if (download_build(ri, build)) {
                warn("download_build()");
                free_koji_build(build);
                return -1;
            }

            free_koji_build(build);
        } else {
            warnx(_("unable to find after build: %s"), ri->after);
            return -2;
        }
    }

    /* did we get a before build specified? */
    if (ri->before == NULL) {
        return 0;
    }

    whichbuild = BEFORE_BUILD;

    /* before build specified, find it */
    if (is_local_build(ri->before) || is_local_rpm(ri, ri->before)) {
        set_worksubdir(ri, LOCAL_WORKDIR, NULL, NULL);

        /* copy before tree */
        if (nftw(ri->before, copytree, FOPEN_MAX, FTW_PHYS) == -1) {
            warn("nftw()");
            return -1;
        }

        /* clean up */
        prune_local(whichbuild);
    } else if (is_remote_rpm(ri->before)) {
        set_worksubdir(ri, LOCAL_WORKDIR, NULL, NULL);

        if (download_rpm(ri->before)) {
            warn("download_rpm()");
            return -1;
        }
    } else if (is_task_id(ri->before) && (task = get_koji_task(ri, ri->before)) != NULL) {
        set_worksubdir(ri, TASK_WORKDIR, NULL, task);

        if (download_task(ri, task)) {
            warn("download_task()");
            free_koji_task(task);
            return -1;
        }

        free_koji_task(task);
    } else if ((build = get_koji_build(ri, ri->before)) != NULL) {
        set_worksubdir(ri, BUILD_WORKDIR, build, NULL);

        if (download_build(ri, build)) {
            warn("download_build()");
            free_koji_build(build);
            return -1;
        }

        free_koji_build(build);
    } else {
        warnx(_("unable to find before build: %s"), ri->before);
        return -1;
    }

    /*
     * init the arches list if the user did not specify it (we have
     * builds now)
     */
    init_arches(ri);

    return 0;
}
