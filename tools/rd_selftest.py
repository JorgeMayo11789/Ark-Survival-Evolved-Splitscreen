# Runs inside qrenderdoc's embedded Python: checks the renderdoc module is available, then exits.
# qrenderdoc doesn't set __file__: run it from the repo folder (the result goes to out\ under it).
import os
import sys
import traceback

OUT = os.path.join(os.getcwd(), "out", "rd_selftest.txt")
with open(OUT, "w") as f:
    f.write("script started, python %s\n" % sys.version.split()[0])
    try:
        import renderdoc as rd

        f.write("renderdoc module ok: %s\n" % ", ".join(n for n in dir(rd) if "Version" in n or n == "OpenCaptureFile"))
        f.write("pyrenderdoc global: %s\n" % ("pyrenderdoc" in globals()))
    except Exception:
        f.write(traceback.format_exc())
os._exit(0)
