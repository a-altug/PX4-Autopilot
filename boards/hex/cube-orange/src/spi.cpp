/****************************************************************************
 *
 *   Copyright (C) 2020 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include <px4_arch/spi_hw_description.h>
#include <drivers/drv_sensor.h>
#include <nuttx/spi/spi.h>

constexpr px4_spi_bus_t px4_spi_buses[SPI_BUS_MAX_BUS_ITEMS] = {
	// Internal: ICM20649, MS5611
	initSPIBus(SPI::Bus::SPI1, {
		initSPIDevice(DRV_IMU_DEVTYPE_ICM20649, SPI::CS{GPIO::PortC, GPIO::Pin2}, SPI::DRDY{GPIO::PortD, GPIO::Pin15}),
		initSPIDevice(DRV_BARO_DEVTYPE_MS5611,  SPI::CS{GPIO::PortD, GPIO::Pin7} /* No DRDY available */),
	}),

	// FRAM
	initSPIBus(SPI::Bus::SPI2, {
		initSPIDevice(SPIDEV_FLASH(0), SPI::CS{GPIO::PortD, GPIO::Pin10})
	}),

	// External (on the Vibration Damped IMU board): ICM20602, ICM20948, MS5611
	initSPIBusExternal(SPI::Bus::SPI4, {
		initSPIConfigExternal(SPI::CS{GPIO::PortC, GPIO::Pin13}), // ICM20602 -- accel/gyro
		initSPIConfigExternal(SPI::CS{GPIO::PortC, GPIO::Pin15}), // ICM20948 -- accel/gyro/mag -- TODO: not sure about this. Either PC15 or PE4
		initSPIConfigExternal(SPI::CS{GPIO::PortC, GPIO::Pin14}), // MS5611

	}),

	/// TODO -- for reference...
	// initSPIBus(SPI::Bus::SPI1, {
	// 	initSPIDevice(DRV_IMU_DEVTYPE_MPU9250, SPI::CS{GPIO::PortE, GPIO::Pin4}, SPI::DRDY{GPIO::PortD, GPIO::Pin15}),
	// 	initSPIDevice(DRV_IMU_DEVTYPE_ICM20948, SPI::CS{GPIO::PortC, GPIO::Pin2}, SPI::DRDY{GPIO::PortD, GPIO::Pin15}),
	// 	initSPIDevice(DRV_BARO_DEVTYPE_MS5611, SPI::CS{GPIO::PortD, GPIO::Pin7}, SPI::DRDY{GPIO::PortB, GPIO::Pin0}),
	// }, {GPIO::PortE, GPIO::Pin3}
	// 	  ),

	// initSPIBus(SPI::Bus::SPI4, {
	// 	initSPIDevice(DRV_IMU_DEVTYPE_ST_LSM9DS1_AG, SPI::CS{GPIO::PortC, GPIO::Pin13}),
	// 	initSPIDevice(DRV_MAG_DEVTYPE_ST_LSM9DS1_M, SPI::CS{GPIO::PortC, GPIO::Pin15}),
	// 	initSPIDevice(DRV_IMU_DEVTYPE_MPU9250, SPI::CS{GPIO::PortC, GPIO::Pin2}),
	// 	initSPIDevice(DRV_IMU_DEVTYPE_ICM20602, SPI::CS{GPIO::PortC, GPIO::Pin13}),
	// 	initSPIDevice(DRV_BARO_DEVTYPE_MS5611, SPI::CS{GPIO::PortC, GPIO::Pin14}),
	// }),
};



static constexpr bool unused = validateSPIConfig(px4_spi_buses);
