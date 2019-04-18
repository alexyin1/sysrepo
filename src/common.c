/**
 * @file common.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief common routines
 *
 * @copyright
 * Copyright (c) 2019 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */
#include "common.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <pwd.h>
#include <grp.h>
#include <inttypes.h>
#include <time.h>
#include <assert.h>
#include <pthread.h>

sr_error_info_t *
sr_sub_conf_add(const char *mod_name, const char *xpath, sr_datastore_t ds, sr_module_change_cb conf_cb,
        void *private_data, uint32_t priority, sr_subscr_options_t sub_opts, sr_subscription_ctx_t *subs)
{
    sr_error_info_t *err_info = NULL;
    struct modsub_conf_s *conf_sub = NULL;
    uint32_t i;
    void *mem[4] = {NULL};

    /* SUBS LOCK */
    if ((err_info = sr_mlock(&subs->subs_lock, SR_SUB_EVENT_LOOP_TIMEOUT * 1000, __func__))) {
        return err_info;
    }

    /* try to find this module subscription SHM mapping, it may already exist */
    for (i = 0; i < subs->conf_sub_count; ++i) {
        if (!strcmp(mod_name, subs->conf_subs[i].module_name) && (subs->conf_subs[i].ds == ds)) {
            break;
        }
    }

    if (i == subs->conf_sub_count) {
        mem[0] = realloc(subs->conf_subs, (subs->conf_sub_count + 1) * sizeof *subs->conf_subs);
        SR_CHECK_MEM_GOTO(!mem[0], err_info, error_unlock);
        subs->conf_subs = mem[0];

        conf_sub = &subs->conf_subs[i];
        memset(conf_sub, 0, sizeof *conf_sub);
        conf_sub->sub_shm.fd = -1;

        /* set attributes */
        mem[1] = strdup(mod_name);
        SR_CHECK_MEM_GOTO(!mem[1], err_info, error_unlock);
        conf_sub->module_name = mem[1];
        conf_sub->ds = ds;

        /* create/open shared memory and map it */
        if ((err_info = sr_shmsub_open_map(mod_name, sr_ds2str(ds), -1, &conf_sub->sub_shm, sizeof(sr_multi_sub_shm_t)))) {
            goto error_unlock;
        }

        /* make the subscription visible only after everything succeeds */
        ++subs->conf_sub_count;
    } else {
        conf_sub = &subs->conf_subs[i];
    }

    /* add another XPath into module-specific subscriptions */
    mem[2] = realloc(conf_sub->subs, (conf_sub->sub_count + 1) * sizeof *conf_sub->subs);
    SR_CHECK_MEM_GOTO(!mem[2], err_info, error_unlock);
    conf_sub->subs = mem[2];

    if (xpath) {
        mem[3] = strdup(xpath);
        SR_CHECK_MEM_RET(!mem[3], err_info);
        conf_sub->subs[conf_sub->sub_count].xpath = mem[3];
    } else {
        conf_sub->subs[conf_sub->sub_count].xpath = NULL;
    }
    conf_sub->subs[conf_sub->sub_count].priority = priority;
    conf_sub->subs[conf_sub->sub_count].opts = sub_opts;
    conf_sub->subs[conf_sub->sub_count].cb = conf_cb;
    conf_sub->subs[conf_sub->sub_count].private_data = private_data;
    conf_sub->subs[conf_sub->sub_count].event_id = 0;
    conf_sub->subs[conf_sub->sub_count].event = SR_SUB_EV_NONE;

    ++conf_sub->sub_count;

    /* SUBS UNLOCK */
    sr_munlock(&subs->subs_lock);
    return NULL;

error_unlock:
    /* SUBS UNLOCK */
    sr_munlock(&subs->subs_lock);

    for (i = 0; i < 4; ++i) {
        free(mem[i]);
    }
    if (conf_sub) {
        sr_shm_clear(&conf_sub->sub_shm);
    }
    if (mem[1]) {
        --subs->conf_sub_count;
    }
    return err_info;
}

void
sr_sub_conf_del(const char *mod_name, const char *xpath, sr_datastore_t ds, sr_module_change_cb conf_cb,
        void *private_data, uint32_t priority, sr_subscr_options_t sub_opts, sr_subscription_ctx_t *subs)
{
    sr_error_info_t *err_info = NULL;
    uint32_t i, j;
    struct modsub_conf_s *conf_sub;

    /* SUBS LOCK */
    if ((err_info = sr_mlock(&subs->subs_lock, SR_SUB_EVENT_LOOP_TIMEOUT * 1000, __func__))) {
        sr_errinfo_free(&err_info);
        return;
    }

    for (i = 0; i < subs->conf_sub_count; ++i) {
        conf_sub = &subs->conf_subs[i];

        if ((conf_sub->ds != ds) || strcmp(mod_name, conf_sub->module_name)) {
            continue;
        }

        for (j = 0; j < conf_sub->sub_count; ++j) {
            if ((!xpath && conf_sub->subs[j].xpath) || (xpath && !conf_sub->subs[j].xpath)
                    || strcmp(conf_sub->subs[j].xpath, xpath)) {
                continue;
            }
            if ((conf_sub->subs[j].priority != priority) || (conf_sub->subs[j].opts != sub_opts)
                    || (conf_sub->subs[j].cb != conf_cb) || (conf_sub->subs[j].private_data != private_data)) {
                continue;
            }

            /* found our subscription, replace it with the last */
            free(conf_sub->subs[j].xpath);
            if (j < conf_sub->sub_count - 1) {
                memcpy(&conf_sub->subs[j], &conf_sub->subs[conf_sub->sub_count - 1], sizeof *conf_sub->subs);
            }
            --conf_sub->sub_count;

            if (!conf_sub->sub_count) {
                /* no other subscriptions for this module, replace it with the last */
                free(conf_sub->module_name);
                free(conf_sub->subs);
                sr_shm_clear(&conf_sub->sub_shm);
                if (i < subs->conf_sub_count - 1) {
                    memcpy(conf_sub, &subs->conf_subs[subs->conf_sub_count - 1], sizeof *conf_sub);
                }
                --subs->conf_sub_count;

                if (!subs->conf_sub_count) {
                    /* no other configuration subscriptions */
                    free(subs->conf_subs);
                    subs->conf_subs = NULL;
                }
            }

            /* SUBS UNLOCK */
            sr_munlock(&subs->subs_lock);
            return;
        }
    }

    /* unreachable */
    assert(0);

    /* SUBS UNLOCK */
    sr_munlock(&subs->subs_lock);
    return;
}

sr_error_info_t *
sr_sub_dp_add(const char *mod_name, const char *xpath, sr_dp_get_items_cb dp_cb, void *private_data,
        sr_subscription_ctx_t *subs)
{
    sr_error_info_t *err_info = NULL;
    struct modsub_dp_s *dp_sub = NULL;
    uint32_t i;
    void *mem[4] = {NULL};

    assert(mod_name && xpath);

    /* SUBS LOCK */
    if ((err_info = sr_mlock(&subs->subs_lock, SR_SUB_EVENT_LOOP_TIMEOUT * 1000, __func__))) {
        return err_info;
    }

    /* try to find this module subscription SHM mapping, it may already exist */
    for (i = 0; i < subs->dp_sub_count; ++i) {
        if (!strcmp(mod_name, subs->dp_subs[i].module_name)) {
            break;
        }
    }

    if (i == subs->dp_sub_count) {
        mem[0] = realloc(subs->dp_subs, (subs->dp_sub_count + 1) * sizeof *subs->dp_subs);
        SR_CHECK_MEM_GOTO(!mem[0], err_info, error_unlock);
        subs->dp_subs = mem[0];

        dp_sub = &subs->dp_subs[i];
        memset(dp_sub, 0, sizeof *dp_sub);

        /* set attributes */
        mem[1] = strdup(mod_name);
        SR_CHECK_MEM_GOTO(!mem[1], err_info, error_unlock);
        dp_sub->module_name = mem[1];

        /* make the subscription visible only after everything succeeds */
        ++subs->dp_sub_count;
    } else {
        dp_sub = &subs->dp_subs[i];
    }

    /* add another XPath and create SHM into module-specific subscriptions */
    mem[2] = realloc(dp_sub->subs, (dp_sub->sub_count + 1) * sizeof *dp_sub->subs);
    SR_CHECK_MEM_GOTO(!mem[2], err_info, error_unlock);
    dp_sub->subs = mem[2];
    memset(dp_sub->subs + dp_sub->sub_count, 0, sizeof *dp_sub->subs);
    dp_sub->subs[dp_sub->sub_count].sub_shm.fd = -1;

    /* set attributes */
    mem[3] = strdup(xpath);
    SR_CHECK_MEM_GOTO(!mem[3], err_info, error_unlock);
    dp_sub->subs[dp_sub->sub_count].xpath = mem[3];
    dp_sub->subs[dp_sub->sub_count].cb = dp_cb;
    dp_sub->subs[dp_sub->sub_count].private_data = private_data;

    /* create specific SHM and map it */
    if ((err_info = sr_shmsub_open_map(mod_name, "state", sr_str_hash(xpath), &dp_sub->subs[dp_sub->sub_count].sub_shm,
            sizeof(sr_sub_shm_t)))) {
        goto error_unlock;
    }

    ++dp_sub->sub_count;

    /* SUBS UNLOCK */
    sr_munlock(&subs->subs_lock);
    return NULL;

error_unlock:
    /* SUBS UNLOCK */
    sr_munlock(&subs->subs_lock);

    for (i = 0; i < 4; ++i) {
        free(mem[i]);
    }
    if (mem[1]) {
        --subs->dp_sub_count;
    }
    return err_info;
}

