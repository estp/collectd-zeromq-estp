/**
 * collectd - src/zeromq_estp.c
 * Copyright (C) 2005-2010  Florian octo Forster
 * Copyright (C) 2009       Aman Gupta
 * Copyright (C) 2010       Julien Ammous
 * Copyright (C) 2012       Paul Colomiets
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Florian octo Forster <octo at verplant.org>
 *   Aman Gupta <aman at tmm1.net>
 *   Julien Ammous
 **/

#define _GNU_SOURCE
#include "collectd.h"
#include "common.h" /* auxiliary functions */
#include "plugin.h" /* plugin_register_*, plugin_dispatch_values */
#include "utils_avltree.h"
#include "network.h"

/* for htons() */
#if HAVE_ARPA_INET_H
# include <arpa/inet.h>
#endif
#include <pthread.h>
#include <alloca.h>
#include <time.h>
#include <stdio.h>

#include <zmq.h>

struct cmq_socket_s {
    void *socket;
    int type;
};
typedef struct cmq_socket_s cmq_socket_t;

struct cmq_cache_entry_s {
    char *fullname;
    char *timestamp;
    char *type;
    char *items;
    int nitems;
    unsigned long mask;
    value_t values[];
};
typedef struct cmq_cache_entry_s cmq_cache_entry_t;

static int cmq_threads_num = 1;
static void *cmq_context = NULL;
static c_avl_tree_t *staging = NULL;

static pthread_t *receive_thread_ids = NULL;
static size_t     receive_thread_num = 0;
static int        sending_sockets_num = 0;

// private data
static int thread_running = 1;
static pthread_t listen_thread_id;

static void cmq_close_callback (void *socket) /* {{{ */
{
    if (socket != NULL)
        (void) zmq_close (socket);
} /* }}} void cmq_close_callback */

static void dispatch_entry(char *fullname, char *timestamp, time_t interval,
                           char *vtype, int values_len, value_t *values) {
    struct tm timest;
    value_list_t vl;
    strcpy(vl.type, vtype);

    if (!strptime (timestamp, "%Y-%m-%dT%H:%M:%S", &timest)) {
        WARNING("ZeroMQ-ESTP: can't parse timestamp");
        return;
    }
    // Hostname
    char *cpos = fullname;
    char *end = strchr(cpos, ':');
    if(!end) {
        WARNING("ZeroMQ-ESTP: No delimiter after hostname");
        return;
    }
    if(end - cpos > 63) {
        WARNING("ZeroMQ-ESTP: Too long hostname");
        return;
    }
    memcpy(vl.host, cpos, end-cpos);
    vl.host[end-cpos] = 0;

    // Plugin name
    cpos = end+1;
    end = strchr(cpos, ':');
    if(!end) {
        WARNING("ZeroMQ-ESTP: No delimiter after application/subsystem name");
        return;
    }
    if(end - cpos > 63) {
        WARNING("ZeroMQ-ESTP: Too long application/subsystem name");
        return;
    }
    memcpy(vl.plugin, cpos, end-cpos);
    vl.plugin[end-cpos] = 0;

    // Plugin instance
    cpos = end+1;
    end = strchr(cpos, ':');
    if(!end) {
        WARNING("ZeroMQ-ESTP: No delimiter after resource name");
        return;
    }
    if(end - cpos > 63) {
        WARNING("ZeroMQ-ESTP: Too long resource name");
        return;
    }
    memcpy(vl.plugin_instance, cpos, end-cpos);
    vl.plugin_instance[end-cpos] = 0;

    // Type instance
    cpos = end+1;
    end = strchr(cpos, ':');
    if(!end) {
        WARNING("ZeroMQ-ESTP: No delimiter after metric name");
        return;
    }
    if(end - cpos > 63) {
        WARNING("ZeroMQ-ESTP: Too long metric name");
        return;
    }
    memcpy(vl.type_instance, cpos, end-cpos);
    vl.type_instance[end-cpos] = 0;

    vl.time = TIME_T_TO_CDTIME_T(timegm(&timest));
    vl.interval = TIME_T_TO_CDTIME_T(interval);
    vl.values = values;
    vl.values_len = values_len;
    vl.meta = NULL;

    plugin_dispatch_values (&vl);
}

