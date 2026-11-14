#!/usr/bin/env python3
import os
import sys
import shutil
import subprocess
import re
import struct
import lz4.block
import UnityPy

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)


def resolve_tool(env_vars, candidate_names):
    for var in env_vars:
        val = os.environ.get(var)
        if val and os.path.isfile(val) and os.access(val, os.X_OK):
            return val
    search_dirs = [
        os.path.join(REPO_ROOT, 'build'),
        SCRIPT_DIR,
        REPO_ROOT,
        os.path.dirname(REPO_ROOT),
    ]
    for d in search_dirs:
        for name in candidate_names:
            p = os.path.join(d, name)
            if os.path.isfile(p) and os.access(p, os.X_OK):
                return p
    for name in candidate_names:
        found = shutil.which(name)
        if found:
            return found
    return None


VKD3D_PATH = resolve_tool(('DXBC2SPV_PATH', 'VKD3D_PATH'), ('dxbc2spv', 'test_vkd3d'))
SPIRV_CROSS_PATH = resolve_tool(('SPIRV_CROSS_PATH',), ('spirv-cross',))

def parse_sgn(dxbc_bytes, chunk_name):
    pos = dxbc_bytes.find(chunk_name.encode())
    if pos == -1:
        return {}
    num_entries = struct.unpack_from('<I', dxbc_bytes, pos + 8)[0]
    entries = {}
    for i in range(num_entries):
        e_start = pos + 16 + i * 24
        name_off, sem_idx, sys_val, comp_type, reg, mask, rw_mask = struct.unpack_from('<IIIIIBBx', dxbc_bytes, e_start)
        sem_name = dxbc_bytes[pos + 8 + name_off : dxbc_bytes.find(b'\x00', pos + 8 + name_off)].decode()
        entries[reg] = f'in_{sem_name}{sem_idx}'
    return entries