void
sr_sub_dp_del(const char *mod_name, const char *xpath, sr_subscription_ctx_t *subs)
{
    sr_error_info_t *err_info = NULL;
    uint32_t i, j;
    struct modsub_dp_s *dp_sub;

    /* SUBS LOCK */
    if ((err_info = sr_mlock(&subs->subs_lock, SR_SUB_EVENT_LOOP_TIMEOUT * 1000, __func__))) {
        sr_errinfo_free(&err_info);
        return;
    }

    for (i = 0; i < subs->dp_sub_count; ++i) {
        dp_sub = &subs->dp_subs[i];

        if (strcmp(mod_name, dp_sub->module_name)) {
            continue;
        }

        for (j = 0; j < dp_sub->sub_count; ++j) {
            if (strcmp(dp_sub->subs[j].xpath, xpath)) {
                continue;
            }

            /* found our subscription, replace it with the last */
            free(dp_sub->subs[j].xpath);
            sr_shm_clear(&dp_sub->subs[j].sub_shm);
            if (j < dp_sub->sub_count - 1) {
                memcpy(&dp_sub->subs[j], &dp_sub->subs[dp_sub->sub_count - 1], sizeof *dp_sub->subs);
            }
            --dp_sub->sub_count;

            if (!dp_sub->sub_count) {
                /* no other subscriptions for this module, replace it with the last */
                free(dp_sub->module_name);
                free(dp_sub->subs);
                if (i < subs->dp_sub_count - 1) {
                    memcpy(dp_sub, &subs->dp_subs[subs->dp_sub_count - 1], sizeof *dp_sub);
                }
                --subs->dp_sub_count;

                if (!subs->dp_sub_count) {
                    /* no other data-provide subscriptions */
                    free(subs->dp_subs);
                    subs->dp_subs = NULL;
                }
            }

            /* SUBS UNLOCK */
            sr_munlock(&subs->subs_lock);
            return;
        }
    }

    /* unreachable */
    assert(0);

    /* SUBS UNLOCK */
    sr_munlock(&subs->subs_lock);
    return;
}

sr_error_info_t *
sr_sub_rpc_add(const char *mod_name, const char *xpath, sr_rpc_cb rpc_cb, sr_rpc_tree_cb rpc_tree_cb,
        void *private_data, sr_subscription_ctx_t *subs)
{
    sr_error_info_t *err_info = NULL;
    struct modsub_rpc_s *rpc_sub = NULL;
    void *mem;

    assert(mod_name && xpath && (rpc_cb || rpc_tree_cb) && (!rpc_cb || !rpc_tree_cb));

    /* SUBS LOCK */
    if ((err_info = sr_mlock(&subs->subs_lock, SR_SUB_EVENT_LOOP_TIMEOUT * 1000, __func__))) {
        return err_info;
    }

    /* add another subscription */
    mem = realloc(subs->rpc_subs, (subs->rpc_sub_count + 1) * sizeof *subs->rpc_subs);
    SR_CHECK_MEM_GOTO(!mem, err_info, error_unlock);
    subs->rpc_subs = mem;
    rpc_sub = &subs->rpc_subs[subs->rpc_sub_count];
    memset(rpc_sub, 0, sizeof *rpc_sub);
    rpc_sub->sub_shm.fd = -1;

    /* set attributes */
    rpc_sub->xpath = strdup(xpath);
    SR_CHECK_MEM_GOTO(!rpc_sub->xpath, err_info, error_unlock);
    rpc_sub->cb = rpc_cb;
    rpc_sub->tree_cb = rpc_tree_cb;
    rpc_sub->private_data = private_data;

    /* create specific SHM and map it */
    if ((err_info = sr_shmsub_open_map(mod_name, "rpc", sr_str_hash(xpath), &rpc_sub->sub_shm, sizeof(sr_sub_shm_t)))) {
        free(rpc_sub->xpath);
        goto error_unlock;
    }

    ++subs->rpc_sub_count;

    /* SUBS UNLOCK */
    sr_munlock(&subs->subs_lock);
    return NULL;

error_unlock:
    /* SUBS UNLOCK */
    sr_munlock(&subs->subs_lock);

    return err_info;
}

void
sr_sub_rpc_del(const char *xpath, sr_subscription_ctx_t *subs)
{
    sr_error_info_t *err_info = NULL;
    uint32_t i;
    struct modsub_rpc_s *rpc_sub;

    /* SUBS LOCK */
    if ((err_info = sr_mlock(&subs->subs_lock, SR_SUB_EVENT_LOOP_TIMEOUT * 1000, __func__))) {
        sr_errinfo_free(&err_info);
        return;
    }

    for (i = 0; i < subs->rpc_sub_count; ++i) {
        rpc_sub = &subs->rpc_subs[i];

        if (strcmp(xpath, rpc_sub->xpath)) {
            continue;
        }

        /* found our subscription, replace it with the last */
        free(rpc_sub->xpath);
        sr_shm_clear(&rpc_sub->sub_shm);
        if (i < subs->rpc_sub_count - 1) {
            memcpy(&rpc_sub, &subs->rpc_subs[subs->rpc_sub_count - 1], sizeof *rpc_sub);
        }
        --subs->rpc_sub_count;

        if (!subs->rpc_sub_count) {
            /* no other RPC/action subscriptions */
            free(subs->rpc_subs);
            subs->rpc_subs = NULL;
        }

        /* SUBS UNLOCK */
        sr_munlock(&subs->subs_lock);
        return;
    }

    /* unreachable */
    assert(0);

    /* SUBS UNLOCK */
    sr_munlock(&subs->subs_lock);
    return;
}

sr_error_info_t *
sr_sub_notif_add(const char *mod_name, const char *xpath, time_t start_time, time_t stop_time, sr_event_notif_cb notif_cb,
        sr_event_notif_tree_cb notif_tree_cb, void *private_data, sr_subscription_ctx_t *subs)
{
    sr_error_info_t *err_info = NULL;
    struct modsub_notif_s *notif_sub = NULL;
    uint32_t i;
    void *mem[4] = {NULL};

    assert(mod_name);

    /* SUBS LOCK */
    if ((err_info = sr_mlock(&subs->subs_lock, SR_SUB_EVENT_LOOP_TIMEOUT * 1000, __func__))) {
        return err_info;
    }

    /* try to find this module subscriptions, they may already exist */
    for (i = 0; i < subs->notif_sub_count; ++i) {
        if (!strcmp(mod_name, subs->notif_subs[i].module_name)) {
            break;
        }
    }

    if (i == subs->notif_sub_count) {
        mem[0] = realloc(subs->notif_subs, (subs->notif_sub_count + 1) * sizeof *subs->notif_subs);
        SR_CHECK_MEM_GOTO(!mem[0], err_info, error_unlock);
        subs->notif_subs = mem[0];

        notif_sub = &subs->notif_subs[i];
        memset(notif_sub, 0, sizeof *notif_sub);
        notif_sub->sub_shm.fd = -1;

        /* set attributes */
        mem[1] = strdup(mod_name);
        SR_CHECK_MEM_GOTO(!mem[1], err_info, error_unlock);
        notif_sub->module_name = mem[1];

        /* create specific SHM and map it */
        if ((err_info = sr_shmsub_open_map(mod_name, "notif", -1, &notif_sub->sub_shm, sizeof(sr_sub_shm_t)))) {
            goto error_unlock;
        }

        /* make the subscription visible only after everything succeeds */
        ++subs->notif_sub_count;
    } else {
        notif_sub = &subs->notif_subs[i];
    }

    /* add another subscription */
    mem[2] = realloc(notif_sub->subs, (notif_sub->sub_count + 1) * sizeof *notif_sub->subs);
    SR_CHECK_MEM_GOTO(!mem[2], err_info, error_unlock);
    notif_sub->subs = mem[2];
    memset(notif_sub->subs + notif_sub->sub_count, 0, sizeof *notif_sub->subs);

    /* set attributes */
    if (xpath) {
        mem[3] = strdup(xpath);
        SR_CHECK_MEM_GOTO(!mem[3], err_info, error_unlock);
        notif_sub->subs[notif_sub->sub_count].xpath = mem[3];
    } else {
        notif_sub->subs[notif_sub->sub_count].xpath = NULL;
    }
    notif_sub->subs[notif_sub->sub_count].start_time = start_time;
    notif_sub->subs[notif_sub->sub_count].replayed = 0;
    notif_sub->subs[notif_sub->sub_count].stop_time = stop_time;
    notif_sub->subs[notif_sub->sub_count].cb = notif_cb;
    notif_sub->subs[notif_sub->sub_count].tree_cb = notif_tree_cb;
    notif_sub->subs[notif_sub->sub_count].private_data = private_data;

    ++notif_sub->sub_count;

    /* SUBS UNLOCK */
    sr_munlock(&subs->subs_lock);
    return NULL;

error_unlock:
    /* SUBS UNLOCK */
    sr_munlock(&subs->subs_lock);

    for (i = 0; i < 4; ++i) {
        free(mem[i]);
    }
    if (mem[1]) {
        --subs->notif_sub_count;
        sr_shm_clear(&notif_sub->sub_shm);
    }
    return err_info;
}

void
sr_sub_notif_del(const char *mod_name, const char *xpath, time_t start_time, time_t stop_time, sr_event_notif_cb notif_cb,
        sr_event_notif_tree_cb notif_tree_cb, void *private_data, sr_subscription_ctx_t *subs, int has_subs_lock)
{
    sr_error_info_t *err_info = NULL;
    uint32_t i, j;
    struct modsub_notif_s *notif_sub;

    if (!has_subs_lock) {
        /* SUBS LOCK */
        if ((err_info = sr_mlock(&subs->subs_lock, SR_SUB_EVENT_LOOP_TIMEOUT * 1000, __func__))) {
            sr_errinfo_free(&err_info);
            return;
        }
    }

    for (i = 0; i < subs->notif_sub_count; ++i) {
        notif_sub = &subs->notif_subs[i];

        if (strcmp(mod_name, notif_sub->module_name)) {
            continue;
        }

        for (j = 0; j < notif_sub->sub_count; ++j) {
            if ((!xpath && notif_sub->subs[j].xpath) || (xpath && (!notif_sub->subs[j].xpath
                    || strcmp(notif_sub->subs[j].xpath, xpath)))) {
                continue;
            }
            if ((start_time != notif_sub->subs[j].start_time) || (stop_time != notif_sub->subs[j].stop_time)
                    || (notif_cb != notif_sub->subs[j].cb) || (notif_tree_cb != notif_sub->subs[j].tree_cb)
                    || (private_data != notif_sub->subs[j].private_data)) {
                continue;
            }

            /* found our subscription, replace it with the last */
            free(notif_sub->subs[j].xpath);
            if (j < notif_sub->sub_count - 1) {
                memcpy(&notif_sub->subs[j], &notif_sub->subs[notif_sub->sub_count - 1], sizeof *notif_sub->subs);
            }
            --notif_sub->sub_count;

            if (!notif_sub->sub_count) {
                /* no other subscriptions for this module, replace it with the last */
                free(notif_sub->module_name);
                sr_shm_clear(&notif_sub->sub_shm);
                free(notif_sub->subs);
                if (i < subs->notif_sub_count - 1) {
                    memcpy(notif_sub, &subs->notif_subs[subs->notif_sub_count - 1], sizeof *notif_sub);
                }
                --subs->notif_sub_count;

                if (!subs->notif_sub_count) {
                    /* no other notification subscriptions */
                    free(subs->notif_subs);
                    subs->notif_subs = NULL;
                }
            }

            if (!has_subs_lock) {
                /* SUBS UNLOCK */
                sr_munlock(&subs->subs_lock);
            }
            return;
        }
    }

    /* unreachable */
    assert(0);

    if (!has_subs_lock) {
        /* SUBS UNLOCK */
        sr_munlock(&subs->subs_lock);
    }
    return;
}