static char *find_suffix(char *fullname) {
    char *endsuffix = fullname + strlen(fullname)-1;
    if(*endsuffix != ':') {
        WARNING("ZeroMQ-ESTP: Metric full name not ends with ':'");
        return NULL;
    }
    char *suffix = endsuffix-1;
    while(suffix > fullname) {
        if(*suffix == '.' || *suffix == ':') {
            return suffix+1;
        }
        --suffix;
    }
    return NULL;
}

static int find_field(char *suffix, char *items) {
    int suffixlen = strlen(suffix) - 1  /* colon at the end */;
    int idx = 0;
    if(!strncmp(suffix, items, suffixlen)
       && (items[suffixlen] == 0 || items[suffixlen] == ',')) {
        return 0;
    }
    char *c;
    for(c = items; *c; ++c) {
        if(*c == ',') {
            c += 1;
            idx += 1;
            if(!strncmp(suffix, c, suffixlen)
               && (c[suffixlen] == 0 || c[suffixlen] == ',')) {
                return idx;
            }
        }
    }
    WARNING("ZeroMQ-ESTP: Can't find suffix in list of suffixes");
    return -1;
}

static void dispatch_multi_entry (char *fullname, char *timestamp,
                                  time_t interval,
                                  char *type, char *items,
                                  value_t value) {
    char *suffix = find_suffix(fullname);
    if(!suffix)
        return;
    int myindex = find_field(suffix, items);
    if(myindex < 0)
        return;

    int clen = suffix-fullname+2;
    char cutname[clen];
    memcpy(cutname, fullname, clen-2);
    cutname[clen-2] = ':';
    cutname[clen-1] = 0;

    cmq_cache_entry_t *entry;
    int rc = c_avl_get (staging, cutname, (void *)&entry);

    if(rc == 0) {
        if(!strcmp(entry->timestamp, timestamp)
           && !strcmp(entry->type, type)
           && !strcmp(entry->items, items)) {
            entry->values[myindex] = value;
            entry->mask |= 1 << myindex;
            if(entry->mask == (1 << entry->nitems)-1) {
                dispatch_entry(cutname, timestamp, interval, type,
                               entry->nitems, entry->values);
                c_avl_remove(staging, cutname, NULL, NULL);
                free(entry);
            }
            return;
        } else {
            INFO("ZeroMQ-ESTP: Clearing cache entry as it is stale\n");
            c_avl_remove(staging, cutname, NULL, NULL);
            free(entry);
        }
    }
    // Ok, let's create new entry
    int tlen = strlen(timestamp) + 1;
    int ylen = strlen(type) + 1;
    int ilen = strlen(items) + 1;
    int nitems = 1;
    char *c;
    for(c = items; *c; ++c)
        if(*c == ',')
            nitems += 1;
    entry = malloc(sizeof(cmq_cache_entry_t)
        + nitems*sizeof(value_t)
        + clen + tlen + ylen + ilen);
    if(!entry) {
        WARNING("Not enough memory for new entry");
        return;
    }
    char *curbuf = ((char *)entry) + sizeof(cmq_cache_entry_t)
                                   + nitems*sizeof(value_t);
    memcpy(curbuf, cutname, clen);
    entry->fullname = curbuf;
    curbuf += clen;
    memcpy(curbuf, timestamp, tlen);
    entry->timestamp = curbuf;
    curbuf += tlen;
    memcpy(curbuf, type, ylen);
    entry->type = curbuf;
    curbuf += ylen;
    memcpy(curbuf, items, ilen);
    entry->items = curbuf;
    curbuf += ilen;
    entry->nitems = nitems;
    entry->mask = 1 << myindex;
    entry->values[myindex] = value;

    c_avl_insert(staging, entry->fullname, entry);
}

