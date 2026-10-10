/*
HALO_CELLS.H

The script engine (source/hs) keeps every value in a 32-bit cell: a syntax
node's value, a function's arguments and result. A narrower value (a
boolean, a short) is a member of a union or structure over the cell, while
other code reads the whole cell as a long and narrows it with a cast. On the
Xbox, which is little-endian, the member's bytes are the cell's first, which
are also its low-order bytes, so both readings agree.

On a big-endian processor (the Wii) the low-order bytes are the cell's last.
There HALO_CELL_MEMBER puts the member at the cell's end, so the member and
the cast agree as they do on the Xbox, and the maps' syntax nodes, which
tools/wii_convert_maps.py converts as whole 32-bit numbers, read right.
HALO_CELL_PADDED(member, padding) is a narrow member and the padding
that fills its cell, in that order on the Xbox and the other way round on a
big-endian processor. HALO_CELL_OFFSET is where a value of so many bytes
starts in its cell. Little-endian builds are unchanged.
*/

#ifndef __HALO_CELLS_H
#define __HALO_CELLS_H

#ifdef __BIG_ENDIAN__
#define HALO_CELL_MEMBER(type, name) struct { char name##_cell_pad[4 - sizeof(type)]; type name; }
#define HALO_CELL_PADDED(member, padding) padding; member
#define HALO_CELL_OFFSET(size) (4 - (size))
#else
#define HALO_CELL_MEMBER(type, name) type name
#define HALO_CELL_PADDED(member, padding) member; padding
#define HALO_CELL_OFFSET(size) 0
#endif

/* a narrow value in the cell at an address, where the cell keeps it (the
script runtime's stores and loads of a boolean or short in a long) */
#define HALO_CELL_AT(type, address) (*(type *)((char *)(address) + HALO_CELL_OFFSET(sizeof(type))))

#endif /* __HALO_CELLS_H */
