"""Dump the action tree of an ARK capture (markers + draw calls) to a text file.

Run: qrenderdoc.exe --python <absolute path to this script>
Paths are absolute because qrenderdoc changes the working directory.
"""
import os
import sys
import traceback

import renderdoc as rd

# qrenderdoc doesn't set __file__: run it from the repo folder (captures\ under it), or set ARK_CAPTURES
CAPTURES = os.environ.get("ARK_CAPTURES", os.path.join(os.getcwd(), "captures"))
CAPTURE = os.path.join(CAPTURES, "ark_frame6243.rdc")
OUT = os.path.join(CAPTURES, "actions.txt")


def walk(controller, actions, depth, out, counter):
    for a in actions:
        counter[0] += 1
        name = a.GetName(controller.GetStructuredFile())
        flags = a.flags
        kind = "M" if (flags & rd.ActionFlags.PushMarker) else ("D" if (flags & rd.ActionFlags.Drawcall) else "-")
        out.write("%s%s %d %s\n" % ("  " * depth, kind, a.eventId, name))
        if a.children:
            walk(controller, a.children, depth + 1, out, counter)


def main():
    with open(OUT, "w", encoding="utf-8") as out:
        try:
            cap = rd.OpenCaptureFile()
            result = cap.OpenFile(CAPTURE, "", None)
            if result != rd.ResultCode.Succeeded:
                out.write("open failed: %s\n" % result)
                return
            result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
            if result != rd.ResultCode.Succeeded:
                out.write("replay failed: %s\n" % result)
                return
            counter = [0]
            walk(controller, controller.GetRootActions(), 0, out, counter)
            out.write("total actions: %d\n" % counter[0])
            controller.Shutdown()
            cap.Shutdown()
        except Exception:
            out.write(traceback.format_exc())


main()
os._exit(0)