static void parse_message (char *data, int dlen)
{

    char fullname[300];
    char timestamp[32];
    unsigned long interval;
    char value[64];

    char *endline = memchr (data, '\n', dlen);
    int headlen = dlen;
    if(endline)
        headlen = endline - data;
    char *hdata = alloca(headlen+1);
    memcpy(hdata, data, headlen);
    hdata[headlen] = 0;

    int rc = sscanf (hdata, "ESTP:%300s %31s %lu %63s",
                     fullname, timestamp, &interval, value);
    if (rc != 4) {
        WARNING("ZeroMQ-ESTP: message has wrong format");
        return;
    }

    int vdouble = 0;
    double dvalue;
    char *end;
    long lvalue = strtol (value, &end, 10);

    if(*end == '.') {
        dvalue = strtod(value, &end);
        vdouble = 1;
    }
    if(end == hdata) {
        WARNING("ZeroMQ-ESTP: wrong value");
        return;
    }

    char vtype[64];
    value_t val;

    // Parse type
    if(*end == ':') {
        ++end;
        if(*end == 'd') {
            strcpy(vtype, "derive");  // FIXME: derive can be negative
            if(vdouble) {
                val.derive = dvalue;
            } else {
                val.derive = lvalue;
            }
        } else if(*end == 'c') {
            strcpy(vtype, "derive");  // non-wrapping counter
            if(vdouble) {
                val.counter = dvalue;
            } else {
                val.counter = lvalue;
            }
        } else if(*end == 'a') {
            strcpy(vtype, "absolute");
            if(vdouble) {
                val.absolute = dvalue;
            } else {
                val.absolute = lvalue;
            }
        } else {
            WARNING("ZeroMQ-ESTP: Unknown type");
            return;
        }
    } else {
        strcpy(vtype, "gauge");
        if(vdouble) {
            val.gauge = dvalue;
        } else {
            val.gauge = lvalue;
        }
    }

    //  Let's find extension
    if(endline) { //  there is extension data
        char *ext = memmem(endline, dlen - headlen, "\n :collectd:", 12);
        if(ext) { //  collectd extension found
            ext += 12;
            char *endext = memchr(ext, '\n', data + dlen - ext);
            int elen;
            if(!endext)
                elen = data + dlen - ext;
            else
                elen = endext - ext;
            char *fullext = alloca(elen+1);
            memcpy(fullext, ext, elen);
            fullext[elen] = 0;
            while (42) {
                char ekey[24];
                char evalue[elen];
                int chars;
                if(sscanf(fullext, " %23[^=]=%s%n", ekey, evalue, &chars) < 2)
                    break;
                fullext += chars;
                if(!strcmp(ekey, "type")) {
                    strncpy(vtype, evalue, 64);
                } else if(!strcmp(ekey, "items")) {
                    dispatch_multi_entry(fullname, timestamp, interval,
                                         vtype, evalue, val);
                    return;
                }
            }
        }
    }

    dispatch_entry(fullname, timestamp, interval, vtype, 1, &val);
}

static void *receive_thread (void *cmq_socket) /* {{{ */
{
    int status;

    assert (cmq_socket != NULL);

    while (thread_running) {
        zmq_msg_t msg;

        (void) zmq_msg_init (&msg);

        status = zmq_recv (cmq_socket, &msg, /* flags = */ 0);
        if (status != 0) {
            if ((errno == EAGAIN) || (errno == EINTR))
                continue;

            ERROR ("zeromq plugin: zmq_recv failed: %s", zmq_strerror (errno));
            break;
        }


        parse_message (zmq_msg_data (&msg), zmq_msg_size (&msg));

        DEBUG("zeromq plugin: received data, parse returned %d", status);

        (void) zmq_msg_close (&msg);
    } /* while (thread_running) */

    DEBUG ("zeromq plugin: Receive thread is terminating.");
    (void) zmq_close (cmq_socket);

    return (NULL);
} /* }}} void *receive_thread */

#define PACKET_SIZE   512

