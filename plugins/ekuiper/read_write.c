/**
 * NEURON IIoT System for Industry 4.0
 * Copyright (C) 2020-2022 EMQ Technologies Co., Ltd All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 **/

#include <nng/nng.h>

#include "neuron.h"
#include "json/neu_json_fn.h"
#include "json/neu_json_rw.h"

#include "json_rw.h"
#include "read_write.h"

static int send_write_tag_req(neu_plugin_t *plugin, neu_json_write_req_t *req);
static int send_write_tags_req(neu_plugin_t *             plugin,
                               neu_json_write_tags_req_t *req);

void send_data(neu_plugin_t *plugin, neu_reqresp_trans_data_t *trans_data)
{
    int rv = 0;

    char *           json_str = NULL;
    json_read_resp_t resp     = {
        .plugin     = plugin,
        .trans_data = trans_data,
    };
    rv = neu_json_encode_by_fn(&resp, json_encode_read_resp, &json_str);
    if (0 != rv) {
        plog_error(plugin, "fail encode trans data to json");
        return;
    }

    nng_msg *msg      = NULL;
    size_t   json_len = strlen(json_str);
    rv                = nng_msg_alloc(&msg, json_len);
    if (0 != rv) {
        plog_error(plugin, "nng cannot allocate msg");
        free(json_str);
        return;
    }

    memcpy(nng_msg_body(msg), json_str, json_len); // no null byte
    plog_debug(plugin, ">> %s", json_str);
    free(json_str);
    rv = nng_sendmsg(plugin->sock, msg,
                     NNG_FLAG_NONBLOCK); // TODO: use aio to send message
    if (0 == rv) {
        NEU_PLUGIN_UPDATE_METRIC(plugin, NEU_METRIC_SEND_MSGS_TOTAL, 1, NULL);
        NEU_PLUGIN_UPDATE_METRIC(plugin, NEU_METRIC_SEND_BYTES_5S, json_len,
                                 NULL);
        NEU_PLUGIN_UPDATE_METRIC(plugin, NEU_METRIC_SEND_BYTES_30S, json_len,
                                 NULL);
        NEU_PLUGIN_UPDATE_METRIC(plugin, NEU_METRIC_SEND_BYTES_60S, json_len,
                                 NULL);
    } else {
        plog_error(plugin, "nng cannot send msg: %s", nng_strerror(rv));
        nng_msg_free(msg);
        NEU_PLUGIN_UPDATE_METRIC(plugin, NEU_METRIC_SEND_MSG_ERRORS_TOTAL, 1,
                                 NULL);
    }
}

void recv_data_callback(void *arg)
{
    int               rv       = 0;
    neu_plugin_t *    plugin   = arg;
    nng_msg *         msg      = NULL;
    size_t            json_len = 0;
    char *            json_str = NULL;
    neu_json_write_t *req      = NULL;

    rv = nng_aio_result(plugin->recv_aio);
    if (0 != rv) {
        plog_error(plugin, "nng_recv error: %s", nng_strerror(rv));
        nng_mtx_lock(plugin->mtx);
        plugin->receiving = false;
        nng_mtx_unlock(plugin->mtx);
        return;
    }

    msg      = nng_aio_get_msg(plugin->recv_aio);
    json_str = nng_msg_body(msg);
    json_len = nng_msg_len(msg);
    plog_debug(plugin, "<< %.*s", (int) json_len, json_str);
    NEU_PLUGIN_UPDATE_METRIC(plugin, NEU_METRIC_RECV_MSGS_TOTAL, 1, NULL);
    NEU_PLUGIN_UPDATE_METRIC(plugin, NEU_METRIC_RECV_BYTES_5S, json_len, NULL);
    NEU_PLUGIN_UPDATE_METRIC(plugin, NEU_METRIC_RECV_BYTES_30S, json_len, NULL);
    NEU_PLUGIN_UPDATE_METRIC(plugin, NEU_METRIC_RECV_BYTES_60S, json_len, NULL);
    NEU_PLUGIN_UPDATE_METRIC(plugin, NEU_METRIC_RECV_MSGS_5S, 1, NULL);
    NEU_PLUGIN_UPDATE_METRIC(plugin, NEU_METRIC_RECV_MSGS_30S, 1, NULL);
    NEU_PLUGIN_UPDATE_METRIC(plugin, NEU_METRIC_RECV_MSGS_60S, 1, NULL);
    if (json_decode_write_req(json_str, json_len, &req) < 0) {
        plog_error(plugin, "fail decode write request json: %.*s",
                   (int) nng_msg_len(msg), json_str);
        goto recv_data_callback_end;
    }

    if (req->singular) {
        rv = send_write_tag_req(plugin, &req->single);
    } else {
        rv = send_write_tags_req(plugin, &req->plural);
    }

    if (0 != rv) {
        plog_error(plugin, "failed to write data");
        goto recv_data_callback_end;
    }

recv_data_callback_end:
    nng_msg_free(msg);
    neu_json_decode_write_free(req);
    nng_recv_aio(plugin->sock, plugin->recv_aio);
}

