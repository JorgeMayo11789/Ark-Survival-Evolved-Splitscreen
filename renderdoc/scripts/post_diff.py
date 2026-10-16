"""Compare player 1's and player 2's post-process draws: bound textures and pixel-shader constants (differences
only), and read the 1x1 eye-adaptation textures.

Run: qrenderdoc.exe --python <absolute path to this script>
"""
import os
import traceback

import renderdoc as rd

# qrenderdoc doesn't set __file__: run it from the repo folder (captures\ under it), or set ARK_CAPTURES
CAPTURES = os.environ.get("ARK_CAPTURES", os.path.join(os.getcwd(), "captures"))
CAPTURE = os.path.join(CAPTURES, "ark_frame6243.rdc")
OUT = os.path.join(CAPTURES, "post_diff.txt")
PAIRS = [(19249, 19254), (19330, 19432), (19340, 19443), (19359, 19462), (19367, 19470), (19385, 19486),
         (19397, 19498), (19409, 19510), (19421, 19522)]
EYE = [(19368, 7004), (19471, 197193)]


def flat_vars(variables, prefix=""):
    result = []
    for v in variables:
        if v.members:
            result.extend(flat_vars(v.members, prefix + v.name + "."))
        else:
            count = v.rows * v.columns
            result.append((prefix + v.name, " ".join("%.5g" % v.value.f32v[i] for i in range(count))))
    return result


def stage_inputs(controller, ev):
    controller.SetFrameEvent(ev, True)
    state = controller.GetPipelineState()
    stage = rd.ShaderStage.Pixel
    shader = state.GetShader(stage)
    srvs = [(s.access.index, int(s.descriptor.resource)) for s in state.GetReadOnlyResources(stage)]
    consts = []
    refl = state.GetShaderReflection(stage)
    entry = state.GetShaderEntryPoint(stage)
    if refl is not None:
        for i, cb in enumerate(refl.constantBlocks):
            bind = state.GetConstantBlock(stage, i, 0)
            variables = controller.GetCBufferVariableContents(
                state.GetGraphicsPipelineObject(), shader, stage, entry, i, bind.descriptor.resource,
                bind.descriptor.byteOffset, bind.descriptor.byteSize)
            for name, value in flat_vars(variables):
                consts.append(("cb%d(res %d).%s" % (i, int(bind.descriptor.resource), name), value))
    return int(shader), srvs, consts


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
            for ev, rid in EYE:
                controller.SetFrameEvent(ev, True)
                v = controller.PickPixel(tex[rid], 0, 0, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
                out.write("eye adaptation %d at %d: %s\n" % (rid, ev, [v.floatValue[i] for i in range(4)]))
            for a, b in PAIRS:
                sa, ra, ca = stage_inputs(controller, a)
                sb, rb, cb = stage_inputs(controller, b)
                out.write("=== P1 %d vs P2 %d: ps %d / %d ===\n" % (a, b, sa, sb))
                out.write(" srv P1 %s\n srv P2 %s\n" % (ra, rb))
                da = dict(ca)
                db = dict(cb)
                for name, _ in ca:
                    if name not in db:
                        continue
                for (na, va), (nb, vb) in zip(ca, cb):
                    short_a = na.split(").", 1)[-1]
                    short_b = nb.split(").", 1)[-1]
                    if va != vb or short_a != short_b:
                        out.write("  %s = %s | %s = %s\n" % (na, va, nb, vb))
            controller.Shutdown()
            cap.Shutdown()
        except Exception:
            out.write(traceback.format_exc())


main()
os._exit(0)
