/*
 * Copyright (c) 2015, Freescale Semiconductor, Inc.
 * Copyright (c) 2016-2017 NXP
 * Copyright (c) 2020 Bjarne Hansen
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*! \file driver_fxos8700.c
    \brief Defines commands to perform various tasks (e.g. read Device ID, Initialize, Read Data) 
    for the FXOS8700 6-axis accelerometer plus magnetometer. Actual I2C interface functions are
    found in sensor_io_i2c files.
*/

#include "sensor_fusion.h"              // Sensor fusion structures and types
#include "driver_fxos8700.h"            // FXOS8700 hardware interface
#include "driver_fxos8700_registers.h"  // describes the FXOS8700 register definitions and bit masks
#include "driver_sensors.h"             // prototypes for *_Init() and *_Read() methods
#include "hal_i2c.h"                    // I2C interface methods


// Command definition to read the WHO_AM_I value.
const registerReadlist_t    FXOS8700_WHO_AM_I_READ[] =
{
    { .readFrom = FXOS8700_WHO_AM_I, .numBytes = 1 }, __END_READ_DATA__
};

// Command definition to read the number of entries in the accel FIFO.
const registerReadlist_t    FXOS8700_F_STATUS_READ[] =
{
    { .readFrom = FXOS8700_STATUS, .numBytes = 1 }, __END_READ_DATA__
};

// Command definition to read the number of entries in the accel FIFO.
registerReadlist_t          FXOS8700_DATA_READ[] =
{
    { .readFrom = FXOS8700_OUT_X_MSB, .numBytes = 6 }, __END_READ_DATA__
};

// Each entry in a RegisterWriteList is composed of: register address, value to write, bit-mask to apply to write (0 enables)
const registerwritelist_t   FXOS8700_Initialization[] =
{
    // write 0000 0000 = 0x00 to CTRL_REG1 to place FXOS8700 into standby
    // [7-1] = 0000 000
    // [0]: active=0
    { FXOS8700_CTRL_REG1, 0x00, 0x00 }, 

    // write 0100 0000 = 0x40 to F_SETUP to enable FIFO in continuous (circular) mode
    // [7-6]: f_mode[1-0]=01 for FIFO continuous mode
    // [5-0]: f_wmrk[5-0]=000000 for no FIFO watermark
    { FXOS8700_F_SETUP, 0x40, 0x00 },

    // write 0001 1111 = 0x1F to M_CTRL_REG1
    // [7]: m_acal=0: auto calibration disabled
    // [6]: m_rst=0: one-shot magnetic reset disabled
    // [5]: m_ost=0: one-shot magnetic measurement disabled
    // [4-2]: m_os=111=7: maximum oversampling (8X at 200 Hz) to reduce magnetometer noise
    // [1-0]: m_hms=11=3: select hybrid mode with accel and magnetometer active
    { FXOS8700_M_CTRL_REG1, 0x1F, 0x00 },   

    // write 0000 0000 = 0x00 to M_CTRL_REG2
    // [7]: reserved
    // [6]: reserved
    // [5]: hyb_autoinc_mode=0 to ensure address wraparound to 0x00 to clear accelerometer FIFO in one read
    // [4]: m_maxmin_dis=0 to retain default min/max latching even though not used
    // [3]: m_maxmin_dis_ths=0
    // [2]: m_maxmin_rst=0
    // [1-0]: m_rst_cnt=00 to enable magnetic reset each cycle
    { FXOS8700_M_CTRL_REG2, 0x00, 0x00 },

    // write 0000 0001= 0x01 to XYZ_DATA_CFG register
    // [7]: reserved
    // [6]: reserved
    // [5]: reserved
    // [4]: hpf_out=0
    // [3]: reserved
    // [2]: reserved
    // [1-0]: fs=01 for 4g mode: 2048 counts / g = 8192 counts / g after 2 bit left shift
    { FXOS8700_XYZ_DATA_CFG, 0x01, 0x00 },  

    // write 0000 0010 = 0x02 to CTRL_REG2 to set MODS bits
    // [7]: st=0: self test disabled
    // [6]: rst=0: reset disabled
    // [5]: unused
    // [4-3]: smods=00
    // [2]: slpe=0: auto sleep disabled
    // [1-0]: mods=10 for high resolution (maximum over sampling)
    { FXOS8700_CTRL_REG2, 0x02, 0x00 },

    // write 00XX X101 = 0x0D to accelerometer control register 1
    // since this is a hybrid sensor with accelerometer and magnetometer sharing an ADC, 
    // the actual ODR is one-half of the the individual ODRs. E.g. ask for 400 Hz, get 200 Hz
    // The values listed below are the actual realized ODRs.
    // [7-6]: aslp_rate=00
    // [5-3]: dr=111 for 0.78Hz data rate giving 0x3D
    // [5-3]: dr=110 for 3.125Hz data rate giving 0x35
    // [5-3]: dr=101 for 6.25Hz data rate giving 0x2D
    // [5-3]: dr=100 for 25Hz data rate giving 0x25
    // [5-3]: dr=011 for 50Hz data rate giving 0x1D
    // [5-3]: dr=010 for 100Hz data rate giving 0x15
    // [5-3]: dr=001 for 200Hz data rate giving 0x0D
    // [5-3]: dr=000 for 400Hz data rate giving 0x05
    // [2]: lnoise=1 for low noise mode (only works in 2g and 4g mode)
    // [1]: f_read=0 for normal 16 bit reads
    // [0]: active=1 to take the part out of standby and enable sampling
#if (ACCEL_ODR_HZ <= 1)                     // select 0.78Hz ODR
    { FXOS8700_CTRL_REG1, 0x3D, 0x00 },
#elif (ACCEL_ODR_HZ <= 3)                   // select 3.125Hz ODR
    { FXOS8700_CTRL_REG1, 0x35, 0x00 },
#elif (ACCEL_ODR_HZ <= 6)                   // select 6.25Hz ODR
    { FXOS8700_CTRL_REG1, 0x2D, 0x00 },
#elif (ACCEL_ODR_HZ <= 30)                  // select 25Hz ODR
    { FXOS8700_CTRL_REG1, 0x25, 0x00 },
#elif (ACCEL_ODR_HZ <= 50)                  // select 50Hz ODR
    { FXOS8700_CTRL_REG1, 0x1D, 0x00 },
#elif (ACCEL_ODR_HZ <= 100)                 // select 100Hz ODR
    { FXOS8700_CTRL_REG1, 0x15, 0x00 },
#elif (ACCEL_ODR_HZ <= 200)                 // select 200Hz ODR
    { FXOS8700_CTRL_REG1, 0x0D, 0x00 },
#else // select 400Hz ODR
    { FXOS8700_CTRL_REG1, 0x05, 0x00 },
#endif
    __END_WRITE_DATA__
};

