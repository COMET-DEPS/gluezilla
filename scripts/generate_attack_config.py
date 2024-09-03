#!/usr/bin/python3

# USAGE: ./generate_attack_config.py <path/to/target_bin> <path/to/compiler_output_<id>.txt>

#Reads the final binary and the `compiler_output.txt` file in the same directory, and creates the `attack_config.toml` file in that directory.
#This file is used by the `loader` component at run time.


""" compiler output format:
#comment

[General]
file_path = path

#comment

[Layout]
sec_name1 section_offset1 phys_victim1 aggr1.1,aggr1.2 aggr_init
sec_name2 section_offset2 phys_victim2 aggr2.1,aggr2.2 aggr_init

"""

import sys
from tomlkit import comment, document, table, inline_table, array, dumps
from elftools.elf.elffile import ELFFile

test_bin_path = str(sys.argv[1])
compiler_output_path = str(sys.argv[2])
package_path = compiler_output_path.rsplit("/", 1)[0]
id = compiler_output_path[18:-4]
attack_config_path = package_path + "/attack_config_" + id + ".toml"
hammer_count = 1000000

null_segment_offset = 0
# sec_name -> [(offset_in_sec, phys_victim, [aggrs], aggr_init, expected_flip)]
layout = {}
# page_file_offset -> [(phys_victim_frame, (phys_victim_offset, phys_aggr, aggr_init, expected_bitflip))]
output = {}

# Read compiler output
compiler_output_file = open(compiler_output_path, "r")
lines = compiler_output_file.readlines()
group = ""

for line in lines:
    line.strip()

    if line[0] == "#" or len(line) == 0:
        continue

    elif line[0] == "[":
        group = line[1:len(line)-2]
        continue

    elif group == "General":
        s = [i.strip() for i in line.split("=")]

    elif group == "Layout":
        s = line.split(" ")
        sec_name = s[0]
        offset_in_sec = s[1]
        phys_victim = s[2]
        expected = s[3]
        aggrs = s[4].split(",")
        aggr_init = s[5].strip()
        if sec_name not in layout:
            layout[sec_name] = []
        layout[sec_name].append((offset_in_sec, phys_victim, aggrs, aggr_init,
                                 expected))

compiler_output_file.close()

# Read linker output
file = open(test_bin_path, "rb")
for segment in ELFFile(file).iter_segments():
    if segment["p_type"] == "PT_NULL":
        null_segment_offset = segment["p_vaddr"]

for section_name in layout.keys():
    section = ELFFile(file).get_section_by_name(section_name)
    section_offset = section["sh_offset"]
    for offset_in_sec, phys_victim, aggrs, aggr_init, expected in layout[section_name]:
        # apply mask using arithmetic
        page_file_offset = int(offset_in_sec, 16) - int(offset_in_sec, 16) % 0x1000 + section_offset
        offset = int(phys_victim, 16) % 0x1000
        frame_addr = int(phys_victim, 16) - offset
        if page_file_offset not in output:
            output[page_file_offset] = []
        else:
            #fails when the same file offset is mapped to different physical frames
            print(hex(page_file_offset) + " " + hex(output[page_file_offset][0][0]) + " " + hex(frame_addr))
            assert(output[page_file_offset][0][0] == frame_addr)
        #frame_addr duplicates
        output[page_file_offset].append((frame_addr, (offset, aggrs, aggr_init, expected)))

# Generate attack_config.toml
dict = {
    "hammer_count": hammer_count,
    "segment_virt_addr": hex(null_segment_offset),
    "victim_frames": [],
    "aggressor_patterns": {}
}

# Intermediate dictionary to build the aggr patterns without duplicates
# aggr_pattern -> aggr_pattern_id
# aggr_pattern is small in practice
aggressor_patterns_rev = {}
aggr_pattern_key = 0

for page_file_offset in output.keys():
    victim_frame = {
        "page_file_offset": hex(page_file_offset),
        "frame_addr": hex(output[page_file_offset][0][0]),
        "victim_bits": array()
    }

    for _, (offset, aggrs, aggr_init, expected) in output[page_file_offset]:
        aggr_pattern = (tuple(aggrs), aggr_init) #no support for aggressors with different init values
        if aggr_pattern not in aggressor_patterns_rev:
            aggressor_patterns_rev[aggr_pattern] = aggr_pattern_key
            aggr_pattern_key += 1
        t = inline_table()
        t.update({
            "offset": hex(offset),
            "bitflip": "'" + expected + "'", #use single quotes, see string comment below
            "aggr_pattern_key": "'" + str(aggressor_patterns_rev[aggr_pattern]) + "'"
        })
        victim_frame["victim_bits"].add_line(t);

    dict["victim_frames"].append(victim_frame)

for aggr_pattern in aggressor_patterns_rev.keys():
    key = str(aggressor_patterns_rev[aggr_pattern])
    assert(key not in dict["aggressor_patterns"])
    init = aggr_pattern[1]
    a = []
    for aggr in aggr_pattern[0]:
        a.append(f"{aggr}({init})")
    a = "'" + ",".join(a) + "'"
    dict["aggressor_patterns"][key] = a

with open(attack_config_path, 'w') as f:
    f.write(dumps(dict).replace("\"", ""))
    # replace double quotes because this toml lib doesn't support hex literals -_-
    # use single quotes for strings







