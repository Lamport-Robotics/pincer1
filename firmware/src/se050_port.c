/* Pincer1 - RP2350/RP2040 platform glue for NXP Plug & Trust (third_party/plug-and-trust, git submodule).
 * I2C0 on GP4/GP5 is the same on the Pico and the Pico 2.
 *
 * Built against the Plug & Trust middleware submodule.
 *
 * Provides the three platform contracts the T1oI2C transport needs:
 *   i2c_a7.h    axI2CInit / axI2CTerm / axI2CWrite / axI2CRead   -> hardware_i2c on I2C0 (GP4 SDA, GP5 SCL)
 *   sm_timer.h  sm_initSleep / sm_sleep / sm_usleep              -> pico sleep_ms / sleep_us
 *   nxLog.h     nLog / nLog_au8 / nLog_Init / nLog_DeInit        -> silenced (the CDC link carries the
 *                                                                   protocol, and logs could leak key data)
 *
 * Notes on the I2C contract (from hostlib/hostLib/platform/linux/i2c_a7.c and phNxpEsePal_i2c.c):
 *   - `addr` arrives in 8-bit form (SMCOM_I2C_ADDRESS = 0x90 -> 7-bit 0x48).
 *   - A NACK must be reported as I2C_NACK_ON_ADDRESS: the SE050 NACKs while it is busy/asleep and the
 *     PAL polls with a back-off (phPalEse_i2c_is_retryable_error). Returning I2C_FAILED is also retried
 *     because T1OI2C_RETRY_ON_I2C_FAILED is defined.
 *   - Reads/writes are single transactions with a STOP (no repeated start), as in the Linux port. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "i2c_a7.h"
#include "nxLog.h"
#include "pico/stdlib.h"
#include "sm_timer.h"

#define SE050_I2C i2c0
#define SE050_PIN_SDA 4
#define SE050_PIN_SCL 5
#define SE050_I2C_BAUD 400000u
#define SE050_I2C_TIMEOUT_US 20000u /* per transaction; the PAL adds its own retries */

static int g_i2c_ctx; /* opaque handle the transport passes back to us */
static bool g_i2c_ready;

i2c_error_t axI2CInit(void **conn_ctx, const char *pDevName)
{
    (void)pDevName; /* connection string unused: the bus is fixed by the wiring */
    if (!g_i2c_ready) {
        i2c_init(SE050_I2C, SE050_I2C_BAUD);
        gpio_set_function(SE050_PIN_SDA, GPIO_FUNC_I2C);
        gpio_set_function(SE050_PIN_SCL, GPIO_FUNC_I2C);
        /* Weak internal pull-ups as a fallback; the board should carry 2.2k-4.7k externals. */
        gpio_pull_up(SE050_PIN_SDA);
        gpio_pull_up(SE050_PIN_SCL);
        g_i2c_ready = true;
    }
    if (conn_ctx) *conn_ctx = &g_i2c_ctx;
    return I2C_OK;
}

void axI2CTerm(void *conn_ctx, int mode)
{
    (void)conn_ctx;
    (void)mode;
    /* Keep the bus initialised: the SE050 is the only device on it and we reopen on retry. */
}

static i2c_error_t map_result(int rc, unsigned short expected)
{
    if (rc == (int)expected) return I2C_OK;
    if (rc == PICO_ERROR_GENERIC) return I2C_NACK_ON_ADDRESS; /* no ACK: SE busy -> PAL retries */
    if (rc == PICO_ERROR_TIMEOUT) return I2C_FAILED;
    return I2C_FAILED;
}

i2c_error_t axI2CWrite(void *conn_ctx, unsigned char bus, unsigned char addr, unsigned char *pTx, unsigned short txLen)
{
    int rc;
    (void)conn_ctx;
    (void)bus;
    if (!pTx || txLen == 0 || txLen > MAX_DATA_LEN) return I2C_FAILED;
    rc = i2c_write_timeout_us(SE050_I2C, (uint8_t)(addr >> 1), pTx, txLen, false, SE050_I2C_TIMEOUT_US);
    return map_result(rc, txLen);
}

i2c_error_t axI2CRead(void *conn_ctx, unsigned char bus, unsigned char addr, unsigned char *pRx, unsigned short rxLen)
{
    int rc;
    (void)conn_ctx;
    (void)bus;
    if (!pRx || rxLen == 0 || rxLen > MAX_DATA_LEN) return I2C_FAILED;
    rc = i2c_read_timeout_us(SE050_I2C, (uint8_t)(addr >> 1), pRx, rxLen, false, SE050_I2C_TIMEOUT_US);
    return map_result(rc, rxLen);
}

/* ---- sm_timer.h ---- */
uint32_t sm_initSleep(void) { return 0; }
void sm_sleep(uint32_t msec) { sleep_ms(msec); }
void sm_usleep(uint32_t microsec) { sleep_us(microsec); }

/* ---- nxLog.h: deliberately silent ---- */
uint8_t nLog_Init(void) { return 0; }
void nLog_DeInit(void) {}
void nLog(const char *comp, int level, const char *format, ...)
{
    (void)comp; (void)level; (void)format;
}
void nLog_au8(const char *comp, int level, const char *message, const unsigned char *array, size_t array_len)
{
    (void)comp; (void)level; (void)message; (void)array; (void)array_len;
}