sr_error_info_t *
sr_subs_del_all(sr_conn_ctx_t *conn, sr_subscription_ctx_t *subs)
{
    sr_error_info_t *err_info = NULL;
    char *mod_name, *path;
    uint32_t i, j;
    int last_removed;
    struct modsub_conf_s *conf_subs;
    struct modsub_dp_s *dp_sub;
    struct modsub_notif_s *notif_sub;

    /* configuration subscriptions */
    for (i = 0; i < subs->conf_sub_count; ++i) {
        conf_subs = &subs->conf_subs[i];
        for (j = 0; j < conf_subs->sub_count; ++j) {
            /* remove the subscriptions from the main SHM */
            if ((err_info = sr_shmmod_conf_subscription(conn, conf_subs->module_name, conf_subs->subs[j].xpath, conf_subs->ds,
                        conf_subs->subs[j].priority, conf_subs->subs[j].opts, subs->evpipe_num, 0, &last_removed))) {
                return err_info;
            }

            /* free xpath */
            free(conf_subs->subs[j].xpath);
        }

        if (last_removed) {
            /* delete the SHM file itself so that there is no leftover event */
            if ((err_info = sr_path_sub_shm(conf_subs->module_name, sr_ds2str(conf_subs->ds), -1, 1, &path))) {
                return err_info;
            }
            if (unlink(path) == -1) {
                SR_LOG_WRN("Failed to unlink SHM \"%s\" (%s).", path, strerror(errno));
            }
            free(path);
        }

        /* free dynamic memory */
        free(conf_subs->module_name);
        free(conf_subs->subs);

        /* remove specific SHM segment */
        sr_shm_clear(&conf_subs->sub_shm);
    }
    free(subs->conf_subs);

    /* data provider subscriptions */
    for (i = 0; i < subs->dp_sub_count; ++i) {
        dp_sub = &subs->dp_subs[i];
        for (j = 0; j < dp_sub->sub_count; ++j) {
            /* remove the subscriptions from the main SHM */
            if ((err_info = sr_shmmod_dp_subscription(conn, dp_sub->module_name, dp_sub->subs[j].xpath, SR_DP_SUB_NONE,
                    subs->evpipe_num, 0))) {
                return err_info;
            }

            /* delete the SHM file itself so that there is no leftover event */
            if ((err_info = sr_path_sub_shm(dp_sub->module_name, "state", sr_str_hash(dp_sub->subs[j].xpath), 1, &path))) {
                return err_info;
            }
            if (unlink(path) == -1) {
                SR_LOG_WRN("Failed to unlink SHM \"%s\" (%s).", path, strerror(errno));
            }
            free(path);

            /* free xpath */
            free(dp_sub->subs[j].xpath);

            /* remove specific SHM segment */
            sr_shm_clear(&dp_sub->subs[j].sub_shm);
        }

        /* free dynamic memory */
        free(dp_sub->module_name);
        free(dp_sub->subs);
    }
    free(subs->dp_subs);

    /* RPC/action subscriptions */
    for (i = 0; i < subs->rpc_sub_count; ++i) {
        /* remove the subscriptions from the main SHM */
        mod_name = sr_get_first_ns(subs->rpc_subs[i].xpath);
        SR_CHECK_INT_RET(!mod_name, err_info);
        if ((err_info = sr_shmmod_rpc_subscription(conn, mod_name, subs->rpc_subs[i].xpath, subs->evpipe_num, 0))) {
            free(mod_name);
            return err_info;
        }

        /* delete the SHM file itself so that there is no leftover event */
        if ((err_info = sr_path_sub_shm(mod_name, "rpc", sr_str_hash(subs->rpc_subs[i].xpath), 1, &path))) {
            free(mod_name);
            return err_info;
        }
        if (unlink(path) == -1) {
            SR_LOG_WRN("Failed to unlink SHM \"%s\" (%s).", path, strerror(errno));
        }
        free(path);
        free(mod_name);

        /* free xpath */
        free(subs->rpc_subs[i].xpath);

        /* remove specific SHM segment */
        sr_shm_clear(&subs->rpc_subs[i].sub_shm);
    }
    free(subs->rpc_subs);

    /* notification subscriptions */
    for (i = 0; i < subs->notif_sub_count; ++i) {
        notif_sub = &subs->notif_subs[i];
        for (j = 0; j < notif_sub->sub_count; ++j) {
            /* remove the subscriptions from the main SHM */
            if ((err_info = sr_shmmod_notif_subscription(conn, notif_sub->module_name, subs->evpipe_num, 0, &last_removed))) {
                return err_info;
            }

            /* free xpath */
            free(notif_sub->subs[j].xpath);
        }

        if (last_removed) {
            /* delete the SHM file itself so that there is no leftover event */
            if ((err_info = sr_path_sub_shm(notif_sub->module_name, "notif", -1, 1, &path))) {
                return err_info;
            }
            if (unlink(path) == -1) {
                SR_LOG_WRN("Failed to unlink SHM \"%s\" (%s).", path, strerror(errno));
            }
            free(path);
        }

        /* free dynamic memory */
        free(notif_sub->module_name);
        free(notif_sub->subs);

        /* remove specific SHM segment */
        sr_shm_clear(&notif_sub->sub_shm);
    }
    free(subs->notif_subs);

    return NULL;
}

sr_error_info_t *
sr_notif_find_subscriber(sr_conn_ctx_t *conn, const char *mod_name, sr_mod_notif_sub_t **notif_subs, uint32_t *notif_sub_count)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_t *shm_mod;

    shm_mod = sr_shmmain_find_module(&conn->main_shm, conn->main_ext_shm.addr, mod_name, 0);
    SR_CHECK_INT_RET(!shm_mod, err_info);

    *notif_subs = (sr_mod_notif_sub_t *)(conn->main_ext_shm.addr + shm_mod->notif_subs);
    *notif_sub_count = shm_mod->notif_sub_count;
    return NULL;
}

sr_error_info_t *
sr_notif_call_callback(sr_conn_ctx_t *conn, sr_event_notif_cb cb, sr_event_notif_tree_cb tree_cb, void *private_data,
        const sr_ev_notif_type_t notif_type, const struct lyd_node *notif_op, time_t notif_ts, sr_sid_t sid)
{
    sr_error_info_t *err_info = NULL;
    const struct lyd_node *next, *elem;
    void *mem;
    char *notif_xpath = NULL;
    sr_val_t *vals = NULL;
    size_t val_count = 0;
    sr_session_ctx_t tmp_sess;

    assert(!notif_op || (notif_op->schema->nodetype == LYS_NOTIF));
    assert((tree_cb && !cb) || (!tree_cb && cb));

    /* prepare temporary session */
    memset(&tmp_sess, 0, sizeof tmp_sess);
    tmp_sess.conn = conn;
    tmp_sess.ds = SR_DS_OPERATIONAL;
    tmp_sess.ev = SR_SUB_EV_NOTIF;
    tmp_sess.sid = sid;

    if (tree_cb) {
        /* callback */
        tree_cb(&tmp_sess, notif_type, notif_op, notif_ts, private_data);
    } else {
        if (notif_op) {
            /* prepare XPath */
            notif_xpath = lyd_path(notif_op);
            SR_CHECK_INT_GOTO(!notif_xpath, err_info, cleanup);

            /* prepare input for sr_val CB */
            LY_TREE_DFS_BEGIN(notif_op, next, elem) {
                /* skip op node */
                if (elem != notif_op) {
                    mem = realloc(vals, (val_count + 1) * sizeof *vals);
                    if (!mem) {
                        SR_ERRINFO_MEM(&err_info);
                        goto cleanup;
                    }
                    vals = mem;

                    if ((err_info = sr_val_ly2sr(elem, &vals[val_count]))) {
                        goto cleanup;
                    }

                    ++val_count;
                }

                LY_TREE_DFS_END(notif_op, next, elem);
            }
        }

        /* callback */
        cb(&tmp_sess, notif_type, notif_xpath, vals, val_count, notif_ts, private_data);
    }

    /* success */

cleanup:
    free(notif_xpath);
    sr_free_values(vals, val_count);
    sr_clear_sess(&tmp_sess);
    return err_info;
}

void
sr_clear_sess(sr_session_ctx_t *tmp_sess)
{
    uint16_t i;

    sr_errinfo_free(&tmp_sess->err_info);
    for (i = 0; i < 2; ++i) {
        lyd_free_withsiblings(tmp_sess->dt[i].edit);
        tmp_sess->dt[i].edit = NULL;
        lyd_free_withsiblings(tmp_sess->dt[i].diff);
        tmp_sess->dt[i].diff = NULL;
    }
}

sr_error_info_t *
sr_ly_ctx_new(struct ly_ctx **ly_ctx)
{
    sr_error_info_t *err_info = NULL;
    char *yang_dir;

    if ((err_info = sr_path_yang_dir(&yang_dir))) {
        return err_info;
    }
    *ly_ctx = ly_ctx_new(yang_dir, 0);
    free(yang_dir);

    if (!*ly_ctx) {
        sr_errinfo_new(&err_info, SR_ERR_INTERNAL, NULL, "Failed to create a new libyang context.");
        return err_info;
    }

    return NULL;
}

sr_error_info_t *
sr_store_module_file(const struct lys_module *ly_mod)
{
    sr_error_info_t *err_info = NULL;
    char *path;

    if ((err_info = sr_path_yang_file(ly_mod->name, ly_mod->rev_size ? ly_mod->rev[0].date : NULL, &path))) {
        return err_info;
    }

    if (!access(path, R_OK)) {
        /* already exists */
        free(path);
        return NULL;
    }

    if (lys_print_path(path, ly_mod, LYS_YANG, NULL, 0, 0)) {
        free(path);
        sr_errinfo_new_ly(&err_info, ly_mod->ctx);
        return err_info;
    }

    /* set permissions */
    if (chmod(path, SR_YANG_PERM)) {
        SR_ERRINFO_SYSERRNO(&err_info, "chmod");
        return err_info;
    }

    SR_LOG_INF("Module file \"%s%s%s\" installed.", ly_mod->name, ly_mod->rev_size ? "@" : "",
            ly_mod->rev_size ? ly_mod->rev[0].date : "");
    free(path);
    return NULL;
}

