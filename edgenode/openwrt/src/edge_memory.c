#include "edge_memory.h"

#include <stdlib.h>
#include <string.h>

#include "edge_sha256.h"

void edge_memory_config_free(edge_memory_config_set *set) {
    if (set == NULL)
        return;
    if (set->items != NULL) {
        for (uint32_t index = 0; index < set->item_count; ++index)
            free(set->items[index].payload);
        free(set->items);
    }
    memset(set, 0, sizeof(*set));
}

bool edge_memory_config_begin(edge_memory_config_set *staging, uint64_t revision,
                              uint32_t item_count, const uint8_t digest[32]) {
    if (staging == NULL || digest == NULL || revision == 0U ||
        item_count > EDGE_MAX_CONFIG_ITEMS)
        return false;
    edge_memory_config_free(staging);
    if (item_count != 0U) {
        staging->items = calloc(item_count, sizeof(*staging->items));
        if (staging->items == NULL)
            return false;
    }
    staging->revision = revision;
    staging->item_count = item_count;
    memcpy(staging->digest, digest, 32U);
    return true;
}

bool edge_memory_config_put(edge_memory_config_set *staging, uint64_t revision,
                            uint32_t index, const uint8_t digest[32],
                            const uint8_t *payload, size_t payload_size) {
    if (staging == NULL || digest == NULL || payload == NULL || payload_size == 0U ||
        revision != staging->revision || index >= staging->item_count)
        return false;
    edge_memory_config_item *item = &staging->items[index];
    uint8_t *copy = malloc(payload_size);
    if (copy == NULL)
        return false;
    memcpy(copy, payload, payload_size);
    free(item->payload);
    item->payload = copy;
    item->payload_size = payload_size;
    memcpy(item->digest, digest, 32U);
    if (!item->present) {
        item->present = true;
        ++staging->received_count;
    }
    return true;
}

bool edge_memory_config_commit(edge_memory_config_set *active,
                               edge_memory_config_set *staging,
                               uint64_t revision, const uint8_t digest[32]) {
    if (active == NULL || staging == NULL || digest == NULL ||
        staging->revision != revision || staging->received_count != staging->item_count ||
        memcmp(staging->digest, digest, 32U) != 0)
        return false;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool ok = edge_sha256_starts(&sha, 0) == 0;
    for (uint32_t index = 0; ok && index < staging->item_count; ++index)
        ok = staging->items[index].present &&
             edge_sha256_update(&sha, staging->items[index].digest, 32U) == 0;
    uint8_t actual[32];
    ok = ok && edge_sha256_finish(&sha, actual) == 0 &&
         memcmp(actual, digest, 32U) == 0;
    mbedtls_sha256_free(&sha);
    if (!ok)
        return false;
    edge_memory_config_free(active);
    *active = *staging;
    memset(staging, 0, sizeof(*staging));
    return true;
}

void edge_memory_outbox_init(edge_memory_outbox *outbox, size_t maximum_bytes) {
    if (outbox == NULL)
        return;
    memset(outbox, 0, sizeof(*outbox));
    outbox->maximum_bytes = maximum_bytes;
}

void edge_memory_outbox_free(edge_memory_outbox *outbox) {
    if (outbox == NULL)
        return;
    edge_memory_message *current = outbox->head;
    while (current != NULL) {
        edge_memory_message *next = current->next;
        free(current);
        current = next;
    }
    const size_t maximum = outbox->maximum_bytes;
    memset(outbox, 0, sizeof(*outbox));
    outbox->maximum_bytes = maximum;
}

bool edge_memory_outbox_put(edge_memory_outbox *outbox, const uint8_t message_id[16],
                            const uint8_t *payload, size_t payload_size) {
    if (outbox == NULL || message_id == NULL || payload == NULL || payload_size == 0U ||
        payload_size > outbox->maximum_bytes || outbox->bytes > outbox->maximum_bytes - payload_size)
        return false;
    for (edge_memory_message *item = outbox->head; item != NULL; item = item->next)
        if (memcmp(item->message_id, message_id, 16U) == 0)
            return true;
    edge_memory_message *item = malloc(sizeof(*item) + payload_size);
    if (item == NULL)
        return false;
    item->next = NULL;
    memcpy(item->message_id, message_id, 16U);
    item->payload_size = payload_size;
    memcpy(item->payload, payload, payload_size);
    if (outbox->tail != NULL)
        outbox->tail->next = item;
    else
        outbox->head = item;
    outbox->tail = item;
    outbox->bytes += payload_size;
    ++outbox->count;
    return true;
}

const edge_memory_message *edge_memory_outbox_first(const edge_memory_outbox *outbox) {
    return outbox != NULL ? outbox->head : NULL;
}

bool edge_memory_outbox_ack(edge_memory_outbox *outbox, const uint8_t message_id[16]) {
    if (outbox == NULL || message_id == NULL)
        return false;
    edge_memory_message *previous = NULL;
    edge_memory_message *current = outbox->head;
    while (current != NULL && memcmp(current->message_id, message_id, 16U) != 0) {
        previous = current;
        current = current->next;
    }
    if (current == NULL)
        return false;
    if (previous != NULL)
        previous->next = current->next;
    else
        outbox->head = current->next;
    if (outbox->tail == current)
        outbox->tail = previous;
    outbox->bytes -= current->payload_size;
    --outbox->count;
    free(current);
    return true;
}
