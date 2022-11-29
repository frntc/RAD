/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__   
         {_________         {______________		Expansion Unit
                
 RADExp - A framework for DMA interfacing with Commodore C64/C128 computers using a Raspberry Pi Zero 2 or 3A+/3B+
 Copyright (c) 2022 Carsten Dachsbacher <frenetic@dachsbacher.de>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/
#ifndef _c64screen_h
#define _c64screen_h

#define VK_F1  133
#define VK_F2  137
#define VK_F3  134
#define VK_F4  138
#define VK_F5  135
#define VK_F6  139
#define VK_F7  136
#define VK_F8  140

#define VK_ESC  95
#define VK_DELETE  20
#define VK_RETURN  13
#define VK_SHIFT_RETURN  141
#define VK_COMMODORE_RETURN  142
#define VK_MOUNT  205 
#define VK_MOUNT_START  77 


#define VK_LEFT   157
#define VK_RIGHT  29
#define VK_UP     145
#define VK_DOWN   17
#define VK_HOME   19
#define VK_S	  83

#define SHIFT_L  1
#define VK_STOP  2
#define VK_CMD  3
#define VK_COMMODORE  4
#define VK_SPACE  32
#define SHIFT_R  6
#define VK_CTRL  7 
#define VK_BACK  8

extern u8 c64screen[ 40 * 25 + 1024 * 4 ]; 
extern u8 c64color[ 40 * 25 + 1024 * 4 ]; 

extern void clearC64();
extern void printC64( u32 x, u32 y, const char *t, u8 color, u8 flag = 0, u32 convert = 0, u32 maxL = 40 );

#endif
