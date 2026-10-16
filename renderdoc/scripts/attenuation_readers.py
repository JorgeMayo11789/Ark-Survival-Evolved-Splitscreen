"""Find every action that reads the light-attenuation buffer (as a shader resource in any stage) and dump the
inputs of those actions: shader ids, viewport, bound resources and constants.

Run: qrenderdoc.exe --python <absolute path to this script>
"""
import os
import traceback

import renderdoc as rd

# qrenderdoc doesn't set __file__: run it from the repo folder (captures\ under it), or set ARK_CAPTURES
CAPTURES = os.environ.get("ARK_CAPTURES", os.path.join(os.getcwd(), "captures"))
CAPTURE = os.path.join(CAPTURES, "ark_frame6243.rdc")
OUT = os.path.join(CAPTURES, "attenuation_readers.txt")
ATTENUATION = 71613
FIRST, LAST = 15600, 21246
STAGES = [rd.ShaderStage.Vertex, rd.ShaderStage.Pixel, rd.ShaderStage.Compute]


def flatten(actions, result):
    for a in actions:
        if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch):
            result.append(a)
        if a.children:
            flatten(a.children, result)


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
            actions = []
            flatten(controller.GetRootActions(), actions)
            sf = controller.GetStructuredFile()
            for a in actions:
                if a.eventId < FIRST or a.eventId > LAST:
                    continue
                controller.SetFrameEvent(a.eventId, False)
                state = controller.GetPipelineState()
                hit_stages = []
                for stage in STAGES:
                    if int(state.GetShader(stage)) == 0:
                        continue
                    for s in state.GetReadOnlyResources(stage):
                        if int(s.descriptor.resource) == ATTENUATION:
                            hit_stages.append(stage)
                if not hit_stages:
                    continue
                vp = state.GetViewport(0)
                out.write("=== %d %s vp %.0f,%.0f %.0fx%.0f stages %s ===\n" % (
                    a.eventId, a.GetName(sf), vp.x, vp.y, vp.width, vp.height, [str(s) for s in hit_stages]))
                for stage in hit_stages:
                    shader = state.GetShader(stage)
                    out.write(" stage %s shader %d\n" % (stage, int(shader)))
                    for s in state.GetReadOnlyResources(stage):
                        out.write("  srv %d -> %d\n" % (s.access.index, int(s.descriptor.resource)))
                    refl = state.GetShaderReflection(stage)
                    entry = state.GetShaderEntryPoint(stage)
                    for i, cb in enumerate(refl.constantBlocks):
                        bind = state.GetConstantBlock(stage, i, 0)
                        variables = controller.GetCBufferVariableContents(
                            state.GetGraphicsPipelineObject() if stage != rd.ShaderStage.Compute
                            else state.GetComputePipelineObject(),
                            shader, stage, entry, i, bind.descriptor.resource, bind.descriptor.byteOffset,
                            bind.descriptor.byteSize)
                        out.write("  cbuffer %d (res %d)\n" % (i, int(bind.descriptor.resource)))
                        for v in variables:
                            for line in fmt_var(v, 2):
                                out.write(line + "\n")
            controller.Shutdown()
            cap.Shutdown()
        except Exception:
            out.write(traceback.format_exc())


main()
os._exit(0)