static int put_single_value (void *socket, const char *name, value_t value,
                             const value_list_t *vl, const data_source_t *ds,
                             char *extdata)
{
    int datalen;
    char data[640];
    char tstring[32];
    time_t timeval = CDTIME_T_TO_TIME_T(vl->time);
    unsigned interval = CDTIME_T_TO_TIME_T(vl->interval);
    struct tm tstruct;
    gmtime_r(&timeval, &tstruct);
    strftime(tstring, 32, "%Y-%m-%dT%H:%M:%S", &tstruct);

    if(ds->type == DS_TYPE_COUNTER) {
        datalen = snprintf(data, 640, "ESTP:%s:%s:%s:%s: %s %d %llu:c%s",
            vl->host, vl->plugin, vl->plugin_instance, name,
            tstring, interval, value.counter, extdata);
    } else if(ds->type == DS_TYPE_GAUGE) {
        datalen = snprintf(data, 640, "ESTP:%s:%s:%s:%s: %s %d %lf%s",
            vl->host, vl->plugin, vl->plugin_instance, name,
            tstring, interval, value.gauge, extdata);
    } else if(ds->type == DS_TYPE_DERIVE) {
        datalen = snprintf(data, 640, "ESTP:%s:%s:%s:%s: %s %d %ld:d%s",
            vl->host, vl->plugin, vl->plugin_instance, name,
            tstring, interval, value.derive, extdata);
    } else if(ds->type == DS_TYPE_ABSOLUTE) {
        datalen = snprintf(data, 640, "ESTP:%s:%s:%s:%s: %s %d %lu:a%s",
            vl->host, vl->plugin, vl->plugin_instance, name,
            tstring, interval, value.absolute, extdata);
    } else {
        WARNING("ZeroMQ-ESTP: Unknown type");
        return -1;
    }
    zmq_msg_t msg;

    if(zmq_msg_init_size(&msg, datalen) != 0) {
        ERROR("zmq_msg_init : %s", zmq_strerror(errno));
        return 1;
    }
    memcpy(zmq_msg_data(&msg), data, datalen);

    // try to send the message
    if(zmq_send(socket, &msg, ZMQ_NOBLOCK) != 0) {
        if(errno == EAGAIN) {
            WARNING("ZeroMQ: Unable to queue message, queue may be full");
            return -1;
        } else {
            ERROR("zmq_send : %s", zmq_strerror(errno));
            return -1;
        }
    }

    DEBUG("ZeroMQ: data sent");

    return 0;
}

static int write_value (const data_set_t *ds, /* {{{ */
                        const value_list_t *vl,
                        user_data_t *user_data)
{
    assert (vl->values_len == ds->ds_num);
    if(ds->ds_num > 1) {
        int i;

        int elen = 100; // prefix and type
        for(i = 0; i < ds->ds_num; ++i)
            elen += strlen(ds->ds[i].name) + 1;
        char *extdata = alloca(elen);
        char *cur = extdata + sprintf(extdata,
            "\n :collectd: type=%s items=%s",
            ds->type, ds->ds[0].name);
        for(i = 1; i < ds->ds_num; ++i) {
            *cur++ = ',';
            int len = strlen(ds->ds[i].name);
            memcpy(cur, ds->ds[i].name, len);
            cur += len;
        }
        *cur = 0;

        for(i = 0; i < ds->ds_num; ++i) {
            if(*vl->type_instance) {
                char name[64];
                int len;
                len = snprintf(name, 63, "%s.%s",
                    vl->type_instance, ds->ds[i].name);
                name[len] = 0;
                put_single_value(user_data->data,
                    name, vl->values[i], vl, &ds->ds[i], extdata);
            } else {
                put_single_value(user_data->data,
                    ds->ds[i].name, vl->values[i], vl, &ds->ds[i], extdata);
            }
        }
    } else {
        char extdata[100] = "";
        if(strcmp(ds->type, "gauge")
           && strcmp(ds->type, "counter")
           && strcmp(ds->type, "derive")
           && strcmp(ds->type, "absolute")) {
            sprintf(extdata, "\n :collectd: type=%s", ds->type);
        }
        put_single_value(user_data->data,
            vl->type_instance, vl->values[0], vl, &ds->ds[0], extdata);
    }
    return 0;
}


