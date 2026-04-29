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


#ifndef __MOD_H__
#define __MOD_H__

#include "modplay.h"
#include "modplay_core.h"
#include "defines.h"

#ifdef __cplusplus
extern "C" {
#endif

int MODFILE_SetMOD(u8 *modfile, int modlength, MODFILE *mod);
BOOL MODFILE_IsMOD(u8 *modfile, int modlength);
int MODFILE_MODGetFormatID(void);
char *MODFILE_MODGetDescription(void);
char *MODFILE_MODGetAuthor(void);
char *MODFILE_MODGetVersion(void);
char *MODFILE_MODGetCopyright(void);


#ifdef __cplusplus
}
#endif

#endif
