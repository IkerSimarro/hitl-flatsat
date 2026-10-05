/*
** FlatSat plant model for software-in-the-loop: the physics between the node processes
**
** Integrates, at 1 kHz, everything that is physical on the FlatSat (hardware/BOM.md) and exchanges it
** with the nodes through the SIL harness (sil_harness.h):
**   - wheel motor: N20 gear motor (1000 rpm at 6 V) with a washer flywheel, DRV8833 driver, Hall encoder
**   - power: protected 18650 cell, TP4056 charger, node loads, the three INA219 rails (ICD 6.3)
** The parameters below are estimates from datasheets and typical values; the hardware-in-the-loop
** campaign (Phase 7) measures the real ones, and the differences go in the test report.
**
** Usage: flatsat_plant [--soc 0..1] [--usb]
*/
#define _DEFAULT_SOURCE

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sil_harness.h"

/* ---- Wheel motor ---- */
#define MOTOR_V_RATED       6.0    /* V */
#define MOTOR_RPM_RATED     1000.0 /* no-load output speed at the rated voltage */
#define MOTOR_R             16.0   /* ohm: about 0.37 A stall at 6 V */
#define MOTOR_TAU_DRIVEN    0.25   /* s: mechanical time constant with the flywheel */
#define MOTOR_TAU_COAST     2.0    /* s: spin-down when not driven */
#define MOTOR_I_STATIC      0.030  /* A: no-load current, static part (brushes, gearbox) */
#define MOTOR_I_VISCOUS     0.030  /* A: no-load current added at rated speed (N20 datasheets: ~60 mA total) */
#define ENCODER_CPR         840    /* 7 pulses x 30:1 gearbox x 4 quadrature edges per output rev */

/* ---- Power ---- */
#define BATT_CAPACITY_AH    2.5   /* protected 18650, derated */
#define BATT_R_INT          0.08  /* ohm */
#define DIODE_DROP          0.30  /* V: Schottky into each node's VSYS (BOM item 13) */
#define LOAD_OBC_A          0.055 /* Pico 2 + CAN board + OLED + idle LoRa */
#define LOAD_ADCS_A         0.040 /* Pico 2 + CAN board */
#define LOAD_ADCS_RESET_A   0.012 /* Pico held in reset */
#define LOAD_DRIVER_A       0.002 /* DRV8833 awake */
#define LOAD_EPS_A          0.045 /* Pico 2 + CAN board + three INA219 */
#define CHARGE_CC_A         0.50  /* TP4056 programmed current */

#define STEP_US 1000

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

