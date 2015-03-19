/* ====================================================================
 * The Kannel Software License, Version 1.0
 *
 * Copyright (c) 2001-2015 Kannel Group
 * Copyright (c) 1998-2001 WapIT Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Kannel Group (http://www.kannel.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Kannel" and "Kannel Group" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please
 *    contact org@kannel.org.
 *
 * 5. Products derived from this software may not be called "Kannel",
 *    nor may "Kannel" appear in their name, without prior written
 *    permission of the Kannel Group.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE KANNEL GROUP OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Kannel Group.  For more information on
 * the Kannel Group, please see <http://www.kannel.org/>.
 *
 * Portions of this software are based upon software originally written at
 * WapIT Ltd., Helsinki, Finland for the Kannel project.
 */

/**
 * bb_store_redis.c - bearerbox box SMS storage/retrieval module using a redis database
 *
 * Author: Alejandro Guerrieri, 2015
 */

#include "gw-config.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>

#include "gwlib/gwlib.h"
#include "msg.h"
#include "sms.h"
#include "bearerbox.h"
#include "bb_store.h"

#ifdef HAVE_REDIS
#include "gwlib/dbpool.h"

static Counter *counter;
static List *loaded;

static DBPool *pool = NULL;

struct store_db_fields {
    Octstr *table;
    Octstr *field_uuid;
    Octstr *field_message;
};

static struct store_db_fields *fields = NULL;


static int store_redis_dump()
{
    /* nothing to do */
    return 0;
}


static long store_redis_messages()
{
    return counter ? counter_value(counter) : -1;
}


static void redis_update(const Octstr *cmd)
{
    int	res;
    DBPoolConn *pc;

#if defined(DLR_TRACE)
     debug("store.redis", 0, "redis cmd: %s", octstr_get_cstr(cmd));
#endif

    pc = dbpool_conn_consume(pool);
    if (pc == NULL) {
        error(0, "Database pool got no connection! Redis update failed!");
        return;
    }

	res = dbpool_conn_update(pc, cmd, NULL);
 
    if (res != 1) {
        error(0, "Store-Redis: Error while updating: command was `%s'",
              octstr_get_cstr(cmd));
    }

    dbpool_conn_produce(pc);
}


static void store_redis_add(Octstr *id, Octstr *os)
{
    Octstr *cmd;

    octstr_binary_to_base64(os);
    cmd = octstr_format("HSET %s %s %s",
                        octstr_get_cstr(fields->table),
                        octstr_get_cstr(id), octstr_get_cstr(os));
    redis_update(cmd);

    octstr_destroy(cmd);
}


static void store_redis_delete(Octstr *id)
{
	Octstr *cmd;

	cmd = octstr_format("HDEL %s %s",
                        octstr_get_cstr(fields->table),
                        octstr_get_cstr(id));
    redis_update(cmd);

    octstr_destroy(cmd);
}


struct store_db_fields *store_db_fields_create(CfgGroup *grp)
{
    struct store_db_fields *ret;

    ret = gw_malloc(sizeof(*ret));
    gw_assert(ret != NULL);
    memset(ret, 0, sizeof(*ret));

    if ((ret->table = cfg_get(grp, octstr_imm("table"))) == NULL) {
        grp_dump(grp);
        panic(0, "Directive 'table' is not specified in 'group = store-db' context!");
    }

    return ret;
}


void store_db_fields_destroy(struct store_db_fields *fields)
{
    /* sanity check */
    if (fields == NULL)
        return;

    octstr_destroy(fields->table);
    octstr_destroy(fields->field_uuid);
    octstr_destroy(fields->field_message);

    gw_free(fields);
}


