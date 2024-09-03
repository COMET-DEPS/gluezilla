#!/usr/bin/python3

import sys
import sqlite3

# USAGE: ./generate_victim_addresses.py <path/to/db.sqlite> <path/to/out_directory>

# Runs the `query.sql` query (in the scripts directory) on the given sqlite database and write the results to `victim_addresses.txt` in the output directory. 
# `victim_addresses.txt` is used by the compiler to search for suitable victim frames.


db_path = str(sys.argv[1])
output_directory = str(sys.argv[2])
db_query_path = "./query.sql"
output_path = output_directory + "/victim_addresses.txt"

output = open(output_path, "w")
db_query = open(db_query_path, "r")

db = sqlite3.connect(db_path)
db.text_factory = lambda b: b.decode(errors = 'ignore')
c = db.cursor()
q = db_query.read()
c.execute(q)
data = c.fetchall()

for row in data:
    victim = row[0]
    bit = row[1]
    sign = "+" if row[2] == 1 else "-"
    aggrs = row[3]
    aggr_init = hex(255)
    #try/except because the db connection reads something weird for 0xff?
    try:
        if int(row[4], 16) == 0:
            aggr_init = hex(0)
    except Exception:
        pass

    output.write(victim + " " + str(bit) + " " + sign + " " + aggrs + " " + aggr_init + "\n")

output.close()
c.close()
db.close()