/**
 * @brief Create startup and running data file for a module.
 *
 * @param[in] ly_mod Module to create daa files for.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_create_data_files(const struct lys_module *ly_mod)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *root = NULL;
    char *path = NULL;

    /* get startup file path */
    if ((err_info = sr_path_startup_file(ly_mod->name, &path))) {
        goto cleanup;
    }

    if (!access(path, F_OK)) {
        /* already exists */
        goto cleanup;
    }

    /* get default values */
    if (lyd_validate_modules(&root, &ly_mod, 1, LYD_OPT_CONFIG)) {
        sr_errinfo_new_ly(&err_info, ly_mod->ctx);
        SR_ERRINFO_VALID(&err_info);
        return err_info;
    }

    /* print them into a file */
    if (lyd_print_path(path, root, LYD_LYB, LYP_WITHSIBLINGS)) {
        sr_errinfo_new_ly(&err_info, ly_mod->ctx);
        sr_errinfo_new(&err_info, SR_ERR_INTERNAL, NULL, "Failed to write data into \"%s\".", path);
        goto cleanup;
    }

    /* set permissions */
    if (chmod(path, SR_FILE_PERM)) {
        SR_ERRINFO_SYSERRNO(&err_info, "chmod");
        goto cleanup;
    }

    /* repeat for running DS */
    free(path);
    path = NULL;
    if ((err_info = sr_path_running_file(ly_mod->name, &path))) {
        return err_info;
    }
    if (lyd_print_path(path, root, LYD_LYB, LYP_WITHSIBLINGS)) {
        sr_errinfo_new_ly(&err_info, ly_mod->ctx);
        sr_errinfo_new(&err_info, SR_ERR_INTERNAL, NULL, "Failed to write data into \"%s\".", path);
        goto cleanup;
    }

    /* set permissions */
    if (chmod(path, SR_FILE_PERM)) {
        SR_ERRINFO_SYSERRNO(&err_info, "chmod");
        goto cleanup;
    }

cleanup:
    free(path);
    lyd_free_withsiblings(root);
    return err_info;
}

/**
 * @brief Check whether a module is internal libyang module.
 *
 * @param[in] ly_mod Module to check.
 * @return 0 if not, non-zero if it is.
 */
static int
sr_ly_module_is_internal(const struct lys_module *ly_mod)
{
    if (!ly_mod->rev_size) {
        return 0;
    }

    if (!strcmp(ly_mod->name, "ietf-yang-metadata") && !strcmp(ly_mod->rev[0].date, "2016-08-05")) {
        return 1;
    } else if (!strcmp(ly_mod->name, "yang") && !strcmp(ly_mod->rev[0].date, "2017-02-20")) {
        return 1;
    } else if (!strcmp(ly_mod->name, "ietf-inet-types") && !strcmp(ly_mod->rev[0].date, "2013-07-15")) {
        return 1;
    } else if (!strcmp(ly_mod->name, "ietf-yang-types") && !strcmp(ly_mod->rev[0].date, "2013-07-15")) {
        return 1;
    } else if (!strcmp(ly_mod->name, "ietf-datastores") && !strcmp(ly_mod->rev[0].date, "2017-08-17")) {
        return 1;
    } else if (!strcmp(ly_mod->name, "ietf-yang-library") && !strcmp(ly_mod->rev[0].date, "2018-01-17")) {
        return 1;
    }

    return 0;
}

sr_error_info_t *
sr_create_module_files_with_imps_r(const struct lys_module *ly_mod)
{
    sr_error_info_t *err_info = NULL;
    uint16_t i;

    if (ly_mod->implemented && (err_info = sr_create_data_files(ly_mod))) {
        return err_info;
    }

    if (!sr_ly_module_is_internal(ly_mod) && (err_info = sr_store_module_file(ly_mod))) {
        return err_info;
    }

    for (i = 0; i < ly_mod->imp_size; ++i) {
        if ((err_info = sr_create_module_files_with_imps_r(ly_mod->imp[i].module))) {
            return err_info;
        }
    }

    return NULL;
}

sr_error_info_t *
sr_create_module_update_imps_r(const struct lys_module *ly_mod)
{
    sr_error_info_t *err_info = NULL;
    struct lys_module *ly_imp_mod;
    uint16_t i;

    for (i = 0; i < ly_mod->imp_size; ++i) {
        ly_imp_mod = ly_mod->imp[i].module;
        if (sr_ly_module_is_internal(ly_imp_mod)) {
            /* skip */
            continue;
        }

        if ((err_info = sr_store_module_file(ly_imp_mod))) {
            return err_info;
        }

        if ((err_info = sr_create_module_update_imps_r(ly_imp_mod))) {
            return err_info;
        }
    }

    return NULL;
}

sr_error_info_t *
sr_path_sub_shm(const char *mod_name, const char *suffix1, int64_t suffix2, int abs_path, char **path)
{
    sr_error_info_t *err_info = NULL;
    int ret;

    if (suffix2 > -1) {
        ret = asprintf(path, "%s/sr_%s.%s.%08x", abs_path ? SR_SHM_DIR : "", mod_name, suffix1, (uint32_t)suffix2);
    } else {
        ret = asprintf(path, "%s/sr_%s.%s", abs_path ? SR_SHM_DIR : "", mod_name, suffix1);
    }

    if (ret == -1) {
        SR_ERRINFO_MEM(&err_info);
    }
    return err_info;
}

sr_error_info_t *
sr_path_evpipe(uint32_t evpipe_num, char **path)
{
    sr_error_info_t *err_info = NULL;

    if (asprintf(path, "%s/sr_evpipe%" PRIu32, sr_get_repo_path(), evpipe_num) == -1) {
        SR_ERRINFO_MEM(&err_info);
    }

    return err_info;
}

sr_error_info_t *
sr_path_running_dir(char **path)
{
    sr_error_info_t *err_info = NULL;

    if (SR_RUNNING_PATH[0]) {
        *path = strdup(SR_RUNNING_PATH);
    } else {
        if (asprintf(path, "%s/data", sr_get_repo_path()) == -1) {
            *path = NULL;
        }
    }

    if (!*path) {
        SR_ERRINFO_MEM(&err_info);
    }
    return err_info;
}

sr_error_info_t *
sr_path_startup_dir(char **path)
{
    sr_error_info_t *err_info = NULL;

    if (SR_STARTUP_PATH[0]) {
        *path = strdup(SR_STARTUP_PATH);
    } else {
        if (asprintf(path, "%s/data", sr_get_repo_path()) == -1) {
            *path = NULL;
        }
    }

    if (!*path) {
        SR_ERRINFO_MEM(&err_info);
    }
    return err_info;
}

sr_error_info_t *
sr_path_notif_dir(char **path)
{
    sr_error_info_t *err_info = NULL;

    if (SR_NOTIFICATION_PATH[0]) {
        *path = strdup(SR_NOTIFICATION_PATH);
    } else {
        if (asprintf(path, "%s/data/notif", sr_get_repo_path()) == -1) {
            *path = NULL;
        }
    }

    if (!*path) {
        SR_ERRINFO_MEM(&err_info);
    }
    return err_info;
}

sr_error_info_t *
sr_path_yang_dir(char **path)
{
    sr_error_info_t *err_info = NULL;

    if (SR_YANG_PATH[0]) {
        *path = strdup(SR_YANG_PATH);
    } else {
        if (asprintf(path, "%s/yang", sr_get_repo_path()) == -1) {
            *path = NULL;
        }
    }

    if (!*path) {
        SR_ERRINFO_MEM(&err_info);
    }
    return err_info;
}

sr_error_info_t *
sr_path_running_file(const char *mod_name, char **path)
{
    sr_error_info_t *err_info = NULL;
    int ret;

    if (SR_RUNNING_PATH[0]) {
        ret = asprintf(path, "%s/%s.running", SR_RUNNING_PATH, mod_name);
    } else {
        ret = asprintf(path, "%s/data/%s.running", sr_get_repo_path(), mod_name);
    }

    if (ret == -1) {
        *path = NULL;
        SR_ERRINFO_MEM(&err_info);
    }
    return err_info;
}

sr_error_info_t *
sr_path_startup_file(const char *mod_name, char **path)
{
    sr_error_info_t *err_info = NULL;
    int ret;

    if (SR_STARTUP_PATH[0]) {
        ret = asprintf(path, "%s/%s.startup", SR_STARTUP_PATH, mod_name);
    } else {
        ret = asprintf(path, "%s/data/%s.startup", sr_get_repo_path(), mod_name);
    }

    if (ret == -1) {
        *path = NULL;
        SR_ERRINFO_MEM(&err_info);
    }
    return err_info;
}

sr_error_info_t *
sr_path_notif_file(const char *mod_name, time_t from_ts, time_t to_ts, char **path)
{
    sr_error_info_t *err_info = NULL;
    int ret;

    if (SR_NOTIFICATION_PATH[0]) {
        ret = asprintf(path, "%s/%s.notif.%lu-%lu", SR_NOTIFICATION_PATH, mod_name, from_ts, to_ts);
    } else {
        ret = asprintf(path, "%s/data/notif/%s.notif.%lu-%lu", sr_get_repo_path(), mod_name, from_ts, to_ts);
    }

    if (ret == -1) {
        *path = NULL;
        SR_ERRINFO_MEM(&err_info);
    }
    return err_info;
}

sr_error_info_t *
sr_path_yang_file(const char *mod_name, const char *mod_rev, char **path)
{
    sr_error_info_t *err_info = NULL;
    int ret;

    if (SR_YANG_PATH[0]) {
        ret = asprintf(path, "%s/%s%s%s.yang", SR_YANG_PATH, mod_name, mod_rev ? "@" : "", mod_rev ? mod_rev : "");
    } else {
        ret = asprintf(path, "%s/yang/%s%s%s.yang", sr_get_repo_path(), mod_name, mod_rev ? "@" : "", mod_rev ? mod_rev : "");
    }

    if (ret == -1) {
        *path = NULL;
        SR_ERRINFO_MEM(&err_info);
    }
    return err_info;
}

