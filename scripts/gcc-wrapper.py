#! /usr/bin/env python
# -*- coding: utf-8 -*-

# Copyright (c) 2011-2012, The Linux Foundation. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in the
#       documentation and/or other materials provided with the distribution.
#     * Neither the name of The Linux Foundation nor
#       the names of its contributors may be used to endorse or promote
#       products derived from this software without specific prior written
#       permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
# OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
# WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
# OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
# ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# Invoke gcc, looking for warnings, and causing a failure if there are
# non-whitelisted warnings.

import errno
import re
import os
import sys
import subprocess

# Note that gcc uses unicode, which may depend on the locale.  TODO:
# force LANG to be set to en_US.UTF-8 to get consistent warnings.

allowed_warnings = set([
   "alignment.c:327",
   "mmu.c:602",
   "return_address.c:62",
   "swab.h:49",
   "SemaLambda.cpp:946",
   "CGObjCGNU.cpp:1414",
   "BugReporter.h:146",
   "RegionStore.cpp:1904",
   "SymbolManager.cpp:484",
   "RewriteObjCFoundationAPI.cpp:737",
   "RewriteObjCFoundationAPI.cpp:696",
   "CommentParser.cpp:394",
   "CommentParser.cpp:391",
   "CommentParser.cpp:356",
   "LegalizeDAG.cpp:3646",
   "IRBuilder.h:844",
   "DataLayout.cpp:193",
   "transport.c:653",
   "xt_socket.c:307",
   "xt_socket.c:161",
   "inet_hashtables.h:356",
   "xc4000.c:1049",
   "xc4000.c:1063",	
   "f_qdss.c:586",
   "mipi_tc358764_dsi2lvds.c:746",
   "dynamic_debug.h:75",
   "hci_conn.c:407",
   "f_qdss.c:740",
   "mipi_novatek.c:569",
   "swab.h:34",
   "cpufreq_intelliactive.c:832",
   "cpufreq_intelliactive.c:875",
   "mxt224s.c:1892",
   "mdp4_video_enhance.c:172",
 ])

# Capture the name of the object file, can find it.
ofile = None

warning_re = re.compile(r'''(.*/|)([^/]+\.[a-z]+:\d+):(\d+:)? warning:''')
def interpret_warning(line):
    """Decode the message from gcc.  The messages we care about have a filename, and a warning"""
    line = line.rstrip('\n')
    m = warning_re.match(line)
    if m and m.group(2) not in allowed_warnings:
        print >> sys.stderr, "error, forbidden warning:", m.group(2)

        # If there is a warning, remove any object if it exists.
        if ofile:
            try:
                os.remove(ofile)
            except OSError:
                pass
        sys.exit(1)

def run_gcc():
    args = sys.argv[1:]
    # Look for -o
    try:
        i = args.index('-o')
        global ofile
        ofile = args[i+1]
    except (ValueError, IndexError):
        pass

    compiler = sys.argv[0]

    try:
        proc = subprocess.Popen(args, stderr=subprocess.PIPE)
        for line in proc.stderr:
            print >> sys.stderr, line,
            interpret_warning(line)

        result = proc.wait()
    except OSError as e:
        result = e.errno
        if result == errno.ENOENT:
            print >> sys.stderr, args[0] + ':',e.strerror
            print >> sys.stderr, 'Is your PATH set correctly?'
        else:
            print >> sys.stderr, ' '.join(args), str(e)

    return result

if __name__ == '__main__':
    status = run_gcc()
    sys.exit(status)
