"""Scene colour snapshots around the lighting passes, and the inputs of the per-view ambient pass (15769 / 15773).

Run: qrenderdoc.exe --python <absolute path to this script>
"""
import os
import traceback

import renderdoc as rd

# qrenderdoc doesn't set __file__: run it from the repo folder (captures\ under it), or set ARK_CAPTURES
CAPTURES = os.environ.get("ARK_CAPTURES", os.path.join(os.getcwd(), "captures"))
CAPTURE = os.path.join(CAPTURES, "ark_frame6243.rdc")
OUT = os.path.join(CAPTURES, "fullframe_pass.txt")
PNG_DIR = CAPTURES
SCENE_COLOR = 6968
SNAPSHOTS = [15893, 15895, 16139, 16605, 19128]
COMPARE = [15894]


def fmt_var(v, depth=0):
    pad = "  " * depth
    if v.members:
        lines = ["%s%s:" % (pad, v.name)]
        for m in v.members:
            lines.extend(fmt_var(m, depth + 1))
        return lines
    count = v.rows * v.columns
    return ["%s%s = %s" % (pad, v.name, " ".join("%.5g" % v.value.f32v[i] for i in range(count)))]


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
            tex = {int(t.resourceId): t.resourceId for t in controller.GetTextures()}
            for ev in SNAPSHOTS:
                controller.SetFrameEvent(ev, True)
                save = rd.TextureSave()
                save.resourceId = tex[SCENE_COLOR]
                save.destType = rd.FileType.PNG
                save.alpha = rd.AlphaMapping.Discard
                save.comp.blackPoint = 0.0
                save.comp.whitePoint = 4.0
                path = os.path.join(PNG_DIR, "sc6968_%d.png" % ev)
                out.write("saved %s: %s\n" % (path, controller.SaveTexture(save, path)))
            for ev in COMPARE:
                controller.SetFrameEvent(ev, True)
                state = controller.GetPipelineState()
                vp = state.GetViewport(0)
                out.write("=== %d vp %.0f,%.0f %.0fx%.0f ===\n" % (ev, vp.x, vp.y, vp.width, vp.height))
                stage = rd.ShaderStage.Pixel
                shader = state.GetShader(stage)
                out.write(" ps %d\n" % int(shader))
                for s in state.GetReadOnlyResources(stage):
                    out.write("  srv %d -> %d\n" % (s.access.index, int(s.descriptor.resource)))
                refl = state.GetShaderReflection(stage)
                entry = state.GetShaderEntryPoint(stage)
                for i, cb in enumerate(refl.constantBlocks):
                    bind = state.GetConstantBlock(stage, i, 0)
                    variables = controller.GetCBufferVariableContents(
                        state.GetGraphicsPipelineObject(), shader, stage, entry, i, bind.descriptor.resource,
                        bind.descriptor.byteOffset, bind.descriptor.byteSize)
                    out.write("  cbuffer %d (res %d)\n" % (i, int(bind.descriptor.resource)))
                    for v in variables:
                        for line in fmt_var(v, 2):
                            out.write(line + "\n")
                blend = state.GetColorBlends()
                if blend:
                    b = blend[0]
                    out.write("  blend enabled %s color %s %s %s\n" % (
                        b.enabled, b.colorBlend.source, b.colorBlend.operation, b.colorBlend.destination))
            controller.Shutdown()
            cap.Shutdown()
        except Exception:
            out.write(traceback.format_exc())


main()
os._exit(0)
