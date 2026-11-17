"""Build the port's static window/input and Vulkan shader compiler profiles.

Inputs are the upstream SDL 2.32.10 source and glslang 16.4.0 release archive.
Original inputs are never modified. No game execution is involved.
"""
from pathlib import Path
import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import tarfile

GLSLANG_SHA512 = 'ef01dc652e5e6c4406cadf7de4b225c983fa9e3f0b74716844b8107b8ee87d1b80c1a039c47679302ce2244d4c44718e00b23429482bd1b51a3b29a6059358ca'

def edit(path, old, new):
    text = path.read_text(encoding='utf-8')
    assert text.count(old) == 1, (path, old[:80])
    path.write_bytes(text.replace(old, new).encode('utf-8'))

def prepare(args):
    root = args.build_root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    sdl, compiler = root / 'sdl-source', root / 'compiler-source'
    stamp = root / 'prepared.json'
    if stamp.exists():
        assert json.loads(stamp.read_text())['profile'] == 1
        return sdl, compiler
    assert not sdl.exists() and not compiler.exists()
    assert hashlib.sha512(args.glslang_archive.read_bytes()).hexdigest() == GLSLANG_SHA512
    shutil.copytree(args.sdl_source, sdl, ignore=shutil.ignore_patterns('.git'))
    with tarfile.open(args.glslang_archive) as archive:
        archive.extractall(root, filter='data')
    (root / 'glslang-16.4.0').rename(compiler)
    edit(sdl / 'src/dynapi/SDL_dynapi.h', '#define SDL_DYNAMIC_API 1', '#define SDL_DYNAMIC_API 0')
    path = sdl / 'src/video/SDL_video.c'
    text = path.read_text(encoding='utf-8')
    start = text.index('int SDL_GL_LoadLibrary(')
    end = text.index('#if 0 /* FIXME */', start)
    text = text[:start] + text[end:]
    text = text.replace('    SDL_GL_ResetAttributes();', '    /* Vulkan windows need no context attributes. */')
    text = text.replace('SDL_GL_LoadLibrary(NULL)', 'SDL_Unsupported()')
    text = text.replace('SDL_GL_MakeCurrent(window, NULL);', '/* No context API in this window layer. */')
    text = text.replace('SDL_GL_UnloadLibrary();', '_this->gl_config.driver_loaded = 0;')
    text = text.replace('SDL_ContextNotSupported("Open' + 'GL")', 'SDL_ContextNotSupported("requested renderer")')
    text = text.replace('Vulkan and Open' + 'GL not supported on same window', 'Only one graphics API is supported per window')
    path.write_bytes(text.encode('utf-8'))
    edit(compiler / 'glslang/CInterface/glslang_c_interface.cpp', '    glslang_shader_t* shader = new glslang_shader_t();',
        '''    // This private compiler accepts only the port's Vulkan GLSL -> SPIR-V contract.
    if (input->language != GLSLANG_SOURCE_GLSL || input->client != GLSLANG_CLIENT_VULKAN ||
        input->target_language != GLSLANG_TARGET_SPV)
        return nullptr;

    glslang_shader_t* shader = new glslang_shader_t();''')
    path = compiler / 'glslang/MachineIndependent/localintermediate.h'
    for target in ('client open' + 'gl100', 'target-env open' + 'gl'):
        edit(path, '        if (spvVersion.openGl > 0)\n            processes.addProcess("' + target + '");\n', '')
    path = compiler / 'glslang/MachineIndependent/ShaderLang.cpp'
    text = path.read_text(encoding='utf-8')
    start = text.index('            if (spvVersion.openGl >= 100 && version < 330) {')
    end = text.index('\n            }', start) + len('\n            }')
    path.write_bytes((text[:start] + text[end:]).encode('utf-8'))
    stamp.write_text(json.dumps(dict(profile=1, compiler_sha512=GLSLANG_SHA512)), encoding='utf-8')
    return sdl, compiler

PROFILE_FILES = {
    'sdl2-vulkan': (('SDL2Config.cmake',), 'SDL2VulkanOnly.cmake', 'cmake'),
    'glslang-vulkan': (('glslang-config.cmake', 'glslangConfig.cmake'), 'GlslangVulkanOnly.cmake', 'lib/cmake/glslang'),
}

