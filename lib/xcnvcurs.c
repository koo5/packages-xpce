/*  $Id$

    Part of XPCE --- The SWI-Prolog GUI toolkit

    Author:        Jan Wielemaker and Anjo Anjewierden
    E-mail:        jan@swi.psy.uva.nl
    WWW:           http://www.swi.psy.uva.nl/projects/xpce/
    Copyright (C): 1985-2002, University of Amsterdam

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
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "../src/msw/xcursor.h"
#include <stdio.h>
#include <malloc.h>

void
convert(FILE *in, FILE *out)
{ char 				line[1024];
  int 				low, high;
  cursor_glyph_file_header	header;
  CursorGlyph			glyphs;
  int				glyph = 0;
  unsigned short 		*data = malloc(64 * 1024);
  unsigned short		*dp = data;


  while(fgets(line, sizeof(line), in))
  { if ( sscanf(line, "Range:%d to %d", &low, &high) == 2 )
    { header.magic   = XPCE_CURSOR_FILE_MAGIC;
      header.entries = high - low + 1;
      header.offset  = low;
      glyphs = malloc(sizeof(cursor_glyph) * header.entries);

      while(fgets(line, sizeof(line), in))
      { int here, right, left, ascent, descent, found;

	if ( sscanf(line, "char #%d", &here) == 1 )
	{ if ( here != glyph )
	  { fprintf(stderr, "Mismatch: here = %d; glyph = %d\n", here, glyph);
	    exit(1);
	  }
	  fgets(line, sizeof(line), in);
	  if ( (found = sscanf(line,
			       "Right: %d%*[ \t]Left: %d%*[ \t]"
			       "Descent: %d%*[ \t]Ascent: %d",
			       &right, &left, &descent, &ascent)) != 4 )
	  { fprintf(stderr, "Failed to parse %s (found = %d)\n", line, found);
	    exit(1);
	  }

	  glyphs[glyph].width  = right - left;
	  glyphs[glyph].height = ascent + descent;
	  glyphs[glyph].hot_x  = -left;
	  glyphs[glyph].hot_y  = ascent;
	  glyphs[glyph].offset = (dp-data)*sizeof(unsigned short);

	  { int x, y;
	    unsigned short d;
	    int bit;

	    for(y=0; y<glyphs[glyph].height; y++)
	    { fgets(line, sizeof(line), in);

	      for(d=0, bit=15, x=0; x<glyphs[glyph].width; x++)
	      { d |= (line[x] == '#' ? 1 : 0) << bit;

		if ( bit-- == 0 )
		{ *dp++ = d;
		  bit = 15;
		}
	      }
	      if ( bit != 15 )
		*dp++ = d;
	    }
	  }
	  glyph++;
	}
      }

      if ( glyph != header.entries )
      { fprintf(stderr, "Only parsed %d of %d glyphs\n",
		glyphs, header.entries);
	exit(1);
      }
      header.dsize = (dp-data) * sizeof(unsigned short);

#ifdef FORMAT_C
    { int i;

      fprintf(out, "/*  X11 cursors for XPCE\n");
      fprintf(out, "    Generated file\n");
      fprintf(out, "*/\n\n");
					/* File header */
      fprintf(out, "static cursor_glyph_file_header x11_glyph_header =\n");
      fprintf(out, "{ %d,\n", header.magic);
      fprintf(out, "  %d,\n", header.entries);
      fprintf(out, "  %d,\n", header.offset);
      fprintf(out, "  %ld\n", header.dsize);
      fprintf(out, "};\n\n");
					/* Cursor glyphs */
      fprintf(out, "static cursor_glyph x11_glyps[] =\n{ ");
      for(i=0; i<glyph; )
      { CursorGlyph gl = &glyphs[i];

	fprintf(out, "{ %2d, %2d, %2d, %2d, %4ld }",
		gl->width, gl->height, gl->hot_x, gl->hot_y, gl->offset);
	if ( ++i < glyph )
	  fprintf(out, ",\n  ");
	else
	  fprintf(out, "\n");
      }
      fprintf(out, "};\n\n");
					/* Data */
      fprintf(out, "static unsigned short x11_glyph_data[] =\n{ ");
      for(i=0; i<dp-data; i++)
      { fprintf(out, "0x%04x", data[i]);
	if ( i+1 < dp-data )
	{ if ( i > 0 && (i+1) % 8 == 0 )
	    fprintf(out, ",\n  ");
	  else
	    fprintf(out, ", ");
	}
      }
      fprintf(out, "\n};\n\n");
    }
#else
      fwrite(&header, sizeof(header), 1, out);
      fwrite(glyphs,  sizeof(cursor_glyph), glyph, out);
      fwrite(data,    sizeof(unsigned short), dp-data, out);
#endif
      fflush(out);
      return;
    }
  }
}


main(int argc, char **argv)
{ if ( argc == 3 )
  { FILE  *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");

    if ( !in )
    { perror(argv[1]);
      exit(1);
    }
    if ( !out )
    { perror(argv[2]);
      exit(1);
    }

    convert(in, out);
    exit(0);
  }

  fprintf(stderr, "Usage: %s showfont-output outfile\n", argv[0]);
  exit(1);
}
