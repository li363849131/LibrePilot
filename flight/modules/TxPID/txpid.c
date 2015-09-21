/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup TxPIDModule TxPID Module
 * @brief      Optional module to tune PID settings using R/C transmitter.
 * Updates PID settings in RAM in real-time using configured Accessory channels as controllers.
 * @{
 *
 * @file       txpid.c
 * @author     The LibrePilot Project, http://www.librepilot.org Copyright (C) 2015.
 *             The OpenPilot Team, http://www.openpilot.org Copyright (C) 2011.
 * @brief      Optional module to tune PID settings using R/C transmitter.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Output object: StabilizationSettings
 *
 * This module will periodically update values of stabilization PID settings
 * depending on configured input control channels. New values of stabilization
 * settings are not saved to flash, but updated in RAM. It is expected that the
 * module will be enabled only for tuning. When desired values are found, they
 * can be read via GCS and saved permanently. Then this module should be
 * disabled again.
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://wiki.openpilot.org/display/Doc/OpenPilot+Architecture
 *
 */

#include "openpilot.h"
#include "txpidsettings.h"
#include "accessorydesired.h"
#include "manualcontrolcommand.h"
#include "stabilizationsettings.h"
#include "attitudesettings.h"
#ifdef REVOLUTION
#include "altitudeholdsettings.h"
#endif
#include "stabilizationbank.h"
#include "stabilizationsettingsbank1.h"
#include "stabilizationsettingsbank2.h"
#include "stabilizationsettingsbank3.h"
#include "flightstatus.h"
#include "txpidstatus.h"
#include "hwsettings.h"

//
// Configuration
//
#define SAMPLE_PERIOD_MS           200
#define TELEMETRY_UPDATE_PERIOD_MS 0 // 0 = update on change (default)

// Sanity checks
#if (TXPIDSETTINGS_PIDS_NUMELEM != TXPIDSETTINGS_INPUTS_NUMELEM) || \
    (TXPIDSETTINGS_PIDS_NUMELEM != TXPIDSETTINGS_MINPID_NUMELEM) || \
    (TXPIDSETTINGS_PIDS_NUMELEM != TXPIDSETTINGS_MAXPID_NUMELEM)
#error Invalid TxPID UAVObject definition (inconsistent number of field elements)
#endif

// Private types

// Private variables

// Private functions
static void updatePIDs(UAVObjEvent *ev);
static uint8_t update(float *var, float val);
static uint8_t updateUint16(uint16_t *var, float val);
static uint8_t updateUint8(uint8_t *var, float val);
static uint8_t updateInt8(int8_t *var, float val);
static float scale(float val, float inMin, float inMax, float outMin, float outMax);

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t TxPIDInitialize(void)
{
    bool txPIDEnabled;
    HwSettingsOptionalModulesData optionalModules;

#ifdef REVOLUTION
    AltitudeHoldSettingsInitialize();
#endif

    HwSettingsInitialize();
    HwSettingsOptionalModulesGet(&optionalModules);

    if (optionalModules.TxPID == HWSETTINGS_OPTIONALMODULES_ENABLED) {
        txPIDEnabled = true;
    } else {
        txPIDEnabled = false;
    }

    if (txPIDEnabled) {
        TxPIDSettingsInitialize();
        TxPIDStatusInitialize();
        AccessoryDesiredInitialize();

        UAVObjEvent ev = {
            .obj    = AccessoryDesiredHandle(),
            .instId = 0,
            .event  = 0,
            .lowPriority = false,
        };
        EventPeriodicCallbackCreate(&ev, updatePIDs, SAMPLE_PERIOD_MS / portTICK_RATE_MS);

#if (TELEMETRY_UPDATE_PERIOD_MS != 0)
        // Change StabilizationSettings update rate from OnChange to periodic
        // to prevent telemetry link flooding with frequent updates in case of
        // control channel jitter.
        // Warning: saving to flash with this code active will change the
        // StabilizationSettings update rate permanently. Use Metadata via
        // browser to reset to defaults (telemetryAcked=true, OnChange).
        UAVObjMetadata metadata;
        StabilizationSettingsInitialize();
        StabilizationSettingsGetMetadata(&metadata);
        metadata.telemetryAcked = 0;
        metadata.telemetryUpdateMode   = UPDATEMODE_PERIODIC;
        metadata.telemetryUpdatePeriod = TELEMETRY_UPDATE_PERIOD_MS;
        StabilizationSettingsSetMetadata(&metadata);

        AttitudeSettingsInitialize();
        AttitudeSettingsGetMetadata(&metadata);
        metadata.telemetryAcked = 0;
        metadata.telemetryUpdateMode   = UPDATEMODE_PERIODIC;
        metadata.telemetryUpdatePeriod = TELEMETRY_UPDATE_PERIOD_MS;
        AttitudeSettingsSetMetadata(&metadata);
#endif /* if (TELEMETRY_UPDATE_PERIOD_MS != 0) */

        return 0;
    }

    return -1;
}

/* stub: module has no module thread */
int32_t TxPIDStart(void)
{
    return 0;
}

MODULE_INITCALL(TxPIDInitialize, TxPIDStart);

/**
 * Update PIDs callback function
 */
static void updatePIDs(UAVObjEvent *ev)
{
    if (ev->obj != AccessoryDesiredHandle()) {
        return;
    }

    TxPIDSettingsData inst;
    TxPIDSettingsGet(&inst);

    if (inst.UpdateMode == TXPIDSETTINGS_UPDATEMODE_NEVER) {
        return;
    }

    uint8_t armed;
    FlightStatusArmedGet(&armed);
    if ((inst.UpdateMode == TXPIDSETTINGS_UPDATEMODE_WHENARMED) &&
        (armed == FLIGHTSTATUS_ARMED_DISARMED)) {
        return;
    }

    StabilizationBankData bank;
    switch (inst.BankNumber) {
    case 0:
        StabilizationSettingsBank1Get((StabilizationSettingsBank1Data *)&bank);
        break;

    case 1:
        StabilizationSettingsBank2Get((StabilizationSettingsBank2Data *)&bank);
        break;

    case 2:
        StabilizationSettingsBank3Get((StabilizationSettingsBank3Data *)&bank);
        break;

    default:
        return;
    }
    StabilizationSettingsData stab;
    StabilizationSettingsGet(&stab);

    AttitudeSettingsData att;
    AttitudeSettingsGet(&att);

#ifdef REVOLUTION
    AltitudeHoldSettingsData altitude;
    AltitudeHoldSettingsGet(&altitude);
#endif
    AccessoryDesiredData accessory;

    TxPIDStatusData txpid_status;
    TxPIDStatusGet(&txpid_status);

    bool easyTuneEnabled        = false;

    uint8_t needsUpdateBank     = 0;
    uint8_t needsUpdateStab     = 0;
    uint8_t needsUpdateAtt      = 0;
#ifdef REVOLUTION
    uint8_t needsUpdateAltitude = 0;
#endif

    // Loop through every enabled instance
    for (uint8_t i = 0; i < TXPIDSETTINGS_PIDS_NUMELEM; i++) {
        if (TxPIDSettingsPIDsToArray(inst.PIDs)[i] != TXPIDSETTINGS_PIDS_DISABLED) {
            float value;
            if (TxPIDSettingsInputsToArray(inst.Inputs)[i] == TXPIDSETTINGS_INPUTS_THROTTLE) {
                ManualControlCommandThrottleGet(&value);
                value = scale(value,
                              inst.ThrottleRange.Min,
                              inst.ThrottleRange.Max,
                              TxPIDSettingsMinPIDToArray(inst.MinPID)[i],
                              TxPIDSettingsMaxPIDToArray(inst.MaxPID)[i]);
            } else if (AccessoryDesiredInstGet(
                           TxPIDSettingsInputsToArray(inst.Inputs)[i] - TXPIDSETTINGS_INPUTS_ACCESSORY0,
                           &accessory) == 0) {
                value = scale(accessory.AccessoryVal, -1.0f, 1.0f,
                              TxPIDSettingsMinPIDToArray(inst.MinPID)[i],
                              TxPIDSettingsMaxPIDToArray(inst.MaxPID)[i]);
            } else {
                continue;
            }

            TxPIDStatusCurPIDToArray(txpid_status.CurPID)[i] = value;

            switch (TxPIDSettingsPIDsToArray(inst.PIDs)[i]) {
            case TXPIDSETTINGS_PIDS_ROLLRATEKP:
                needsUpdateBank |= update(&bank.RollRatePID.Kp, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLRATEPID:
                easyTuneEnabled  = true;
                needsUpdateBank |= update(&bank.RollRatePID.Kp, value);
                needsUpdateBank |= update(&bank.RollRatePID.Ki, value * inst.PitchRollRateFactors.I);
                needsUpdateBank |= update(&bank.RollRatePID.Kd, value * inst.PitchRollRateFactors.D);
                break;
            case TXPIDSETTINGS_PIDS_PITCHRATEPID:
                easyTuneEnabled  = true;
                needsUpdateBank |= update(&bank.PitchRatePID.Kp, value);
                needsUpdateBank |= update(&bank.PitchRatePID.Ki, value * inst.PitchRollRateFactors.I);
                needsUpdateBank |= update(&bank.PitchRatePID.Kd, value * inst.PitchRollRateFactors.D);
                break;
            case TXPIDSETTINGS_PIDS_ROLLRATEKI:
                needsUpdateBank |= update(&bank.RollRatePID.Ki, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLRATEKD:
                needsUpdateBank |= update(&bank.RollRatePID.Kd, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLRATEILIMIT:
                needsUpdateBank |= update(&bank.RollRatePID.ILimit, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLRATERESP:
                needsUpdateBank |= updateUint16(&bank.ManualRate.Roll, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLATTITUDEKP:
                needsUpdateBank |= update(&bank.RollPI.Kp, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLATTITUDEKI:
                needsUpdateBank |= update(&bank.RollPI.Ki, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLATTITUDEILIMIT:
                needsUpdateBank |= update(&bank.RollPI.ILimit, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLATTITUDERESP:
                needsUpdateBank |= updateUint8(&bank.RollMax, value);
                break;
            case TXPIDSETTINGS_PIDS_PITCHRATEKP:
                needsUpdateBank |= update(&bank.PitchRatePID.Kp, value);
                break;
            case TXPIDSETTINGS_PIDS_PITCHRATEKI:
                needsUpdateBank |= update(&bank.PitchRatePID.Ki, value);
                break;
            case TXPIDSETTINGS_PIDS_PITCHRATEKD:
                needsUpdateBank |= update(&bank.PitchRatePID.Kd, value);
                break;
            case TXPIDSETTINGS_PIDS_PITCHRATEILIMIT:
                needsUpdateBank |= update(&bank.PitchRatePID.ILimit, value);
                break;
            case TXPIDSETTINGS_PIDS_PITCHRATERESP:
                needsUpdateBank |= updateUint16(&bank.ManualRate.Pitch, value);
                break;
            case TXPIDSETTINGS_PIDS_PITCHATTITUDEKP:
                needsUpdateBank |= update(&bank.PitchPI.Kp, value);
                break;
            case TXPIDSETTINGS_PIDS_PITCHATTITUDEKI:
                needsUpdateBank |= update(&bank.PitchPI.Ki, value);
                break;
            case TXPIDSETTINGS_PIDS_PITCHATTITUDEILIMIT:
                needsUpdateBank |= update(&bank.PitchPI.ILimit, value);
                break;
            case TXPIDSETTINGS_PIDS_PITCHATTITUDERESP:
                needsUpdateBank |= updateUint8(&bank.PitchMax, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLPITCHRATEKP:
                needsUpdateBank |= update(&bank.RollRatePID.Kp, value);
                needsUpdateBank |= update(&bank.PitchRatePID.Kp, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLPITCHRATEKI:
                needsUpdateBank |= update(&bank.RollRatePID.Ki, value);
                needsUpdateBank |= update(&bank.PitchRatePID.Ki, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLPITCHRATEKD:
                needsUpdateBank |= update(&bank.RollRatePID.Kd, value);
                needsUpdateBank |= update(&bank.PitchRatePID.Kd, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLPITCHRATEILIMIT:
                needsUpdateBank |= update(&bank.RollRatePID.ILimit, value);
                needsUpdateBank |= update(&bank.PitchRatePID.ILimit, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLPITCHRATERESP:
                needsUpdateBank |= updateUint16(&bank.ManualRate.Roll, value);
                needsUpdateBank |= updateUint16(&bank.ManualRate.Pitch, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLPITCHATTITUDEKP:
                needsUpdateBank |= update(&bank.RollPI.Kp, value);
                needsUpdateBank |= update(&bank.PitchPI.Kp, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLPITCHATTITUDEKI:
                needsUpdateBank |= update(&bank.RollPI.Ki, value);
                needsUpdateBank |= update(&bank.PitchPI.Ki, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLPITCHATTITUDEILIMIT:
                needsUpdateBank |= update(&bank.RollPI.ILimit, value);
                needsUpdateBank |= update(&bank.PitchPI.ILimit, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLPITCHATTITUDERESP:
                needsUpdateBank |= updateUint8(&bank.RollMax, value);
                needsUpdateBank |= updateUint8(&bank.PitchMax, value);
                break;
            case TXPIDSETTINGS_PIDS_YAWRATEKP:
                needsUpdateBank |= update(&bank.YawRatePID.Kp, value);
                break;
            case TXPIDSETTINGS_PIDS_YAWRATEKI:
                needsUpdateBank |= update(&bank.YawRatePID.Ki, value);
                break;
            case TXPIDSETTINGS_PIDS_YAWRATEKD:
                needsUpdateBank |= update(&bank.YawRatePID.Kd, value);
                break;
            case TXPIDSETTINGS_PIDS_YAWRATEILIMIT:
                needsUpdateBank |= update(&bank.YawRatePID.ILimit, value);
                break;
            case TXPIDSETTINGS_PIDS_YAWRATERESP:
                needsUpdateBank |= updateUint16(&bank.ManualRate.Yaw, value);
                break;
            case TXPIDSETTINGS_PIDS_YAWATTITUDEKP:
                needsUpdateBank |= update(&bank.YawPI.Kp, value);
                break;
            case TXPIDSETTINGS_PIDS_YAWATTITUDEKI:
                needsUpdateBank |= update(&bank.YawPI.Ki, value);
                break;
            case TXPIDSETTINGS_PIDS_YAWATTITUDEILIMIT:
                needsUpdateBank |= update(&bank.YawPI.ILimit, value);
                break;
            case TXPIDSETTINGS_PIDS_YAWATTITUDERESP:
                needsUpdateBank |= updateUint8(&bank.YawMax, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLEXPO:
                needsUpdateBank |= updateInt8(&bank.StickExpo.Roll, value);
                break;
            case TXPIDSETTINGS_PIDS_PITCHEXPO:
                needsUpdateBank |= updateInt8(&bank.StickExpo.Pitch, value);
                break;
            case TXPIDSETTINGS_PIDS_ROLLPITCHEXPO:
                needsUpdateBank |= updateInt8(&bank.StickExpo.Roll, value);
                needsUpdateBank |= updateInt8(&bank.StickExpo.Pitch, value);
                break;
            case TXPIDSETTINGS_PIDS_YAWEXPO:
                needsUpdateBank |= updateInt8(&bank.StickExpo.Yaw, value);
                break;
            case TXPIDSETTINGS_PIDS_GYROTAU:
                needsUpdateStab |= update(&stab.GyroTau, value);
                break;
            case TXPIDSETTINGS_PIDS_ACROROLLFACTOR:
                needsUpdateBank |= updateUint8(&bank.AcroInsanityFactor.Roll, value);
                break;
            case TXPIDSETTINGS_PIDS_ACROPITCHFACTOR:
                needsUpdateBank |= updateUint8(&bank.AcroInsanityFactor.Pitch, value);
                break;
            case TXPIDSETTINGS_PIDS_ACROROLLPITCHFACTOR:
                needsUpdateBank |= updateUint8(&bank.AcroInsanityFactor.Roll, value);
                needsUpdateBank |= updateUint8(&bank.AcroInsanityFactor.Pitch, value);
                break;
            case TXPIDSETTINGS_PIDS_ACCELTAU:
                needsUpdateAtt  |= update(&att.AccelTau, value);
                break;
            case TXPIDSETTINGS_PIDS_ACCELKP:
                needsUpdateAtt  |= update(&att.AccelKp, value);
                break;
            case TXPIDSETTINGS_PIDS_ACCELKI:
                needsUpdateAtt  |= update(&att.AccelKi, value);
                break;

#ifdef REVOLUTION
            case TXPIDSETTINGS_PIDS_ALTITUDEPOSKP:
                needsUpdateAltitude |= update(&altitude.VerticalPosP, value);
                break;
            case TXPIDSETTINGS_PIDS_ALTITUDEVELOCITYKP:
                needsUpdateAltitude |= update(&altitude.VerticalVelPID.Kp, value);
                break;
            case TXPIDSETTINGS_PIDS_ALTITUDEVELOCITYKI:
                needsUpdateAltitude |= update(&altitude.VerticalVelPID.Ki, value);
                break;
            case TXPIDSETTINGS_PIDS_ALTITUDEVELOCITYKD:
                needsUpdateAltitude |= update(&altitude.VerticalVelPID.Kd, value);
                break;
            case TXPIDSETTINGS_PIDS_ALTITUDEVELOCITYBETA:
                needsUpdateAltitude |= update(&altitude.VerticalVelPID.Beta, value);
                break;
#endif
            default:
                PIOS_Assert(0);
            }
        }
    }
    if (needsUpdateStab) {
        StabilizationSettingsSet(&stab);
    }
    if (needsUpdateAtt) {
        AttitudeSettingsSet(&att);
    }
#ifdef REVOLUTION
    if (needsUpdateAltitude) {
        AltitudeHoldSettingsSet(&altitude);
    }
#endif
    if (easyTuneEnabled && (inst.RatePIDRecalculateYaw != TXPIDSETTINGS_RATEPIDRECALCULATEYAW_FALSE)) {
        float newKp = (bank.RollRatePID.Kp + bank.PitchRatePID.Kp) * .5f * inst.YawRateFactors.P;
        needsUpdateBank |= update(&bank.YawRatePID.Kp, newKp);
        needsUpdateBank |= update(&bank.YawRatePID.Ki, newKp * inst.YawRateFactors.I);
        needsUpdateBank |= update(&bank.YawRatePID.Kd, newKp * inst.YawRateFactors.D);
    }
    if (needsUpdateBank) {
        switch (inst.BankNumber) {
        case 0:
            StabilizationSettingsBank1Set((StabilizationSettingsBank1Data *)&bank);
            break;

        case 1:
            StabilizationSettingsBank2Set((StabilizationSettingsBank2Data *)&bank);
            break;

        case 2:
            StabilizationSettingsBank3Set((StabilizationSettingsBank3Data *)&bank);
            break;

        default:
            return;
        }
    }

    if (needsUpdateStab ||
        needsUpdateAtt ||
#ifdef REVOLUTION
        needsUpdateAltitude ||
#endif /* REVOLUTION */
        needsUpdateBank) {
        TxPIDStatusSet(&txpid_status);;
    }
}

/**
 * Scales input val from [inMin..inMax] range to [outMin..outMax].
 * If val is out of input range (inMin <= inMax), it will be bound.
 * (outMin > outMax) is ok, in that case output will be decreasing.
 *
 * \returns scaled value
 */
static float scale(float val, float inMin, float inMax, float outMin, float outMax)
{
    // bound input value
    if (val > inMax) {
        val = inMax;
    }
    if (val < inMin) {
        val = inMin;
    }

    // normalize input value to [0..1]
    if (inMax <= inMin) {
        val = 0.0f;
    } else {
        val = (val - inMin) / (inMax - inMin);
    }

    // update output bounds
    if (outMin > outMax) {
        float t = outMin;
        outMin = outMax;
        outMax = t;
        val    = 1.0f - val;
    }

    return (outMax - outMin) * val + outMin;
}

/**
 * Updates var using val if needed.
 * \returns 1 if updated, 0 otherwise
 */
static uint8_t update(float *var, float val)
{
    /* FIXME: this is not an entirely correct way
     * to check if the two floating point
     * numbers are 'not equal'.
     * Epsilon of 1e-9 is probably okay for the range
     * of numbers we see here*/
    if (fabsf(*var - val) > 1e-9f) {
        *var = val;
        return 1;
    }
    return 0;
}

/**
 * Updates var using val if needed.
 * \returns 1 if updated, 0 otherwise
 */
static uint8_t updateUint16(uint16_t *var, float val)
{
    uint16_t roundedVal = (uint16_t)roundf(val);

    if (*var != roundedVal) {
        *var = roundedVal;
        return 1;
    }
    return 0;
}

/**
 * Updates var using val if needed.
 * \returns 1 if updated, 0 otherwise
 */
static uint8_t updateUint8(uint8_t *var, float val)
{
    uint8_t roundedVal = (uint8_t)roundf(val);

    if (*var != roundedVal) {
        *var = roundedVal;
        return 1;
    }
    return 0;
}

/**
 * Updates var using val if needed.
 * \returns 1 if updated, 0 otherwise
 */
static uint8_t updateInt8(int8_t *var, float val)
{
    int8_t roundedVal = (int8_t)roundf(val);

    if (*var != roundedVal) {
        *var = roundedVal;
        return 1;
    }
    return 0;
}

/**
 * @}
 */

/**
 * @}
 */
