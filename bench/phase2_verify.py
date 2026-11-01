#!/usr/bin/env python3
"""Repeatable Phase 2 local verification: per-domain rate isolation and AIMD reaction."""
import concurrent.futures, http.client, json, os, subprocess, sys, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
cfg_path = "/tmp/pg-phase2-bench.json"
cfg = json.load(open(os.path.join(ROOT, "config/gateway.local.json")))
cfg["listen"]["port"] = 18000; cfg["admin"]["port"] = 19000
# Keep this Phase 2 regression check independent of Phase 3 policy selection and the deliberately
# failing/slow proxies. The test injects its own fault on proxy1 for the AIMD check below.
cfg["proxies"] = cfg["proxies"][:1]
cfg["rate_limits"]["site-a.example"] = {"rate": 50, "burst": 1}
cfg["workers"] = {"min": 4, "max": 8, "alpha": 1, "beta": 0.5}
json.dump(cfg, open(cfg_path, "w"))
bed = subprocess.Popen([os.path.join(ROOT,"testbed/run_local.sh"), "start"], stdout=subprocess.DEVNULL)
bed.wait()
gw = subprocess.Popen([os.path.join(ROOT,"build/gateway"), "--config", cfg_path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(.4)
def request(host, n=0):
    c=http.client.HTTPConnection("127.0.0.1",18000,timeout=8)
    c.request("GET", "http://%s/products/%d"%(host,n), headers={"Host":host}); r=c.getresponse(); r.read(); c.close(); return r.status
def admin():
    c=http.client.HTTPConnection("127.0.0.1",19000,timeout=3); c.request("GET","/api/stats"); x=json.loads(c.getresponse().read()); c.close(); return x
def fault(pct):
    c=http.client.HTTPConnection("127.0.0.1",3228,timeout=3); c.request("GET","/fault?fail_pct=%d"%pct); c.getresponse().read(); c.close()
try:
    with concurrent.futures.ThreadPoolExecutor(max_workers=80) as ex:
        a=list(ex.map(lambda i:request("site-a.example",i),range(100)))
        b=list(ex.map(lambda i:request("site-b.example",i),range(100)))
    c=http.client.HTTPConnection("127.0.0.1",8081); c.request("GET","/stats"); stats=json.loads(c.getresponse().read()); c.close()
    a_max=max(stats["site-a.example"]["per_second"].values())
    assert a_max <= 50, ("site A exceeded 50/s",a_max)
    assert all(x==200 for x in a+b)
    # Two healthy controller windows raise C above min; one all-failure window brings it down.
    for _ in range(2):
        request("test.example"); time.sleep(1.05)
    before=admin()["aimd_limit"]
    fault(100)
    for i in range(20): request("test.example",i)
    time.sleep(1.1); after=admin()["aimd_limit"]
    fault(0)
    for _ in range(2): request("test.example"); time.sleep(1.05)
    recovered=admin()["aimd_limit"]
    assert after < before and recovered > after, (before,after,recovered)
    print("PHASE2 PASS: site-a max %d/s; site-b %d requests; AIMD %d -> %d -> %d" % (a_max,len(b),before,after,recovered))
finally:
    gw.terminate(); gw.wait(timeout=3)
    subprocess.run([os.path.join(ROOT,"testbed/run_local.sh"),"stop"], stdout=subprocess.DEVNULL)
