#!/usr/bin/env python3

from __future__ import print_function

import os
import shutil
import sys
import subprocess
import platform
import optparse

def generate_project(project_name, folder_name, clean):
    build_dir = os.path.join("build", project_name)

    if clean and os.path.exists(build_dir):
        shutil.rmtree(build_dir)

    if not os.path.exists(build_dir):
        os.makedirs(build_dir)

    subprocess.call(["cmake", "-G", "Xcode", "-S", folder_name, "-B", build_dir])

parser = optparse.OptionParser()
parser.set_defaults(clean = False)
parser.add_option("--clean", "-c", action="store_true", dest="clean")

(options, args) = parser.parse_args()

generate_project("lrutrack", ".", options.clean)
generate_project("lrutest", "test", options.clean)
generate_project("lrutest32", "test32", options.clean)
