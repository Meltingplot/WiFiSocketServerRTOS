/*
 SPI.cpp - SPI library for esp8266

 Copyright (c) 2015 Hristo Gochkov. All rights reserved.
 This file is part of the esp8266 core for Arduino environment.

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include <cmath>

#include "esp_attr.h"

#include "HSPI.h"

#include "esp8266/spi.h"
#include "esp8266/gpio.h"

HSPIClass::HSPIClass() {
}

void HSPIClass::InitMaster(uint8_t mode, uint32_t clockReg, bool msbFirst)
{
	gpio_reset_pin(SCK);
	PIN_PULLUP_EN(PERIPHS_IO_MUX_MTMS_U);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTMS_U, FUNC_HSPI_CLK);

	gpio_reset_pin(MOSI);
	PIN_PULLUP_EN(PERIPHS_IO_MUX_MTCK_U);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U, FUNC_HSPID_MOSI);

	gpio_reset_pin(MISO);
	PIN_PULLUP_EN(PERIPHS_IO_MUX_MTDI_U);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDI_U, FUNC_HSPIQ_MISO);

	REG(SPI_CTRL(MSPI)) = (msbFirst) ? 0 : SPI_WR_BIT_ORDER | SPI_RD_BIT_ORDER;

	REG(SPI_USER(MSPI)) = SPI_USR_MOSI | SPIUDUPLEX;
	REG(SPI_USER1(MSPI)) = (7 << SPI_USR_MOSI_BITLEN_S) | (7 << SPI_USR_MISO_BITLEN_S);
	REG(SPI_CTRL1(MSPI)) = 0;
	REG(SPI_SLAVE(MSPI)) = 0;

	const bool CPOL = (mode & 0x02); ///< CPOL (Clock Polarity)
	const bool CPHA = (mode & 0x01); ///< CPHA (Clock Phase)

	/*
	 SPI_MODE0 0x00 - CPOL: 0  CPHA: 0
	 SPI_MODE1 0x01 - CPOL: 0  CPHA: 1
	 SPI_MODE2 0x10 - CPOL: 1  CPHA: 0
	 SPI_MODE3 0x11 - CPOL: 1  CPHA: 1
	 */

	if (CPHA)
	{
		REG(SPI_USER(MSPI)) |= (SPI_CK_OUT_EDGE | SPI_CK_I_EDGE);
	}
	else
	{
		REG(SPI_USER(MSPI)) &= ~(SPI_CK_OUT_EDGE | SPI_CK_I_EDGE);
	}

	if (CPOL)
	{
		REG(SPI_PIN(MSPI)) |= 1ul << 29;
	}
	else
	{
		REG(SPI_PIN(MSPI)) &= ~(1ul << 29);
	}

	setClockDivider(clockReg);
}

void HSPIClass::end() {
	// Select GPIO pin function and reset
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDI_U, FUNC_GPIO12);
	gpio_reset_pin(MISO);
	gpio_set_direction(MISO, GPIO_MODE_INPUT);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U, FUNC_GPIO13);
	gpio_reset_pin(MOSI);
	gpio_set_direction(MOSI, GPIO_MODE_INPUT);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTMS_U, FUNC_GPIO14);
	gpio_reset_pin(SCK);
	gpio_set_direction(SCK, GPIO_MODE_INPUT);
}

// Begin a transaction without changing settings
void IRAM_ATTR HSPIClass::beginTransaction() {
	while(REG(SPI_CMD(MSPI)) & SPI_USR) {}
}

void IRAM_ATTR HSPIClass::endTransaction() {
}

// clockDiv is NOT the required division ratio, it is the value to write to the REG(SPI_CLOCK(MSPI)) register
void HSPIClass::setClockDivider(uint32_t clockDiv)
{
	// From the datasheet:
	// bits 0-5  spi_clkcnt_L = (divider - 1)
	// bits 6-11 spi_clkcnt_H = floor(divider/2) - 1
	// bits 12-17 spi_clkcnt_N = divider - 1
	// bits 18-30 spi_clkdiv_pre = prescaler - 1
	// bit 31 = set to run at sysclock speed
	// We assume the divider is >1 but <64 so we need only worry about the low bits

	if (clockDiv == 0x80000000)
	{
		REG(PERIPHS_IO_MUX) |= (1 << 9); // Set bit 9 if sysclock required
	}
	else
	{
		REG(PERIPHS_IO_MUX) &= ~(1 << 9);
	}
	REG(SPI_CLOCK(MSPI)) = clockDiv;
}

void HSPIClass::setDataBits(uint16_t bits)
{
	const uint32_t mask = ~((SPI_USR_MOSI_BITLEN << SPI_USR_MOSI_BITLEN_S) | (SPI_USR_MISO_BITLEN << SPI_USR_MISO_BITLEN_S));
	bits--;
	REG(SPI_USER1(MSPI)) = ((REG(SPI_USER1(MSPI)) & mask) | ((bits << SPI_USR_MOSI_BITLEN_S) | (bits << SPI_USR_MISO_BITLEN_S)));
}

uint32_t IRAM_ATTR HSPIClass::transfer32(uint32_t data)
{
	while(REG(SPI_CMD(MSPI)) & SPI_USR) {}
	// Set to 32Bits transfer
	setDataBits(32);
	// LSBFIRST Byte first
	REG(SPI_W0(MSPI)) = data;
	REG(SPI_CMD(MSPI)) |= SPI_USR;
	while(REG(SPI_CMD(MSPI)) & SPI_USR) {}
	return REG(SPI_W0(MSPI));
}

/**
 * @param out uint32_t *
 * @param in  uint32_t *
 * @param size uint32_t
 */
void IRAM_ATTR HSPIClass::transferDwords(const uint32_t * out, uint32_t * in, uint32_t size) {
	while(size != 0) {
		if (size > 16) {
			transferDwords_(out, in, 16);
			size -= 16;
			if(out) out += 16;
			if(in) in += 16;
		} else {
			transferDwords_(out, in, size);
			size = 0;
		}
	}
}

void IRAM_ATTR HSPIClass::transferDwords_(const uint32_t * out, uint32_t * in, uint8_t size) {
	while(REG(SPI_CMD(MSPI)) & SPI_USR) {}

	// Set in/out Bits to transfer
	setDataBits(size * 32);

	volatile uint32_t * fifoPtr = &REG(SPI_W0(MSPI));
	uint8_t dataSize = size;

	if (out != nullptr) {
		while(dataSize != 0) {
			*fifoPtr++ = *out++;
			dataSize--;
		}
	} else {
		// no out data, so fill with dummy data
		while(dataSize != 0) {
			*fifoPtr++ = 0xFFFFFFFF;
			dataSize--;
		}
	}

	REG(SPI_CMD(MSPI)) |= SPI_USR;
	while(REG(SPI_CMD(MSPI)) & SPI_USR) {}

	if (in != nullptr) {
		volatile uint32_t * fifoPtrRd = &REG(SPI_W0(MSPI));
		while(size != 0) {
			*in++ = *fifoPtrRd++;
			size--;
		}
	}
}

// End
