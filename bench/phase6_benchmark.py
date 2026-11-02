#!/usr/bin/env python3
"""Reproducible Phase 6 comparison suite. Requires the local test bed and a built gateway."""
import concurrent.futures
import csv
import http.client
import json
import pathlib
import socket
import subprocess
import tempfile
import time
import urllib.request
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "bench/results/phase6_benchmark.csv"
GATEWAY_PORT, ADMIN_PORT = 18006, 19006
PROXIES = [3128, 3129, 3130]


def percentile(values, p):
    if not values: return 0.0
    values = sorted(values)
    return values[max(0, min(len(values)-1, int((len(values)*p + .999999)-1)))]


def reachable(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=.2): return True
    except OSError: return False


def stats():
    conn = http.client.HTTPConnection("127.0.0.1", ADMIN_PORT, timeout=3)
    conn.request("GET", "/api/stats")
    response = conn.getresponse(); data = json.loads(response.read()); conn.close()
    return data


def request(index):
    conn = http.client.HTTPConnection("127.0.0.1", GATEWAY_PORT, timeout=20)
    started = time.perf_counter()
    conn.request("GET", f"http://test.example/products/{index}", headers={"Host":"test.example"})
    response = conn.getresponse(); response.read(); status = response.status
    elapsed = (time.perf_counter()-started)*1000; conn.close()
    return status, elapsed


def set_fault(pct):
    with urllib.request.urlopen(f"http://127.0.0.1:3230/fault?fail_pct={pct}", timeout=3) as r: r.read()


def config(policy="roundrobin", forwarder="epoll", pool=8, fixed=True):
    c=json.loads((ROOT/"config/gateway.local.json").read_text())
    c["listen"].update(host="127.0.0.1",port=GATEWAY_PORT)
    c["admin"].update(host="127.0.0.1",port=ADMIN_PORT)
    c["policy"]=policy; c["forwarder"]=forwarder
    c["forwarder_pool"]={"max_connections_per_proxy":pool,"max_total_connections":pool*3,"idle_timeout_ms":30000}
    c["workers"].update(min=24 if fixed else 4,max=24,alpha=1,beta=.5)
    c["ingress"].update(threads=32,pending_connections=512)
    c["rate_limits"]["default"]={"rate":10000,"burst":10000}
    c["circuit"].update(threshold=8,cooldown_ms=1500,probe_interval_ms=500)
    c["output"]={"format":"none","path":""}
    return c


def one(label, value, health_aware=True, **opts):
    conf=config(**opts)
    with tempfile.NamedTemporaryFile("w",suffix=".json",delete=False) as f:
        json.dump(conf,f); config_path=f.name
    log=open("/tmp/phase6-benchmark-gateway.log","w")
    proc=subprocess.Popen([str(ROOT/"build/gateway"),"--config",config_path],stdout=log,stderr=subprocess.STDOUT)
    try:
        before=None
        for _ in range(120):
            if proc.poll() is not None: raise RuntimeError(pathlib.Path("/tmp/phase6-benchmark-gateway.log").read_text())
            try: before=stats(); break
            except OSError: time.sleep(.05)
        if before is None: raise RuntimeError("gateway did not become ready")
        query=urllib.parse.urlencode({"policy":conf["policy"],"rate":10000,"burst":10000,"health_aware":str(health_aware).lower()})
        with urllib.request.urlopen(f"http://127.0.0.1:{ADMIN_PORT}/api/control/settings?{query}",timeout=3) as r: r.read()
        results=[]; started=time.perf_counter()
        with concurrent.futures.ThreadPoolExecutor(max_workers=24) as pool:
            results=list(pool.map(request,range(120)))
        elapsed=time.perf_counter()-started; after=stats()
        passed=sum(1 for status,_ in results if status<500)
        client_count=len(results); gateway_count=after["requests_total"]-before["requests_total"]
        return {"scenario":label,"setting":value,"requests":client_count,"throughput_rps":round(client_count/elapsed,2),
                "p50_ms":round(percentile([x[1] for x in results],.5),3),"p95_ms":round(percentile([x[1] for x in results],.95),3),
                "p99_ms":round(percentile([x[1] for x in results],.99),3),"success_rate":round(passed/client_count,5),
                "gateway_request_delta":gateway_count,"accuracy_delta_pct":round(abs(gateway_count-client_count)*100/client_count,3),
                "metrics_match_1pct":abs(gateway_count-client_count)*100<=client_count}
    finally:
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill(); proc.wait()
        log.close(); pathlib.Path(config_path).unlink(missing_ok=True)