static int store_redis_getall(int ignore_err, void(*cb)(Octstr*, void*), void *data)
{
    DBPoolConn *pc;
    Octstr *cmd;
    Octstr *os, *key;
    List *result, *row;

    cmd = octstr_format("HGETALL %s", octstr_get_cstr(fields->table));

#if defined(DLR_TRACE)
    debug("store.redis", 0, "redis cmd: %s", octstr_get_cstr(cmd));
#endif

    pc = dbpool_conn_consume(pool);
    if (pc == NULL) {
        error(0, "Database pool got no connection! Redis HGETALL failed!");
        dbpool_conn_produce(pc);
        return -1;
    }
    if (dbpool_conn_select(pc, cmd, NULL, &result) != 0) {
        error(0, "Failed to fetch messages from redis with cmd `%s'",
              octstr_get_cstr(cmd));
        octstr_destroy(cmd);
        dbpool_conn_produce(pc);
        return -1;
    } 

	dbpool_conn_produce(pc);
    octstr_destroy(cmd);

    if (gwlist_len(result) > 0) {
        while ((row = gwlist_extract_first(result)) != NULL) {
            if (gwlist_len(row) > 1) {
                key = gwlist_extract_first(row);
                os = gwlist_extract_first(row);
                debug("store.redis", 0, "Found entry for message ID <%s>", octstr_get_cstr(key));
                octstr_base64_to_binary(os);
                if (os == NULL) {
                    error(0, "Could not base64 decode message ID <%s>", octstr_get_cstr(key));
                } else {
                    cb(os, data);
                }
                octstr_destroy(os);
                octstr_destroy(key);
            }
            gwlist_destroy(row, octstr_destroy_item);
        }
    } else {
        debug("store.redis", 0, "No messages loaded from redis store");
    }
    gwlist_destroy(result, NULL);

    return 0;
}


struct status {
    const char *format;
    Octstr *status;
};

static void status_cb(Octstr *msg_s, void *d)
{
    struct status *data = d;
    struct tm tm;
    char id[UUID_STR_LEN + 1];
    Msg *msg;

    msg = store_msg_unpack(msg_s);

    if (msg == NULL)
        return;
    /* transform the time value */
#if LOG_TIMESTAMP_LOCALTIME
    tm = gw_localtime(msg->sms.time);
#else
    tm = gw_gmtime(msg->sms.time);
#endif
    if (msg->sms.udhdata)
        octstr_binary_to_hex(msg->sms.udhdata, 1);
    if (msg->sms.msgdata &&
        (msg->sms.coding == DC_8BIT || msg->sms.coding == DC_UCS2 ||
        (msg->sms.coding == DC_UNDEF && msg->sms.udhdata)))
        octstr_binary_to_hex(msg->sms.msgdata, 1);

    uuid_unparse(msg->sms.id, id);

    octstr_format_append(data->status, data->format,
        id,
        (msg->sms.sms_type == mo ? "MO" :
        msg->sms.sms_type == mt_push ? "MT-PUSH" :
        msg->sms.sms_type == mt_reply ? "MT-REPLY" :
        msg->sms.sms_type == report_mo ? "DLR-MO" :
        msg->sms.sms_type == report_mt ? "DLR-MT" : ""),
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        (msg->sms.sender ? octstr_get_cstr(msg->sms.sender) : ""),
        (msg->sms.receiver ? octstr_get_cstr(msg->sms.receiver) : ""),
        (msg->sms.smsc_id ? octstr_get_cstr(msg->sms.smsc_id) : ""),
        (msg->sms.boxc_id ? octstr_get_cstr(msg->sms.boxc_id) : ""),
        (msg->sms.udhdata ? octstr_get_cstr(msg->sms.udhdata) : ""),
        (msg->sms.msgdata ? octstr_get_cstr(msg->sms.msgdata) : ""));

    msg_destroy(msg);
}

