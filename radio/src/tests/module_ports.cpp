/*
 * Copyright (C) EdgeTX
 *
 * Based on code named
 *   opentx - https://github.com/opentx/opentx
 *   th9x - http://code.google.com/p/th9x
 *   er9x - http://code.google.com/p/er9x
 *   gruvin9x - http://code.google.com/p/gruvin9x
 *
 * License GPLv2: http://www.gnu.org/licenses/gpl-2.0.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "gtests.h"
#include "hal/module_port.h"
#include "pulses/modules_constants.h"

#if defined(HARDWARE_EXTERNAL_MODULE)
TEST(ports, isPortUsed)
{
  modulePortInit();

  const etx_serial_init serialCfg = {
    .baudrate = 57600,
    .encoding = ETX_Encoding_8N1,
    .direction = ETX_Dir_TX_RX,
    .polarity = ETX_Pol_Normal,
  };
  
  auto mod_st = modulePortInitSerial(EXTERNAL_MODULE, ETX_MOD_PORT_SPORT, &serialCfg);
  EXPECT_TRUE(mod_st != nullptr);
  EXPECT_TRUE(mod_st && mod_st->rx.port != nullptr);

  auto module = modulePortGetModuleForPort(ETX_MOD_PORT_SPORT);
  EXPECT_EQ(EXTERNAL_MODULE, module);
  
  if (mod_st) modulePortDeInit(mod_st);
  EXPECT_FALSE(modulePortIsPortUsed(ETX_MOD_PORT_SPORT));
}
#endif

#if defined(INTERNAL_MODULE_PXX1) && defined(HARDWARE_EXTERNAL_MODULE)
#include "pulses/pxx1.h"

TEST(pxx1_ports, deactivateRX_pxx1)
{
  modulePortInit();
  g_model.moduleData[EXTERNAL_MODULE].type = MODULE_TYPE_R9M_PXX1;

  void* ext_ctx = Pxx1Driver.init(EXTERNAL_MODULE);
  EXPECT_TRUE(ext_ctx != nullptr);
  EXPECT_TRUE(modulePortIsPortUsed(ETX_MOD_PORT_SPORT));
  if (!ext_ctx) return;

  auto ext_drv = pulsesGetModuleDriver(EXTERNAL_MODULE);
  ext_drv->drv = &Pxx1Driver;
  ext_drv->ctx = ext_ctx;

  void* int_ctx = Pxx1Driver.init(INTERNAL_MODULE);
  EXPECT_TRUE(int_ctx != nullptr);
  EXPECT_EQ(INTERNAL_MODULE, modulePortGetModuleForPort(ETX_MOD_PORT_SPORT));
  if (!int_ctx) return;
  
  Pxx1Driver.deinit(int_ctx);
  EXPECT_EQ(EXTERNAL_MODULE, modulePortGetModuleForPort(ETX_MOD_PORT_SPORT));

  Pxx1Driver.deinit(ext_ctx);
  memset(ext_drv, 0, sizeof(module_pulse_driver));

  EXPECT_FALSE(modulePortIsPortUsed(ETX_MOD_PORT_SPORT));
}

#include "pulses/multi.h"

TEST(pxx1_ports, deactivateRX_multi)
{
  modulePortInit();
  g_model.moduleData[EXTERNAL_MODULE].type = MODULE_TYPE_MULTIMODULE;

  void* ext_ctx = MultiDriver.init(EXTERNAL_MODULE);
  EXPECT_TRUE(ext_ctx != nullptr);
  EXPECT_TRUE(modulePortIsPortUsed(ETX_MOD_PORT_SPORT));
  if (!ext_ctx) return;

  auto ext_drv = pulsesGetModuleDriver(EXTERNAL_MODULE);
  ext_drv->drv = &MultiDriver;
  ext_drv->ctx = ext_ctx;

  uint8_t buffer[64];
  uint8_t channelStart = g_model.moduleData[EXTERNAL_MODULE].channelsStart;
  int16_t* channels = &channelOutputs[channelStart];
  uint8_t nChannels = 16;
  
  MultiDriver.sendPulses(ext_ctx, buffer, channels, nChannels);
  EXPECT_FALSE(buffer[0x1A] & 2);
  
  void* int_ctx = Pxx1Driver.init(INTERNAL_MODULE);
  EXPECT_TRUE(int_ctx != nullptr);
  EXPECT_EQ(INTERNAL_MODULE, modulePortGetModuleForPort(ETX_MOD_PORT_SPORT));
  if (!int_ctx) return;

  MultiDriver.sendPulses(ext_ctx, buffer, channels, nChannels);
  EXPECT_TRUE(buffer[0x1A] & 2);

  Pxx1Driver.deinit(int_ctx);
  EXPECT_EQ(EXTERNAL_MODULE, modulePortGetModuleForPort(ETX_MOD_PORT_SPORT));

  MultiDriver.sendPulses(ext_ctx, buffer, channels, nChannels);
  EXPECT_FALSE(buffer[0x1A] & 2);

  MultiDriver.deinit(ext_ctx);
  memset(ext_drv, 0, sizeof(module_pulse_driver));

  EXPECT_FALSE(modulePortIsPortUsed(ETX_MOD_PORT_SPORT));
}
#endif
