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


#ifndef __XM_H__
#define __XM_H__

#include "modplay.h"

#ifdef __cplusplus
extern "C" {
#endif

int MODFILE_SetXM(u8 *xmfile, int xmlength, MODFILE *xm);
BOOL MODFILE_IsXM(u8 *xmfile, int xmlength);
int MODFILE_XMGetFormatID(void);
char *MODFILE_XMGetDescription(void);
char *MODFILE_XMGetAuthor(void);
char *MODFILE_XMGetVersion(void);
char *MODFILE_XMGetCopyright(void);

#ifdef __cplusplus
}
#endif


#endif