static Octstr *store_redis_status(int status_type)
{
    Octstr *ret = octstr_create("");
    const char *format;
    struct status data;

    /* check if we are active */
    if (pool == NULL)
        return ret;

    /* set the type based header */
    if (status_type == BBSTATUS_HTML) {
        octstr_append_cstr(ret, "<table border=1>\n"
            "<tr><td>SMS ID</td><td>Type</td><td>Time</td><td>Sender</td><td>Receiver</td>"
            "<td>SMSC ID</td><td>BOX ID</td><td>UDH</td><td>Message</td>"
            "</tr>\n");

        format = "<tr><td>%s</td><td>%s</td>"
                "<td>%04d-%02d-%02d %02d:%02d:%02d</td>"
                "<td>%s</td><td>%s</td><td>%s</td>"
                "<td>%s</td><td>%s</td><td>%s</td></tr>\n";
    } else if (status_type == BBSTATUS_XML) {
        format = "<message>\n\t<id>%s</id>\n\t<type>%s</type>\n\t"
                "<time>%04d-%02d-%02d %02d:%02d:%02d</time>\n\t"
                "<sender>%s</sender>\n\t"
                "<receiver>%s</receiver>\n\t<smsc-id>%s</smsc-id>\n\t"
                "<box-id>%s</box-id>\n\t"
                "<udh-data>%s</udh-data>\n\t<msg-data>%s</msg-data>\n\t"
                "</message>\n";
    } else {
        octstr_append_cstr(ret, "[SMS ID] [Type] [Time] [Sender] [Receiver] [SMSC ID] [BOX ID] [UDH] [Message]\n");
        format = "[%s] [%s] [%04d-%02d-%02d %02d:%02d:%02d] [%s] [%s] [%s] [%s] [%s] [%s]\n";
    }

    data.format = format;
    data.status = ret;
    /* ignore error because files may disappear */
    store_redis_getall(1, status_cb, &data);

    /* set the type based footer */
    if (status_type == BBSTATUS_HTML) {
        octstr_append_cstr(ret,"</table>");
    }

    return ret;
}


static void dispatch(Octstr *msg_s, void *data)
{
    Msg *msg;
    void (*receive_msg)(Msg*) = data;

    if (msg_s == NULL)
        return;

    msg = store_msg_unpack(msg_s);
    if (msg != NULL) {
        receive_msg(msg);
        counter_increase(counter);
    } else {
        error(0, "Could not unpack message from redis store!");
    }
}


static int store_redis_load(void(*receive_msg)(Msg*))
{
    int rc;

    /* check if we are active */
    if (pool == NULL)
        return 0;
        
    /* sanity check */
    if (receive_msg == NULL)
        return -1;

    rc = store_redis_getall(0, dispatch, receive_msg);

    info(0, "Loaded %ld messages from store.", counter_value(counter));

    /* allow using of storage */
    gwlist_remove_producer(loaded);

    return rc;
}


static int store_redis_save(Msg *msg)
{
    char id[UUID_STR_LEN + 1];
    Octstr *id_s;

    /* always set msg id and timestamp */
    if (msg_type(msg) == sms && uuid_is_null(msg->sms.id))
        uuid_generate(msg->sms.id);

    if (msg_type(msg) == sms && msg->sms.time == MSG_PARAM_UNDEFINED)
        time(&msg->sms.time);

    if (pool == NULL)
        return 0;

    /* block here if store still not loaded */
    gwlist_consume(loaded);

    switch (msg_type(msg)) {
        case sms:
        {
            Octstr *os = store_msg_pack(msg);

            if (os == NULL) {
                error(0, "Could not pack message.");
                return -1;
            }
            uuid_unparse(msg->sms.id, id);
            id_s = octstr_create(id);
            store_redis_add(id_s, os);
            octstr_destroy(id_s);
            counter_increase(counter);
            octstr_destroy(os);
            break;
        }
        case ack:
        {
            uuid_unparse(msg->ack.id, id);
            id_s = octstr_create(id);
        	store_redis_delete(id_s);
            octstr_destroy(id_s);
            counter_decrease(counter);
            break;
        }
        default:
            return -1;
    }

    return 0;
}


static int store_redis_save_ack(Msg *msg, ack_status_t status)
{
    int ret;
    Msg *nack = msg_create(ack);

    nack->ack.nack = status;
    uuid_copy(nack->ack.id, msg->sms.id);
    nack->ack.time = msg->sms.time;
    ret = store_redis_save(nack);
    msg_destroy(nack);

    return ret;
}


