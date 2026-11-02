#!/usr/bin/env python3
"""Small controlled local load generator used by the dashboard demo."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import http.client
import itertools
import time


def worker(index, stop_at, counts, lock):
    n=index
    while time.monotonic()<stop_at:
        domain="site-a.example" if n%2==0 else "site-b.example"
        conn=http.client.HTTPConnection("127.0.0.1",8000,timeout=5)
        try:
            conn.request("GET",f"http://{domain}/products/demo-{n}",headers={"Host":domain})
            response=conn.getresponse();response.read();status=response.status
        except OSError:
            status=0
        finally:
            conn.close()
        with lock: counts["total"]+=1;counts["success"]+=int(0<status<500)
        n+=1


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--duration-seconds",type=int,default=60)
    parser.add_argument("--concurrency",type=int,default=12)
    args=parser.parse_args()
    if not 1<=args.concurrency<=128 or not 1<=args.duration_seconds<=3600: raise SystemExit("limits: concurrency 1..128, duration 1..3600")
    from threading import Lock
    counts={"total":0,"success":0};lock=Lock();deadline=time.monotonic()+args.duration_seconds
    with ThreadPoolExecutor(max_workers=args.concurrency) as pool:
        futures=[pool.submit(worker,i,deadline,counts,lock) for i in range(args.concurrency)]
        for f in futures:f.result()
    print(f"requests={counts['total']} success={counts['success']} duration_seconds={args.duration_seconds}")


if __name__=="__main__":main()