/* Open-circuit voltage of a Li-ion cell against state of charge (piecewise linear) */
static double ocv(double soc)
{
    static const double s[] = {0.0, 0.05, 0.1, 0.2, 0.4, 0.6, 0.8, 0.9, 1.0};
    static const double v[] = {3.00, 3.30, 3.45, 3.60, 3.70, 3.80, 3.95, 4.05, 4.20};
    int                 i;

    if (soc <= 0.0)
    {
        return v[0];
    }
    for (i = 1; i < (int)(sizeof(s) / sizeof(s[0])); i++)
    {
        if (soc <= s[i])
        {
            return v[i - 1] + (v[i] - v[i - 1]) * (soc - s[i - 1]) / (s[i] - s[i - 1]);
        }
    }
    return v[8];
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    sil_harness_t *h = sil_harness();
    double         soc = 0.8;
    double         rpm = 0.0;   /* wheel output speed */
    double         revs = 0.0;  /* wheel output angle */
    double         t_prev;
    int            i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--soc") == 0 && i + 1 < argc)
        {
            soc = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "--usb") == 0)
        {
            h->usb_power = 1;
        }
    }
    if (h == NULL)
    {
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("flatsat_plant: running, battery %.0f %%, USB power %s\n", soc * 100, h->usb_power ? "on" : "off");

    t_prev = now_s();
    while (running)
    {
        double t  = now_s();
        double dt = t - t_prev;
        double vbat_prev = h->battery_v;
        double duty;
        double rpm_nl;
        double motor_a;
        double supply_motor_a;
        double obc_a;
        double adcs_a;
        double load_a;
        double charge_a = 0.0;
        double batt_a;
        int    driven;

        t_prev = t;
        if (dt <= 0.0 || dt > 0.1)
        {
            dt = STEP_US / 1e6; /* first step, or the process was paused */
        }

        /* ---- Wheel motor ---- */
        /* Driver inputs float low while the ADCS node is in reset; nSLEEP low disables the driver */
        duty   = (h->adcs_run && h->motor_nsleep) ? h->motor_duty : 0.0;
        duty   = duty > 1.0 ? 1.0 : (duty < -1.0 ? -1.0 : duty);
        driven = fabs(duty) > 1e-3;
        rpm_nl = MOTOR_RPM_RATED * vbat_prev / MOTOR_V_RATED;
        if (driven)
        {
            rpm += (duty * rpm_nl - rpm) * dt / MOTOR_TAU_DRIVEN;
        }
        else
        {
            rpm -= rpm * dt / MOTOR_TAU_COAST;
        }
        revs += rpm / 60.0 * dt;

        /* Winding current from the back-EMF difference; the supply sees it scaled by the PWM duty */
        if (driven)
        {
            double back_emf = MOTOR_V_RATED * rpm / MOTOR_RPM_RATED;
            motor_a         = fabs(duty * vbat_prev - back_emf) / MOTOR_R +
                              (fabs(rpm) > 1.0 ? MOTOR_I_STATIC : 0.0) + MOTOR_I_VISCOUS * fabs(rpm) / MOTOR_RPM_RATED;
            supply_motor_a  = motor_a * fabs(duty);
        }
        else
        {
            motor_a        = 0.0;
            supply_motor_a = 0.0;
        }

        /* ---- Loads and rails ---- */
        obc_a  = LOAD_OBC_A + h->radio_ma / 1000.0;
        adcs_a = (h->adcs_run ? LOAD_ADCS_A : LOAD_ADCS_RESET_A) + (h->motor_nsleep ? LOAD_DRIVER_A : 0.0) +
                 supply_motor_a;
        load_a = obc_a + adcs_a + LOAD_EPS_A;

        /* ---- Charger and battery ---- */
        if (h->usb_power && soc < 1.0)
        {
            /* Constant current, tapering over the last 5 % (constant-voltage phase) */
            charge_a = soc < 0.95 ? CHARGE_CC_A : CHARGE_CC_A * (1.0 - soc) / 0.05;
        }
        batt_a = load_a - charge_a; /* positive = discharging */
        soc -= batt_a * dt / 3600.0 / BATT_CAPACITY_AH;
        soc = soc < 0.0 ? 0.0 : (soc > 1.0 ? 1.0 : soc);

        h->battery_v     = (float)(ocv(soc) - batt_a * BATT_R_INT);
        h->battery_soc   = (float)soc;
        h->charger_chrg  = (uint8_t)(h->usb_power && charge_a > 0.05);
        h->charger_stdby = (uint8_t)(h->usb_power && charge_a <= 0.05);

        h->wheel_rpm     = (float)rpm;
        h->encoder_count = (int32_t)floor(revs * ENCODER_CPR);
        h->motor_ma      = (float)(motor_a * 1000.0);
        h->rail_v[0]     = h->battery_v;
        h->rail_ma[0]    = (float)(load_a * 1000.0);
        h->rail_v[1]     = (float)(h->battery_v - DIODE_DROP);
        h->rail_ma[1]    = (float)(obc_a * 1000.0);
        h->rail_v[2]     = (float)(h->battery_v - DIODE_DROP);
        h->rail_ma[2]    = (float)(adcs_a * 1000.0);
        h->plant_steps++;

        usleep(STEP_US);
    }
    return 0;
}
