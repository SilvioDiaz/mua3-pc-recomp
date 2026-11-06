"""Translate every stored shader input (.in) of the MUA3 cache with two totk_xlate builds and compare.

  python xlate_diff.py <cache_dir> <old_exe> <new_exe> <work_dir>
"""
import json, os, struct, subprocess, sys, hashlib
from pathlib import Path

cache, old_exe, new_exe, work = Path(sys.argv[1]), sys.argv[2], sys.argv[3], Path(sys.argv[4])
SPIRV_VAL = r"C:\msys64\ucrt64\bin\spirv-val.exe"
CONTROL = 0x880
TEX = [None, "1d", "1darray", "buffer", "2d", "2darray", "2dms", "2dmsarray", "3d", "cube", "cubearray"]
FMT = [None, "R8Unorm", "R8Snorm", "R8Uint", "R8Sint", "R16Float", "R16Unorm", "R16Snorm", "R16Uint", "R16Sint", "R32Float",
       "R32Uint", "R32Sint", "R8G8Unorm", "R8G8Snorm", "R8G8Uint", "R8G8Sint", "R16G16Float", "R16G16Unorm", "R16G16Snorm",
       "R16G16Uint", "R16G16Sint", "R32G32Float", "R32G32Uint", "R32G32Sint", "R8G8B8A8Unorm", "R8G8B8A8Snorm",
       "R8G8B8A8Uint", "R8G8B8A8Sint", "R16G16B16A16Float", "R16G16B16A16Unorm", "R16G16B16A16Snorm", "R16G16B16A16Uint",
       "R16G16B16A16Sint", "R32G32B32A32Float", "R32G32B32A32Uint", "R32G32B32A32Sint", "R10G10B10A2Unorm",
       "R10G10B10A2Uint", "R11G11B10Float"]
STAGE = {0: "vs", 4: "ps", 5: "cs"}
attr = lambda t: "sint" if t == 1 else "uint" if t == 2 else "float"

codes = work / "code"; codes.mkdir(parents=True, exist_ok=True)
jobs = []
for f in sorted((cache / "in").glob("*.in")):
    b = f.read_bytes()
    magic, ver, stage, length, c1_len, asz = struct.unpack_from("<6I", b, 0)
    if magic != 0x49534B54 or asz != 156 or stage not in STAGE:
        continue
    a = b[24:24 + asz]; o = 24 + asz
    control = b[o:o + CONTROL]; code = b[o + CONTROL:o + CONTROL + length]; c1 = b[o + CONTROL + length:o + CONTROL + length + c1_len]
    st = STAGE[stage]; key = f.stem
    cp = codes / f"{key}.code"; cp.write_bytes(code)
    tex_t, img_t, img_f, at, fo = a[0:32], a[32:64], a[64:96], a[96:128], a[128:136]
    local = struct.unpack_from("<3H", a, 136); shared = struct.unpack_from("<I", a, 144)[0]
    alpha_func = a[150]; alpha_ref_bits = struct.unpack_from("<I", a, 152)[0]
    job = {"key": key, "stage": st, "debug": False, "code": str(cp), "c1": c1.hex(),
           "tex": {str(i): TEX[t] for i, t in enumerate(tex_t) if 0 < t < len(TEX)},
           "img": {str(i): {"type": TEX[t], "fmt": FMT[img_f[i]] if 0 < img_f[i] < len(FMT) else "R8G8B8A8Unorm"}
                   for i, t in enumerate(img_t) if 0 < t < len(TEX)},
           "attr": {str(i): attr(t) for i, t in enumerate(at) if t},
           "fout": {str(i): attr(t) for i, t in enumerate(fo) if t},
           "shared": shared}
    if alpha_func:
        job["alpha_test"] = alpha_func; job["alpha_ref_bits"] = alpha_ref_bits
    if st == "cs":
        job["local"] = list(local) if any(local) else list(struct.unpack_from("<3I", control, 0x748))
    if st == "ps" and control[0x718] & 1:
        job["early_fragment_tests"] = True
    jobs.append(job)


def run(exe, out):
    out.mkdir(parents=True, exist_ok=True)
    p = subprocess.run([exe, "--serve"], input="".join(json.dumps({"job": j, "out": str(out)}) + "\n" for j in jobs),
                       capture_output=True, text=True, encoding="utf-8")
    res = {}
    for line in p.stdout.splitlines():
        w = line.split(" ", 2)
        if len(w) >= 2:
            res[w[1]] = None if w[0] == "ok" else (w[2] if len(w) > 2 else "?")
    return res


ra = run(old_exe, work / "old"); rb = run(new_exe, work / "new")
by_stage = {}
changed, invalid, newfail, fixed = [], [], [], []
for j in jobs:
    k, st = j["key"], j["stage"]
    by_stage.setdefault(st, [0, 0]); by_stage[st][0] += 1
    ea, eb = ra.get(k, "no reply"), rb.get(k, "no reply")
    if ea is None and eb is not None: newfail.append((k, eb))
    if ea is not None and eb is None: fixed.append((k, ea))
    if eb is None:
        sa, sb = work / "old" / f"{k}.spv", work / "new" / f"{k}.spv"
        if not sa.exists() or sa.read_bytes() != sb.read_bytes():
            changed.append(k); by_stage[st][1] += 1
        v = subprocess.run([SPIRV_VAL, "--target-env", "vulkan1.3", str(sb)], capture_output=True, text=True)
        if v.returncode: invalid.append((k, (v.stdout + v.stderr).strip().splitlines()[:1]))
print(f"jobs {len(jobs)}  old ok {sum(v is None for v in ra.values())}  new ok {sum(v is None for v in rb.values())}")
print("changed SPIR-V by stage (total, changed):", by_stage)
print(f"changed {len(changed)}  new failures {len(newfail)}  fixed failures {len(fixed)}  spirv-val invalid (new) {len(invalid)}")
for x in newfail[:10]: print("NEWFAIL", x)
for x in invalid[:10]: print("INVALID", x)
(work / "changed.txt").write_text("\n".join(changed), newline="\n")