sr_error_info_t *
sr_get_pwd(uid_t *uid, char **user)
{
    sr_error_info_t *err_info = NULL;
    struct passwd pwd, *pwd_p;
    char *buf = NULL;
    ssize_t buflen = 0;
    int ret;

    assert(uid && user);

    do {
        if (!buflen) {
            /* learn suitable buffer size */
            buflen = sysconf(_SC_GETPW_R_SIZE_MAX);
            if (buflen == -1) {
                buflen = 2048;
            }
        } else {
            /* enlarge buffer */
            buflen += 2048;
        }

        /* allocate some buffer */
        buf = sr_realloc(buf, buflen);
        SR_CHECK_MEM_RET(!buf, err_info);

        if (*user) {
            /* user -> UID */
            ret = getpwnam_r(*user, &pwd, buf, buflen, &pwd_p);
        } else {
            /* UID -> user */
            ret = getpwuid_r(*uid, &pwd, buf, buflen, &pwd_p);
        }
    } while (ret && (ret == ERANGE));
    if (ret) {
        if (*user) {
            sr_errinfo_new(&err_info, SR_ERR_INTERNAL, NULL, "Retrieving user \"%s\" passwd entry failed (%s).",
                    *user, strerror(ret));
        } else {
            sr_errinfo_new(&err_info, SR_ERR_INTERNAL, NULL, "Retrieving UID \"%lu\" passwd entry failed (%s).",
                    (unsigned long int)*uid, strerror(ret));
        }
        goto cleanup;
    } else if (!pwd_p) {
        if (*user) {
            sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, NULL, "Retrieving user \"%s\" passwd entry failed (No such user).",
                    *user);
        } else {
            sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, NULL, "Retrieving UID \"%lu\" passwd entry failed (No such UID).",
                    (unsigned long int)*uid);
        }
        goto cleanup;
    }

    if (*user) {
        /* assign UID */
        *uid = pwd.pw_uid;
    } else {
        /* assign user */
        *user = strdup(pwd.pw_name);
        SR_CHECK_MEM_GOTO(!*user, err_info, cleanup);
    }

    /* success */

cleanup:
    free(buf);
    return err_info;
}

/**
 * @brief Get GID from group name or vice versa.
 *
 * @param[in,out] gid GID.
 * @param[in,out] group Group name.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_get_grp(gid_t *gid, char **group)
{
    sr_error_info_t *err_info = NULL;
    struct group grp, *grp_p;
    char *buf = NULL;
    ssize_t buflen = 0;
    int ret;

    assert(gid && group);

    do {
        if (!buflen) {
            /* learn suitable buffer size */
            buflen = sysconf(_SC_GETGR_R_SIZE_MAX);
            if (buflen == -1) {
                buflen = 2048;
            }
        } else {
            /* enlarge buffer */
            buflen += 2048;
        }

        /* allocate some buffer */
        buf = sr_realloc(buf, buflen);
        SR_CHECK_MEM_RET(!buf, err_info);

        if (*group) {
            /* group -> GID */
            ret = getgrnam_r(*group, &grp, buf, buflen, &grp_p);
        } else {
            /* GID -> group */
            ret = getgrgid_r(*gid, &grp, buf, buflen, &grp_p);
        }
    } while (ret && (ret == ERANGE));
    if (ret) {
        if (*group) {
            sr_errinfo_new(&err_info, SR_ERR_INTERNAL, NULL, "Retrieving group \"%s\" grp entry failed (%s).",
                    *group, strerror(ret));
        } else {
            sr_errinfo_new(&err_info, SR_ERR_INTERNAL, NULL, "Retrieving GID \"%lu\" grp entry failed (%s).",
                    (unsigned long int)*gid, strerror(ret));
        }
        goto cleanup;
    } else if (!grp_p) {
        if (*group) {
            sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, NULL, "Retrieving group \"%s\" grp entry failed (No such group).",
                    *group);
        } else {
            sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, NULL, "Retrieving GID \"%lu\" grp entry failed (No such GID).",
                    (unsigned long int)*gid);
        }
        goto cleanup;
    }

    if (*group) {
        /* assign GID */
        *gid = grp.gr_gid;
    } else {
        /* assign group */
        *group = strdup(grp.gr_name);
        SR_CHECK_MEM_GOTO(!*group, err_info, cleanup);
    }

    /* success */

cleanup:
    free(buf);
    return err_info;
}

sr_error_info_t *
sr_chmodown(const char *path, const char *owner, const char *group, mode_t perm)
{
    sr_error_info_t *err_info = NULL;
    sr_error_t err_code;
    uid_t uid = -1;
    gid_t gid = -1;

    assert(path);

    if (perm > 00666) {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, NULL, "Only read and write permissions can be set.");
        return err_info;
    } else if (perm & 00111) {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, NULL, "Setting execute permissions has no effect.");
        return err_info;
    }

    /* we are going to change the owner */
    if (owner && (err_info = sr_get_pwd(&uid, (char **)&owner))) {
        return err_info;
    }

    /* we are going to change the group */
    if (group && (err_info = sr_get_grp(&gid, (char **)&group))) {
        return err_info;
    }

    /* apply owner changes, if any */
    if (chown(path, uid, gid) == -1) {
        if ((errno == EACCES) || (errno = EPERM)) {
            err_code = SR_ERR_UNAUTHORIZED;
        } else {
            err_code = SR_ERR_INTERNAL;
        }
        sr_errinfo_new(&err_info, err_code, NULL, "Changing owner of \"%s\" failed (%s).", path, strerror(errno));
        return err_info;
    }

    /* apply permission changes, if any */
    if (((int)perm != -1) && (chmod(path, perm) == -1)) {
        if ((errno == EACCES) || (errno = EPERM)) {
            err_code = SR_ERR_UNAUTHORIZED;
        } else {
            err_code = SR_ERR_INTERNAL;
        }
        sr_errinfo_new(&err_info, err_code, NULL, "Changing permissions (mode) of \"%s\" failed (%s).", path, strerror(errno));
        return err_info;
    }

    return NULL;
}

sr_error_info_t *
sr_perm_check(const char *mod_name, int wr)
{
    sr_error_info_t *err_info = NULL;
    char *path;

    /* use startup file, it does not matter */
    if ((err_info = sr_path_startup_file(mod_name, &path))) {
        return err_info;
    }

    /* check against effective permissions */
    if (eaccess(path, (wr ? W_OK : R_OK)) == -1) {
        if (errno == EACCES) {
            sr_errinfo_new(&err_info, SR_ERR_UNAUTHORIZED, NULL, "%s permission \"%s\" check failed.",
                    wr ? "Write" : "Read", mod_name);
        } else {
            SR_ERRINFO_SYSERRNO(&err_info, "eaccess");
        }
    }

    free(path);
    return err_info;
}

sr_error_info_t *
sr_perm_get(const char *mod_name, char **owner, char **group, mode_t *perm)
{
    sr_error_info_t *err_info = NULL;
    struct stat st;
    char *path;
    int ret;

    if (owner) {
        *owner = NULL;
    }
    if (group) {
        *group = NULL;
    }

    /* use startup file, it does not matter */
    if ((err_info = sr_path_startup_file(mod_name, &path))) {
        return err_info;
    }

    /* stat */
    ret = stat(path, &st);
    free(path);
    if (ret == -1) {
        if (errno == EACCES) {
            sr_errinfo_new(&err_info, SR_ERR_UNAUTHORIZED, NULL, "Learning \"%s\" permissions failed.", mod_name);
        } else {
            SR_ERRINFO_SYSERRNO(&err_info, "stat");
        }
        return err_info;
    }

    /* get owner */
    if (owner && (err_info = sr_get_pwd(&st.st_uid, owner))) {
        goto error;
    }

    /* get group */
    if (group && (err_info = sr_get_grp(&st.st_gid, group))) {
        goto error;
    }

    /* get perms */
    if (perm) {
        *perm = st.st_mode & 0007777;
    }

    return NULL;

error:
    if (owner) {
        free(*owner);
    }
    if (group) {
        free(*group);
    }
    return err_info;
}

void
sr_time_get(struct timespec *ts, uint32_t add_ms)
{
    sr_error_info_t *err_info = NULL;

    if (clock_gettime(CLOCK_REALTIME, ts) == -1) {
        SR_ERRINFO_SYSERRNO(&err_info, "clock_gettime");
        /* will not happen anyway */
        sr_errinfo_free(&err_info);
        return;
    }

    add_ms += ts->tv_nsec / 1000000;
    ts->tv_nsec %= 1000000;
    ts->tv_nsec += (add_ms % 1000) * 1000000;
    ts->tv_sec += add_ms / 1000;
}

sr_error_info_t *
sr_shm_remap(sr_shm_t *shm, size_t new_shm_size)
{
    sr_error_info_t *err_info = NULL;
    size_t shm_file_size;

    /* read the new shm size if not set */
    if (!new_shm_size && (err_info = sr_file_get_size(shm->fd, &shm_file_size))) {
        return err_info;
    }

    if ((!new_shm_size && (shm_file_size == shm->size)) || (new_shm_size && (new_shm_size == shm->size))) {
        /* mapping is fine, the size has not changed */
        return NULL;
    }

    if (shm->addr) {
        munmap(shm->addr, shm->size);
    }

    /* truncate if needed */
    if (new_shm_size && (ftruncate(shm->fd, new_shm_size) == -1)) {
        shm->addr = NULL;
        sr_errinfo_new(&err_info, SR_ERR_SYS, NULL, "Failed to truncate shared memory (%s).", strerror(errno));
        return err_info;
    }

    shm->size = new_shm_size ? new_shm_size : shm_file_size;

    /* map */
    shm->addr = mmap(NULL, shm->size, PROT_READ | PROT_WRITE, MAP_SHARED, shm->fd, 0);
    if (shm->addr == MAP_FAILED) {
        shm->addr = NULL;
        sr_errinfo_new(&err_info, SR_ERR_NOMEM, NULL, "Failed to map shared memory (%s).", strerror(errno));
        return err_info;
    }

    return NULL;
}

void
sr_shm_clear(sr_shm_t *shm)
{
    if (shm->addr) {
        munmap(shm->addr, shm->size);
        shm->addr = NULL;
    }
    if (shm->fd > -1) {
        close(shm->fd);
        shm->fd = -1;
    }
    shm->size = 0;
}

off_t
sr_shmcpy(char *shm_addr, const void *src, size_t size, char **shm_end)
{
    off_t ret;

    if (!size) {
        return 0;
    }

    if (src) {
        memcpy(*shm_end, src, size);
    }
    ret = *shm_end - shm_addr;
    *shm_end += size;

    return ret;
}