static int json_value_to_tag_value(union neu_json_value *req,
                                   enum neu_json_type t, neu_dvalue_t *value)
{
    switch (t) {
    case NEU_JSON_INT:
        value->type      = NEU_TYPE_INT64;
        value->value.u64 = req->val_int;
        break;
    case NEU_JSON_STR:
        value->type = NEU_TYPE_STRING;
        strncpy(value->value.str, req->val_str, sizeof(value->value.str));
        break;
    case NEU_JSON_DOUBLE:
        value->type      = NEU_TYPE_DOUBLE;
        value->value.d64 = req->val_double;
        break;
    case NEU_JSON_BOOL:
        value->type          = NEU_TYPE_BOOL;
        value->value.boolean = req->val_bool;
        break;
    case NEU_JSON_BYTES:
        value->type               = NEU_TYPE_BYTES;
        value->value.bytes.length = req->val_bytes.length;
        memcpy(value->value.bytes.bytes, req->val_bytes.bytes,
               req->val_bytes.length);
        break;
    default:
        return -1;
    }
    return 0;
}

static int send_write_tag_req(neu_plugin_t *plugin, neu_json_write_req_t *req)
{
    neu_reqresp_head_t header = {
        .type = NEU_REQ_WRITE_TAG,
    };

    neu_req_write_tag_t cmd = {
        .driver = req->node,
        .group  = req->group,
        .tag    = req->tag,
    };

    if (0 != json_value_to_tag_value(&req->value, req->t, &cmd.value)) {
        plog_error(plugin, "invalid tag value type: %d", req->t);
        return -1;
    }

    if (0 != neu_plugin_op(plugin, header, &cmd)) {
        plog_error(plugin, "neu_plugin_op(NEU_REQ_WRITE_TAG) fail");
        return -1;
    }

    req->node  = NULL; // ownership moved
    req->group = NULL; // ownership moved
    req->tag   = NULL; // ownership moved
    return 0;
}

static int send_write_tags_req(neu_plugin_t *             plugin,
                               neu_json_write_tags_req_t *req)
{
    for (int i = 0; i < req->n_tag; i++) {
        if (req->tags[i].t == NEU_JSON_STR) {
            if (strlen(req->tags[i].value.val_str) >= NEU_VALUE_SIZE) {
                return -1;
            }
        }
    }

    neu_reqresp_head_t header = {
        .type = NEU_REQ_WRITE_TAGS,
    };

    neu_req_write_tags_t cmd = { 0 };
    cmd.driver               = req->node;
    cmd.group                = req->group;
    cmd.n_tag                = req->n_tag;
    cmd.tags                 = calloc(cmd.n_tag, sizeof(neu_resp_tag_value_t));
    if (NULL == cmd.tags) {
        return -1;
    }

    for (int i = 0; i < cmd.n_tag; i++) {
        strcpy(cmd.tags[i].tag, req->tags[i].tag);
        if (0 !=
            json_value_to_tag_value(&req->tags[i].value, req->tags[i].t,
                                    &cmd.tags[i].value)) {
            plog_error(plugin, "invalid tag value type: %d", req->tags[i].t);
            free(cmd.tags);
            return -1;
        }
    }

    if (0 != neu_plugin_op(plugin, header, &cmd)) {
        plog_error(plugin, "neu_plugin_op(NEU_REQ_WRITE_TAGS) fail");
        free(cmd.tags);
        return -1;
    }

    req->node  = NULL; // ownership moved
    req->group = NULL; // ownership moved

    return 0;
}
