"""Exercise file boundaries and state transitions against a Python oracle."""
from pathlib import Path
import random
import subprocess
import tempfile

exe = (Path(__file__).resolve().parents[1] / 'labyrinth/labyrinth').resolve()
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'map.txt'
    def invoke(contents, extra=(), expected=0):
        path.write_bytes(contents)
        result = subprocess.run([str(exe), '-p', '1', '-m', str(path), *extra], capture_output=True, timeout=3)
        assert result.returncode == expected, (result.returncode, result.stderr)
        return result
    full = (b'.'*100+b'\n')*100
    assert invoke(full).stdout == full
    assert invoke(b'...').stdout == b'...'
    for bad in [(b'.'*101+b'\n'), b'.\n'*101, b'', b'..\n.\n', b'.x\n', b'11\n', b'.#.\n']:
        invoke(bad, expected=1)
    for direction in ['up', 'left', 'bad']:
        original = b'1.\n..\n'
        invoke(original, ['--move', direction], 1)
        assert path.read_bytes() == original
    rng = random.Random(2026)
    board = [list('1.........')] + [list('..........') for _ in range(9)]
    row = col = 0
    for _ in range(500):
        direction, dr, dc = rng.choice([('up',-1,0),('down',1,0),('left',0,-1),('right',0,1)])
        nr, nc = row+dr, col+dc
        valid = 0 <= nr < 10 and 0 <= nc < 10
        before = (''.join(''.join(line)+'\n' for line in board)).encode()
        invoke(before, ['--move',direction], 0 if valid else 1)
        if valid:
            board[row][col]='.'; board[nr][nc]='1'; row,col=nr,nc
        expected = (''.join(''.join(line)+'\n' for line in board)).encode()
        assert path.read_bytes() == expected
print('M1 parser boundaries and 500 seeded move transitions passed')