sr_error_info_t *
sr_mutex_init(pthread_mutex_t *lock, int shared)
{
    sr_error_info_t *err_info = NULL;
    pthread_mutexattr_t attr;
    int ret;

    /* check address alignment */
    if ((uintptr_t)lock % sizeof lock->__align) {
        sr_errinfo_new(&err_info, SR_ERR_INTERNAL, NULL, "Mutex address not aligned.");
        return err_info;
    }

    if (shared) {
        /* init attr */
        if ((ret = pthread_mutexattr_init(&attr))) {
            sr_errinfo_new(&err_info, SR_ERR_INIT_FAILED, NULL, "Initializing pthread attr failed (%s).", strerror(ret));
            return err_info;
        }
        if ((ret = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED))) {
            pthread_mutexattr_destroy(&attr);
            sr_errinfo_new(&err_info, SR_ERR_INIT_FAILED, NULL, "Changing pthread attr failed (%s).", strerror(ret));
            return err_info;
        }

        if ((ret = pthread_mutex_init(lock, &attr))) {
            pthread_mutexattr_destroy(&attr);
            sr_errinfo_new(&err_info, SR_ERR_INIT_FAILED, NULL, "Initializing pthread mutex failed (%s).", strerror(ret));
            return err_info;
        }
        pthread_mutexattr_destroy(&attr);
    } else {
        if ((ret = pthread_mutex_init(lock, NULL))) {
            sr_errinfo_new(&err_info, SR_ERR_INIT_FAILED, NULL, "Initializing pthread mutex failed (%s).", strerror(ret));
            return err_info;
        }
    }

    return NULL;
}

sr_error_info_t *
sr_mlock(pthread_mutex_t *lock, int timeout_ms, const char *func)
{
    sr_error_info_t *err_info = NULL;
    struct timespec abs_ts;
    int ret;

    assert(timeout_ms);

    if (timeout_ms == -1) {
        ret = pthread_mutex_lock(lock);
    } else {
        sr_time_get(&abs_ts, (uint32_t)timeout_ms);
        ret = pthread_mutex_timedlock(lock, &abs_ts);
    }
    if (ret) {
        SR_ERRINFO_LOCK(&err_info, func, ret);
        return err_info;
    }

    return NULL;
}

void
sr_munlock(pthread_mutex_t *lock)
{
    int ret;

    ret = pthread_mutex_unlock(lock);
    if (ret) {
        SR_LOG_WRN("Unlocking a mutex failed (%s).", strerror(ret));
    }
}

/**
 * @brief Wrapper for pthread_cond_init().
 *
 * @param[out] cond Condition variable to initialize.
 * @param[in] shared Whether the condition will be shared among processes.
 * @return err_info, NULL on error.
 */
static sr_error_info_t *
sr_cond_init(pthread_cond_t *cond, int shared)
{
    sr_error_info_t *err_info = NULL;
    pthread_condattr_t attr;
    int ret;

    /* check address alignment */
    if ((uintptr_t)cond % sizeof cond->__align) {
        sr_errinfo_new(&err_info, SR_ERR_INTERNAL, NULL, "Condition variable address not aligned.");
        return err_info;
    }

    if (shared) {
        /* init attr */
        if ((ret = pthread_condattr_init(&attr))) {
            sr_errinfo_new(&err_info, SR_ERR_INIT_FAILED, NULL, "Initializing pthread attr failed (%s).", strerror(ret));
            return err_info;
        }
        if ((ret = pthread_condattr_setpshared(&attr, PTHREAD_PROCESS_SHARED))) {
            pthread_condattr_destroy(&attr);
            sr_errinfo_new(&err_info, SR_ERR_INIT_FAILED, NULL, "Changing pthread attr failed (%s).", strerror(ret));
            return err_info;
        }

        if ((ret = pthread_cond_init(cond, &attr))) {
            pthread_condattr_destroy(&attr);
            sr_errinfo_new(&err_info, SR_ERR_INIT_FAILED, NULL, "Initializing pthread rwlock failed (%s).", strerror(ret));
            return err_info;
        }
        pthread_condattr_destroy(&attr);
    } else {
        if ((ret = pthread_cond_init(cond, NULL))) {
            sr_errinfo_new(&err_info, SR_ERR_INIT_FAILED, NULL, "Initializing pthread rwlock failed (%s).", strerror(ret));
            return err_info;
        }
    }

    return NULL;
}

sr_error_info_t *
sr_rwlock_init(sr_rwlock_t *rwlock, int shared)
{
    sr_error_info_t *err_info = NULL;

    if ((err_info = sr_mutex_init(&rwlock->mutex, shared))) {
        return err_info;
    }
    rwlock->readers = 0;
    if ((err_info = sr_cond_init(&rwlock->cond, shared))) {
        pthread_mutex_destroy(&rwlock->mutex);
        return err_info;
    }

    return NULL;
}

void
sr_rwlock_destroy(sr_rwlock_t *rwlock)
{
    pthread_mutex_destroy(&rwlock->mutex);
    pthread_cond_destroy(&rwlock->cond);
}

sr_error_info_t *
sr_rwlock(sr_rwlock_t *rwlock, int timeout_ms, int wr, const char *func)
{
    sr_error_info_t *err_info = NULL;
    struct timespec timeout_ts;
    int ret;

    assert(timeout_ms > 0);

    sr_time_get(&timeout_ts, timeout_ms);

    /* MUTEX LOCK */
    ret = pthread_mutex_timedlock(&rwlock->mutex, &timeout_ts);
    if (ret) {
        SR_ERRINFO_LOCK(&err_info, func, ret);
        return err_info;
    }

    if (wr) {
        /* write lock */
        ret = 0;
        while (!ret && rwlock->readers) {
            /* COND WAIT */
            ret = pthread_cond_timedwait(&rwlock->cond, &rwlock->mutex, &timeout_ts);
        }

        if (ret) {
            /* MUTEX UNLOCK */
            pthread_mutex_unlock(&rwlock->mutex);

            SR_ERRINFO_COND(&err_info, func, ret);
            return err_info;
        }
    } else {
        /* read lock */
        ++rwlock->readers;

        /* MUTEX UNLOCK */
        pthread_mutex_unlock(&rwlock->mutex);
    }

    return NULL;
}

void
sr_rwunlock(sr_rwlock_t *rwlock, int wr, const char *func)
{
    sr_error_info_t *err_info = NULL;
    struct timespec timeout_ts;
    int ret;

    if (!wr) {
        sr_time_get(&timeout_ts, SR_RWLOCK_READ_TIMEOUT);

        /* MUTEX LOCK */
        ret = pthread_mutex_timedlock(&rwlock->mutex, &timeout_ts);
        if (ret) {
            SR_ERRINFO_LOCK(&err_info, func, ret);
            sr_errinfo_free(&err_info);
        }

        if (!rwlock->readers) {
            SR_ERRINFO_INT(&err_info);
            sr_errinfo_free(&err_info);
        } else {
            /* remove a reader */
            --rwlock->readers;
        }
    }

    /* we are unlocking a write lock, there can be no readers */
    assert(!wr || !rwlock->readers);

    if (!rwlock->readers) {
        /* broadcast on condition */
        pthread_cond_broadcast(&rwlock->cond);
    }

    /* MUTEX UNLOCK */
    pthread_mutex_unlock(&rwlock->mutex);
}

void *
sr_realloc(void *ptr, size_t size)
{
    void *new_mem;

    new_mem = realloc(ptr, size);
    if (!new_mem) {
        free(ptr);
    }

    return new_mem;
}

sr_error_info_t *
sr_cp(const char *to, const char *from)
{
    sr_error_info_t *err_info = NULL;
    int fd_to = -1, fd_from = -1;
    char * out_ptr, buf[4096];
    ssize_t nread, nwritten;

    fd_from = open(from, O_RDONLY);
    if (fd_from < 0) {
        sr_errinfo_new(&err_info, SR_ERR_SYS, NULL, "Opening \"%s\" failed (%s).", from, strerror(errno));
        goto cleanup;
    }

    fd_to = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd_to < 0) {
        sr_errinfo_new(&err_info, SR_ERR_SYS, NULL, "Creating \"%s\" failed (%s).", to, strerror(errno));
        goto cleanup;
    }

    while ((nread = read(fd_from, buf, sizeof buf)) > 0) {
        out_ptr = buf;
        do {
            nwritten = write(fd_to, out_ptr, nread);
            if (nwritten >= 0) {
                nread -= nwritten;
                out_ptr += nwritten;
            } else if (errno != EINTR) {
                SR_ERRINFO_SYSERRNO(&err_info, "write");
                goto cleanup;
            }
        } while (nread > 0);
    }
    if (nread == -1) {
        SR_ERRINFO_SYSERRNO(&err_info, "read");
        goto cleanup;
    }

    /* success */

cleanup:
    if (fd_from > -1) {
        close(fd_from);
    }
    if (fd_to > -1) {
        close(fd_to);
    }
    return err_info;
}

