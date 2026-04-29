/*******************************************************************************
 *  Copyright (c) 2002-2024 Christian Nowak <chnowak@web.de>                   *
 *   This file is part of chn's modplay.                                       *
 *                                                                             *
 *  modplay is free software: you can redistribute it and/or modify it         *
 *  under the terms of the GNU General Public License as published by the Free *
 *  Software Foundation, either version 3 of the License, or (at your option)  *
 *  any later version.                                                         *
 *                                                                             *          
 *  modplay is distributed in the hope that it will be useful, but             * 
 *  WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY *
 *  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License    *
 *  for more details.                                                          *
 *                                                                             *
 *  You should have received a copy of the GNU General Public License along    *
 *  with modplay. If not, see <https://www.gnu.org/licenses/>.                 *
 *******************************************************************************/


#ifndef __ENVELOPE_H__
#define __ENVELOPE_H__

#ifdef __cplusplus
extern "C" {
#endif


#include "defines.h"

#define ENV_WIDTH   65536
#define ENV_HEIGHT  65536

typedef struct EnvPoint {

  u16 x,y;
} EnvPoint;

typedef struct EnvelopeConfig {

  BOOL enabled;
  u8 numPoints;  /* # of envelope points */
  u8 loop_start;
  u8 loop_end;
  u8 sustain;

  EnvPoint *envPoints;
} EnvelopeConfig;

typedef struct Envelope {

  EnvelopeConfig *envConfig;

  BOOL triggered;
  BOOL hold;
  u8 curPoint;
  u16 value;
  u16 position;
} Envelope;


void EnvReset(Envelope *env);
void EnvTrigger(Envelope *env);
BOOL EnvProcess(Envelope *env);
void EnvRelease(Envelope *env);


#ifdef __cplusplus
}
#endif

#endif
