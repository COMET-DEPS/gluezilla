#!/usr/bin/python3

# This script toggles the segment type of the segment containing the .dbl_text section between PT_NULL and PT_LOAD
# USAGE: ./toggle_segment_type.py <path/to/executable>

import sys
from elftools.elf.elffile import ELFFile

filename = sys.argv[1]
file = open(filename, "r+b")
elf = ELFFile(file)
section = elf.get_section_by_name(".dbl_text")

for (segment_idx, segment) in enumerate(elf.iter_segments()):
    if segment.section_in_segment(section): break

ehdr = elf.header
phoff = ehdr["e_phoff"]
e_phentsize = ehdr["e_phentsize"]
e_phnum = ehdr["e_phnum"]
assert(segment_idx < e_phnum)

offset = phoff + segment_idx * e_phentsize
file.seek(offset)
org = int.from_bytes(file.read(1), "little") #byteorder irrelevant
new = 0 if org else 1
file.seek(offset)
file.write(new.to_bytes(1, "little"))

print("Change program header type from " + ("PT_LOAD" if org else "PT_NULL") + " to " + ("PT_LOAD" if new else "PT_NULL"))

file.close()