sr_error_info_t *
sr_mkpath(char *path, mode_t mode)
{
    char *p;
    sr_error_info_t *err_info = NULL;

    assert(path[0] == '/');

    for (p = strchr(path + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = '\0';
        if (mkdir(path, mode) == -1) {
            if (errno != EEXIST) {
                *p = '/';
                SR_ERRINFO_SYSERRNO(&err_info, "mkdir");
                return err_info;
            }
        }
        *p = '/';
    }

    if (mkdir(path, mode) == -1) {
        if (errno != EEXIST) {
            SR_ERRINFO_SYSERRNO(&err_info, "mkdir");
            return err_info;
        }
    }

    return NULL;
}

char *
sr_get_first_ns(const char *expr)
{
    int i;

    if (expr[0] != '/') {
        return NULL;
    }
    if (expr[1] == '/') {
        expr += 2;
    } else {
        ++expr;
    }

    if (!isalpha(expr[0]) && (expr[0] != '_')) {
        return NULL;
    }
    for (i = 1; expr[i] && (isalnum(expr[i]) || (expr[i] == '_') || (expr[i] == '-') || (expr[i] == '.')); ++i);
    if (expr[i] != ':') {
        return NULL;
    }

    return strndup(expr, i);
}

const char *
sr_ds2str(sr_datastore_t ds)
{
    switch (ds) {
    case SR_DS_RUNNING:
        return "running";
    case SR_DS_STARTUP:
        return "startup";
    case SR_DS_OPERATIONAL:
        return "operational";
    }

    return NULL;
}

sr_error_info_t *
sr_msleep(uint32_t msec)
{
    sr_error_info_t *err_info = NULL;
    struct timespec ts;
    int ret;

    memset(&ts, 0, sizeof ts);
    ts.tv_sec = msec / 1000;
    ts.tv_nsec = (msec % 1000) * 1000000;

    do {
        ret = nanosleep(&ts, &ts);
    } while ((ret == -1) && (errno = EINTR));

    if (ret == -1) {
        SR_ERRINFO_SYSERRNO(&err_info, "nanosleep");
        return err_info;
    }

    return NULL;
}

sr_error_info_t *
sr_file_get_size(int fd, size_t *size)
{
    sr_error_info_t *err_info = NULL;
    struct stat st;

    if (fstat(fd, &st) == -1) {
        SR_ERRINFO_SYSERRNO(&err_info, "fstat");
        return err_info;
    }

    *size = st.st_size;
    return NULL;
}

const char *
sr_ly_leaf_value_str(const struct lyd_node *leaf)
{
    assert(leaf->schema->nodetype & (LYS_LEAF | LYS_LEAFLIST));
    return ((struct lyd_node_leaf_list *)leaf)->value_str;
}

const char *
sr_ev2str(sr_sub_event_t ev)
{
    sr_error_info_t *err_info = NULL;

    switch (ev) {
    case SR_SUB_EV_UPDATE:
        return "update";
    case SR_SUB_EV_CHANGE:
        return "change";
    case SR_SUB_EV_DONE:
        return "done";
    case SR_SUB_EV_ABORT:
        return "abort";
    case SR_SUB_EV_DP:
        return "data-provide";
    case SR_SUB_EV_RPC:
        return "rpc";
    case SR_SUB_EV_NOTIF:
        return "notif";
    default:
        SR_ERRINFO_INT(&err_info);
        sr_errinfo_free(&err_info);
        break;
    }

    return NULL;
}

sr_notif_event_t
sr_ev2api(sr_sub_event_t ev)
{
    sr_error_info_t *err_info = NULL;

    switch (ev) {
    case SR_SUB_EV_UPDATE:
        return SR_EV_UPDATE;
    case SR_SUB_EV_CHANGE:
        return SR_EV_CHANGE;
    case SR_SUB_EV_DONE:
        return SR_EV_DONE;
    case SR_SUB_EV_ABORT:
        return SR_EV_ABORT;
    default:
        SR_ERRINFO_INT(&err_info);
        sr_errinfo_free(&err_info);
        break;
    }

    return 0;
}

sr_error_info_t *
sr_val_ly2sr(const struct lyd_node *node, sr_val_t *sr_val)
{
    sr_error_info_t *err_info = NULL;
    char *ptr;
    const struct lyd_node_leaf_list *leaf;

    sr_val->xpath = lyd_path(node);
    SR_CHECK_MEM_GOTO(!sr_val->xpath, err_info, error);

    sr_val->dflt = node->dflt;

    switch (node->schema->nodetype) {
    case LYS_LEAF:
    case LYS_LEAFLIST:
        leaf = (const struct lyd_node_leaf_list *)node;
        switch (leaf->value_type) {
        case LY_TYPE_BINARY:
            sr_val->type = SR_BINARY_T;
            sr_val->data.binary_val = strdup(leaf->value_str);
            SR_CHECK_MEM_GOTO(!sr_val->data.binary_val, err_info, error);
            break;
        case LY_TYPE_BITS:
            sr_val->type = SR_BITS_T;
            sr_val->data.bits_val = strdup(leaf->value_str);
            SR_CHECK_MEM_GOTO(!sr_val->data.bits_val, err_info, error);
            break;
        case LY_TYPE_BOOL:
            sr_val->type = SR_BOOL_T;
            sr_val->data.bool_val = leaf->value.bln ? true : false;
            break;
        case LY_TYPE_DEC64:
            sr_val->type = SR_DECIMAL64_T;
            sr_val->data.decimal64_val = strtod(leaf->value_str, &ptr);
            if (ptr[0]) {
                sr_errinfo_new(&err_info, SR_ERR_VALIDATION_FAILED, NULL, "Value \"%s\" is not a valid decimal64 number.",
                        leaf->value_str);
                goto error;
            }
            break;
        case LY_TYPE_EMPTY:
            sr_val->type = SR_LEAF_EMPTY_T;
            sr_val->data.string_val = NULL;
            break;
        case LY_TYPE_ENUM:
            sr_val->type = SR_ENUM_T;
            sr_val->data.enum_val = strdup(leaf->value_str);
            SR_CHECK_MEM_GOTO(!sr_val->data.enum_val, err_info, error);
            break;
        case LY_TYPE_IDENT:
            sr_val->type = SR_IDENTITYREF_T;
            sr_val->data.identityref_val = strdup(leaf->value_str);
            SR_CHECK_MEM_GOTO(!sr_val->data.identityref_val, err_info, error);
            break;
        case LY_TYPE_INST:
            sr_val->type = SR_INSTANCEID_T;
            sr_val->data.instanceid_val = strdup(leaf->value_str);
            SR_CHECK_MEM_GOTO(!sr_val->data.instanceid_val, err_info, error);
            break;
        case LY_TYPE_INT8:
            sr_val->type = SR_INT8_T;
            sr_val->data.int8_val = leaf->value.int8;
            break;
        case LY_TYPE_INT16:
            sr_val->type = SR_INT16_T;
            sr_val->data.int16_val = leaf->value.int16;
            break;
        case LY_TYPE_INT32:
            sr_val->type = SR_INT32_T;
            sr_val->data.int32_val = leaf->value.int32;
            break;
        case LY_TYPE_INT64:
            sr_val->type = SR_INT64_T;
            sr_val->data.int64_val = leaf->value.int64;
            break;
        case LY_TYPE_STRING:
            sr_val->type = SR_STRING_T;
            sr_val->data.string_val = strdup(leaf->value_str);
            SR_CHECK_MEM_GOTO(!sr_val->data.string_val, err_info, error);
            break;
        case LY_TYPE_UINT8:
            sr_val->type = SR_UINT8_T;
            sr_val->data.uint8_val = leaf->value.uint8;
            break;
        case LY_TYPE_UINT16:
            sr_val->type = SR_UINT16_T;
            sr_val->data.uint16_val = leaf->value.uint16;
            break;
        case LY_TYPE_UINT32:
            sr_val->type = SR_UINT32_T;
            sr_val->data.uint32_val = leaf->value.uint32;
            break;
        case LY_TYPE_UINT64:
            sr_val->type = SR_UINT64_T;
            sr_val->data.uint64_val = leaf->value.uint64;
            break;
        default:
            SR_ERRINFO_INT(&err_info);
            return err_info;
        }
        break;
    case LYS_CONTAINER:
        if (((struct lys_node_container *)node->schema)->presence) {
            sr_val->type = SR_CONTAINER_PRESENCE_T;
        } else {
            sr_val->type = SR_CONTAINER_T;
        }
        break;
    case LYS_LIST:
        sr_val->type = SR_LIST_T;
        break;
    case LYS_NOTIF:
        sr_val->type = SR_NOTIFICATION_T;
        break;
    case LYS_ANYXML:
        sr_val->type = SR_ANYXML_T;
        /* TODO sr_val->data.anyxml_val = */
        break;
    case LYS_ANYDATA:
        sr_val->type = SR_ANYDATA_T;
        /* TODO sr_val->data.anydata_val = */
        break;
    default:
        SR_ERRINFO_INT(&err_info);
        return err_info;
    }

    return NULL;

error:
    free(sr_val->xpath);
    return err_info;
}

char *
sr_val_sr2ly_str(struct ly_ctx *ctx, const sr_val_t *sr_val, char *buf)
{
    struct lys_node_leaf *sleaf;

    if (!sr_val) {
        return NULL;
    }

    switch (sr_val->type) {
    case SR_STRING_T:
    case SR_BINARY_T:
    case SR_BITS_T:
    case SR_ENUM_T:
    case SR_IDENTITYREF_T:
    case SR_INSTANCEID_T:
    case SR_ANYDATA_T:
    case SR_ANYXML_T:
        return (sr_val->data.string_val);
    case SR_LEAF_EMPTY_T:
        return NULL;
    case SR_BOOL_T:
        return sr_val->data.bool_val ? "true" : "false";
    case SR_DECIMAL64_T:
        /* get fraction-digits */
        sleaf = (struct lys_node_leaf *)ly_ctx_get_node(ctx, NULL, sr_val->xpath, 0);
        if (!sleaf) {
            return NULL;
        }
        while (sleaf->type.base == LY_TYPE_LEAFREF) {
            sleaf = sleaf->type.info.lref.target;
        }
        sprintf(buf, "%.*f", sleaf->type.info.dec64.dig, sr_val->data.decimal64_val);
        return buf;
    case SR_UINT8_T:
    case SR_UINT16_T:
    case SR_UINT32_T:
        sprintf(buf, "%u", sr_val->data.uint32_val);
        return buf;
    case SR_UINT64_T:
        sprintf(buf, "%"PRIu64, sr_val->data.uint64_val);
        return buf;
    case SR_INT8_T:
    case SR_INT16_T:
    case SR_INT32_T:
        sprintf(buf, "%d", sr_val->data.int32_val);
        return buf;
    case SR_INT64_T:
        sprintf(buf, "%"PRId64, sr_val->data.int64_val);
        return buf;
    default:
        return NULL;
    }
}

sr_error_info_t *
sr_val_sr2ly(struct ly_ctx *ctx, const char *xpath, const char *val_str, int dflt, int output, struct lyd_node **root)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *node;
    int opts;

    opts = LYD_PATH_OPT_UPDATE | (dflt ? LYD_PATH_OPT_DFLT : 0) | (output ? LYD_PATH_OPT_OUTPUT : 0);

    ly_errno = 0;
    node = lyd_new_path(*root, ctx, xpath, (void *)val_str, 0, opts);
    if (!node && ly_errno) {
        sr_errinfo_new_ly(&err_info, ctx);
        return err_info;
    }

    if (!*root) {
        *root = node;
    }
    return NULL;
}

void
sr_ly_split(struct lyd_node *sibling)
{
    struct lyd_node *first, *last;

    if (!sibling || !sibling->prev->next) {
        return;
    }

    /* only works with top-level nodes */
    assert(!sibling->parent);

    /* find first and last node */
    for (first = sibling->prev; first->prev->next; first = first->prev);
    last = first->prev;

    /* correct left sibling list */
    first->prev = sibling->prev;
    sibling->prev->next = NULL;

    /* correct right sibling list */
    sibling->prev = last;
}

void
sr_ly_link(struct lyd_node *first, struct lyd_node *sibling)
{
    struct lyd_node *last;

    if (!first || !sibling) {
        return;
    }

    assert(!first->prev->next && !sibling->prev->next && (first != sibling));

    /* remember the last node */
    last = sibling->prev;

    /* link sibling lists together */
    sibling->prev = first->prev;
    first->prev->next = sibling;
    first->prev = last;
}