def main():
    testbed_owned=False
    ready=all(reachable(p) for p in [*PROXIES,3228,3229,3230]) and (reachable(8081) or reachable(18080))
    if not ready:
        subprocess.run([str(ROOT/"testbed/run_local.sh"),"start"],check=True); testbed_owned=True
    elif reachable(GATEWAY_PORT) or reachable(ADMIN_PORT):
        raise SystemExit("benchmark ports 18006/19006 are occupied")
    rows=[]
    try:
        set_fault(0)
        for policy in ("random","roundrobin","leastconn","latency","weighted"):
            rows.append(one("routing_policy",policy,policy=policy))
        for size in (2,4,8,16): rows.append(one("pool_size",str(size),pool=size))
        rows.append(one("worker_mode","fixed",fixed=True)); rows.append(one("worker_mode","aimd",fixed=False))
        rows.append(one("keep_alive","off_simple",forwarder="simple")); rows.append(one("keep_alive","on_epoll",forwarder="epoll"))
        rows.append(one("health_aware_fault_trial","faults_off",health_aware=True,policy="weighted"))
        set_fault(20)
        rows.append(one("health_aware_fault_trial","faults_on_aware",health_aware=True,policy="weighted"))
        rows.append(one("health_aware_fault_trial","faults_on_unaware",health_aware=False,policy="weighted"))
    finally:
        set_fault(0)
        if testbed_owned: subprocess.run([str(ROOT/"testbed/run_local.sh"),"stop"],check=False)
    OUT.parent.mkdir(parents=True,exist_ok=True)
    references={"routing_policy":"roundrobin","pool_size":"8","worker_mode":"fixed",
                "keep_alive":"off_simple","health_aware_fault_trial":"faults_on_aware"}
    for row in rows:
        ref=next((x for x in rows if x["scenario"]==row["scenario"] and
                  x["setting"]==references[row["scenario"]]),row)
        delta=abs(row["throughput_rps"]-ref["throughput_rps"])/max(ref["throughput_rps"],1)
        if row["setting"]==ref["setting"]: interpretation="reference measurement"
        elif delta<.05: interpretation="difference under 5%; inconclusive at this sample size"
        elif row["throughput_rps"]>ref["throughput_rps"]: interpretation=f"{delta:.1%} higher throughput than {ref['setting']}"
        else: interpretation=f"{delta:.1%} lower throughput than {ref['setting']}"
        row["interpretation"]=interpretation
    with OUT.open("w",newline="") as f:
        writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
    md=["# Phase 6 benchmark comparison","",f"Run date: {time.strftime('%Y-%m-%d %H:%M:%S %Z')}","",
        "Each scenario sends 120 requests at concurrency 24 through the current gateway build. Percentiles use client-observed request duration. Metrics accuracy compares completed client responses with the gateway's `requests_total` delta.","",
        "| Scenario | Setting | req/s | p50 ms | p95 ms | p99 ms | Success | Counter delta | Interpretation |","|---|---|---:|---:|---:|---:|---:|---:|---|"]
    for x in rows:
        md.append(f"| {x['scenario']} | {x['setting']} | {x['throughput_rps']} | {x['p50_ms']} | {x['p95_ms']} | {x['p99_ms']} | {100*x['success_rate']:.1f}% | {x['accuracy_delta_pct']}% | {x['interpretation']} |")
    md.extend(["","The `health_aware_fault_trial` compares the same injected Proxy 3 fault with health-aware selection enabled and disabled. The disabled mode ignores the rolling healthy flag but still excludes OPEN and HALF_OPEN circuits. Treat differences under 5% as inconclusive. This small local test-bed sample is comparative, not a claim that the handbook's absolute performance targets are met.",""])
    (OUT.with_suffix(".md")).write_text("\n".join(md))
    print(json.dumps({"results":str(OUT),"rows":rows,
        "note":"The fault comparison toggles rolling health eligibility while always preserving OPEN/HALF_OPEN circuit exclusion."},indent=2))


if __name__=="__main__": main()
