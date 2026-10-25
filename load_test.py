import asyncio
import time
import statistics
from sandbox import SandboxedExecutor

C_CODE = """
#include <stdio.h>
int main() {
    int id;
    if (scanf("%d", &id) == 1) {
        printf("Assessment Candidate #%d Verified [Score: %d]\\n", id, id * 10);
    }
    return 0;
}
"""

PYTHON_CODE = """
import sys
data = sys.stdin.read().strip()
val = int(data) if data else 0
print(f"Python Candidate #{val} Verified [Score: {val * 10}]")
"""

async def single_submission(executor: SandboxedExecutor, candidate_id: int, language: str, code: str):
    t0 = time.perf_counter()
    res = await executor.run_code(
        language=language,
        source_code=code,
        input_data=f"{candidate_id}\n"
    )
    elapsed = (time.perf_counter() - t0) * 1000.0  # ms
    return candidate_id, res, elapsed

async def run_concurrent_load_test(concurrency: int = 50, language: str = "c"):
    executor = SandboxedExecutor()
    code = C_CODE if language == "c" else PYTHON_CODE

    print(f"\n=======================================================")
    print(f"🚀 Launching Online Assessment Surge: {concurrency} Concurrent Submissions")
    print(f"Language: {language.upper()} | Kernel Sandboxing: Landlock LSM + setrlimit")
    print(f"=======================================================\n")

    t_start = time.perf_counter()
    tasks = [single_submission(executor, i + 1, language, code) for i in range(concurrency)]
    results = await asyncio.gather(*tasks)
    total_wall_clock = time.perf_counter() - t_start

    success_count = sum(1 for _, res, _ in results if res.get("status") == "success")
    error_count = concurrency - success_count
    latencies = [lat for _, _, lat in results]

    latencies.sort()
    avg_latency = statistics.mean(latencies)
    p50 = statistics.median(latencies)
    p95 = latencies[int(len(latencies) * 0.95)]
    p99 = latencies[int(len(latencies) * 0.99)]
    rps = concurrency / total_wall_clock

    print(f"📊 Assessment Surge Benchmark Results:")
    print(f"-------------------------------------------------------")
    print(f"  • Total Submissions  : {concurrency}")
    print(f"  • Successful         : {success_count} ({success_count / concurrency * 100:.1f}%)")
    print(f"  • Failed             : {error_count}")
    print(f"  • Total Wall Time    : {total_wall_clock:.2f} seconds")
    print(f"  • Throughput (RPS)   : {rps:.2f} submissions/second")
    print(f"  • Min Latency        : {min(latencies):.2f} ms")
    print(f"  • Median (p50)       : {p50:.2f} ms")
    print(f"  • 95th Percentile    : {p95:.2f} ms")
    print(f"  • 99th Percentile    : {p99:.2f} ms")
    print(f"  • Max Latency        : {max(latencies):.2f} ms")
    print(f"-------------------------------------------------------\n")

    # Sample verification
    sample_id, sample_res, sample_lat = results[0]
    print(f"Sample Candidate Output (#{sample_id}): {sample_res.get('output', '').strip()} ({sample_lat:.1f}ms)")
    assert success_count == concurrency, f"Expected {concurrency} successes, got {success_count}"
    print(f"✅ Concurrency Assertion Passed: All {concurrency} submissions handled concurrently without drops!\n")

if __name__ == "__main__":
    asyncio.run(run_concurrent_load_test(50, "c"))