#define FXOS8700_COUNTSPERG     8192        //assumes +/-4 g range on accelerometer
#define FXOS8700_COUNTSPERUT    10

// All sensor drivers and initialization functions have a similar prototype
// sensor = pointer to linked list element used by the sensor fusion subsystem to specify required sensors
// sfg = pointer to top level data structure for sensor fusion

int8_t FXOS8700_Accel_Init(struct PhysicalSensor *sensor, SensorFusionGlobals *sfg) {
    //Use the same init function for thermometer, magnetometer and accelererometer - it will
    //end up being called three times, but that's OK
    //TODO - can move the accel stuff in here, and mag stuff following...
  return FXOS8700_Init(sensor, sfg);
}

int8_t FXOS8700_Mag_Init(struct PhysicalSensor *sensor, SensorFusionGlobals *sfg) {
    //Use the same init function for thermometer, magnetometer and accelererometer - it will
    //end up being called three times, but that's OK
  return FXOS8700_Init(sensor, sfg);
}

int8_t FXOS8700_Therm_Init(struct PhysicalSensor *sensor, SensorFusionGlobals *sfg) {
    //Use the same init function for thermometer, magnetometer and accelererometer - it will
    //end up being called three times, but that's OK
  return FXOS8700_Init(sensor, sfg);
}

int8_t FXOS8700_Init(struct PhysicalSensor *sensor, SensorFusionGlobals *sfg) {
    int32_t status;
    uint8_t reg;

    status = Sensor_I2C_Read_Register(&sensor->deviceInfo, sensor->addr, FXOS8700_WHO_AM_I, 1, &reg);

    if (status==SENSOR_ERROR_NONE) {
#if F_USING_ACCEL
       sfg->Accel.iWhoAmI = reg;
       sfg->Accel.iCountsPerg = FXOS8700_COUNTSPERG;
       sfg->Accel.fgPerCount = 1.0F / FXOS8700_COUNTSPERG;
#endif
#if F_USING_MAG
       sfg->Mag.iWhoAmI = reg;
       sfg->Mag.iCountsPeruT = FXOS8700_COUNTSPERUT;
       sfg->Mag.fCountsPeruT = (float) FXOS8700_COUNTSPERUT;
       sfg->Mag.fuTPerCount = 1.0F / FXOS8700_COUNTSPERUT;
#endif
       if (reg != FXOS8700_WHO_AM_I_PROD_VALUE) {
          return SENSOR_ERROR_INIT;  // The whoAmI did not match
       }
    } else {
        // whoAmI will retain default value of zero
        // return with error
        return status;
    }

    // Configure and start the fxos8700 sensor.  This does multiple register writes
    // (see FXOS8700_Initialization definition above)
    status = Sensor_I2C_Write_List(&sensor->deviceInfo, sensor->addr, FXOS8700_Initialization );
    sensor->isInitialized = F_USING_ACCEL | F_USING_MAG;
#if F_USING_ACCEL
    sfg->Accel.isEnabled = true;
#endif
#if F_USING_MAG
    sfg->Mag.isEnabled = true;
#endif

    return (status);
} // end FXOS8700_Init()

