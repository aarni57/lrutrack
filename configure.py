#!/usr/bin/env python3

from __future__ import print_function

import os
import shutil
import sys
import subprocess
import platform
import optparse

def generate_project(clean):
    build_dir = "build"

    if clean and os.path.exists(build_dir):
        shutil.rmtree(build_dir)

    if not os.path.exists(build_dir):
        os.makedirs(build_dir)

    subprocess.call(["cmake", "-G", "Xcode", "-B", build_dir])

parser = optparse.OptionParser()
parser.set_defaults(clean = False)
parser.add_option("--clean", "-c", action="store_true", dest="clean")

(options, args) = parser.parse_args()

generate_project(options.clean)
