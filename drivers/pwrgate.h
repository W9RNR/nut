#ifndef PWRGATE_H
#define PWRGATE_H

typedef struct {
    char   mode[32];      /* Charging, Discharging, etc. */
    double ps_voltage;    /* Power Supply */
    double bat_voltage;   /* Battery */
    double bat_current;   /* Amps */
    double sol_voltage;   /* Solar */
    double temp;          /* Internal Temp */
    int    min_single;    /* Minutes on battery */
    double min_target;
} parsed_t;

#endif