#if F_USING_ACCEL
int8_t FXOS8700_Accel_Read(struct PhysicalSensor *sensor, SensorFusionGlobals *sfg) {
    uint8_t                     I2C_Buffer[6 * ACCEL_FIFO_SIZE];    // I2C read buffer
    int32_t                     status;         // I2C transaction status
    int8_t                      j;              // scratch
    uint8_t                     fifo_packet_count;
    int16_t                     sample[3];

    if(!(sensor->isInitialized & F_USING_ACCEL)) {
       return SENSOR_ERROR_INIT;
    }

    // read the F_STATUS register (mapped to STATUS) and extract number of
    // measurements available (lower 6 bits)
    status = Sensor_I2C_Read(&sensor->deviceInfo,
                             sensor->addr, FXOS8700_F_STATUS_READ, I2C_Buffer);
    if (status == SENSOR_ERROR_NONE) {
#ifdef SIMULATOR_MODE
      fifo_packet_count = 1;
#else
      fifo_packet_count = I2C_Buffer[0] & FXOS8700_F_STATUS_F_CNT_MASK;
#endif
      // return if there are no measurements in the sensor FIFO.
      // this will only occur when the calling frequency equals or exceeds
      // ACCEL_ODR_HZ
      if (fifo_packet_count == 0) {
        return (SENSOR_ERROR_READ);
      }
    } else {
      return (status);
    }

    // Steady state when fusing at 40 Hz is 5 packets per cycle to read (accel
    // updates at 200 Hz). Noticed that I2C reads > 126 bytes don't work, so
    // limit the number of FIFO packets per burst read. With the address
    // auto-increment and wrap turned on, the registers are read
    // 0x01,0x02,...0x05,0x06,0x01,0x02,...  So we read 6 bytes per packet.
#define MAX_FIFO_PACKETS_PER_READ 15  // for max of 90 bytes per I2C xaction.
    FXOS8700_DATA_READ[0].readFrom = FXOS8700_OUT_X_MSB;  
    while ((fifo_packet_count > 0) && (status == SENSOR_ERROR_NONE)) {
      if (MAX_FIFO_PACKETS_PER_READ < fifo_packet_count) {
        FXOS8700_DATA_READ[0].numBytes = 6 * MAX_FIFO_PACKETS_PER_READ;
        fifo_packet_count -= MAX_FIFO_PACKETS_PER_READ;
      } else {
        FXOS8700_DATA_READ[0].numBytes = 6 * fifo_packet_count;
        fifo_packet_count = 0;
      }
      status = Sensor_I2C_Read(&sensor->deviceInfo,
                               sensor->addr, FXOS8700_DATA_READ, I2C_Buffer);
      if (status == SENSOR_ERROR_NONE) {
        for (j = 0; j < FXOS8700_DATA_READ[0].numBytes; j+=6) {
            // place the measurements read into the accelerometer buffer structure 
            sample[CHX] = (I2C_Buffer[j + 0] << 8) | (I2C_Buffer[j + 1]); 
            sample[CHY] = (I2C_Buffer[j + 2] << 8) | (I2C_Buffer[j + 3]); 
            sample[CHZ] = (I2C_Buffer[j + 4] << 8) | (I2C_Buffer[j + 5]);
            conditionSample(sample);  //truncate negative values to -32767
            // place the 6 bytes read into the 16 bit accelerometer structure 
            addToFifo((union FifoSensor*) &(sfg->Accel), ACCEL_FIFO_SIZE, sample);
        } // end transfer all bytes from each packet
      } // end processing a burst read
    } // end emptying all packets from FIFO
    return (status);
}  // end FXOS8700_ReadAccData()
#endif

