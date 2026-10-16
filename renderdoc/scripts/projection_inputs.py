"""Shadow projection inputs per view: pixel/vertex shader constants and bound textures for the projection draws,
plus the light-attenuation buffer saved as PNG after all projections.

Run: qrenderdoc.exe --python <absolute path to this script>
"""
import os
import traceback

import renderdoc as rd

# qrenderdoc doesn't set __file__: run it from the repo folder (captures\ under it), or set ARK_CAPTURES
CAPTURES = os.environ.get("ARK_CAPTURES", os.path.join(os.getcwd(), "captures"))
CAPTURE = os.path.join(CAPTURES, "ark_frame6243.rdc")
OUT = os.path.join(CAPTURES, "projection_inputs.txt")
PNG_DIR = CAPTURES
EVENTS = [13868, 14408, 15023, 15592]  # P1, P2, P2, P1 projection draws
ATTENUATION = 71613
SAVE_AT = [13870, 14410, 15025, 15594, 15700]  # attenuation after each projection, and after all


def fmt_var(v, depth=0):
    lines = []
    pad = "  " * depth
    if v.members:
        lines.append("%s%s:" % (pad, v.name))
        for m in v.members:
            lines.extend(fmt_var(m, depth + 1))
    else:
        count = v.rows * v.columns
        vals = [v.value.f32v[i] for i in range(count)]
        lines.append("%s%s = %s" % (pad, v.name, " ".join("%.5g" % x for x in vals)))
    return lines


def dump_stage(controller, state, stage, out):
    refl = state.GetShaderReflection(stage)
    if refl is None:
        out.write("  (no %s shader)\n" % stage)
        return
    entry = state.GetShaderEntryPoint(stage)
    for i, cb in enumerate(refl.constantBlocks):
        bind = state.GetConstantBlock(stage, i, 0)
        variables = controller.GetCBufferVariableContents(
            state.GetGraphicsPipelineObject(), state.GetShader(stage), stage, entry, i, bind.descriptor.resource,
            bind.descriptor.byteOffset, bind.descriptor.byteSize)
        out.write("  cbuffer %s (res %d):\n" % (cb.name, int(bind.descriptor.resource)))
        for v in variables:
            for line in fmt_var(v, 2):
                out.write(line + "\n")
    srvs = state.GetReadOnlyResources(stage)
    for s in srvs:
        out.write("  srv slot %d -> res %d\n" % (s.access.index, int(s.descriptor.resource)))
    for i, r in enumerate(refl.readOnlyResources):
        out.write("  srv name %d: %s (bind %d)\n" % (i, r.name, r.fixedBindNumber))


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
            for ev in EVENTS:
                controller.SetFrameEvent(ev, True)
                state = controller.GetPipelineState()
                vp = state.GetViewport(0)
                out.write("=== event %d viewport %.0f,%.0f %.0fx%.0f ===\n" % (ev, vp.x, vp.y, vp.width, vp.height))
                out.write(" VS\n")
                dump_stage(controller, state, rd.ShaderStage.Vertex, out)
                out.write(" PS\n")
                dump_stage(controller, state, rd.ShaderStage.Pixel, out)
            for ev in SAVE_AT:
                controller.SetFrameEvent(ev, True)
                save = rd.TextureSave()
                save.resourceId = rd.ResourceId.Null()
                for t in controller.GetTextures():
                    if int(t.resourceId) == ATTENUATION:
                        save.resourceId = t.resourceId
                save.destType = rd.FileType.PNG
                path = os.path.join(PNG_DIR, "attenuation_after_%d.png" % ev)
                res = controller.SaveTexture(save, path)
                out.write("saved %s: %s\n" % (path, res))
            controller.Shutdown()
            cap.Shutdown()
        except Exception:
            out.write(traceback.format_exc())


main()
os._exit(0)
