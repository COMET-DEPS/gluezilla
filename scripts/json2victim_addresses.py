#!/usr/bin/python3

import sys
import json
import re
from pathlib import Path

# USAGE: ./json2victim_addresses.py path/to/json X
# This script creates a victim_addresses.txt file from the json output of the small `templater` tool in `rowhammer_tools`
# Only the victims that occured more than arg:X times are included in the output
# Setting X to 1 will emit all discoved victims during the templating step
# The number of occurences for the victim that occured the most is printed to stdout

# IMPORTANT!!
# This script puts all data of all garbage count together, and then it select the ones that occured >=X times!

path = sys.argv[1]
threshold = int(sys.argv[2])

file = open(path, "r")
json_data = json.load(file)
file.close()

victim_locations = json_data["victims"]
aggressors = json_data["aggr_patterns"]
distribution = json_data["distribution"]

histogram = {}

for x in distribution.values():
    for y in x:
        for i in y:
            histogram[i] = histogram.get(i, 0) + 1

file = open(Path(path).parent / "victim_addresses.txt", "w")
max = 0
count = 0
for k,v in histogram.items():
    if v > max: max = v
    if v >= threshold:
        addr, bit, sign, aggr_idx = re.search(r"(0x[0-9a-f]+) (\d)([+-]) (\d+)", victim_locations[k]).groups()
        aggr_idx = int(aggr_idx)
        aggr1, aggr1_init, aggr2, aggr2_init = re.search(r"(0x[0-9a-f]+)\((0x[0f]+)\),(0x[0-9a-f]+)\((0x[0f]+)\)", aggressors[aggr_idx]).groups()
        assert(aggr1_init == aggr2_init)
        file.write(addr + " " + bit + " " + sign + " " + aggr1 + "," + aggr2 + " " + aggr1_init + "\n")
        count += 1
file.close()
#print("Max: " + str(max))
print(str(count))