#if F_USING_MAG
// read FXOS8700 magnetometer over I2C
int8_t FXOS8700_Mag_Read(struct PhysicalSensor *sensor, SensorFusionGlobals *sfg) {
    uint8_t                     I2C_Buffer[6];  // I2C read buffer
    int32_t                     status;         // I2C transaction status
    int16_t                     sample[3];

    if(!(sensor->isInitialized & F_USING_MAG))
    {
        return SENSOR_ERROR_INIT;
    }

    // read the six sequential magnetometer output bytes
    FXOS8700_DATA_READ[0].readFrom = FXOS8700_M_OUT_X_MSB;
    FXOS8700_DATA_READ[0].numBytes = 6;
    status =  Sensor_I2C_Read(&sensor->deviceInfo, sensor->addr, FXOS8700_DATA_READ, I2C_Buffer );
    if (status==SENSOR_ERROR_NONE) {
        // place the 6 bytes read into the magnetometer structure
        sample[CHX] = (I2C_Buffer[0] << 8) | I2C_Buffer[1];
        sample[CHY] = (I2C_Buffer[2] << 8) | I2C_Buffer[3];
        sample[CHZ] = (I2C_Buffer[4] << 8) | I2C_Buffer[5];
        conditionSample(sample);  // truncate negative values to -32767
        addToFifo((union FifoSensor*) &(sfg->Mag), MAG_FIFO_SIZE, sample);
    }
    return status;
}//end FXOS8700_ReadMagData()
#endif  //F_USING_MAG

// read temperature register over I2C
int8_t FXOS8700_Therm_Read(struct PhysicalSensor *sensor, SensorFusionGlobals *sfg) {
    int8_t                      I2C_Buffer;     // I2C read buffer
    int32_t                     status;         // I2C transaction status

    if(!(sensor->isInitialized)) {
        return SENSOR_ERROR_INIT;
    }

    // read the Temperature register 0x51
    FXOS8700_DATA_READ[0].readFrom = FXOS8700_TEMP;
    FXOS8700_DATA_READ[0].numBytes = 1;
    status =  Sensor_I2C_Read(&sensor->deviceInfo, sensor->addr, FXOS8700_DATA_READ, (uint8_t*)(&I2C_Buffer) );
    if (status==SENSOR_ERROR_NONE) {
        // convert the byte to temperature and place in sfg structure
        sfg->Temp.temperatureC = (float)I2C_Buffer * 0.96; //section 14.3 of manual says 0.96 degC/LSB
    }
    return status;
}//end FXOS8700_Therm_Read()

// This is the composite read function that handles both accel and mag portions of the FXOS8700
// It returns the first failing status flag
int8_t FXOS8700_Read(struct PhysicalSensor *sensor, SensorFusionGlobals *sfg) {
    int8_t  sts1 = 0;
    int8_t  sts2 = 0;
    int8_t  sts3 = 0;
#if F_USING_ACCEL
        sts1 = FXOS8700_Accel_Read(sensor, sfg);
#endif

#if F_USING_MAG
        sts2 = FXOS8700_Mag_Read(sensor, sfg);
        sts3 = FXOS8700_Therm_Read(sensor, sfg);
#endif

    return (sts1 + sts2 + sts3);
} // end FXOS8700_Read()

// Each entry in a RegisterWriteList is composed of: register address, value to write, bit-mask to apply to write (0 enables)
const registerwritelist_t   FXOS8700_FULL_IDLE[] =
{
  // Set ACTIVE = other bits unchanged
  { FXOS8700_CTRL_REG1, 0x00, 0x01 },
    __END_WRITE_DATA__
};

// FXOS8700_Idle places the entire sensor into STANDBY mode (wakeup time = 1/ODR+1ms)
// This driver is all-on or all-off. It does not support mag or accel only.
// If you want that functionality, you can write your own using the initialization
// function in this file as a starting template.  We've chosen not to cover all
// permutations in the interest of simplicity.
int8_t FXOS8700_Idle(struct PhysicalSensor *sensor, SensorFusionGlobals *sfg) {
    int32_t     status;
    if(sensor->isInitialized == (F_USING_ACCEL|F_USING_MAG)) {
        status = Sensor_I2C_Write_List(&sensor->deviceInfo, sensor->addr, FXOS8700_FULL_IDLE );
        sensor->isInitialized = 0;
#if F_USING_ACCEL
        sfg->Accel.isEnabled = false;
#endif
#if F_USING_MAG
        sfg->Mag.isEnabled = false;
#endif
    } else {
      return SENSOR_ERROR_INIT;
    }
    return status;
} // end FXOS8700_Idle()
