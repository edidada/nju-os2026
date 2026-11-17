#!/usr/bin/env bash
set -euo pipefail
mkdir -p evidence
git rev-parse HEAD > evidence/commit.txt
gcc -std=gnu2x -O2 -Wall -Wextra -Werror -Imymalloc -DM5_TEST_COUNT -c mymalloc/mymalloc.c -o evidence/test-allocator.o
gcc -std=gnu2x -O2 -Imymalloc -Itestkit evidence/test-allocator.o mymalloc/start.c mymalloc/tests/*.c testkit/testkit.c -pthread -o evidence/public
TK_RUN=1 TK_VERBOSE=1 ./evidence/public | tee evidence/public.output
python3 - <<'PY'
from pathlib import Path
import re
s=Path('evidence/public.output').read_text();scores=re.findall(r'(\d+)/(\d+) test cases passed',s)
assert scores and all(a==b and int(a)==3 for a,b in scores),scores
PY
gcc -std=c11 -O2 -Wall -Wextra -Werror -Imymalloc -ffreestanding -fno-builtin -c mymalloc/mymalloc.c -o evidence/allocator.o
nm -u evidence/allocator.o | tee evidence/undefined.txt
python3 - <<'PY'
from pathlib import Path
symbols={line.split()[-1] for line in Path('evidence/undefined.txt').read_text().splitlines()}
assert symbols <= {'vmalloc','vmfree'},symbols
PY
gcc -std=c11 -O2 -Imymalloc -DFREESTANDING -ffreestanding -fno-builtin -static -nostdlib -nostartfiles -no-pie mymalloc/*.c -o evidence/freestanding
size evidence/freestanding | tee evidence/size.txt
python3 - <<'PY'
from pathlib import Path
v=Path('evidence/size.txt').read_text().splitlines()[1].split();assert sum(map(int,v[:3]))<1048576
PY
for mode in normal sanitized; do
 flags='-O2'
 if [ "$mode" = sanitized ]; then flags='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer'; fi
 gcc -std=c11 $flags -Wall -Wextra -Werror -Imymalloc mymalloc/mymalloc.c ci/stress.c -pthread -o "evidence/stress-$mode"
 "./evidence/stress-$mode" | tee "evidence/stress-$mode.output"
done

gcc -std=c11 -O2 -Wall -Wextra -Werror -Imymalloc mymalloc/mymalloc.c ci/benchmark.c -pthread -o evidence/benchmark
for config in '1 16 1 10000' '8 16 1 10000' '1 64 256 200' '8 64 256 200' '1 2048 1 10000' '8 8192 1 2000'; do
 read -r threads bytes batch iterations <<< "$config"
 for repeat in 1 2 3; do
  ./evidence/benchmark "$threads" "$bytes" "$batch" "$iterations" > "evidence/benchmark-$threads-$bytes-$batch-$repeat.csv"
 done
done

# Fixed source baseline, compiled and measured beside current code on this runner.
git show c75b7c9caea04bd0f51782b16f04d48842cd0695:mymalloc/mymalloc.c > evidence/baseline.c
printf '\nvoid mymalloc_trim(void) {}\n' >> evidence/baseline.c
gcc -std=c11 -O2 -Wall -Wextra -Werror -Imymalloc evidence/baseline.c ci/benchmark.c -pthread -o evidence/benchmark-baseline
for repeat in 1 2 3; do
 for config in '1 16 1 10000' '8 16 1 10000' '8 64 256 200'; do
  read -r threads bytes batch iterations <<< "$config"
  for version in baseline current; do
   executable=evidence/benchmark
   if [ "$version" = baseline ]; then executable=evidence/benchmark-baseline; fi
   "$executable" "$threads" "$bytes" "$batch" "$iterations" > "evidence/paired-$version-$threads-$bytes-$batch-$repeat.csv"
  done
 done
done

gcc -std=c11 -O2 -Wall -Wextra -Werror -Imymalloc -DFREESTANDING -ffreestanding -fno-builtin -fno-stack-protector -static -nostdlib -nostartfiles -no-pie mymalloc/mymalloc.c ci/freestanding.c -o evidence/freestanding-runtime
timeout 30 ./evidence/freestanding-runtime
printf 'PASS: freestanding 512KiB pool, four sizes exhausted twice, contents preserved, identical capacity recovered, bitmap empty\n' > evidence/freestanding-runtime.result