def transpile_dxbc_to_glsl(dxbc_bytes, sp, name_map, is_vert, shader_name=""):
    tmp_dxbc = f'/tmp/sub_{os.getpid()}_{id(dxbc_bytes)}.dxbc'
    tmp_spv = f'/tmp/sub_{os.getpid()}_{id(dxbc_bytes)}.spv'
    try:
        with open(tmp_dxbc, 'wb') as f:
            f.write(dxbc_bytes)
        res = subprocess.run([VKD3D_PATH, tmp_dxbc, tmp_spv], capture_output=True)
        if res.returncode != 0:
            return None
        res2 = subprocess.run([SPIRV_CROSS_PATH, tmp_spv, '--version', '150', '--glsl-emit-ubo-as-plain-uniforms'], capture_output=True, text=True)
        if res2.returncode != 0:
            return None
        code = res2.stdout
    finally:
        if os.path.exists(tmp_dxbc):
            os.remove(tmp_dxbc)
        if os.path.exists(tmp_spv):
            os.remove(tmp_spv)

    # Strip version, extension, and preprocessor directives
    lines = []
    for line in code.splitlines():
        if line.startswith('#version') or line.startswith('#extension') or 'GL_ARB_shading_language_420pack' in line or line.startswith('#ifdef') or line.startswith('#endif'):
            continue
        lines.append(line)
    code = '\n'.join(lines)

    # 1. Texture sampler mapping
    if sp and hasattr(sp, 'm_TextureParams'):
        for t in sp.m_TextureParams:
            t_name = name_map.get(t.m_NameIndex, f'_Tex{t.m_Index}')
            code = re.sub(rf'\bSPIRV_Cross_Combinedt{t.m_Index}s[0-9]+\b', t_name, code)
    # Generic fallback
    code = re.sub(r'\bSPIRV_Cross_Combinedt0s[0-9]+\b', '_MainTex', code)

    # Fallback for unbound _FaceTex and _OutlineTex in TextMeshPro
    if not is_vert and ('TextMesh' in shader_name or 'Distance' in shader_name):
        code = re.sub(r'texture\(_FaceTex,\s*[^)]+\)', 'vec4(1.0)', code)
        code = re.sub(r'texture\(_OutlineTex,\s*[^)]+\)', 'vec4(1.0)', code)

    # NOTE: Unity GLCore uploads Alpha8 as R8 with swizzle (0,0,0,R), so .w already holds the value; no fix needed.

    # Sample SDF atlas via manual bilinear texelFetch (independent of sampler/mip state)
    if not is_vert and 'Distance Field' in shader_name:
        code = re.sub(r'(?:texture|textureLod)\(_MainTex,\s*([^;]*?)(?:,\s*0\.0)?\);', r'sdfBilinear(\1);', code)
        func = (
            'vec4 sdfBilinear(vec2 uv) {\n'
            '    ivec2 sz = textureSize(_MainTex, 0);\n'
            '    vec2 p = uv * vec2(sz) - 0.5;\n'
            '    ivec2 i0 = ivec2(floor(p));\n'
            '    vec2 f = fract(p);\n'
            '    ivec2 mx = sz - 1;\n'
            '    vec4 a = texelFetch(_MainTex, clamp(i0, ivec2(0), mx), 0);\n'
            '    vec4 b = texelFetch(_MainTex, clamp(i0 + ivec2(1,0), ivec2(0), mx), 0);\n'
            '    vec4 c = texelFetch(_MainTex, clamp(i0 + ivec2(0,1), ivec2(0), mx), 0);\n'
            '    vec4 d = texelFetch(_MainTex, clamp(i0 + ivec2(1,1), ivec2(0), mx), 0);\n'
            '    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);\n'
            '}\n'
        )
        code = code.replace('void main()', func + 'void main()', 1)

    # TMP: denominator is (1.0 + softness) with softness = r.y*scale; transpiler produced r.y*(scale+1)
    if not is_vert and 'Distance Field' in shader_name:
        code = re.sub(r'(r\d)\.z = \1\.y \* (?:vary_|v)3\.y;\s*\n\s*\1\.y \*= (?:vary_|v)3\.y \+ 1\.0;',
                      r'\1.z = \1.y * v3.y;\n    \1.y = \1.z + 1.0;', code)

    # Guard inversesqrt in VS to prevent NaN or Inf, and fix D3D11 ComputeScreenPos/ComputeGrabScreenPos Y-flip for OpenGL
    if is_vert:
        code = re.sub(r'inversesqrt\((r[0-9]+\.w)\)', r'inversesqrt(max(\1, 0.00001))', code)
        code = code.replace('vec3(0.5, 0.5, -0.5)', 'vec3(0.5, 0.5, 0.5)')

    # 2. Constant buffer mapping with precise component packing
    cb_sizes = {}
    for m in re.finditer(r'struct\s+cb([0-9]+)_struct(?:_[0-9]+)?\s*\{\s*vec4\s+_m0\[([0-9]+)\];\s*\};\s*uniform\s+cb\1_struct(?:_[0-9]+)?\s+cb\1_([0-9]+);', code):
        cb_num = int(m.group(1))
        size = int(m.group(2))
        cb_sizes[cb_num] = max(cb_sizes.get(cb_num, 0), size)

    # Strip spirv-cross struct cb and uniform cb declarations
    code = re.sub(r'struct\s+cb[0-9]+_struct(?:_[0-9]+)?\s*\{[^}]*\};', '', code)
    code = re.sub(r'uniform\s+cb[0-9]+_struct(?:_[0-9]+)?\s+cb[0-9]+_[0-9]+;', '', code)

    # Replace cbN_N._m0 with cbN_N_m0
    code = re.sub(r'\bcb([0-9]+)_([0-9]+)\._m0\b', r'cb\1_\2_m0', code)

    uniform_decls = []
    declared_uniforms = set()
    pack_stmts = []

    if sp and hasattr(sp, 'm_ConstantBuffers'):
        for cb_idx, cb in enumerate(sp.m_ConstantBuffers):
            cb_var = f'cb{cb_idx}_{cb_idx}_m0'
            req_size = cb.m_Size // 16
            if cb_sizes.get(cb_idx, 0) < req_size:
                cb_sizes[cb_idx] = req_size

            # Matrices
            for m in cb.m_MatrixParams:
                m_name = name_map.get(m.m_NameIndex, f'mtx_{m.m_NameIndex}')
                full_name = f'hlslcc_mtx{m.m_RowCount}x4{m_name}'
                if full_name not in declared_uniforms:
                    uniform_decls.append(f'uniform vec4 {full_name}[{m.m_RowCount}];')
                    declared_uniforms.add(full_name)
                start_vec = m.m_Index // 16
                for r in range(m.m_RowCount):
                    pack_stmts.append(f'    {cb_var}[{start_vec + r}] = {full_name}[{r}];')

            # Vectors and scalars
            for v in cb.m_VectorParams:
                v_name = name_map.get(v.m_NameIndex, f'vec_{v.m_NameIndex}')
                dim = getattr(v, 'm_Dim', 4)
                type_name = 'float' if dim == 1 else f'vec{dim}'
                if v_name not in declared_uniforms:
                    uniform_decls.append(f'uniform {type_name} {v_name};')
                    declared_uniforms.add(v_name)
                start_vec = v.m_Index // 16
                comp = (v.m_Index % 16) // 4
                if dim == 1:
                    c_name = ['x', 'y', 'z', 'w'][comp]
                    pack_stmts.append(f'    {cb_var}[{start_vec}].{c_name} = {v_name};')
                elif dim == 2:
                    swz = 'xy' if comp == 0 else 'zw'
                    pack_stmts.append(f'    {cb_var}[{start_vec}].{swz} = {v_name};')
                elif dim == 3:
                    pack_stmts.append(f'    {cb_var}[{start_vec}].xyz = {v_name};')
                else:
                    pack_stmts.append(f'    {cb_var}[{start_vec}] = {v_name};')

    arr_decls = []
    for cb_num, size in cb_sizes.items():
        arr_decls.append(f'vec4 cb{cb_num}_{cb_num}_m0[{size}];')

    init_func = 'void init_cb() {\n' + '\n'.join(pack_stmts) + '\n}'
    code = init_func + '\n' + code
    code = re.sub(r'\bvoid\s+main\s*\(\s*\)\s*\{', 'void main() {\n    init_cb();', code)

    # 3. Exact vertex attribute names from ISGN, fragment outputs, and varyings
    if is_vert:
        sgn = parse_sgn(dxbc_bytes, 'ISGN')
        for reg, in_name in sgn.items():
            code = re.sub(rf'layout\(location\s*=\s*{reg}\)\s*in\s+(vec[234]|float)\s+v{reg};', rf'in \1 {in_name};', code)
            code = re.sub(rf'\bin\s+(vec[234]|float)\s+v{reg};', rf'in \1 {in_name};', code)
            code = re.sub(rf'\bv{reg}\b', in_name, code)
        code = re.sub(r'layout\(location\s*=\s*([0-9]+)\)\s*in\s+(vec[234]|float)\s+v([0-9]+);', r'in \1 in_ATTR\2;', code)
        code = re.sub(r'\bin\s+(vec[234]|float)\s+v([0-9]+);', r'in \1 in_ATTR\2;', code)
        code = re.sub(r'\bv([0-9]+)\b', r'in_ATTR\1', code)
        # Varying outputs
        code = re.sub(r'layout\(location\s*=\s*([0-9]+)\)\s*out\s+(vec[234]|float)\s+o([0-9]+);', r'out \1 vary_\2;', code)
        code = re.sub(r'\bout\s+(vec[234]|float)\s+o([0-9]+);', r'out \1 vary_\2;', code)
        code = re.sub(r'\bo([0-9]+)\b', r'vary_\1', code)
    else:
        # Varying inputs
        code = re.sub(r'layout\(location\s*=\s*([0-9]+)\)\s*in\s+(vec[234]|float)\s+v([0-9]+);', r'in \1 vary_\2;', code)
        code = re.sub(r'\bin\s+(vec[234]|float)\s+v([0-9]+);', r'in \1 vary_\2;', code)
        code = re.sub(r'\bv([0-9]+)\b', r'vary_\1', code)
        # Fragment outputs
        code = re.sub(r'layout\(location\s*=\s*0\)\s*out\s+vec4\s+o0;', 'out vec4 SV_Target0;', code)
        code = re.sub(r'out\s+vec4\s+o0;', 'out vec4 SV_Target0;', code)
        code = re.sub(r'\bo0\b', 'SV_Target0', code)
        code = re.sub(r'layout\(location\s*=\s*([0-9]+)\)\s*out\s+vec4\s+o([0-9]+);', r'out vec4 SV_Target\1;', code)
        code = re.sub(r'\bo([0-9]+)\b', r'SV_Target\1', code)

    # Optional diagnostic build: MAI_DEBUG=1 shows SDF sample / thresholds as colour, no discard
    if os.environ.get('MAI_DEBUG') and not is_vert and 'Distance Field' in shader_name:
        code = re.sub(r'_discard\([^;]*\);', '', code)
        code = code.replace('r0.w = (r0.y == 0.0 && r0.z == 0.0) ? r0.x : r0.w;',
                            'r0.w = (r0.y == 0.0 && r0.z == 0.0) ? r0.x : r0.w;\n    dbgd = r0.w;')
        code = re.sub(r'SV_Target0 = r0 \* vary_1\.wwww;',
                      'SV_Target0 = vec4(step(4.0, vary_3.y), step(8.0, vary_3.y), step(16.0, vary_3.y), 1.0);', code)
        arr_decls.append('float dbgd;')

    decl_str = '\n'.join(uniform_decls) + '\n' + '\n'.join(arr_decls)
    return (decl_str + '\n' + code).strip()