def profile_marker(name, prefix):
    """Marker proving the audited profile was built, next to the package config
    file so one directory can serve as the find_package(<name>_DIR) hint.
    The config directory depends on the platform (cmake, lib/cmake/<name>,
    lib64/cmake/<name>, lib/<triplet>/cmake/<name>): look where it was installed."""
    configs, marker, default_directory = PROFILE_FILES[name]
    for config in configs:
        for found in sorted(prefix.rglob(config)):
            return found.with_name(marker)
    return prefix / default_directory / marker

def build_profile(source, name, options, common, args, parallel_jobs):
    build, prefix = args.build_root.resolve() / (name + '-build'), args.prefix.resolve() / name
    if profile_marker(name, prefix).exists():
        return
    subprocess.run([args.cmake, '-S', str(source), '-B', str(build), *common, *options,
        '-DCMAKE_INSTALL_PREFIX=' + str(prefix)], check=True)
    subprocess.run([args.cmake, '--build', str(build), '--config', 'Release', '--target', 'install', '--parallel', str(parallel_jobs)], check=True)
    marker = profile_marker(name, prefix)
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text('# Audited Vulkan dependency profile 1.\n', encoding='utf-8')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdl-source', type=Path, required=True)
    parser.add_argument('--glslang-archive', type=Path, required=True)
    parser.add_argument('--build-root', type=Path, required=True)
    parser.add_argument('--prefix', type=Path, required=True)
    parser.add_argument('--cache-dir', type=Path, default=None)
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--generator', default='Ninja',
        help='CMake generator (Ninja, "Unix Makefiles", "Visual Studio 17 2022", ...)')
    parser.add_argument('--cmake', default='cmake', help='cmake executable')
    args = parser.parse_args()

    # Check cache first
    if args.cache_dir:
        cache_root = args.cache_dir.resolve()
        sdl_cached = cache_root / 'sdl2-vulkan'
        glslang_cached = cache_root / 'glslang-vulkan'
        sdl_m = profile_marker('sdl2-vulkan', sdl_cached)
        glslang_m = profile_marker('glslang-vulkan', glslang_cached)
        if sdl_m.exists() and glslang_m.exists():
            prefix_sdl = args.prefix.resolve() / 'sdl2-vulkan'
            prefix_glslang = args.prefix.resolve() / 'glslang-vulkan'
            if not prefix_sdl.exists():
                shutil.copytree(sdl_cached, prefix_sdl)
            if not prefix_glslang.exists():
                shutil.copytree(glslang_cached, prefix_glslang)
            return

    sdl, compiler = prepare(args)
    if args.prepare_only:
        return
    common = ['-G', args.generator]
    if 'Visual Studio' in args.generator:
        common += ['-A', 'x64']  # MSBuild platform; Make/Ninja generators reject -A
    profiles = [
        (sdl, 'sdl2-vulkan', ['-DSDL_SHARED=OFF', '-DSDL_STATIC=ON', '-DSDL_TEST=OFF', '-DSDL_TESTS=OFF',
            '-DSDL_OPENGL=OFF', '-DSDL_OPENGLES=OFF', '-DSDL_VULKAN=ON', '-DSDL_RENDER_D3D=OFF',
            '-DSDL_RENDER_METAL=OFF', '-DSDL_FORCE_STATIC_VCRT=OFF']),
        (compiler, 'glslang-vulkan', ['-DBUILD_EXTERNAL=OFF', '-DBUILD_SHARED_LIBS=OFF', '-DGLSLANG_TESTS=OFF',
            '-DENABLE_GLSLANG_BINARIES=OFF', '-DENABLE_HLSL=OFF', '-DENABLE_OPT=OFF', '-DENABLE_RTTI=OFF',
            '-DGLSLANG_ENABLE_INSTALL=ON'])]

    parallel_jobs = max(4, os.cpu_count() or 4)
    # Build both dependencies concurrently
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        futures = [
            executor.submit(build_profile, source, name, options, common, args, parallel_jobs)
            for source, name, options in profiles
        ]
        for f in concurrent.futures.as_completed(futures):
            f.result()

    # Update cache if enabled
    if args.cache_dir:
        cache_root = args.cache_dir.resolve()
        cache_root.mkdir(parents=True, exist_ok=True)
        for name in ('sdl2-vulkan', 'glslang-vulkan'):
            src = args.prefix.resolve() / name
            dst = cache_root / name
            if src.exists() and not dst.exists():
                shutil.copytree(src, dst)

if __name__ == '__main__':
    main()
