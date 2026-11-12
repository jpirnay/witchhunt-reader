# tools/devctl.py against a model of the device's port (tools/test_devctl.py): no pyserial and no device needed.
add_test(NAME DevctlHost COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/tools/test_devctl.py)