static int cmq_config_mode (oconfig_item_t *ci) /* {{{ */
{
    char buffer[64] = "";
    int status;

    status = cf_util_get_string_buffer (ci, buffer, sizeof (buffer));
    if (status != 0)
        return (-1);

    if (strcasecmp ("Publish", buffer) == 0)
        return (ZMQ_PUB);
    else if (strcasecmp ("Subscribe", buffer) == 0)
        return (ZMQ_SUB);
    else if (strcasecmp ("Push", buffer) == 0)
        return (ZMQ_PUSH);
    else if (strcasecmp ("Pull", buffer) == 0)
        return (ZMQ_PULL);

    ERROR ("zeromq plugin: Unrecognized communication pattern: \"%s\"",
           buffer);
    return (-1);
} /* }}} int cmq_config_mode */

static int cmq_config_socket (oconfig_item_t *ci) /* {{{ */
{
    int type;
    int status;
    int i;
    int endpoints_num;
    void *cmq_socket;

    type = cmq_config_mode (ci);
    if (type < 0)
        return (-1);

    if (cmq_context == NULL) {
        cmq_context = zmq_init (cmq_threads_num);
        if (cmq_context == NULL) {
            ERROR ("zeromq plugin: Initializing ZeroMQ failed: %s",
                   zmq_strerror (errno));
            return (-1);
        }

        INFO("ZeroMQ: Using %d threads", cmq_threads_num);
    }

    /* Create a new socket */
    cmq_socket = zmq_socket (cmq_context, type);
    if (cmq_socket == NULL) {
        ERROR ("zeromq plugin: zmq_socket failed: %s",
               zmq_strerror (errno));
        return (-1);
    }

    if (type == ZMQ_SUB) {
        /* Subscribe to all messages */
        /* TODO(tailhook) implement subscription configuration */
        status = zmq_setsockopt (cmq_socket, ZMQ_SUBSCRIBE,
                                 /* prefix = */ "", /* prefix length = */ 0);
        if (status != 0) {
            ERROR ("zeromq plugin: zmq_setsockopt (ZMQ_SUBSCRIBE) failed: %s",
                   zmq_strerror (errno));
            (void) zmq_close (cmq_socket);
            return (-1);
        }
    }

    /* Iterate over all children and do all the binds and connects requested. */
    endpoints_num = 0;
    for (i = 0; i < ci->children_num; i++) {
        oconfig_item_t *child = ci->children + i;

        if (strcasecmp ("Bind", child->key) == 0) {
            char *value = NULL;

            status = cf_util_get_string (child, &value);
            if (status != 0)
                continue;

            DEBUG("Binding to %s", value);
            status = zmq_bind (cmq_socket, value);
            if (status != 0) {
                ERROR ("zeromq plugin: zmq_bind (\"%s\") failed: %s",
                       value, zmq_strerror (errno));
                sfree (value);
                continue;
            }
            endpoints_num++;
            continue;
        } /* Bind */
        else if (strcasecmp ("Connect", child->key) == 0) {
            char *value = NULL;

            status = cf_util_get_string (child, &value);
            if (status != 0)
                continue;

            DEBUG("Connecting to %s", value);
            status = zmq_connect (cmq_socket, value);
            if (status != 0) {
                ERROR ("zeromq plugin: zmq_connect (\"%s\") failed: %s",
                       value, zmq_strerror (errno));
                sfree (value);
                continue;
            }

            sfree (value);

            endpoints_num++;
            continue;
        } /* Connect */
        else if( strcasecmp("HWM", child->key) == 0 ) {
            int tmp;
            uint64_t hwm;

            status = cf_util_get_int(child, &tmp);
            if( status != 0 )
                continue;

            hwm = (uint64_t) tmp;

            status = zmq_setsockopt (cmq_socket, ZMQ_HWM, &hwm, sizeof(hwm));
            if (status != 0) {
                ERROR ("zeromq plugin: zmq_setsockopt (ZMQ_HWM) failed: %s", zmq_strerror (errno));
                (void) zmq_close (cmq_socket);
                return (-1);
            }

            continue;
        } /* HWM */
        else {
            ERROR ("zeromq plugin: The \"%s\" config option is now allowed here.",
                   child->key);
        }
    } /* for (i = 0; i < ci->children_num; i++) */

    if (endpoints_num == 0) {
        ERROR ("zeromq plugin: No (valid) \"Bind\"/\"Connect\" "
               "option was found in this \"Socket\" block.");
        (void) zmq_close (cmq_socket);
        return (-1);
    }

    /* If this is a receiving socket, create a new receive thread */
    if ((type == ZMQ_SUB) || (type == ZMQ_PULL)) {
        pthread_t *thread_ptr;

        thread_ptr = realloc (receive_thread_ids,
                              sizeof (*receive_thread_ids) * (receive_thread_num + 1));
        if (thread_ptr == NULL) {
            ERROR ("zeromq plugin: realloc failed.");
            return (-1);
        }
        receive_thread_ids = thread_ptr;
        thread_ptr = receive_thread_ids + receive_thread_num;

        status = pthread_create (thread_ptr,
                                 /* attr = */ NULL,
                                 /* func = */ receive_thread,
                                 /* args = */ cmq_socket);
        if (status != 0) {
            char errbuf[1024];
            ERROR ("zeromq plugin: pthread_create failed: %s",
                   sstrerror (errno, errbuf, sizeof (errbuf)));
            (void) zmq_close (cmq_socket);
            return (-1);
        }

        receive_thread_num++;
    }

    /* If this is a sending socket, register a new write function */
    else if ((type == ZMQ_PUB) || (type == ZMQ_PUSH)) {
        user_data_t ud = { NULL, NULL };
        char name[20];

        ud.data = cmq_socket;
        ud.free_func = cmq_close_callback;

        ssnprintf (name, sizeof (name), "zeromq/%i", sending_sockets_num);
        sending_sockets_num++;

        plugin_register_write (name, write_value, &ud);
    }

    return (0);
} /* }}} int cmq_config_socket */

