/*
 *  eCoach
 *
 *  Copyright (C) 2010  Sampo Savola
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  See the file COPYING
 */
#ifndef _UPLOAD_DLG_H
#define _UPLOAD_DLG_H

#include "analyzer.h"
/* Osso */
#include <libosso.h>
#include "gconf_helper.h"
#include "gconf_keys.h"
void upload(AnalyzerView *data);
static int first_time_authentication(AnalyzerView *data);

/*
typedef struct _UploadDlg {
  
  osso_context_t *osso;
  
  
}UploadDlg;
*/
#endif

