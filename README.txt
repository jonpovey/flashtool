README for flashtool utility
----------------------------
Written by Jon Povey <jon.povey@racelogic.co.uk>
Copyright (C) 2011 Racelogic Limited
Released under the GPL v2.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
--------------------------

This utility is for erasing and writing binary images to raw MTD NAND flash
under Linux.

It supports software ECC generation compatible with the 4-bit hardware ECC
in TI DM355 and DM365 SoCs, and raw mode MTD writes with software OOB layout
to write the UBL in the format the RBL expects on those SoCs.
"--legacy" is for writing DM355 UBL, "--dm365-rbl" for DM365.

"--ubi" mode is for writing a UBI filesystem image, in this mode the last
pages per eraseblock that are all-FF are not written to flash, to avoid ECC
corruption.

"--failbad" will cause the operation to fail if any bad blocks are encountered.
This is useful for writing areas such as UBL or ABL if the loader for these
(i.e. the RBL) does not support bad block tables. Without --failbad, bad blocks
are skipped.

In normal operation flashtool will attempt to mark new bad blocks it encouters.
See the code, this handling could maybe use some improvement.

"--maxoff" can provide an upper limit for writing. If not using the --failbad
switch this allows writing a binary image into a specific area with bad block
skipping, but failing the operation if it would overrun into some other area
you have assigned.

Run "flashtool" with no arguments for usage instructions.
