"""Offline checks for where the Vulkan dependency profile markers are placed."""
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import build_vulkan_dependencies
from build_vulkan_dependencies import profile_marker

GLSLANG_MARKER = 'GlslangVulkanOnly.cmake'
SDL_MARKER = 'SDL2VulkanOnly.cmake'


def install(prefix, config_directory, config_file):
    directory = prefix / config_directory
    directory.mkdir(parents=True)
    (directory / config_file).write_text('# config\n', encoding='utf-8')
    return directory


class ProfileMarkerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.prefix = Path(temporary.name)

    def test_glslang_marker_follows_the_install_layout(self):
        for layout in ('lib/cmake/glslang', 'lib64/cmake/glslang', 'lib/x86_64-linux-gnu/cmake/glslang'):
            with self.subTest(layout=layout):
                prefix = self.prefix / layout.replace('/', '_')
                directory = install(prefix, layout, 'glslang-config.cmake')
                self.assertEqual(profile_marker('glslang-vulkan', prefix), directory / GLSLANG_MARKER)

    def test_glslang_marker_accepts_the_camel_case_config_name(self):
        directory = install(self.prefix, 'lib64/cmake/glslang', 'glslangConfig.cmake')
        self.assertEqual(profile_marker('glslang-vulkan', self.prefix), directory / GLSLANG_MARKER)

    def test_sdl_marker_follows_the_install_layout(self):
        for layout in ('cmake', 'lib/cmake/SDL2', 'lib64/cmake/SDL2'):
            with self.subTest(layout=layout):
                prefix = self.prefix / layout.replace('/', '_')
                directory = install(prefix, layout, 'SDL2Config.cmake')
                self.assertEqual(profile_marker('sdl2-vulkan', prefix), directory / SDL_MARKER)

    def test_build_profile_writes_the_marker_in_lib64(self):
        args = SimpleNamespace(build_root=self.prefix / 'build', prefix=self.prefix / 'deps', cmake='cmake')
        prefix = args.prefix / 'glslang-vulkan'

        def fake_cmake(command, check):
            if '--build' in command:
                install(prefix, 'lib64/cmake/glslang', 'glslang-config.cmake')

        with mock.patch.object(build_vulkan_dependencies.subprocess, 'run', side_effect=fake_cmake):
            build_vulkan_dependencies.build_profile(Path('source'), 'glslang-vulkan', [], [], args, 1)
        self.assertTrue((prefix / 'lib64/cmake/glslang' / GLSLANG_MARKER).exists())
        self.assertFalse((prefix / 'lib').exists())
        with mock.patch.object(build_vulkan_dependencies.subprocess, 'run') as run:
            build_vulkan_dependencies.build_profile(Path('source'), 'glslang-vulkan', [], [], args, 1)
        run.assert_not_called()

    def test_build_profile_creates_the_marker_directory_when_no_config_was_installed(self):
        args = SimpleNamespace(build_root=self.prefix / 'build', prefix=self.prefix / 'deps', cmake='cmake')
        with mock.patch.object(build_vulkan_dependencies.subprocess, 'run'):
            build_vulkan_dependencies.build_profile(Path('source'), 'glslang-vulkan', [], [], args, 1)
        self.assertTrue((args.prefix / 'glslang-vulkan/lib/cmake/glslang' / GLSLANG_MARKER).exists())

    def test_default_location_without_an_install(self):
        self.assertEqual(profile_marker('glslang-vulkan', self.prefix / 'missing'),
                         self.prefix / 'missing/lib/cmake/glslang' / GLSLANG_MARKER)
        self.assertEqual(profile_marker('sdl2-vulkan', self.prefix / 'missing'),
                         self.prefix / 'missing/cmake' / SDL_MARKER)


if __name__ == '__main__':
    unittest.main()
