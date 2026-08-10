/*
 * OV3660 driver.
 *
 * This file is part of the OpenMV project, Copyright (c) 2013/2014 Ibrahim Abdelkader.
 * Licensed under the MIT license.
 * Pulled from upstream espressif/esp32-camera (sensors/private_include/ov3660.h)
 * for OV3660 support in this fork.
 */
#ifndef __OV3660_H__
#define __OV3660_H__

#include "sensor.h"

/**
 * @brief Detect sensor pid
 *
 * @param slv_addr SCCB address
 * @param id Detection result
 * @return
 *     0:       Can't detect this sensor
 *     Nonzero: This sensor has been detected
 */
int ov3660_detect(int slv_addr, sensor_id_t *id);

/**
 * @brief initialize sensor function pointers
 *
 * @param sensor pointer of sensor
 * @return
 *      Always 0
 */
int ov3660_init(sensor_t *sensor);

#endif // __OV3660_H__
