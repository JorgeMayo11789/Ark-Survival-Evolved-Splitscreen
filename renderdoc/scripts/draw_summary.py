"""One line per draw/dispatch/clear: event id, render targets, depth target, viewport, scissor, shaders.

Run: qrenderdoc.exe --python <absolute path to this script>
"""
import os
import traceback

import renderdoc as rd

# qrenderdoc doesn't set __file__: run it from the repo folder (captures\ under it), or set ARK_CAPTURES
CAPTURES = os.environ.get("ARK_CAPTURES", os.path.join(os.getcwd(), "captures"))
CAPTURE = os.path.join(CAPTURES, "ark_frame6243.rdc")
OUT = os.path.join(CAPTURES, "draws.txt")


def flatten(actions, result):
    for a in actions:
        if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch | rd.ActionFlags.Clear):
            result.append(a)
        if a.children:
            flatten(a.children, result)


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
            names = {}
            for res in controller.GetResources():
                names[int(res.resourceId)] = res.name
            texinfo = {}
            for t in controller.GetTextures():
                texinfo[int(t.resourceId)] = "%dx%d %s" % (t.width, t.height, t.format.Name())
            out.write("== textures ==\n")
            for rid, info in sorted(texinfo.items()):
                out.write("tex %d %s %s\n" % (rid, names.get(rid, ""), info))
            actions = []
            flatten(controller.GetRootActions(), actions)
            sf = controller.GetStructuredFile()
            out.write("== actions ==\n")
            for a in actions:
                controller.SetFrameEvent(a.eventId, False)
                state = controller.GetPipelineState()
                rts = [int(r.resource) for r in state.GetOutputTargets() if int(r.resource) != 0]
                depth = int(state.GetDepthTarget().resource)
                vps = state.GetViewport(0)
                sc = state.GetScissor(0)
                ps = int(state.GetShader(rd.ShaderStage.Pixel))
                vs = int(state.GetShader(rd.ShaderStage.Vertex))
                out.write("%d %s | rt %s depth %d | vp %.0f,%.0f %.0fx%.0f | sc %d,%d %dx%d | vs %d ps %d\n" % (
                    a.eventId, a.GetName(sf), rts, depth, vps.x, vps.y, vps.width, vps.height,
                    sc.x, sc.y, sc.width, sc.height, vs, ps))
            controller.Shutdown()
            cap.Shutdown()
        except Exception:
            out.write(traceback.format_exc())


main()
os._exit(0)
