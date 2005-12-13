/* the Music Player Daemon (MPD)
 * (c)2003-2004 by Warren Dukes (shank@mercury.chem.pitt.edu)
 * This project's homepage is: http://www.musicpd.org
 *
 * Common functions used for Ogg data streams (Ogg-Vorbis and OggFLAC)
 * (c) 2005 by Eric Wong <normalperson@yhbt.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _OGG_COMMON_H
#define _OGG_COMMON_H

#include "../inputPlugin.h"

#if defined(HAVE_OGGFLAC) || defined(HAVE_OGGVORBIS)

typedef enum _ogg_stream_type { VORBIS, FLAC } ogg_stream_type;

ogg_stream_type ogg_stream_type_detect(InputStream * inStream);

#endif /* defined(HAVE_OGGFLAC || defined(HAVE_OGGVORBIS) */

#endif /* _OGG_COMMON_H */
