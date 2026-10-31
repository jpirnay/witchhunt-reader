"""The firmware must download its web bundle from the release it was published under.

scripts/build_html.py stamps WEB_BUNDLE_RELEASE_TAG, which src/network/WebAssets.cpp puts in the
download URL. The tag is the release's own name, which need not be the version in platformio.ini:
2.41 was published under the tag "2.41RC", stamped "2.41", and every download hit a 404 (#581).
Checks get_release_tag() per env, and that release.yml hands the build the tag it publishes under.
Usage: python test/web/release_tag_test.py
"""

import ast
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load_get_release_tag():
    # build_html.py builds the bundle when it is imported, so only the function is taken from it.
    with open(os.path.join(ROOT, "scripts", "build_html.py"), encoding="utf-8") as f:
        tree = ast.parse(f.read())
    fn = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == "get_release_tag")
    namespace = {"os": os}
    exec(compile(ast.Module(body=[fn], type_ignores=[]), "build_html.py", "exec"), namespace)
    return namespace["get_release_tag"]


def check(name, actual, expected, failures):
    if actual != expected:
        failures.append(f"{name}: got {actual!r}, expected {expected!r}")


def main():
    get_release_tag = load_get_release_tag()
    failures = []
    saved = {k: os.environ.pop(k, None) for k in ("CROSSPOINT_RELEASE_TAG", "CROSSPOINT_RC_VERSION")}
    try:
        os.environ["CROSSPOINT_RELEASE_TAG"] = "2.41RC"
        for env in ("gh_release", "x4pro_gh_release", "lilygo_gh_release"):
            check(f"{env} under tag 2.41RC", get_release_tag(env, "2.41"), "2.41RC", failures)
        check("default under a release tag", get_release_tag("default", "2.41"), "", failures)
        del os.environ["CROSSPOINT_RELEASE_TAG"]
        check("gh_release built locally", get_release_tag("gh_release", "2.41"), "2.41", failures)

        os.environ["CROSSPOINT_RC_VERSION"] = "2.41.0-rc.3"
        check("gh_release_rc", get_release_tag("gh_release_rc", "2.41"), "2.41.0-rc.3", failures)
        check("x4pro_gh_release_rc", get_release_tag("x4pro_gh_release_rc", "2.41"), "2.41.0-rc.3", failures)
        check("default", get_release_tag("default", "2.41"), "", failures)
    finally:
        for k, v in saved.items():
            os.environ.pop(k, None)
            if v is not None:
                os.environ[k] = v

    with open(os.path.join(ROOT, ".github", "workflows", "release.yml"), encoding="utf-8") as f:
        workflow = f.read()
    if "CROSSPOINT_RELEASE_TAG: ${{ github.event.release.tag_name }}" not in workflow:
        failures.append("release.yml does not pass the release's tag to the build as CROSSPOINT_RELEASE_TAG")

    for failure in failures:
        print(f"FAIL {failure}")
    print("release tag: " + ("FAILED" if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