def pack_subprogram(glsl_str):
    if not glsl_str:
        return struct.pack('<8I', 201802150, 6, 0, 0, 0, 0, 0, 0) + struct.pack('<4I', 0, 0, 0, 0)
    glsl_b = glsl_str.encode('utf-8')
    code_len = len(glsl_b)
    pad_len = (4 - (code_len % 4)) % 4
    padded_code = glsl_b + b'\x00' * pad_len
    header = struct.pack('<8I', 201802150, 6, 0, 0, 0, 0, 0, code_len)
    tail = struct.pack('<4I', 0, 0, 0, 0)
    return header + padded_code + tail

def patch_asset_file(fpath):
    if not os.path.exists(fpath):
        return
    print(f"[*] Processing {fpath}...")
    bak_path = fpath + ".orig"
    if not os.path.exists(bak_path):
        shutil.copy2(fpath, bak_path)

    env = UnityPy.load(bak_path)
    patched_shaders = 0

    for obj in env.objects:
        if obj.type.name != 'Shader':
            continue
        try:
            data = obj.read()
        except Exception:
            continue

        platforms = [p.value if hasattr(p, 'value') else p for p in data.platforms]
        if 4 not in platforms:
            continue

        p4_idx = platforms.index(4)
        blob = bytes(data.compressedBlob)
        off = data.offsets[p4_idx]
        c_len = data.compressedLengths[p4_idx]
        d_len = data.decompressedLengths[p4_idx]
        try:
            decomp = lz4.block.decompress(blob[off:off+c_len], uncompressed_size=d_len)
        except Exception:
            continue

        shader_name = data.m_ParsedForm.m_Name if (hasattr(data, 'm_ParsedForm') and data.m_ParsedForm) else ""
        vert_to_frag = {}
        vert_indices = set()
        frag_indices = set()
        sub_info = {}
        if hasattr(data, 'm_ParsedForm') and data.m_ParsedForm:
            for sub in data.m_ParsedForm.m_SubShaders:
                for p in sub.m_Passes:
                    name_map = dict((idx, name) for name, idx in p.m_NameIndices)
                    v_subs = p.progVertex.m_SubPrograms if (hasattr(p, 'progVertex') and p.progVertex) else []
                    f_subs = p.progFragment.m_SubPrograms if (hasattr(p, 'progFragment') and p.progFragment) else []
                    for sp in v_subs:
                        vert_indices.add(sp.m_BlobIndex)
                        sub_info[sp.m_BlobIndex] = (name_map, sp)
                    for sp in f_subs:
                        frag_indices.add(sp.m_BlobIndex)
                        sub_info[sp.m_BlobIndex] = (name_map, sp)
                    for i, vs in enumerate(v_subs):
                        matched = None
                        # 1. Exact keyword match
                        for fs in f_subs:
                            if set(fs.m_KeywordIndices) == set(vs.m_KeywordIndices):
                                matched = fs.m_BlobIndex
                                break
                        # 2. Subset keyword match
                        if matched is None:
                            for fs in f_subs:
                                if set(fs.m_KeywordIndices).issubset(set(vs.m_KeywordIndices)):
                                    matched = fs.m_BlobIndex
                                    break
                        # 3. Fallback to index modulo
                        if matched is None and len(f_subs) > 0:
                            matched = f_subs[i % len(f_subs)].m_BlobIndex
                        vert_to_frag[vs.m_BlobIndex] = matched

        num_subs = struct.unpack_from('<I', decomp, 0)[0]
        dxbc_subs = []
        for s in range(num_subs):
            e_off, e_len = struct.unpack_from('<II', decomp, 4 + s * 8)
            s_data = decomp[e_off:e_off+e_len]
            dxbc_pos = s_data.find(b'DXBC')
            if dxbc_pos != -1:
                size = struct.unpack_from('<I', s_data, dxbc_pos + 24)[0]
                dxbc_subs.append(s_data[dxbc_pos:dxbc_pos+size])
            else:
                dxbc_subs.append(None)

        # Transpile all DXBC blobs
        glsl_cache = {}
        for s_idx, dxbc in enumerate(dxbc_subs):
            if dxbc is None:
                continue
            is_vert = (s_idx in vert_indices)
            info = sub_info.get(s_idx)
            if info:
                name_map, sp = info
            else:
                name_map, sp = {}, None
            glsl = transpile_dxbc_to_glsl(dxbc, sp, name_map, is_vert, shader_name)
            if glsl:
                glsl_cache[s_idx] = glsl

        gl_header = (
            "#version 330\n"
            "#extension GL_ARB_explicit_attrib_location : enable\n"
            "#extension GL_ARB_separate_shader_objects : enable\n"
            "#extension GL_ARB_shader_bit_encoding : enable\n"
            "#extension GL_ARB_shading_language_420pack : enable\n\n"
        )

        new_entries = []
        for s_idx in range(num_subs):
            if s_idx in vert_indices:
                v_glsl = glsl_cache.get(s_idx, "")
                f_idx = vert_to_frag.get(s_idx)
                f_glsl = glsl_cache.get(f_idx, "") if f_idx is not None else ""
                combined = (
                    f"#ifdef VERTEX\n{gl_header}{v_glsl}\n#endif\n"
                    f"#ifdef FRAGMENT\n{gl_header}{f_glsl}\n#endif\n"
                )
                new_entries.append(pack_subprogram(combined))
            else:
                new_entries.append(pack_subprogram(None))

        table_size = 4 + len(new_entries) * 8
        cur_offset = table_size
        offsets = []
        for entry in new_entries:
            offsets.append((cur_offset, len(entry)))
            cur_offset += len(entry)

        new_decomp = bytearray(struct.pack('<I', len(new_entries)))
        for o, l in offsets:
            new_decomp.extend(struct.pack('<II', o, l))
        for entry in new_entries:
            new_decomp.extend(entry)

        comp_p15 = lz4.block.compress(bytes(new_decomp), mode='high_compression', store_size=False)

        if 15 in platforms:
            p15_idx = platforms.index(15)
            data.offsets[p15_idx] = len(data.compressedBlob)
            data.compressedLengths[p15_idx] = len(comp_p15)
            data.decompressedLengths[p15_idx] = len(new_decomp)
        else:
            data.platforms.append(15)
            data.offsets.append(len(data.compressedBlob))
            data.compressedLengths.append(len(comp_p15))
            data.decompressedLengths.append(len(new_decomp))
        data.compressedBlob.extend(list(comp_p15))

        if hasattr(data, 'm_ParsedForm') and data.m_ParsedForm:
            for sub in data.m_ParsedForm.m_SubShaders:
                for p in sub.m_Passes:
                    if hasattr(p, 'progVertex') and p.progVertex:
                        for s in p.progVertex.m_SubPrograms:
                            s.m_GpuProgramType = 6
                    if hasattr(p, 'progFragment') and p.progFragment:
                        for s in p.progFragment.m_SubPrograms:
                            s.m_GpuProgramType = 6
        data.save()
        patched_shaders += 1

    with open(fpath, 'wb') as f:
        f.write(env.file.save())
    print(f"[+] {fpath} patched: {patched_shaders} shaders converted to native Linux GLCore!")

