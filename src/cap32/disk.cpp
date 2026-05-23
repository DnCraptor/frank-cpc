/* Caprice32 - Amstrad CPC Emulator
   (c) Copyright 1997-2005 Ulrich Doewich

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "disk.h"

void t_sector_setData(t_sector *sector, byte *data)
{
   sector->data = data;
}

byte *t_sector_getDataForWrite(t_sector *sector)
{
   return sector->data;
}

byte *t_sector_getDataForRead(t_sector *sector)
{
   if (sector->weak_versions == 0) {
      sector->weak_versions = 1;
   }
   sector->weak_read_version = (sector->weak_read_version + 1) % sector->weak_versions;
   return &sector->data[sector->weak_read_version * sector->data_size];
}

void t_sector_setSizes(t_sector *sector, dword size, dword total_size)
{
   sector->data_size = size;
   sector->total_size = total_size;
   sector->weak_read_version = 0;
   sector->weak_versions = 1;
   if ((sector->data_size > 0) && (sector->data_size <= sector->total_size)) {
      sector->weak_versions = sector->total_size / sector->data_size;
   }
}

dword t_sector_getTotalSize(const t_sector *sector)
{
   return sector->total_size;
}