/*
 * Config schema:
 *
 * <Plugin "zeromq_estp">
 *   Threads 2
 *
 *   <Socket Publish>
 *     HWM 300
 *     Connect "tcp://localhost:6666"
 *   </Socket>
 *   <Socket Subscribe>
 *     Bind "tcp://eth0:6666"
 *     Bind "tcp://collectd.example.com:6666"
 *   </Socket>
 * </Plugin>
 */
static int cmq_config (oconfig_item_t *ci) /* {{{ */
{
    int status;
    int i;

    for (i = 0; i < ci->children_num; i++) {
        oconfig_item_t *child = ci->children + i;

        if (strcasecmp ("Socket", child->key) == 0)
            status = cmq_config_socket (child);
        else if (strcasecmp ("Threads", child->key) == 0) {
            int tmp = 0;
            status = cf_util_get_int (child, &tmp);
            if ((status == 0) && (tmp >= 1))
                cmq_threads_num = tmp;
        } else {
            WARNING ("zeromq plugin: The \"%s\" config option is not allowed here.",
                     child->key);
        }
    }

    return (0);
} /* }}} int cmq_config */

static int plugin_init (void)
{
    int major, minor, patch;
    zmq_version (&major, &minor, &patch);
    INFO("ZeroMQ plugin loaded (zeromq v%d.%d.%d).", major, minor, patch);
    staging = c_avl_create ((void *) strcmp);
    if (staging == NULL) {
        ERROR("ZeroMQ-ESTP : c_avl_create failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int my_shutdown (void)
{
    if( cmq_context ) {

        thread_running = 0;

        DEBUG("ZeroMQ: shutting down");

        if( zmq_term(cmq_context) != 0 ) {
            ERROR("zmq_term : %s", zmq_strerror(errno));
            return 1;
        }

        pthread_join(listen_thread_id, NULL);

    }

    if( staging ) {
        while (42) {
            char *key = NULL;
            cmq_cache_entry_t *value = NULL;

            int rc = c_avl_pick (staging, (void *) &key, (void *) &value);
            if (rc != 0)
                break;

            free (key);
            free (value);
        }

        c_avl_destroy (staging);
    }

    return 0;
}

void module_register (void)
{
    plugin_register_complex_config("zeromq_estp", cmq_config);
    plugin_register_init("zeromq_estp", plugin_init);
    plugin_register_shutdown ("zeromq_estp", my_shutdown);
}

