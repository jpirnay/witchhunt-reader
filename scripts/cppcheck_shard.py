#!/usr/bin/env python3
"""Print the `pio check --src-filters` value of one CI cppcheck shard.

Usage: cppcheck_shard.py <shard> <shards>

pio check runs one cppcheck process per file, so a file's findings do not depend on which
files share its shard. A file's shard follows from its path alone: adding a file moves no
other file, so the results each shard keeps between runs stay valid.

Shard 0 starts from pio check's default directories and drops what the other shards take,
so the shards cover the default set by construction, a file `git ls-files` does not list
included.
"""

import subprocess
import sys
import zlib


def main():
    shard, shards = int(sys.argv[1]), int(sys.argv[2])
    files = subprocess.check_output(['git', 'ls-files', 'src/*.c', 'src/*.cpp'], text=True).split()
    taken = [f for f in files if zlib.crc32(f.encode()) % shards == shard]
    if shard == 0:
        others = [f for f in files if f not in taken]
        print(' '.join(['+<src>', '+<include>'] + [f'-<{f}>' for f in others]))
    else:
        print(' '.join(['-<*>'] + [f'+<{f}>' for f in taken]))


if __name__ == '__main__':
    main()
