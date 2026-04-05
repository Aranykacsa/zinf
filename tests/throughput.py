import csv
import matplotlib.pyplot as plt

SECTOR_SIZE = 512  # bytes

def load_results(csvfile):
    sizes_mb = []
    speeds_mb = []
    with open(csvfile) as f:
        r = csv.DictReader(f)
        for row in r:
            sectors = int(row["SectorsWritten"])
            mb = sectors * SECTOR_SIZE / (1024 * 1024)  # MB
            
            kbps = float(row["Throughput_KBps"])
            mbps = kbps / 1024.0  # MB/s

            sizes_mb.append(mb)
            speeds_mb.append(mbps)
    return sizes_mb, speeds_mb

def plot(sizes_mb, speeds_mb):
    plt.figure(figsize=(12, 6))
    plt.plot(sizes_mb, speeds_mb, marker='o')
    plt.xlabel("Batch size (MB)")
    plt.ylabel("Throughput (MB/s)")
    plt.title("ZINF Raw Throughput vs Batch Size (MB/s)")
    plt.grid(True)
    plt.show()

if __name__ == "__main__":
    import sys
    if len(sys.argv) != 2:
        print("Usage: python3 throughput_graph.py benchmark.csv")
        exit(1)
    sizes_mb, speeds_mb = load_results(sys.argv[1])
    plot(sizes_mb, speeds_mb)

