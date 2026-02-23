# Small script that reads flasher_args.json to create a single
# flashable image that can be used for 'M997 S1'.

import json
import argparse
import os
import subprocess

argparser = argparse.ArgumentParser()
argparser.add_argument("build_system", type=str)
argparser.add_argument("build_dir", type=str)
argparser.add_argument("output", type=str)

args = argparser.parse_args()

bins = []

if args.build_system == "cmake":
    with open(os.path.join(args.build_dir, "flasher_args.json"), "r") as flasher_args:
        contents = json.load(flasher_args)
        try:
            partition_table = contents["partition_table"]
        except KeyError:
            partition_table = contents["partition-table"]
        app = contents["app"]
        bootloader = contents["bootloader"]
        bins.extend([partition_table, app, bootloader])

    for b in bins:
        b["offset"] = int(b["offset"], 16)
    bins.sort(key=lambda b: b["offset"])

else:
    with open(os.path.join(args.build_dir, "flasher_args")) as flasher_args:
        contents = flasher_args.read().split()
        contents_i = iter(contents)

        for c in contents_i:
            try:
                bin = dict()
                bin["offset"] = int(c, 16)
                c = next(contents_i)
                bin["file"] = c
                bins.append(bin)
            except:
                pass

    bins.sort(key=lambda b: b["offset"])

with open(args.output, 'wb') as imgfile:
    pos = 0
    img = b''

    for b in bins:
        fill = b["offset"] - pos
        img += b'\xFF' * fill

        if args.build_system == "cmake":
            bfile = open(os.path.join(args.build_dir, b["file"]), "rb")
        else:
            if os.name == 'nt':
                bfilepath = subprocess.check_output(["cygpath", "-m", b["file"]]).strip(b"\n").decode()
            else:
                bfilepath = b["file"]
            bfile = open(bfilepath, "rb")

        bcontent = bfile.read()
        img += bcontent
        pos += fill + len(bcontent)
        bfile.close()

    imgfile.write(img)