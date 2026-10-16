"""Disassemble the full-frame pass's pixel shader (event 15894), describe its input textures and save them.

Run: qrenderdoc.exe --python <absolute path to this script>
"""
import os
import traceback

import renderdoc as rd

# qrenderdoc doesn't set __file__: run it from the repo folder (captures\ under it), or set ARK_CAPTURES
CAPTURES = os.environ.get("ARK_CAPTURES", os.path.join(os.getcwd(), "captures"))
CAPTURE = os.path.join(CAPTURES, "ark_frame6243.rdc")
OUT = os.path.join(CAPTURES, "fullframe_shader.txt")
PNG_DIR = CAPTURES
EVENT = 15894


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
            controller.SetFrameEvent(EVENT, True)
            state = controller.GetPipelineState()
            for s in state.GetReadOnlyResources(rd.ShaderStage.Pixel):
                rid = int(s.descriptor.resource)
                t = tex.get(rid)
                if t is not None:
                    out.write("srv %d: res %d %dx%d %s\n" % (s.access.index, rid, t.width, t.height, t.format.Name()))
                    if rid in (71696, 71693, 6961):
                        save = rd.TextureSave()
                        save.resourceId = t.resourceId
                        save.destType = rd.FileType.PNG
                        save.alpha = rd.AlphaMapping.Discard
                        path = os.path.join(PNG_DIR, "fullframe_input_%d.png" % rid)
                        out.write("  saved %s: %s\n" % (path, controller.SaveTexture(save, path)))
            for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
                refl = state.GetShaderReflection(stage)
                targets = controller.GetDisassemblyTargets(True)
                text = controller.DisassembleShader(state.GetGraphicsPipelineObject(), refl, targets[0])
                out.write("==== %s shader %d (%s) ====\n%s\n" % (stage, int(state.GetShader(stage)), targets[0], text))
            controller.Shutdown()
            cap.Shutdown()
        except Exception:
            out.write(traceback.format_exc())


main()
os._exit(0)