sr_error_info_t *
sr_ly_data_dup_xpath_select(const struct lyd_node *data, char **xpaths, uint16_t xp_count, struct lyd_node **new_data)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *root;
    struct ly_set *cur_set, *set = NULL;
    size_t i;

    *new_data = NULL;

    if (!xp_count) {
        /* no XPaths */
        return NULL;
    }

    /* get only the selected subtrees in a set */
    for (i = 0; i < xp_count; ++i) {
        cur_set = lyd_find_path(data, xpaths[i]);
        if (!cur_set) {
            sr_errinfo_new_ly(&err_info, lyd_node_module(data)->ctx);
            goto error;
        }

        /* merge into one set */
        if (set) {
            if (ly_set_merge(set, cur_set, 0)) {
                ly_set_free(cur_set);
                sr_errinfo_new_ly(&err_info, lyd_node_module(data)->ctx);
                goto error;
            }
        } else {
            set = cur_set;
        }
    }

    for (i = 0; i < set->number; ++i) {
        /* duplicate filtered subtree */
        root = lyd_dup(set->set.d[i], LYD_DUP_OPT_RECURSIVE | LYD_DUP_OPT_WITH_PARENTS);
        if (!root) {
            sr_errinfo_new_ly(&err_info, lyd_node_module(data)->ctx);
            goto error;
        }

        /* find top-level parent */
        while (root->parent) {
            root = root->parent;
        }

        /* merge into the final result */
        if (*new_data) {
            if (lyd_merge(*new_data, root, LYD_OPT_DESTRUCT | LYD_OPT_EXPLICIT)) {
                lyd_free_withsiblings(root);
                sr_errinfo_new_ly(&err_info, lyd_node_module(data)->ctx);
                goto error;
            }
        } else {
            *new_data = root;
        }
    }

    ly_set_free(set);
    return NULL;

error:
    ly_set_free(set);
    lyd_free_withsiblings(*new_data);
    *new_data = NULL;
    return err_info;
}

sr_error_info_t *
sr_ly_data_xpath_complement(struct lyd_node **data, const char *xpath)
{
    sr_error_info_t *err_info = NULL;
    struct ly_ctx *ctx;
    struct ly_set *node_set = NULL, *depth_set = NULL;
    struct lyd_node *parent;
    uint16_t depth, max_depth;
    size_t i;

    assert(data);

    if (!*data || !xpath) {
        return NULL;
    }

    ctx = lyd_node_module(*data)->ctx;

    node_set = lyd_find_path(*data, xpath);
    if (!node_set) {
        sr_errinfo_new_ly(&err_info, ctx);
        goto cleanup;
    }

    depth_set = ly_set_new();
    if (!depth_set) {
        sr_errinfo_new_ly(&err_info, ctx);
        goto cleanup;
    }

    /* store the depth of every node */
    max_depth = 1;
    for (i = 0; i < node_set->number; ++i) {
        for (parent = node_set->set.d[i], depth = 0; parent; parent = parent->parent, ++depth);

        if (ly_set_add(depth_set, (void *)((uintptr_t)depth), LY_SET_OPT_USEASLIST) == -1) {
            sr_errinfo_new_ly(&err_info, ctx);
            goto cleanup;
        }

        if (depth > max_depth) {
            max_depth = depth;
        }
    }

    assert(node_set->number == depth_set->number);

    /* free subtrees from the most nested to top-level */
    for (depth = max_depth; depth; --depth) {
        for (i = 0; i < node_set->number; ++i) {
            if (depth == (uintptr_t)depth_set->set.g[i]) {
                if (node_set->set.d[i] == *data) {
                    /* freeing the first top-level sibling */
                    *data = (*data)->next;
                }
                lyd_free(node_set->set.d[i]);
            }
        }
    }

    /* success */

cleanup:
    ly_set_free(node_set);
    ly_set_free(depth_set);
    return err_info;
}

int
sr_ly_is_userord(const struct lyd_node *node)
{
    assert(node);

    if ((node->schema->nodetype & (LYS_LIST | LYS_LEAFLIST)) && (node->schema->flags & LYS_USERORDERED)) {
        return 1;
    }

    return 0;
}

/*
 * Bob Jenkin's one-at-a-time hash
 * http://www.burtleburtle.net/bob/hash/doobs.html
 *
 * Spooky hash is faster, but it works only for little endian architectures.
 */
uint32_t
sr_str_hash(const char *str)
{
    uint32_t hash, i, len;

    len = strlen(str);
    for (hash = i = 0; i < len; ++i) {
        hash += str[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

sr_error_info_t *
sr_xpath_trim_last_node(const char *xpath, char **trim_xpath, char **last_node_xpath)
{
    sr_error_info_t *err_info = NULL;
    const char *ptr;
    char skip_end;
    int skipping;

    *trim_xpath = NULL;
    *last_node_xpath = NULL;

    assert(xpath[0] == '/');

    skipping = 0;
    for (ptr = xpath + strlen(xpath) - 1; skipping || (ptr[0] != '/'); --ptr) {
        if (skipping && (ptr[0] == skip_end)) {
            /* we found the character that started the subexpression */
            skipping = 0;
        } else if (ptr[0] == ']') {
            /* we are in a subexpression (predicate), these slashes are not the ones we are looking for */
            skip_end = '[';
            skipping = 1;
        }
    }

    if (ptr == xpath) {
        /* top-level node, whole xpath is trimmed */
        return NULL;
    }

    *trim_xpath = strndup(xpath, ptr - xpath);
    SR_CHECK_MEM_GOTO(!*trim_xpath, err_info, error);
    *last_node_xpath = strdup(ptr + 1);
    SR_CHECK_MEM_GOTO(!*last_node_xpath, err_info, error);
    return NULL;

error:
    free(*trim_xpath);
    free(*last_node_xpath);
    return err_info;
}

char *
sr_xpath_first_node(const char *xpath)
{
    const char *ptr;
    char quote = 0;

    assert(xpath && (xpath[0] == '/'));

    for (ptr = xpath + 1; ptr[0] && (quote || (ptr[0] != '/')); ++ptr) {
        if (quote && (ptr[0] == quote)) {
            quote = 0;
        } else if (!quote && ((ptr[0] == '\'') || (ptr[0] == '\"'))) {
            quote = ptr[0];
        }
    }

    if (quote) {
        /* invalid xpath */
        return NULL;
    }

    return strndup(xpath, ptr - xpath);
}

size_t
sr_xpath_len_no_predicates(const char *xpath)
{
    size_t len = 0;
    int predicate = 0;
    const char *ptr;
    char quoted = 0;

    for (ptr = xpath; ptr[0]; ++ptr) {
        if (quoted) {
            if (ptr[0] == quoted) {
                quoted = 0;
            }
        } else {
            switch (ptr[0]) {
            case '[':
                ++predicate;
                break;
            case ']':
                --predicate;
                break;
            case '\'':
            case '\"':
                assert(predicate);
                quoted = ptr[0];
                break;
            default:
                ++len;
                break;
            }
        }
    }

    if (quoted || predicate) {
        return 0;
    }
    return len;
}

sr_error_info_t *
sr_ly_find_last_parent(struct lyd_node **parent, int nodetype)
{
    sr_error_info_t *err_info = NULL;

    if (!*parent) {
        return NULL;
    }

    while (*parent) {
        if ((*parent)->schema->nodetype & nodetype) {
            /* we found the desired node */
            return NULL;
        }

        switch ((*parent)->schema->nodetype) {
        case LYS_CONTAINER:
        case LYS_LIST:
            if (!(*parent)->child) {
                /* list/container without children, this is the parent */
                return NULL;
            } else {
                *parent = (*parent)->child;
            }
            break;
        case LYS_LEAF:
            assert(lys_is_key((struct lys_node_leaf *)(*parent)->schema, NULL));
            if (!(*parent)->next) {
                /* last key of the last in-depth list, the list instance is what we are looking for */
                *parent = (*parent)->parent;
                return NULL;
            } else {
                *parent = (*parent)->next;
            }
            break;
        default:
            SR_ERRINFO_INT(&err_info);
            return err_info;
        }
    }

    /* should be unreachable */
    SR_ERRINFO_INT(&err_info);
    return err_info;
}

struct lyd_node *
sr_module_data_unlink(struct lyd_node **data, const struct lys_module *ly_mod)
{
    struct lyd_node *next, *node, *mod_data = NULL;

    assert(data && ly_mod);

    LY_TREE_FOR_SAFE(*data, next, node) {
        if (lyd_node_module(node) == ly_mod) {
            /* properly unlink this node */
            if (node == *data) {
                *data = next;
            }
            sr_ly_split(node);
            if (next) {
                sr_ly_split(next);
                if (*data && (*data != next)) {
                    sr_ly_link(*data, next);
                }
            }

            /* connect it to other data from this module */
            if (mod_data) {
                sr_ly_link(mod_data, node);
            } else {
                mod_data = node;
            }
        }
    }

    return mod_data;
}

sr_error_info_t *
sr_module_config_data_append(const struct lys_module *ly_mod, sr_datastore_t ds, struct lyd_node **data)
{
    sr_error_info_t *err_info = NULL;
    sr_datastore_t file_ds;
    struct lyd_node *mod_data = NULL;
    char *path;

    if (ds == SR_DS_OPERATIONAL) {
        file_ds = SR_DS_RUNNING;
    } else {
        file_ds = ds;
    }

    /* prepare correct file path */
    if (file_ds == SR_DS_RUNNING) {
        err_info = sr_path_running_file(ly_mod->name, &path);
    } else {
        err_info = sr_path_startup_file(ly_mod->name, &path);
    }
    if (err_info) {
        goto error;
    }

    /* load data from a persistent storage */
    ly_errno = 0;
    mod_data = lyd_parse_path(ly_mod->ctx, path, LYD_LYB, LYD_OPT_CONFIG | LYD_OPT_STRICT | LYD_OPT_NOEXTDEPS);
    free(path);
    if (ly_errno) {
        sr_errinfo_new_ly(&err_info, ly_mod->ctx);
        goto error;
    }

    if (*data) {
        sr_ly_link(*data, mod_data);
    } else {
        *data = mod_data;
    }
    return NULL;

error:
    lyd_free_withsiblings(mod_data);
    return err_info;
}

sr_error_info_t *
sr_module_config_data_set(const char *mod_name, sr_datastore_t ds, struct lyd_node *mod_data)
{
    sr_error_info_t *err_info = NULL;
    char *path;

    assert(ds != SR_DS_OPERATIONAL);

    if (ds == SR_DS_RUNNING) {
        err_info = sr_path_running_file(mod_name, &path);
    } else {
        err_info = sr_path_startup_file(mod_name, &path);
    }
    if (err_info) {
        return err_info;
    }

    if (lyd_print_path(path, mod_data, LYD_LYB, LYP_WITHSIBLINGS)) {
        sr_errinfo_new_ly(&err_info, lyd_node_module(mod_data)->ctx);
        sr_errinfo_new(&err_info, SR_ERR_INTERNAL, NULL, "Failed to store data file \"%s\".", path);
        free(path);
        return err_info;
    }
    free(path);

    return NULL;
}