static void store_redis_shutdown()
{
    dbpool_destroy(pool);
    store_db_fields_destroy(fields);
        
    counter_destroy(counter);
    gwlist_destroy(loaded, NULL);
}


int store_redis_init(Cfg *cfg)
{
    CfgGroup *grp;
    List *grplist;
    Octstr *redis_host, *redis_pass, *redis_id;
    long redis_port = 0, redis_database = -1, redis_idle_timeout = -1;
    Octstr *p = NULL;
    long pool_size;
    DBConf *db_conf = NULL;

    store_messages = store_redis_messages;
    store_save = store_redis_save;
    store_save_ack = store_redis_save_ack;
    store_load = store_redis_load;
    store_dump = store_redis_dump;
    store_shutdown = store_redis_shutdown;
    store_status = store_redis_status;

    /*
     * Check for all mandatory directives that specify the field names
     * of the used Redis key
     */
    if (!(grp = cfg_get_single_group(cfg, octstr_imm("store-db"))))
        panic(0, "Store-Redis: group 'store-db' is not specified!");

    if (!(redis_id = cfg_get(grp, octstr_imm("id"))))
        panic(0, "Store-Redis: directive 'id' is not specified!");

    fields = store_db_fields_create(grp);
    gw_assert(fields != NULL);

    /*
     * Now grab the required information from the 'redis-connection' group
     * with the id we just obtained.
     *
     * We have to loop through all available Redis connection definitions
     * and search for the one we are looking for.
     */
    grplist = cfg_get_multi_group(cfg, octstr_imm("redis-connection"));
    while (grplist && (grp = gwlist_extract_first(grplist)) != NULL) {
        p = cfg_get(grp, octstr_imm("id"));
        if (p != NULL && octstr_compare(p, redis_id) == 0) {
            goto found;
        }
        if (p != NULL)
            octstr_destroy(p);
    }
    panic(0, "Connection settings for 'redis-connection' with id '%s' are not specified!",
          octstr_get_cstr(redis_id));

found:
    octstr_destroy(p);
    gwlist_destroy(grplist, NULL);

    if (cfg_get_integer(&pool_size, grp, octstr_imm("max-connections")) == -1 || pool_size == 0)
        pool_size = 1;

    if (!(redis_host = cfg_get(grp, octstr_imm("host")))) {
        grp_dump(grp);
        panic(0, "Directive 'host' is not specified in 'group = redis-connection' context!");
    }
    if (cfg_get_integer(&redis_port, grp, octstr_imm("port")) == -1) {
        grp_dump(grp);
        panic(0, "Directive 'port' is not specified in 'group = redis-connection' context!");
    }
    redis_pass = cfg_get(grp, octstr_imm("password"));
    cfg_get_integer(&redis_database, grp, octstr_imm("database"));
    cfg_get_integer(&redis_idle_timeout, grp, octstr_imm("idle-timeout"));

    /*
     * Ok, ready to connect to Redis
     */
    db_conf = gw_malloc(sizeof(DBConf));
    gw_assert(db_conf != NULL);

    db_conf->redis = gw_malloc(sizeof(RedisConf));
    gw_assert(db_conf->redis != NULL);

    db_conf->redis->host = redis_host;
    db_conf->redis->port = redis_port;
    db_conf->redis->password = redis_pass;
    db_conf->redis->database = redis_database;
    db_conf->redis->idle_timeout = redis_idle_timeout;

    pool = dbpool_create(DBPOOL_REDIS, db_conf, pool_size);
    gw_assert(pool != NULL);

    /*
     * Panic on failure to connect. Should we just try to reconnect?
     */
    if (dbpool_conn_count(pool) == 0)
        panic(0, "Redis database pool has no connections!");

    loaded = gwlist_create();
    gwlist_add_producer(loaded);
    counter = counter_create();

    octstr_destroy(redis_id);

    return 0;
}

#endif
