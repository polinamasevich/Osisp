import csv
import os
import subprocess
import sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(BASE, "lab4.exe")
DATA = os.path.join(BASE, "data")
EXP_CSV = os.path.join(DATA, "experiments.csv")
STG_CSV = os.path.join(DATA, "stages.csv")

RUNS = 3
MODES = ["single", "batch"]
REQUESTS = [100, 250, 500, 750, 865, 1000]
SEED = 8081


def parse_stdout(text):
    out = {"wall_ms": None, "stages": []}
    in_stages = False
    for ln in text.splitlines():
        s = ln.strip()
        if s.startswith("WALL_MS="):
            out["wall_ms"] = float(s.split("=")[1].split()[0])
        elif s.startswith("STAGE ITEMS"):
            in_stages = True
        elif in_stages:
            if s.startswith("BARRIER_WAIT_TOTAL_MS"):
                in_stages = False
                continue
            parts = s.split()
            if len(parts) == 6:
                out["stages"].append({
                    "stage": int(parts[0]), "items": int(parts[1]),
                    "batches": int(parts[2]), "busy_pct": float(parts[3]),
                    "proc_ms": float(parts[4]), "barrier_ms": float(parts[5])})
    return out


def main():
    os.makedirs(DATA, exist_ok=True)
    for f in (EXP_CSV, STG_CSV):
        if os.path.exists(f):
            os.remove(f)
    total = len(MODES) * len(REQUESTS) * RUNS
    done = 0
    with open(STG_CSV, "w", newline="", encoding="utf-8") as fs:
        wst = csv.writer(fs)
        wst.writerow(["mode", "requests", "run", "stage", "items", "batches",
                      "busy_pct", "proc_ms", "barrier_ms"])
        for mode in MODES:
            for n in REQUESTS:
                for run in range(1, RUNS + 1):
                    cmd = [EXE, "--mode", mode, "--requests", str(n),
                           "--seed", str(SEED), "--quiet", "--csv", EXP_CSV]
                    r = subprocess.run(cmd, capture_output=True, text=True)
                    done += 1
                    if r.returncode != 0:
                        print(f"FAIL mode={mode} requests={n} run={run} rc={r.returncode}")
                        print(r.stdout[-1500:])
                        print(r.stderr[-1500:])
                        sys.exit(1)
                    info = parse_stdout(r.stdout)
                    for st in info["stages"]:
                        wst.writerow([mode, n, run, st["stage"], st["items"],
                                      st["batches"], st["busy_pct"], st["proc_ms"],
                                      st["barrier_ms"]])
                    print(f"[{done}/{total}] mode={mode} requests={n} run={run} "
                          f"wall={info['wall_ms']} ms violations-check rc=0")
    print("OK ->", EXP_CSV)
    print("OK ->", STG_CSV)


if __name__ == "__main__":
    main()
