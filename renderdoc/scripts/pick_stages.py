"""Read a shadowed and a sunlit pixel in each player's half after each stage, to find where player 2's shadowed
areas start to differ from player 1's.

Run: qrenderdoc.exe --python <absolute path to this script>
"""
import os
import traceback

import renderdoc as rd

# qrenderdoc doesn't set __file__: run it from the repo folder (captures\ under it), or set ARK_CAPTURES
CAPTURES = os.environ.get("ARK_CAPTURES", os.path.join(os.getcwd(), "captures"))
CAPTURE = os.path.join(CAPTURES, "ark_frame6243.rdc")
OUT = os.path.join(CAPTURES, "pick_stages.txt")
# (label, x, y) in the 1808x1016 scene buffers
POINTS = [("P1 back", 900, 330), ("P1 sand", 300, 470), ("P1 rock shade", 520, 250),
          ("P2 back", 890, 840), ("P2 sand", 1200, 960), ("P2 rock shade", 900, 640)]
# (label, event after which to read, texture)
STAGES = [("lit 6903", 15774, 6903), ("6968 after 15894", 15895, 6968), ("6968 pre-translucency", 19128, 6968),
          ("6961 composite", 19255, 6961), ("TAA P1 197190", 19331, 197190), ("TAA P2 6980", 19433, 6980),
          ("7079 P1", 19386, 7079), ("7085 P1", 19398, 7085), ("7079b P1", 19410, 7079),
          ("7079 P2", 19487, 7079), ("7085 P2", 19499, 7085), ("7079b P2", 19511, 7079)]


def main():
    with open(OUT, "w", encoding="utf-8") as out:
        try:
            cap = rd.OpenCaptureFile()
            if cap.OpenFile(CAPTURE, "", None) != rd.ResultCode.Succeeded:
                out.write("open failed\n")
                return
            result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
            if result != rd.ResultCode.Succeeded:
                out.write("replay failed\n")
                return
            tex = {int(t.resourceId): t for t in controller.GetTextures()}
            for label, ev, rid in STAGES:
                controller.SetFrameEvent(ev, True)
                t = tex[rid]
                out.write("== %s (%dx%d) ==\n" % (label, t.width, t.height))
                for name, x, y in POINTS:
                    if x >= t.width or y >= t.height:
                        continue
                    v = controller.PickPixel(t.resourceId, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
                    out.write("  %-14s %.4f %.4f %.4f\n" % (name, v.floatValue[0], v.floatValue[1], v.floatValue[2]))
            controller.SetFrameEvent(21246, True)
            t = tex[6239]
            out.write("== final (%dx%d) ==\n" % (t.width, t.height))
            for name, x, y in POINTS:
                fx, fy = int(x * 1920 / 1808), int(y * 1080 / 1016)
                v = controller.PickPixel(t.resourceId, fx, fy, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
                out.write("  %-14s %.4f %.4f %.4f\n" % (name, v.floatValue[0], v.floatValue[1], v.floatValue[2]))
            controller.Shutdown()
            cap.Shutdown()
        except Exception:
            out.write(traceback.format_exc())


main()
os._exit(0)
