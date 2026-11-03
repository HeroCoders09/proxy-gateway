#!/usr/bin/env python3
"""Small controlled local load generator used by the dashboard demo."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import http.client
import itertools
import json
from pathlib import Path
import tempfile
import time

RETAILERS = ("northstar", "harbor", "copperlane")
SKUS = ("SKU-1042", "SKU-2088", "SKU-3105", "SKU-4471", "SKU-5026")
SUMMARY = Path(tempfile.gettempdir()) / "phase6-pricelens-summary.json"


def worker(index, stop_at, counts, offers, lock):
    n=index
    while time.monotonic()<stop_at:
        retailer=RETAILERS[n%len(RETAILERS)]
        domain=f"{retailer}.pricelens.test"
        sku=SKUS[(n//len(RETAILERS))%len(SKUS)]
        conn=http.client.HTTPConnection("127.0.0.1",8000,timeout=5)
        try:
            conn.request("GET",f"http://{domain}/api/offers/{sku}",headers={"Host":domain})
            response=conn.getresponse();body=response.read();status=response.status
            record=json.loads(body) if status==200 else None
        except OSError:
            status=0;record=None
        except (ValueError, json.JSONDecodeError):
            status=0;record=None
        finally:
            conn.close()
        with lock:
            counts["total"]+=1;counts["success"]+=int(0<status<500)
            if record:
                offers.setdefault(sku,{})[record["retailer"]]=record
                temporary=SUMMARY.with_suffix(".tmp")
                temporary.write_text(json.dumps({"offers":offers,"requests":counts["total"],"updated_at_unix_ms":int(time.time()*1000)}))
                temporary.replace(SUMMARY)
        n+=1


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--duration-seconds",type=int,default=60)
    parser.add_argument("--concurrency",type=int,default=12)
    args=parser.parse_args()
    if not 1<=args.concurrency<=128 or not 1<=args.duration_seconds<=3600: raise SystemExit("limits: concurrency 1..128, duration 1..3600")
    from threading import Lock
    counts={"total":0,"success":0};offers={};lock=Lock();deadline=time.monotonic()+args.duration_seconds
    SUMMARY.unlink(missing_ok=True)
    with ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        futures=[pool.submit(worker,i,deadline,counts,offers,lock) for i in range(args.concurrency)]
        for f in futures:f.result()
    print(f"requests={counts['total']} success={counts['success']} duration_seconds={args.duration_seconds}")


if __name__=="__main__":main()