def main():
    if len(sys.argv) > 1 and sys.argv[1] in ('-h', '--help'):
        print(f"Usage: {sys.argv[0]} [GAME_DIR]")
        print("Environment variables: DXBC2SPV_PATH / VKD3D_PATH, SPIRV_CROSS_PATH, MAI_ONLY")
        return 0

    if not VKD3D_PATH:
        print("[!] Error: dxbc2spv (or test_vkd3d) not found. Build it with `make` or set DXBC2SPV_PATH.", file=sys.stderr)
        return 1
    if not SPIRV_CROSS_PATH:
        print("[!] Error: spirv-cross binary not found in build/, PATH, or SPIRV_CROSS_PATH.", file=sys.stderr)
        return 1

    if len(sys.argv) > 1:
        base_dir = os.path.abspath(sys.argv[1])
    elif os.path.isdir(os.path.join(SCRIPT_DIR, 'Sinmai_Data')):
        base_dir = SCRIPT_DIR
    else:
        base_dir = os.getcwd()

    data_dir = os.path.join(base_dir, 'Sinmai_Data')
    if not os.path.isdir(data_dir):
        print(f"[!] Error: Sinmai_Data directory not found under {base_dir}", file=sys.stderr)
        return 1

    files = [
        os.path.join(data_dir, 'Resources', 'unity_builtin_extra'),
        os.path.join(data_dir, 'globalgamemanagers.assets'),
        os.path.join(data_dir, 'resources.assets')
    ]
    only = os.environ.get('MAI_ONLY')
    for f in files:
        if only and only not in f:
            continue
        patch_asset_file(f)
    print("\n[+] All shaders successfully patched for Linux Native execution!")
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
