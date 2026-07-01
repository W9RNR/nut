/* drivers/pwrgate.c
 *
 * NUT driver for West Mountain Radio Epic PwrGate (CDC-ACM)
 * Optimized for stability and LiFePO4 battery chemistry.
 */

#include "main.h"
#include "dstate.h"
#include "serial.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include "pwrgate.h"

#define DRIVER_NAME      "WMR PwrGate ASCII Driver"
#define DRIVER_VERSION   "0.3.0"

upsdrv_info_t upsdrv_info = {
    DRIVER_NAME,
    DRIVER_VERSION,
    "Rod Repp <rod@repp.com>",
    DRV_EXPERIMENTAL,
    { NULL, NULL }
};

int fd = -1;
double lb_threshold = 11.5; 
extern char *device_path;

/* --- Utilities --- */

static const char* skip_spaces(const char *s) {
    while (s && *s && isspace((unsigned char)*s)) s++;
    return s;
}

static int scan_val_after_tag(const char *line, const char *tag, double *out) {
    const char *p = strstr(line, tag);
    if (!p) return 0;
    p += strlen(tag);
    p = skip_spaces(p);
    char buf[32] = {0};
    size_t i = 0;
    while (*p && i < sizeof(buf)-1 && (isdigit(*p) || *p=='+' || *p=='-' || *p=='.')) {
        buf[i++] = *p++;
    }
    if (!i) return 0;
    *out = strtod(buf, NULL);
    return 1;
}

/* --- Driver Hooks --- */

void upsdrv_makevartable(void) {
    addvar(VAR_VALUE, "port", "Device path (e.g., /dev/ttyACM0)");
    addvar(VAR_VALUE, "lowbatt", "Low battery threshold (Default 11.5 for LiFePO4)");
}

void upsdrv_initinfo(void) {
    const char *v;
    dstate_setinfo("device.mfr", "West Mountain Radio"); 
    dstate_setinfo("device.model", "Epic PwrGate");
    dstate_setinfo("driver.version", DRIVER_VERSION);
    
    if ((v = getval("lowbatt"))) lb_threshold = atof(v);
}

void upsdrv_initups(void) {
    fd = ser_open(device_path);
    if (fd < 0) fatal_with_errno(EXIT_FAILURE, "Cannot open %s", device_path);

    /* 1. Setup Terminal Attributes */
    struct termios tty;
    if (tcgetattr(fd, &tty) == 0) {
        cfmakeraw(&tty);
        tty.c_cflag |= (CLOCAL | CREAD);
        tty.c_cflag &= ~CRTSCTS; // No Hardware Flow Control
        tty.c_iflag &= ~(IXON | IXOFF | IXANY); // No Software Flow Control
        tcsetattr(fd, TCSANOW, &tty);
    }
    ser_set_speed(fd, device_path, B115200);

    /* 2. Clear buffers and "Wake Up" the stream */
    upslogx(LOG_INFO, "PwrGate: Sending wakeup/escape sequence...");
    tcflush(fd, TCIOFLUSH);
    
    for (int i = 0; i < 4; i++) {
        ser_send_pace(fd, 0, "\r");
        usleep(250000); 
    }
    tcflush(fd, TCIFLUSH);
}

void upsdrv_updateinfo(void) {
    char raw[512];
    static parsed_t last_p;
    int got;

    /* 1. Flush the backlog so we don't get "drifting" stale data */
    tcflush(fd, TCIFLUSH);

    /* 2. Fetch the FRESHEST line */
    /* Use \n as the delimiter to handle the \r\n pair properly */
    got = ser_get_line(fd, raw, sizeof(raw) - 1, '\n', "", 2, 0);

    if (got <= 0) {
        dstate_datastale();
        return;
    }
    raw[got] = '\0';
    upslogx(LOG_DEBUG, "FRESH RAW: [%s]", raw);

    /* 3. Robust Tag-Based Parsing (Ignores position/drifting) */
    
    // Power Supply Voltage
    char *ps_ptr = strstr(raw, "PS=");
    if (ps_ptr) last_p.ps_voltage = strtod(ps_ptr + 3, NULL);

    // Battery Voltage
    char *bat_ptr = strstr(raw, "Bat=");
    if (bat_ptr) last_p.bat_voltage = strtod(bat_ptr + 4, NULL);

    // Battery Current (usually follows the comma after Bat voltage)
    char *c_ptr = strchr(raw, ','); 
    if (c_ptr) {
        last_p.bat_current = strtod(c_ptr + 1, NULL);
    } else {
        // Fallback: look for 'A' suffix
        char *a_ptr = strstr(raw, "A");
        if (a_ptr) {
             /* Walk back to find the start of the number if needed */
        }
    }

    // Solar Voltage
    char *sol_ptr = strstr(raw, "Sol=");
    if (sol_ptr) last_p.sol_voltage = strtod(sol_ptr + 4, NULL);

    // Minutes
    char *min_ptr = strstr(raw, "Min=");
    if (min_ptr) {
        int m1 = 0, m2 = 0;
        if (sscanf(min_ptr, "Min=%d/%d", &m1, &m2) >= 1) {
            dstate_setinfo("battery.runtime", "%d", m1 * 60);
        }
    }
    /* 4. Status Mapping */

    const char *status = (last_p.ps_voltage > 1.0) ? "OL" : "OB";



    if (strstr(raw, "Charging") || strstr(raw, "MPPT")) {

        status = (last_p.ps_voltage > 1.0) ? "OL CHRG" : "OB CHRG";

    } else if (strstr(raw, "Charged") || strstr(raw, "Trickle")) {

        status = "OL";

    } else if (strstr(raw, "No Bat") || strstr(raw, "Bad Bat") || strstr(raw, "Bad temp")) {

        status = "OFF FNT";

    }



    if (last_p.bat_voltage <= lb_threshold && (last_p.ps_voltage < 1.0)) {

        dstate_setinfo("ups.status", "OB LB");

    } else {

        dstate_setinfo("ups.status", "%s", status);

    }



    /* 5. LiFePO4 Voltage Lookup (0-100%) */

    double v = last_p.bat_voltage;

    double charge = 0.0;

    if (v >= 13.6)      charge = 100.0;

    else if (v >= 13.3) charge = 90.0;

    else if (v >= 13.2) charge = 70.0;

    else if (v >= 13.1) charge = 40.0;

    else if (v >= 13.0) charge = 20.0;

    else if (v >= 12.0) charge = 10.0;

    else                charge = 0.0;



    /* 6. Update NUT */

    dstate_setinfo("input.voltage", "%.2f", last_p.ps_voltage);

    dstate_setinfo("battery.voltage", "%.2f", last_p.bat_voltage);

    dstate_setinfo("battery.current", "%.2f", last_p.bat_current);

    dstate_setinfo("battery.charge", "%.0f", charge);

    dstate_setinfo("ambient.solar.voltage", "%.2f", last_p.sol_voltage);
    // Always signal data OK if we got this far
    dstate_dataok();
}
 
void upsdrv_cleanup(void) {

    if (fd >= 0) ser_close(fd, device_path);

}

void upsdrv_shutdown(void) { }
void upsdrv_tweak_prognames(void) { }
void upsdrv_help(void) { printf("%s %s\n", DRIVER_NAME, DRIVER_VERSION); }
