#include "power.h"
#include "i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ─── AXP202 Register Map ─────────────────────────────────────────────────────
#define AXP202_ADDR             0x35
#define AXP202_PWR_OUT_CTRL     0x12   // Power output control register

// ─── Bit positions in AXP202_PWR_OUT_CTRL ────────────────────────────────────
// Bit 0: EXTEN
// Bit 1: DC3  (powers ESP32 — never disable this!)
// Bit 2: LDO2 (backlight)
// Bit 3: LDO3 (audio / backplane depending on board revision)
// Bit 4: DC2
// Bit 6: LDO4
#define AXP202_LDO2_BIT         2      // Backlight
#define AXP202_LDO3_BIT         3      // Audio / backplane power

// ─── Low-level helpers ───────────────────────────────────────────────────────

i2c_status_t axp202_read_reg(uint8_t reg, uint8_t *data)
{
    return i2c_read(AXP202_ADDR, reg, data, 1);
}

i2c_status_t axp202_write_reg(uint8_t reg, uint8_t data)
{
    return i2c_write(AXP202_ADDR, reg, &data, 1);
}

// ─── Generic bit-set helper with verify ──────────────────────────────────────
// Sets a single bit in a register and confirms the write stuck.
// This is the reusable pattern for all AXP202 rail enables.

static i2c_status_t axp202_set_bit(uint8_t reg, uint8_t bit, const char *label)
{
    i2c_status_t err;
    uint8_t current = 0;
    uint8_t verify  = 0;

    // Step 1: Read current register state
    err = axp202_read_reg(reg, &current);
    if (err != ESP_OK) {
        printf("[AXP202] %s: read failed (err=%d)\n", label, err);
        return err;
    }
    printf("[AXP202] %s: reg=0x%02X before=0x%02X\n", label, reg, current);

    // Step 2: Check if already set — skip unnecessary write
    if (current & (1 << bit)) {
        printf("[AXP202] %s: already enabled, skipping write\n", label);
        return ESP_OK;
    }

    // Step 3: Set bit and write back
    uint8_t new_val = current | (1 << bit);
    err = axp202_write_reg(reg, new_val);
    if (err != ESP_OK) {
        printf("[AXP202] %s: write failed (err=%d)\n", label, err);
        return err;
    }

    // Step 4: Read back to verify write actually stuck
    err = axp202_read_reg(reg, &verify);
    if (err != ESP_OK) {
        printf("[AXP202] %s: verify read failed (err=%d)\n", label, err);
        return err;
    }
    printf("[AXP202] %s: reg=0x%02X after=0x%02X\n", label, reg, verify);

    // Step 5: Confirm the bit is now set
    if (verify & (1 << bit)) {
        printf("[AXP202] %s: ENABLED successfully\n", label);
        return ESP_OK;
    }

    printf("[AXP202] %s: FAILED — bit did not stick after write\n", label);
    return ESP_FAIL;
}

// ─── Rail Enable Functions ───────────────────────────────────────────────────
// Separated per rail so you can enable/disable them independently later.
// BMA423 is on the always-on rail (DC3/VCC) — no explicit enable needed.
// These are here for completeness and future phases (display, audio).

i2c_status_t axp202_enable_ldo2(void)   // Backlight
{
    return axp202_set_bit(AXP202_PWR_OUT_CTRL, AXP202_LDO2_BIT, "LDO2(backlight)");
}

i2c_status_t axp202_enable_ldo3(void)   // Audio / backplane
{
    return axp202_set_bit(AXP202_PWR_OUT_CTRL, AXP202_LDO3_BIT, "LDO3(audio)");
}

// ─── I2C Bus Scanner ─────────────────────────────────────────────────────────
// Run this during debug to confirm which devices are actually responding.
// Remove from production build once hardware is confirmed working.

void i2c_scan(void)
{
    printf("\n[I2C SCAN] Scanning bus...\n");
    printf("     00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\n");

    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        uint8_t dummy = 0;
        i2c_status_t err = i2c_read(addr, 0x00, &dummy, 1);

        if (addr % 16 == 0) {
            printf("0x%02X:", addr & 0xF0);
        }

        if (err == ESP_OK) {
            printf(" %02X", addr);
            found++;
        } else {
            printf(" --");
        }

        if ((addr + 1) % 16 == 0) {
            printf("\n");
        }
    }

    printf("\n[I2C SCAN] Found %d device(s)\n", found);
    printf("[I2C SCAN] Expected: AXP202=0x35, BMA423=0x18 or 0x19, PCF8563=0x51, FT6236=0x38\n\n");
}

// ─── Power Init Sequence ──────────────────────────────────────────────────────
// Phase 1 scope: BMA423 is always-on, so this sequence is minimal.
// LDO2/LDO3 enables are included as stubs for future phases.

i2c_status_t power_init(void)
{
    i2c_status_t err;

    printf("\n[POWER] Starting AXP202 power init\n");

    // ── Diagnostic: dump current power register state ────────────────────────
    uint8_t pwr_reg = 0;
    err = axp202_read_reg(AXP202_PWR_OUT_CTRL, &pwr_reg);
    if (err != ESP_OK) {
        printf("[POWER] FATAL: Cannot read AXP202 power register\n");
        return err;
    }
    printf("[POWER] AXP202 reg 0x12 = 0x%02X\n", pwr_reg);
    printf("[POWER]   DC3  (ESP32)     : %s\n", (pwr_reg & (1 << 1)) ? "ON" : "OFF");
    printf("[POWER]   LDO2 (backlight) : %s\n", (pwr_reg & (1 << 2)) ? "ON" : "OFF");
    printf("[POWER]   LDO3 (audio)     : %s\n", (pwr_reg & (1 << 3)) ? "ON" : "OFF");

    // ── BMA423 is on always-on rail — no explicit enable needed ──────────────
    // DC3 powers ESP32 and the sensor VCC together.
    // Attempting to disable DC3 would kill the MCU itself.
    printf("[POWER] BMA423 is on always-on rail (DC3) — no enable needed\n");

    // ── Stabilization delay ───────────────────────────────────────────────────
    // 50ms gives the BMA423 time to complete its internal NVM load after power.
    // Datasheet specifies ~2ms minimum but 50ms is safe margin.
    vTaskDelay(pdMS_TO_TICKS(50));

    printf("[POWER] Power init complete\n\n");
    return ESP_OK;
}