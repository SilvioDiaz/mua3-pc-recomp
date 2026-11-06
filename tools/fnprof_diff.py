"""Compare two TOTK_FNPROF outputs (runtime/fnprof.c): the functions only one run entered, then the ones whose
entry count changed the most.

  python tools/fnprof_diff.py before.txt after.txt [--top 40]

Each input line is "<hex addr> <count> <where>". Counts are per profiled window, so keep the two windows the same
length (TOTK_FNPROF=<from>:<to>) for the ratios to mean something.
"""
import argparse


def load(path):
    rows = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            parts = line.rstrip('\n').split(' ', 2)
            if len(parts) >= 2:
                rows[int(parts[0], 16)] = (int(parts[1]), parts[2] if len(parts) > 2 else '')
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--top', type=int, default=40)
    args = ap.parse_args()
    a, b = load(args.a), load(args.b)

    for title, src, other in (('only in ' + args.a, a, b), ('only in ' + args.b, b, a)):
        only = sorted((k for k in src if k not in other), key=lambda k: -src[k][0])
        print(f'== {title}: {len(only)} functions')
        for k in only[:args.top]:
            print(f'  {k:x} {src[k][0]:>10} {src[k][1]}')

    both = [k for k in a if k in b]
    both.sort(key=lambda k: -abs(b[k][0] - a[k][0]) / max(a[k][0], b[k][0]))
    print(f'== count changed most (of {len(both)} in both): before -> after')
    for k in both[:args.top]:
        if a[k][0] == b[k][0]:
            break
        print(f'  {k:x} {a[k][0]:>10} -> {b[k][0]:<10} {a[k][1]}')


if __name__ == '__main__':
    main()
