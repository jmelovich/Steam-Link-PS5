#!/usr/bin/env python3
# ps5-native-app-boilerplate - Host tooling regression tests.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Exercises identity initialization and deployment resolution without a console.

import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ToolTests(unittest.TestCase):
    def run_init(self, param, **values):
        environment = os.environ.copy()
        environment.update(values)
        return subprocess.run(
            ["bash", str(ROOT / "tools/init-project.sh"), str(param)],
            cwd=ROOT,
            env=environment,
            check=False,
            capture_output=True,
            text=True,
        )

    def test_init_coordinates_media_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            param = Path(directory) / "param.json"
            param.write_text(
                json.dumps(
                    {
                        "contentId": "UP9000-PPSA99999_00-HELLOWORLD000001",
                        "localizedParameters": {
                            "defaultLanguage": "en-US",
                            "en-US": {"titleName": "Old"},
                        },
                        "gameIntent": {"permittedIntents": [{"intentType": "launchActivity"}]},
                    }
                ),
                encoding="utf-8",
            )
            result = self.run_init(
                param,
                TITLE_ID="PPSA12345",
                APP_NAME="Moon Client",
                APP_CATEGORY="media",
                CONTENT_SUFFIX="MOONCLIENT000001",
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            configured = json.loads(param.read_text(encoding="utf-8"))
            self.assertEqual(configured["titleId"], "PPSA12345")
            self.assertEqual(configured["conceptId"], "12345")
            self.assertEqual(configured["contentId"], "UP9000-PPSA12345_00-MOONCLIENT000001")
            self.assertEqual(configured["localizedParameters"]["en-US"]["titleName"], "Moon Client")
            self.assertEqual(configured["applicationCategoryType"], 65536)
            self.assertEqual(configured["contentBadgeType"], 2)
            self.assertNotIn("gameIntent", configured)

    def test_init_rejects_invalid_title_without_rewriting(self):
        with tempfile.TemporaryDirectory() as directory:
            param = Path(directory) / "param.json"
            original = '{"contentId":"UP9000-PPSA99999_00-HELLOWORLD000001"}\n'
            param.write_text(original, encoding="utf-8")
            result = self.run_init(param, TITLE_ID="PPSA12", APP_NAME="Broken")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(param.read_text(encoding="utf-8"), original)

    def test_init_derives_game_suffix_and_preserves_mode(self):
        with tempfile.TemporaryDirectory() as directory:
            param = Path(directory) / "param.json"
            param.write_text(
                json.dumps(
                    {
                        "contentId": "UP9000-PPSA99999_00-HELLOWORLD000001",
                        "localizedParameters": {
                            "defaultLanguage": "en-US",
                            "en-US": {"titleName": "Old"},
                        },
                    }
                ),
                encoding="utf-8",
            )
            param.chmod(0o640)
            result = self.run_init(
                param, TITLE_ID="PPSA54321", APP_NAME="Native Sample", CONTENT_SUFFIX=""
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            configured = json.loads(param.read_text(encoding="utf-8"))
            self.assertEqual(configured["contentId"], "UP9000-PPSA54321_00-NATIVESAMPLE0000")
            self.assertEqual(configured["applicationCategoryType"], 0)
            self.assertEqual(configured["contentBadgeType"], 1)
            self.assertEqual(
                configured["gameIntent"]["permittedIntents"],
                [{"intentType": "launchActivity"}],
            )
            self.assertEqual(param.stat().st_mode & 0o777, 0o640)

    def test_undeploy_dry_run_resolves_only_current_title(self):
        environment = os.environ.copy()
        environment.update(PS5_HOST="192.0.2.1", DEPLOY_DRY_RUN="1")
        result = subprocess.run(
            ["bash", str(ROOT / "tools/deploy.sh"), "undeploy"],
            cwd=ROOT,
            env=environment,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        # Read the title from param.json rather than hardcoding it: this
        # assertion was inherited asserting the boilerplate's placeholder
        # PPSA99999 and silently failed once the project took its own ID.
        title_id = json.loads(
            (ROOT / "sce_sys/param.json").read_text(encoding="utf-8")
        )["titleId"]
        self.assertIn(f"/data/homebrew/{title_id}/", result.stdout)
        self.assertIn(f"{title_id}.{{ffpkg,ffpfsc}}", result.stdout)
        self.assertIn("no network request was sent", result.stdout)

    def test_deploy_dry_run_uses_mocked_build_and_no_network(self):
        with tempfile.TemporaryDirectory() as directory:
            sandbox = Path(directory)
            (sandbox / "tools").mkdir()
            (sandbox / "sce_sys").mkdir()
            shutil.copy2(ROOT / "tools/deploy.sh", sandbox / "tools/deploy.sh")
            (sandbox / "sce_sys/param.json").write_text(
                '{"titleId":"PPSA12345"}\n', encoding="utf-8"
            )

            mock_bin = sandbox / "mock-bin"
            mock_bin.mkdir()
            mock_make = mock_bin / "make"
            mock_make.write_text(
                "#!/usr/bin/env bash\n"
                "mkdir -p \"$MOCK_ROOT/dist\"\n"
                "printf package > \"$MOCK_ROOT/dist/PPSA12345.ffpkg\"\n",
                encoding="utf-8",
            )
            mock_make.chmod(0o755)

            environment = os.environ.copy()
            environment.update(
                PS5_HOST="192.0.2.1",
                DEPLOY_DRY_RUN="1",
                DEPLOY_FORMAT="ffpkg",
                MOCK_ROOT=str(sandbox),
                PATH=f"{mock_bin}{os.pathsep}{environment['PATH']}",
            )
            result = subprocess.run(
                ["bash", str(sandbox / "tools/deploy.sh")],
                cwd=sandbox,
                env=environment,
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("/data/homebrew/PPSA12345.ffpkg", result.stdout)
            self.assertIn("no network request was sent", result.stdout)

    def test_deploy_uses_selected_app_param_title(self):
        with tempfile.TemporaryDirectory() as directory:
            sandbox = Path(directory)
            (sandbox / "tools").mkdir()
            (sandbox / "sce_sys").mkdir()
            (sandbox / "examples/demo/sce_sys").mkdir(parents=True)
            shutil.copy2(ROOT / "tools/deploy.sh", sandbox / "tools/deploy.sh")
            (sandbox / "sce_sys/param.json").write_text(
                '{"titleId":"PPSA12345"}\n', encoding="utf-8"
            )
            selected_param = sandbox / "examples/demo/sce_sys/param.json"
            selected_param.write_text(
                '{"titleId":"PPSA54321"}\n', encoding="utf-8"
            )

            environment = os.environ.copy()
            environment.update(
                PS5_HOST="192.0.2.1",
                DEPLOY_DRY_RUN="1",
                APP_PARAM="examples/demo/sce_sys/param.json",
            )
            result = subprocess.run(
                ["bash", str(sandbox / "tools/deploy.sh"), "undeploy"],
                cwd=sandbox,
                env=environment,
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("/data/homebrew/PPSA54321/", result.stdout)
            self.assertNotIn("/data/homebrew/PPSA12345/", result.stdout)

    # The boilerplate also asserted the content of its own CI file,
    # .github/workflows/tooling.yml, down to literal strings such as
    # "ps5-native-app-boilerplate-$GITHUB_SHA". This project replaced that
    # workflow with .github/workflows/build.yml, so those assertions tested a
    # file that does not exist here and could only error. The workflow is
    # covered by actually running on GitHub; what is kept below is the part
    # that asserts this repository's own behaviour.

    def test_ffpfsc_image_is_refused_before_anything_is_built(self):
        # Asked for by name, the image is refused before anything is built.
        result = subprocess.run(
            ["bash", str(ROOT / "tools/build.sh"), "Ffpfsc"],
            capture_output=True,
            text=True,
            env={**os.environ, "ENABLE_FFPFSC": "0"},
            check=False,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("switched off for now", result.stderr)

    def test_content_version_is_stamped_from_a_release_tag(self):
        # Stores and updaters compare contentVersion, not the git tag, so the
        # mapping has to be exact and must refuse anything NN.NNN.NNN cannot
        # hold rather than silently truncating to a lower version.
        script = str(ROOT / "tools/set-content-version.sh")
        cases = {
            "v1.2.3": "01.002.003",
            "1.2.3": "01.002.003",
            "v0.1.0": "00.001.000",
            "v2.10.0-rc1": "02.010.000",
            "v99.999.999": "99.999.999",
        }
        with tempfile.TemporaryDirectory() as directory:
            param = Path(directory) / "param.json"
            for version, expected in cases.items():
                shutil.copy2(ROOT / "sce_sys/param.json", param)
                result = subprocess.run(
                    ["bash", script, version],
                    env={**os.environ, "PARAM_JSON": str(param)},
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                written = json.loads(param.read_text(encoding="utf-8"))
                self.assertEqual(written["contentVersion"], expected, version)
                # Only that one field moves; the rest of param.json is intact.
                baseline = json.loads(
                    (ROOT / "sce_sys/param.json").read_text(encoding="utf-8")
                )
                self.assertEqual(
                    {k: v for k, v in written.items() if k != "contentVersion"},
                    {k: v for k, v in baseline.items() if k != "contentVersion"},
                )
                self.assertRegex(written["contentVersion"], r"^\d{2}\.\d{3}\.\d{3}$")

            for bad in ("v100.0.0", "v1.1000.0", "v1.2", "notaversion", ""):
                shutil.copy2(ROOT / "sce_sys/param.json", param)
                result = subprocess.run(
                    ["bash", script, bad],
                    env={**os.environ, "PARAM_JSON": str(param)},
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertNotEqual(result.returncode, 0, f"{bad!r} should be refused")

            # --check must agree with a matching value, reject a mismatch, and
            # never write: CI relies on it to refuse a release that would be
            # listed but never offered to a console as an update.
            shutil.copy2(ROOT / "sce_sys/param.json", param)
            subprocess.run(
                ["bash", script, "v4.5.6"],
                env={**os.environ, "PARAM_JSON": str(param)},
                check=True,
                capture_output=True,
            )
            before = param.read_text(encoding="utf-8")
            ok = subprocess.run(
                ["bash", script, "--check", "v4.5.6"],
                env={**os.environ, "PARAM_JSON": str(param)},
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(ok.returncode, 0, ok.stderr)
            bad = subprocess.run(
                ["bash", script, "--check", "v4.5.7"],
                env={**os.environ, "PARAM_JSON": str(param)},
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn("mismatch", bad.stderr)
            self.assertEqual(param.read_text(encoding="utf-8"), before, "--check wrote")

    def test_release_workflow_verifies_the_version_and_drafts_the_release(self):
        workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        # A tag build must CHECK the committed contentVersion, not overwrite it:
        # ps5-homebrew-catalog reads it from the repository at the tag, so
        # stamping it at build time would leave the committed value behind.
        self.assertIn("tools/set-content-version.sh --check", workflow)
        self.assertIn("--draft", workflow)
        # Publishing from GitHub's web UI creates the tag and the release
        # together, so the release can already exist when this job runs.
        # Creating it again fails and the ZIP is never attached; the workflow
        # has to upload into the existing release instead.
        self.assertIn("gh release view", workflow)
        self.assertIn("gh release upload", workflow)
        # Pinned, not floating: this toolchain needs the Clang 18 image, which
        # only ubuntu-24.04 carries. Ignore comments, which explain the pin.
        code = "\n".join(
            line for line in workflow.splitlines() if not line.lstrip().startswith("#")
        )
        self.assertNotIn("ubuntu-latest", code)
        self.assertIn("ubuntu-24.04", code)

    def test_native_writer_anchors_relro_and_checks_load_congruence(self):
        source = (ROOT / "tooling/native/sce_module_writer.cpp").read_text(
            encoding="utf-8"
        )
        # RELRO is anchored at its first section: .data.rel.ro whenever lld
        # keeps it, otherwise the GOT, so a program without relocated
        # read-only data still converts.
        self.assertIn("relro_origin(image, relro_start)", source)
        self.assertIn('if (input.name == ".data.rel.ro")', source)
        self.assertIn("relro_source.file_offset", source)
        self.assertIn(
            "header.offset % header.alignment == header.address % header.alignment",
            source,
        )
        self.assertNotIn("const std::uint64_t relro_file = got.file_offset", source)


if __name__ == "__main__":
    unittest.main()
