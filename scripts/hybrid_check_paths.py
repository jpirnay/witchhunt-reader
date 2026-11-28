"""
PlatformIO post-configuration hook: give `pio check` our src/ include path when it runs inside pioarduino's hybrid
compile.

A C3 env whose core is not yet rebuilt from [firmware_tuned]'s custom_sdkconfig (a cold cache, as on a CI runner)
takes pioarduino's hybrid compile: espidf.py sets ARDUINO_LIB_COMPILE_FLAG=Build and points PROJECT_SRC_DIR at its
.dummy scaffold to compile the IDF libraries. `pio check` reads its include paths from the integration dump of that
run, so src/ is missing and every include relative to src/ becomes a missingInclude finding, which fails the check
(#950: 460 of them over the three CI shards). For the dump only, src/ goes back on the project environment's CPPPATH
(the dump reads projenv's, not env's); the hybrid compile itself builds the IDF from .dummy and does not see our
headers. scripts/iram_budget.py stands aside in the same run.
"""

Import("env", "projenv")  # noqa: F821  (SCons)

if env.IsIntegrationDump() and env.subst("$ARDUINO_LIB_COMPILE_FLAG") == "Build":  # noqa: F821
    src_dir = env.GetProjectConfig().get_optional_dir("src")  # noqa: F821
    projenv.AppendUnique(CPPPATH=[src_dir])  # noqa: F821
    print("hybrid_check_paths: the core rebuild run took the source directory; %s is back on the include path" % src_dir)
