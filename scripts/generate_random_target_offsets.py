#!/usr/bin/python3

from elftools.elf.elffile import ELFFile
from pathlib import Path
from capstone import *
import numpy as np
import sys
from tomlkit import comment, document, table, inline_table, array, dumps

# This script generates a random target_offsets.toml file
# USAGE: ./generate_target_offsets.py <path/to/out_directory> <distribution_type> <number_of_fixed_flips> <total_number_of_flips> <list/of/paths/to/executable>
# `list/of/paths/to/executable` is a space seperated list of paths, it can be the result of a wildcard expansion
# Note: number_of_range_targets = total_number_of_targets - number_of_fixed_targets
# `total_number_of_targets` and `number_of_fixed_targets` can be expressed as absolute values or as percentage of the number of asm instructions, e.g.,
#    8 12 --> 12 targets in total of which 8 are fixed targets and 4 are range targets
#    60% 12 --> 12 targets in total, 60% * 12 (= 7) are fixed targets, the others are range targets
#    8 2% --> 2% * number-of-asm-instructions targets in total of which 8 are fixed targets, the others are range targets
#    10% 20% --> 20% * number-of-asm-instructions targets in total, 10% * total_number_of_targets are fixed targets, the other are range targets

output_dir = sys.argv[1]
dist_type = sys.argv[2]
splits = sys.argv[3]
tots = sys.argv[4]
file_paths = sys.argv[5:]

for file_path in file_paths:
    print(file_path)

    doc = document()
    t = inline_table()
    t.update({
        "type": "none",
        "offset": 0x0
    })
    a = array()
    a.add_line(t)

    rng = np.random.default_rng()
    file = open(file_path, "rb")
    section_name = ".dbl_text"
    elf_file = ELFFile(file)
    section = elf_file.get_section_by_name(section_name)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    section_code = section.data()
    disasm = [x for x in md.disasm(section_code, 0)] #0 bcs we need offset wrt start of section
    high = section["sh_size"]
    low = 1 #for simplicity, so the "I 0x0" should always be present in the output file
    print("low = " + str(hex(low)) + ", high = " + str(hex(high)))
    print("instructions = " + str(len(disasm)))

    if tots.endswith("%"):
        total = int(float(tots[:-1]) * len(disasm) / 100)
    else:
        total = int(tots)
    if splits.endswith("%"):
        split = int(float(splits[:-1]) * total / 100)
    else:
        split = int(splits)
    print("total = " + str(total) + ", fixed = " + str(split) + ", range = " + str(total-split))
    assert(len(disasm) >= total + 2 * (total - split))

    match dist_type:
        case "uniform": # [size]
            dist = rng.permutation(range(1, len(disasm)))
        #case "binomial": # [p, size]
        #    dist = rng.binomial(high, float(params[0]), int(params[1]))
        case other:
            print("Distribution type not supported")
            assert(False)

    #fixed
    for i in range(0, split):
        c = dist[i]
        instr = disasm[c]
        d = rng.integers(0, instr.size)
        t = inline_table()
        t.update({
            "type": "fixed",
            "offset": int(instr.address + d),
            "bit": int(rng.integers(0, 8)),
            "sign": ["+", "-"][rng.integers(0,2)]
        })
        a.add_line(t)

    #range
    tmp = array()
    for i in range(split, total):
        c = dist[i]
        instr = disasm[c]
        d = rng.integers(0, instr.size)
        t = inline_table()
        t.update({
            "type": "range",
            "start_offset": int(instr.address + d),
            "range": 4
        })
        tmp.add_line(t)

    for t in tmp:
        i += 1
        t.add("normal_dest", disasm[dist[i]].address)
        i += 1
        t.add("flipped_dest", disasm[dist[i]].address)
        a.add_line(t)

    #write toml
    doc.add("sections", [{
        "name": ".dbl_text",
        "values": a
    }])

    with open(output_dir + "/target_offsets_" + Path(file_path).stem.rsplit("_", 1)[0] + ".toml", 'w') as f:
        f.write(dumps(doc))

    file.close()

