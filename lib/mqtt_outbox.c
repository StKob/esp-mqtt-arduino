#include "include/mqtt_outbox.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "include/mqtt_config.h"
#include "sys/queue.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#ifndef CONFIG_MQTT_CUSTOM_OUTBOX
static const char *TAG = "outbox";

typedef struct outbox_item {
    char *buffer;
    int len;
    int msg_id;
    int msg_type;
    int msg_qos;
    outbox_tick_t tick;
    pending_state_t pending;
    STAILQ_ENTRY(outbox_item) next;
} outbox_item_t;

STAILQ_HEAD(outbox_list_t, outbox_item);

struct outbox_t {
    _Atomic uint64_t size;
    struct outbox_list_t *list;
};

outbox_handle_t outbox_init(void)
{
    outbox_handle_t outbox = calloc(1, sizeof(struct outbox_t));
    ESP_MEM_CHECK(TAG, outbox, return NULL);
    outbox->list = calloc(1, sizeof(struct outbox_list_t));
    ESP_MEM_CHECK(TAG, outbox->list, {free(outbox); return NULL;});
    outbox->size = 0;
    STAILQ_INIT(outbox->list);
    return outbox;
}

outbox_item_handle_t outbox_enqueue(outbox_handle_t outbox, outbox_message_handle_t message, outbox_tick_t tick)
{
    outbox_item_handle_t item = calloc(1, sizeof(outbox_item_t));
    ESP_MEM_CHECK(TAG, item, return NULL);
    item->msg_id = message->msg_id;
    item->msg_type = message->msg_type;
    item->msg_qos = message->msg_qos;
    item->tick = tick;
    item->len =  message->len + message->remaining_len;
    item->pending = QUEUED;
    item->buffer = heap_caps_malloc(message->len + message->remaining_len, MQTT_OUTBOX_MEMORY);
    ESP_MEM_CHECK(TAG, item->buffer, {
        free(item);
        return NULL;
    });
    memcpy(item->buffer, message->data, message->len);
    if (message->remaining_data) {
        memcpy(item->buffer + message->len, message->remaining_data, message->remaining_len);
    }
    STAILQ_INSERT_TAIL(outbox->list, item, next);
    outbox->size += item->len;
    ESP_LOGD(TAG, "ENQUEUE msgid=%d, msg_type=%d, len=%d, size=%"PRIu64, message->msg_id, message->msg_type, message->len + message->remaining_len, outbox_get_size(outbox));
    return item;
}

outbox_item_handle_t outbox_get(outbox_handle_t outbox, int msg_id)
{
    outbox_item_handle_t item;
    STAILQ_FOREACH(item, outbox->list, next) {
        if (item->msg_id == msg_id) {
            return item;
        }
    }
    return NULL;
}

outbox_item_handle_t outbox_dequeue(outbox_handle_t outbox, pending_state_t pending, outbox_tick_t *tick)
{
    outbox_item_handle_t item;
    STAILQ_FOREACH(item, outbox->list, next) {
        if (item->pending == pending) {
            if (tick) {
                *tick = item->tick;
            }
            return item;
        }
    }
    return NULL;
}

esp_err_t outbox_delete_item(outbox_handle_t outbox, outbox_item_handle_t item_to_delete)
{
    outbox_item_handle_t item;
    STAILQ_FOREACH(item, outbox->list, next) {
        if (item == item_to_delete) {
            STAILQ_REMOVE(outbox->list, item, outbox_item, next);
            outbox->size -= item->len;
            free(item->buffer);
            free(item);
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

uint8_t *outbox_item_get_data(outbox_item_handle_t item,  size_t *len, uint16_t *msg_id, int *msg_type, int *qos)
{
    if (item) {
        *len = item->len;
        *msg_id = item->msg_id;
        *msg_type = item->msg_type;
        *qos = item->msg_qos;
        return (uint8_t *)item->buffer;
    }
    return NULL;
}

esp_err_t outbox_delete(outbox_handle_t outbox, int msg_id, int msg_type)
{
    outbox_item_handle_t item, tmp;
    STAILQ_FOREACH_SAFE(item, outbox->list, next, tmp) {
        if (item->msg_id == msg_id && (0xFF & (item->msg_type)) == msg_type) {
            STAILQ_REMOVE(outbox->list, item, outbox_item, next);
            outbox->size -= item->len;
            free(item->buffer);
            free(item);
            ESP_LOGD(TAG, "DELETED msgid=%d, msg_type=%d, remain size=%"PRIu64, msg_id, msg_type, outbox_get_size(outbox));
            return ESP_OK;
        }

    }
    return ESP_FAIL;
}

esp_err_t outbox_set_pending(outbox_handle_t outbox, int msg_id, pending_state_t pending)
{
    outbox_item_handle_t item = outbox_get(outbox, msg_id);
    if (item) {
        item->pending = pending;
        return ESP_OK;
    }
    return ESP_FAIL;
}

pending_state_t outbox_item_get_pending(outbox_item_handle_t item)
{
    if (item) {
        return item->pending;
    }
    return QUEUED;
}

esp_err_t outbox_set_tick(outbox_handle_t outbox, int msg_id, outbox_tick_t tick)
{
    outbox_item_handle_t item = outbox_get(outbox, msg_id);
    if (item) {
        item->tick = tick;
        return ESP_OK;
    }
    return ESP_FAIL;
}

int outbox_delete_single_expired(outbox_handle_t outbox, outbox_tick_t current_tick, outbox_tick_t timeout)
{
    int msg_id = -1;
    outbox_item_handle_t item;
    STAILQ_FOREACH(item, outbox->list, next) {
        if (current_tick - item->tick > timeout) {
            STAILQ_REMOVE(outbox->list, item, outbox_item, next);
            free(item->buffer);
            outbox->size -= item->len;
            msg_id = item->msg_id;
            free(item);
            return msg_id;
        }

    }
    return msg_id;
}

int outbox_delete_expired(outbox_handle_t outbox, outbox_tick_t current_tick, outbox_tick_t timeout)
{
    int deleted_items = 0;
    outbox_item_handle_t item, tmp;
    STAILQ_FOREACH_SAFE(item, outbox->list, next, tmp) {
        if (current_tick - item->tick > timeout) {
            STAILQ_REMOVE(outbox->list, item, outbox_item, next);
            free(item->buffer);
            outbox->size -= item->len;
            free(item);
            deleted_items ++;
        }

    }
    return deleted_items;
}

uint64_t outbox_get_size(outbox_handle_t outbox)
{
    return outbox->size;
}

void outbox_delete_all_items(outbox_handle_t outbox)
{
    outbox_item_handle_t item, tmp;
    STAILQ_FOREACH_SAFE(item, outbox->list, next, tmp) {
        STAILQ_REMOVE(outbox->list, item, outbox_item, next);
        outbox->size -= item->len;
        free(item->buffer);
        free(item);
    }
}
void outbox_destroy(outbox_handle_t outbox)
{
    outbox_delete_all_items(outbox);
    free(outbox->list);
    free(outbox);
}

#endif /* CONFIG_MQTT_CUSTOM_OUTBOX */
