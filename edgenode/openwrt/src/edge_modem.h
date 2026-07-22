#pragma once

#include <stdbool.h>

typedef struct {
    char imei[16];
    char iccid[23];
    int registration_status;
    int csq;
    int rssi_dbm;
    unsigned signal_percent;
    bool registered;
} edge_modem_info;

bool edge_modem_probe(const char *port, edge_modem_info *info);
bool edge_modem_read_status(const char *path, edge_modem_info *info, bool *available);
bool edge_modem_save_identity(const edge_modem_info *info);
bool edge_modem_write_status(const char *path, const edge_modem_info *info, bool available);
int edge_modem_initialize(const char *port, const char *status_path);
int edge_modem_monitor(const char *port, const char *status_path, unsigned interval_sec);
